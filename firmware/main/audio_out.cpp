#include "audio_out.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

#include "board_pins.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"

namespace audio_out {

static const char *TAG = "audio_out";

constexpr size_t RING_BYTES = SAMPLE_RATE * sizeof(int16_t) * 2;  // 2 s of mono
constexpr size_t CHUNK_FRAMES = 256;
constexpr uint32_t AMP_IDLE_OFF_MS = 10000;
constexpr uint32_t AMP_WAKE_MS = 10;  // silence after SD goes HIGH, before audio
// 8 x 1023 frames = 186 ms queued in DMA (1023 is the most one 4092-byte DMA
// buffer holds). While Spotify streams, the writer task measurably runs up to
// 60-110 ms late at times (not a priority problem: priority 22 changed
// nothing); with 33 ms of DMA every late run was an audible click.
constexpr uint32_t DMA_DESC_NUM = 8;
constexpr uint32_t DMA_FRAME_NUM = 1023;
constexpr float SPEAKER_SAFE_GAIN = 0.7f;
constexpr float TWO_PI = 6.28318531f;

static i2s_chan_handle_t s_tx;
static RingbufHandle_t s_ring;

// Shared with the writer task, guarded by s_lock.
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static float s_gain = 0.3f;
static uint32_t s_ramp_ms = CLICK_RAMP_MS;
static float s_max_gain = DEFAULT_MAX_GAIN;
static uint32_t s_gain_gen = 0;  // bumped on every gain change, so the task recomputes its ramp
static bool s_from_zero = false;  // next ramp starts from silence (fade_in)
static bool s_pattern_pending = false;
static Pattern s_pattern;

static std::atomic<bool> s_stop_req{false};
static std::atomic<bool> s_amp_on{false};
static std::atomic<uint32_t> s_underruns{0};  // DMA ran dry and sent zeros
static std::atomic<bool> s_flush_req{false};
static std::atomic<bool> s_paused{false};
static std::atomic<uint64_t> s_frames_written{0};
static std::atomic<uint64_t> s_frames_played{0};
static std::atomic<uint64_t> s_frames_dropped{0};

static bool IRAM_ATTR on_send_q_ovf(i2s_chan_handle_t, i2s_event_data_t *, void *) {
    s_underruns++;
    return false;
}

static uint32_t ms_to_samples(uint32_t ms) {
    return (uint64_t)ms * SAMPLE_RATE / 1000;
}

static void amp_set(bool on) {
    gpio_set_level(pins::AMP_SD, on ? 1 : 0);
    s_amp_on = on;
    ESP_LOGI(TAG, "amp %s", on ? "on" : "off");
}

static void write_frames(const int16_t *frames, size_t count) {
    size_t written;
    i2s_channel_write(s_tx, frames, count * 2 * sizeof(int16_t), &written, portMAX_DELAY);
}

static void write_silence(uint32_t frames) {
    static const int16_t silence[CHUNK_FRAMES * 2] = {};
    while (frames > 0) {
        const size_t k = std::min<size_t>(frames, CHUNK_FRAMES);
        write_frames(silence, k);
        frames -= k;
    }
}

// Switches the amp off once everything already queued in DMA has played out,
// so SD never goes LOW mid-signal (that pops).
static void amp_off_after_drain() {
    write_silence(DMA_DESC_NUM * DMA_FRAME_NUM + ms_to_samples(AMP_WAKE_MS));
    amp_set(false);
}

// Reads up to max_frames mono samples. Waits only for the first piece; a second
// read picks up data that wrapped around the end of the ring.
static size_t read_ring(int16_t *dst, size_t max_frames, TickType_t wait) {
    size_t got = 0;
    const size_t max_bytes = max_frames * sizeof(int16_t);
    while (got < max_bytes) {
        size_t len = 0;
        void *item = xRingbufferReceiveUpTo(s_ring, &len, got ? 0 : wait, max_bytes - got);
        if (!item) break;
        memcpy(reinterpret_cast<uint8_t *>(dst) + got, item, len);
        vRingbufferReturnItem(s_ring, item);
        got += len;
    }
    return got / sizeof(int16_t);
}

static void flush_ring() {
    size_t len;
    while (void *item = xRingbufferReceiveUpTo(s_ring, &len, 0, RING_BYTES)) {
        vRingbufferReturnItem(s_ring, item);
        s_frames_dropped += len / sizeof(int16_t);
    }
}

// Pattern generator state, in samples. Owned by the writer task.
struct ToneState {
    bool active = false;
    bool endless = false;
    float inc = 0;  // phase increment per sample
    float phase = 0;
    uint32_t on = 0, period = 0, group = 0, cycle = 0;
    uint32_t ramp = 0;  // attack/release per beep
    uint32_t pos = 0;   // position in the cycle
    uint32_t remaining = 0;
    TickType_t started = 0;
    uint32_t underruns_at_start = 0;
};

static void start_tone(ToneState &t, const Pattern &p) {
    t.on = std::max<uint32_t>(ms_to_samples(p.on_ms), 1);
    t.period = t.on + ms_to_samples(p.off_ms);
    t.group = t.period * std::max<uint32_t>(p.beeps, 1);
    t.cycle = t.group + ms_to_samples(p.pause_ms);
    t.ramp = std::max<uint32_t>(std::min(ms_to_samples(CLICK_RAMP_MS), t.on / 4), 1);
    t.inc = TWO_PI * std::clamp(p.freq_hz, 20.0f, 20000.0f) / SAMPLE_RATE;
    t.phase = 0;
    t.pos = 0;
    t.endless = p.total_ms == 0;
    t.remaining = ms_to_samples(p.total_ms);
    t.active = true;
    t.started = xTaskGetTickCount();
    t.underruns_at_start = s_underruns;
}

// Fills `out` with the next samples of the pattern; silent parts are generated
// too, so gain ramps keep running through the pauses.
static size_t generate_tone(ToneState &t, int16_t *out, size_t max_frames) {
    const size_t n = t.endless ? max_frames : std::min<size_t>(max_frames, t.remaining);
    for (size_t i = 0; i < n; i++) {
        float v = 0;
        if (t.pos < t.group) {
            const uint32_t q = t.pos % t.period;
            if (q < t.on) {
                const float env = std::min({1.0f, (float)q / t.ramp, (float)(t.on - q) / t.ramp});
                v = env * sinf(t.phase);
            }
        }
        out[i] = (int16_t)(32767.0f * v);
        t.phase += t.inc;
        if (t.phase >= TWO_PI) t.phase -= TWO_PI;
        if (++t.pos >= t.cycle) t.pos = 0;
    }
    if (!t.endless) {
        t.remaining -= n;
        if (t.remaining == 0) {
            t.active = false;
            ESP_LOGI(TAG, "tone done: %lu ms, %lu underruns",
                     (unsigned long)pdTICKS_TO_MS(xTaskGetTickCount() - t.started),
                     (unsigned long)(s_underruns - t.underruns_at_start));
        }
    }
    return n;
}

static void writer_task(void *) {
    // If audio output hangs, alarms are silent: let the task watchdog reboot.
    ESP_ERROR_CHECK(esp_task_wdt_add(nullptr));
    static int16_t mono[CHUNK_FRAMES];
    static int16_t frames[CHUNK_FRAMES * 2];

    float cur_gain = 0, step = 0;
    uint32_t seen_gen = UINT32_MAX;
    bool was_muting = false;
    ToneState tone;
    TickType_t last_audio = 0;

    // Stream diagnostics, logged every 10 s while PCM plays.
    constexpr TickType_t REPORT_PERIOD = pdMS_TO_TICKS(10000);
    TickType_t report_at = 0;
    bool streaming = false;  // PCM flowed in the last loop
    uint32_t dropouts = 0;   // queue ran dry mid-stream
    uint32_t underruns_at_report = s_underruns;
    size_t min_queued = SIZE_MAX;
    int peak = 0;
    int64_t last_write_us = 0;
    int64_t max_gap_us = 0;  // longest time between two DMA writes while streaming
    uint32_t stalls = 0;     // gaps over STALL_US
    constexpr int64_t STALL_US = 30000;

    for (;;) {
        esp_task_wdt_reset();
        portENTER_CRITICAL(&s_lock);
        float target = std::min(s_gain, s_max_gain);
        const float cap = s_max_gain;
        uint32_t ramp_ms = s_ramp_ms;
        const uint32_t gen = s_gain_gen;
        const bool from_zero = s_from_zero;
        s_from_zero = false;
        bool new_pattern = s_pattern_pending;
        const Pattern pattern = s_pattern;
        s_pattern_pending = false;
        portEXIT_CRITICAL(&s_lock);

        if (new_pattern) start_tone(tone, pattern);
        if (from_zero) cur_gain = 0;

        if (s_flush_req.exchange(false)) flush_ring();

        const bool stopping = s_stop_req;
        // A paused stream fades out like a stop, then holds its queued PCM.
        const bool pausing = s_paused && !tone.active;
        const bool muting = stopping || pausing;
        if (muting) {
            target = 0;
            ramp_ms = CLICK_RAMP_MS;
        }

        size_t n = 0;
        if (tone.active) {
            n = generate_tone(tone, mono, CHUNK_FRAMES);
        } else if (pausing && cur_gain == 0) {
            vTaskDelay(pdMS_TO_TICKS(10));  // faded out: leave the queue alone
        } else {
            // At least one tick, or an idle ring turns this into a busy loop.
            n = read_ring(mono, CHUNK_FRAMES, std::max<TickType_t>(pdMS_TO_TICKS(s_amp_on ? 5 : 100), 1));
            s_frames_played += n;
            if (streaming && n < CHUNK_FRAMES && !pausing) dropouts++;
            streaming = n > 0 && !pausing;
            if (streaming) {
                const size_t queued = (RING_BYTES - xRingbufferGetCurFreeSize(s_ring)) / sizeof(int16_t);
                min_queued = std::min(min_queued, queued);
                if (report_at == 0) report_at = xTaskGetTickCount() + REPORT_PERIOD;
            }
        }

        if (n == 0) {
            if (stopping) {
                tone.active = false;
                cur_gain = 0;
                if (s_amp_on) amp_off_after_drain();
                s_stop_req = false;
            } else if (s_amp_on && xTaskGetTickCount() - last_audio > pdMS_TO_TICKS(AMP_IDLE_OFF_MS)) {
                amp_set(false);
            }
            continue;
        }

        bool recalc = gen != seen_gen || muting != was_muting;
        if (!s_amp_on) {
            amp_set(true);
            write_silence(ms_to_samples(AMP_WAKE_MS));
            cur_gain = 0;
            recalc = true;
        }
        if (recalc) {
            step = std::max(fabsf(target - cur_gain), 1e-6f) / ms_to_samples(ramp_ms);
            seen_gen = gen;
            was_muting = muting;
        }
        last_audio = xTaskGetTickCount();

        for (size_t i = 0; i < n; i++) {
            if (cur_gain < target) {
                cur_gain = std::min(cur_gain + step, target);
            } else if (cur_gain > target) {
                cur_gain = std::max(cur_gain - step, target);
            }
            const int16_t v = (int16_t)lrintf(mono[i] * std::min(cur_gain, cap));
            frames[2 * i] = v;
            frames[2 * i + 1] = v;
            peak = std::max(peak, std::abs((int)v));
        }
        write_frames(frames, n);
        const int64_t now_us = esp_timer_get_time();
        if (streaming && last_write_us != 0) {
            max_gap_us = std::max(max_gap_us, now_us - last_write_us);
            if (now_us - last_write_us > STALL_US) stalls++;
        }
        last_write_us = streaming ? now_us : 0;

        if (report_at != 0 && xTaskGetTickCount() >= report_at) {
            ESP_LOGI(TAG, "stream: queue min %u ms, %lu dropouts, %lu DMA underruns, max writer gap %lu ms, "
                          "%lu gaps > 30 ms, peak %d%% FS, gain %.2f, internal heap %u (min %u)",
                     (unsigned)(min_queued == SIZE_MAX ? 0 : min_queued * 1000 / SAMPLE_RATE),
                     (unsigned long)dropouts, (unsigned long)(s_underruns - underruns_at_report),
                     (unsigned long)(max_gap_us / 1000), (unsigned long)stalls, peak * 100 / 32767, cur_gain,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
            report_at = streaming ? xTaskGetTickCount() + REPORT_PERIOD : 0;
            dropouts = 0;
            underruns_at_report = s_underruns;
            min_queued = SIZE_MAX;
            peak = 0;
            max_gap_us = 0;
            stalls = 0;
            // Printing over USB takes a while; don't count it as a writer stall.
            last_write_us = streaming ? esp_timer_get_time() : 0;
        }

        if (stopping && cur_gain == 0) {
            tone.active = false;
            flush_ring();
            amp_off_after_drain();
            s_stop_req = false;
        }
    }
}

esp_err_t init() {
    gpio_config_t sd_cfg = {};
    sd_cfg.pin_bit_mask = 1ULL << pins::AMP_SD;
    sd_cfg.mode = GPIO_MODE_OUTPUT;
    ESP_RETURN_ON_ERROR(gpio_config(&sd_cfg), TAG, "amp SD pin");
    gpio_set_level(pins::AMP_SD, 0);

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;  // send zeros when starved
    chan_cfg.dma_desc_num = DMA_DESC_NUM;
    chan_cfg.dma_frame_num = DMA_FRAME_NUM;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, nullptr), TAG, "i2s channel");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = pins::I2S_BCLK,
            .ws = pins::I2S_WS,
            .dout = pins::I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {},
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg), TAG, "i2s std mode");
    i2s_event_callbacks_t cbs = {};
    cbs.on_send_q_ovf = on_send_q_ovf;
    ESP_RETURN_ON_ERROR(i2s_channel_register_event_callback(s_tx, &cbs, nullptr), TAG, "i2s callbacks");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "i2s enable");

    s_ring = xRingbufferCreateWithCaps(RING_BYTES, RINGBUF_TYPE_BYTEBUF, MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(s_ring, ESP_ERR_NO_MEM, TAG, "ring buffer");

    BaseType_t ok = xTaskCreatePinnedToCore(writer_task, "audio_out", 4096, nullptr, 18, nullptr, 1);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "writer task");

    ESP_LOGI(TAG, "I2S %lu Hz on BCLK=%d WS=%d DOUT=%d, amp SD=%d, max_gain %.2f", (unsigned long)SAMPLE_RATE,
             pins::I2S_BCLK, pins::I2S_WS, pins::I2S_DOUT, pins::AMP_SD, s_max_gain);
    return ESP_OK;
}

size_t write_pcm(const int16_t *stereo, size_t frames, TickType_t timeout) {
    int16_t mono[CHUNK_FRAMES];
    size_t done = 0;
    while (done < frames) {
        const size_t n = std::min(CHUNK_FRAMES, frames - done);
        const int16_t *src = stereo + 2 * done;
        for (size_t i = 0; i < n; i++) {
            mono[i] = (int16_t)((src[2 * i] + src[2 * i + 1]) / 2);
        }
        if (xRingbufferSend(s_ring, mono, n * sizeof(int16_t), timeout) != pdTRUE) break;
        done += n;
    }
    s_frames_written += done;
    return done;
}

void flush() {
    s_flush_req = true;
}

void set_paused(bool paused) {
    s_paused = paused;
}

Stats stats() {
    Stats st;
    st.frames_written = s_frames_written;
    st.frames_played = s_frames_played;
    st.frames_dropped = s_frames_dropped;
    UBaseType_t free_bytes = xRingbufferGetCurFreeSize(s_ring);
    st.frames_queued = (RING_BYTES - free_bytes) / sizeof(int16_t);
    return st;
}

void set_gain(float gain, uint32_t ramp_ms) {
    portENTER_CRITICAL(&s_lock);
    s_gain = std::clamp(gain, 0.0f, 1.0f);
    s_ramp_ms = std::max(ramp_ms, CLICK_RAMP_MS);
    s_gain_gen++;
    portEXIT_CRITICAL(&s_lock);
}

float gain() {
    portENTER_CRITICAL(&s_lock);
    float g = s_gain;
    portEXIT_CRITICAL(&s_lock);
    return g;
}

void set_max_gain(float max_gain) {
    max_gain = std::clamp(max_gain, 0.0f, 1.0f);
    if (max_gain > SPEAKER_SAFE_GAIN) {
        ESP_LOGW(TAG, "max_gain %.2f is above %.2f and may exceed the speaker's 3 W rating", max_gain,
                 SPEAKER_SAFE_GAIN);
    }
    portENTER_CRITICAL(&s_lock);
    s_max_gain = max_gain;
    s_gain_gen++;
    portEXIT_CRITICAL(&s_lock);
}

float max_gain() {
    portENTER_CRITICAL(&s_lock);
    float g = s_max_gain;
    portEXIT_CRITICAL(&s_lock);
    return g;
}

void fade_in(float gain, uint32_t ramp_ms) {
    portENTER_CRITICAL(&s_lock);
    s_gain = std::clamp(gain, 0.0f, 1.0f);
    s_ramp_ms = std::max(ramp_ms, CLICK_RAMP_MS);
    s_from_zero = true;
    s_gain_gen++;
    portEXIT_CRITICAL(&s_lock);
}

void play_pattern(const Pattern &pattern) {
    portENTER_CRITICAL(&s_lock);
    s_pattern = pattern;
    s_pattern_pending = true;
    portEXIT_CRITICAL(&s_lock);
    s_stop_req = false;  // a stop still fading out must not end the new pattern
}

void play_tone(float freq_hz, uint32_t duration_ms) {
    play_pattern({freq_hz, duration_ms, 0, 1, 0, duration_ms});
}

void stop() {
    s_stop_req = true;
}

bool amp_enabled() {
    return s_amp_on;
}

}  // namespace audio_out
