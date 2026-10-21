#pragma once
// I2S driver, amp SD pin, mono downmix, gain/fade, tone generator.
//
// Output is 44.1 kHz 16-bit stereo I2S, both channels carrying the same mono
// signal. Queued PCM lives in a PSRAM ring buffer and is drained by a writer
// task, which applies the gain ramp and switches the amp on when audio starts
// and off after 10 s of silence.

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

namespace audio_out {

constexpr uint32_t SAMPLE_RATE = 44100;
constexpr float DEFAULT_MAX_GAIN = 0.6f;
constexpr uint32_t CLICK_RAMP_MS = 20;  // shortest gain ramp, avoids clicks

esp_err_t init();

// Queues interleaved 16-bit stereo PCM, downmixed to mono. Waits up to
// `timeout` for space; returns the number of frames queued.
size_t write_pcm(const int16_t *stereo, size_t frames, TickType_t timeout);

// Drops queued PCM (not a running tone). The amp stays on.
void flush();

// Pauses PCM playback: fades out over 20 ms and stops draining the queue, so
// writers fill it up and then block. Resuming fades back in. Tones ignore it.
void set_paused(bool paused);

struct Stats {
    uint64_t frames_written;  // queued by write_pcm
    uint64_t frames_played;   // taken from the queue for output
    uint64_t frames_dropped;  // discarded by flush() or stop()
    size_t frames_queued;
};
Stats stats();

// Output gain 0..1, reached linearly over `ramp_ms` (at least CLICK_RAMP_MS).
// The effective gain never exceeds max_gain().
void set_gain(float gain, uint32_t ramp_ms = CLICK_RAMP_MS);
float gain();

// Hard ceiling on the output gain, for speaker protection. 0..1.
void set_max_gain(float max_gain);
float max_gain();

// Like set_gain, but the ramp always starts from silence.
void fade_in(float gain, uint32_t ramp_ms);

// Beep pattern: groups of `beeps` beeps (`on_ms` sound, `off_ms` silence),
// each group followed by `pause_ms` silence. Runs `total_ms`, or until stop()
// if 0. Each beep has a short attack and release, so it never clicks.
struct Pattern {
    float freq_hz;
    uint32_t on_ms;
    uint32_t off_ms;
    uint32_t beeps;
    uint32_t pause_ms;
    uint32_t total_ms;
};

// Plays a pattern of full-scale sine beeps (times the gain), replacing any
// running one. While it lasts, queued PCM is held back.
void play_pattern(const Pattern &pattern);

// One continuous sine of `duration_ms`.
void play_tone(float freq_hz, uint32_t duration_ms);

// Fades out over 20 ms, drops the tone and queued PCM, and switches the amp off.
void stop();

bool amp_enabled();

}  // namespace audio_out
