#include "display.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <ctime>
#include <string>

#include "alarm.h"
#include "board_pins.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_lcd_io_i2c.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net.h"
#include "settings.h"
#include "spotify_connect.h"

namespace display {

static const char *TAG = "display";

constexpr int W = 128, H = 64;
constexpr uint8_t I2C_ADDR = 0x3C;
constexpr uint32_t I2C_HZ = 400 * 1000;
constexpr bool ROTATE_180 = true;  // the module is mounted with its pins at the bottom
// Two-colour panels have a yellow band of 16 rows: at the top when rotated, else at the bottom.
constexpr int BAND_Y = ROTATE_180 ? 0 : H - 16;
constexpr TickType_t FRAME = pdMS_TO_TICKS(50);  // 20 fps, smooth enough for blinks
constexpr uint32_t SCROLL_PAUSE_MS = 2000;  // at either end of a scrolling line
constexpr uint32_t SCROLL_MS_PER_PX = 40;

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static uint8_t s_fb[W * H / 8];  // SSD1306 layout: 8 pages of 128 columns, bit 0 on top

// --- font: 5 x 7, one byte per column, bit 0 on top --------------------------

static const uint8_t FONT[][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x5F, 0x00, 0x00}, {0x00, 0x07, 0x00, 0x07, 0x00},
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, {0x24, 0x2A, 0x7F, 0x2A, 0x12}, {0x23, 0x13, 0x08, 0x64, 0x62},
    {0x36, 0x49, 0x56, 0x20, 0x50}, {0x00, 0x05, 0x03, 0x00, 0x00}, {0x00, 0x1C, 0x22, 0x41, 0x00},
    {0x00, 0x41, 0x22, 0x1C, 0x00}, {0x2A, 0x1C, 0x7F, 0x1C, 0x2A}, {0x08, 0x08, 0x3E, 0x08, 0x08},
    {0x00, 0x50, 0x30, 0x00, 0x00}, {0x08, 0x08, 0x08, 0x08, 0x08}, {0x00, 0x60, 0x60, 0x00, 0x00},
    {0x20, 0x10, 0x08, 0x04, 0x02}, {0x3E, 0x51, 0x49, 0x45, 0x3E}, {0x00, 0x42, 0x7F, 0x40, 0x00},
    {0x42, 0x61, 0x51, 0x49, 0x46}, {0x21, 0x41, 0x45, 0x4B, 0x31}, {0x18, 0x14, 0x12, 0x7F, 0x10},
    {0x27, 0x45, 0x45, 0x45, 0x39}, {0x3C, 0x4A, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03},
    {0x36, 0x49, 0x49, 0x49, 0x36}, {0x06, 0x49, 0x49, 0x29, 0x1E}, {0x00, 0x36, 0x36, 0x00, 0x00},
    {0x00, 0x56, 0x36, 0x00, 0x00}, {0x08, 0x14, 0x22, 0x41, 0x00}, {0x14, 0x14, 0x14, 0x14, 0x14},
    {0x00, 0x41, 0x22, 0x14, 0x08}, {0x02, 0x01, 0x51, 0x09, 0x06}, {0x32, 0x49, 0x79, 0x41, 0x3E},
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, {0x7F, 0x49, 0x49, 0x49, 0x36}, {0x3E, 0x41, 0x41, 0x41, 0x22},
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, {0x7F, 0x49, 0x49, 0x49, 0x41}, {0x7F, 0x09, 0x09, 0x09, 0x01},
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, {0x7F, 0x08, 0x08, 0x08, 0x7F}, {0x00, 0x41, 0x7F, 0x41, 0x00},
    {0x20, 0x40, 0x41, 0x3F, 0x01}, {0x7F, 0x08, 0x14, 0x22, 0x41}, {0x7F, 0x40, 0x40, 0x40, 0x40},
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, {0x7F, 0x04, 0x08, 0x10, 0x7F}, {0x3E, 0x41, 0x41, 0x41, 0x3E},
    {0x7F, 0x09, 0x09, 0x09, 0x06}, {0x3E, 0x41, 0x51, 0x21, 0x5E}, {0x7F, 0x09, 0x19, 0x29, 0x46},
    {0x46, 0x49, 0x49, 0x49, 0x31}, {0x01, 0x01, 0x7F, 0x01, 0x01}, {0x3F, 0x40, 0x40, 0x40, 0x3F},
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, {0x3F, 0x40, 0x38, 0x40, 0x3F}, {0x63, 0x14, 0x08, 0x14, 0x63},
    {0x07, 0x08, 0x70, 0x08, 0x07}, {0x61, 0x51, 0x49, 0x45, 0x43}, {0x00, 0x7F, 0x41, 0x41, 0x00},
    {0x02, 0x04, 0x08, 0x10, 0x20}, {0x00, 0x41, 0x41, 0x7F, 0x00}, {0x04, 0x02, 0x01, 0x02, 0x04},
    {0x40, 0x40, 0x40, 0x40, 0x40}, {0x00, 0x01, 0x02, 0x04, 0x00}, {0x20, 0x54, 0x54, 0x54, 0x78},
    {0x7F, 0x48, 0x44, 0x44, 0x38}, {0x38, 0x44, 0x44, 0x44, 0x20}, {0x38, 0x44, 0x44, 0x48, 0x7F},
    {0x38, 0x54, 0x54, 0x54, 0x18}, {0x08, 0x7E, 0x09, 0x01, 0x02}, {0x0C, 0x52, 0x52, 0x52, 0x3E},
    {0x7F, 0x08, 0x04, 0x04, 0x78}, {0x00, 0x44, 0x7D, 0x40, 0x00}, {0x20, 0x40, 0x44, 0x3D, 0x00},
    {0x7F, 0x10, 0x28, 0x44, 0x00}, {0x00, 0x41, 0x7F, 0x40, 0x00}, {0x7C, 0x04, 0x18, 0x04, 0x78},
    {0x7C, 0x08, 0x04, 0x04, 0x78}, {0x38, 0x44, 0x44, 0x44, 0x38}, {0x7C, 0x14, 0x14, 0x14, 0x08},
    {0x08, 0x14, 0x14, 0x18, 0x7C}, {0x7C, 0x08, 0x04, 0x04, 0x08}, {0x48, 0x54, 0x54, 0x54, 0x20},
    {0x04, 0x3F, 0x44, 0x40, 0x20}, {0x3C, 0x40, 0x40, 0x20, 0x7C}, {0x1C, 0x20, 0x40, 0x20, 0x1C},
    {0x3C, 0x40, 0x30, 0x40, 0x3C}, {0x44, 0x28, 0x10, 0x28, 0x44}, {0x0C, 0x50, 0x50, 0x50, 0x3C},
    {0x44, 0x64, 0x54, 0x4C, 0x44}, {0x00, 0x08, 0x36, 0x41, 0x00}, {0x00, 0x00, 0x7F, 0x00, 0x00},
    {0x00, 0x41, 0x36, 0x08, 0x00}, {0x08, 0x04, 0x08, 0x10, 0x08},
};  // ' ' .. '~'

struct ExtraGlyph {
    char32_t cp;
    uint8_t cols[5];
};

static const ExtraGlyph EXTRA[] = {
    {U'Ä', {0x7D, 0x12, 0x11, 0x12, 0x7D}}, {U'Ö', {0x3D, 0x42, 0x42, 0x42, 0x3D}},
    {U'Ü', {0x3D, 0x40, 0x40, 0x40, 0x3D}}, {U'ä', {0x20, 0x55, 0x54, 0x55, 0x78}},
    {U'ö', {0x38, 0x45, 0x44, 0x45, 0x38}}, {U'ü', {0x3C, 0x41, 0x40, 0x21, 0x7C}},
    {U'ß', {0x7E, 0x01, 0x45, 0x4A, 0x30}},
};

// Other Latin-1 letters lose their accents (U+00C0 .. U+00FF).
static const char LATIN1_FOLD[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPsaaaaaaaceeeeiiiidnooooo/ouuuuypy";

static const uint8_t *glyph(char32_t cp) {
    for (const ExtraGlyph &g : EXTRA) {
        if (g.cp == cp) return g.cols;
    }
    if (cp >= 0xC0 && cp <= 0xFF) cp = (unsigned char)LATIN1_FOLD[cp - 0xC0];
    switch (cp) {
        case U'‘': case U'’': cp = '\''; break;
        case U'“': case U'”': case U'„': cp = '"'; break;
        case U'–': case U'—': cp = '-'; break;
        default: break;
    }
    if (cp < 0x20 || cp > 0x7E) cp = '?';
    return FONT[cp - 0x20];
}

static std::u32string decode_utf8(const std::string &s) {
    std::u32string out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = s[i];
        const int len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        if (len == 0 || i + len > s.size()) {
            out += U'?';
            i++;
            continue;
        }
        char32_t cp = len == 1 ? c : c & (0x7F >> len);
        for (int k = 1; k < len; k++) cp = (cp << 6) | (s[i + k] & 0x3F);
        out += cp;
        i += len;
    }
    return out;
}

// --- drawing ------------------------------------------------------------------

struct Clip {
    int x0 = 0, x1 = W;  // drawing is limited to x0 <= x < x1
};
static Clip s_clip;

static void paint(int x, int y, int w, int h, bool on) {
    for (int px = std::max(x, s_clip.x0); px < std::min(x + w, s_clip.x1); px++) {
        for (int py = std::max(y, 0); py < std::min(y + h, H); py++) {
            if (on) {
                s_fb[(py / 8) * W + px] |= 1 << (py % 8);
            } else {
                s_fb[(py / 8) * W + px] &= ~(1 << (py % 8));
            }
        }
    }
}

static void fill(int x, int y, int w, int h) {
    paint(x, y, w, h, true);
}

static void clear(int x, int y, int w, int h) {
    paint(x, y, w, h, false);
}

constexpr int CHAR_W = 6;  // 5 columns + 1 space

static int text_width(const std::u32string &text, int scale = 1) {
    return text.empty() ? 0 : ((int)text.size() * CHAR_W - 1) * scale;
}

static void draw_text(int x, int y, const std::u32string &text, int scale = 1) {
    for (char32_t cp : text) {
        if (x >= s_clip.x1) break;
        const uint8_t *g = glyph(cp);
        for (int col = 0; col < 5; col++) {
            for (int row = 0; row < 7; row++) {
                if (g[col] & (1 << row)) fill(x + col * scale, y + row * scale, scale, scale);
            }
        }
        x += CHAR_W * scale;
    }
}

static void draw_text(int x, int y, const std::string &utf8, int scale = 1) {
    draw_text(x, y, decode_utf8(utf8), scale);
}

// A line from x to the right edge; if too long it scrolls to its end and back
// to the start, pausing at both ends. Restarts when the text changes.
struct Scroller {
    std::string text;
    uint32_t since_ms = 0;

    void draw(int x, int y, const std::string &utf8, uint32_t now_ms) {
        if (utf8 != text) {
            text = utf8;
            since_ms = now_ms;
        }
        const std::u32string t = decode_utf8(utf8);
        const int overflow = text_width(t) - (W - x);
        int offset = 0;
        if (overflow > 0) {
            const uint32_t travel = overflow * SCROLL_MS_PER_PX;
            const uint32_t cycle = 2 * (SCROLL_PAUSE_MS + travel);
            uint32_t phase = (now_ms - since_ms) % cycle;
            if (phase >= cycle / 2) phase = cycle - phase;  // on the way back
            if (phase > SCROLL_PAUSE_MS) offset = std::min<int>((phase - SCROLL_PAUSE_MS) / SCROLL_MS_PER_PX, overflow);
        }
        s_clip = {x, W};
        draw_text(x - offset, y, t);
        s_clip = {};
    }
};

static Scroller s_lines[3], s_title, s_artist;

static void draw_wifi_bars(int x, int y, int bars) {
    for (int i = 0; i < 4; i++) {
        const int h = 3 + 3 * i;
        if (i < bars) {
            fill(x + 4 * i, y + 12 - h, 3, h);
        } else {
            fill(x + 4 * i, y + 11, 3, 1);  // an empty slot
        }
    }
}

static void draw_play(int x, int y) {
    for (int c = 0; c < 4; c++) fill(x + c, y + c, 1, 7 - 2 * c);
}

static void draw_pause(int x, int y) {
    fill(x, y, 2, 7);
    fill(x + 3, y, 2, 7);
}

// --- face ---------------------------------------------------------------------
// Two robot eyes. Awake: they blink and glance around. Happy while music plays:
// arched eyes that bob, with an equaliser in the bottom band. Sleepy at night:
// closed eyes and rising z's.

enum class Mood { AWAKE, HAPPY, SLEEPY };

// The eyes stay out of the yellow band, also while glancing up or down.
constexpr int EYE_W = 30, EYE_H = 34, EYE_R = 9, EYE_GAP = 20, EYE_CY = BAND_Y == 0 ? 40 : 27;
constexpr uint32_t BLINK_MS = 160;
constexpr int EQ_BARS = 16;

struct Face {
    float dx = 0, dy = 0;  // current gaze offset
    float tx = 0, ty = 0;  // where the gaze is heading
    uint32_t blink_at = 0, blink_start = 0, glance_at = 0;
    float eq[EQ_BARS] = {};
};
static Face s_face_anim;

static uint32_t rnd(uint32_t lo, uint32_t hi) {
    return lo + esp_random() % (hi - lo + 1);
}

// Filled rectangle with its corners rounded by r.
static void fill_round(int x, int y, int w, int h, int r) {
    r = std::min({r, w / 2, h / 2});
    for (int row = 0; row < h; row++) {
        float d = 0;  // how far into a corner this row is
        if (row < r) d = r - row - 0.5f;
        if (row >= h - r) d = row - (h - r) + 0.5f;
        const int inset = d > 0 ? (int)lroundf(r - sqrtf(r * r - d * d)) : 0;
        fill(x + inset, y + row, w - 2 * inset, 1);
    }
}

static void clear_disc(int cx, int cy, int r) {
    for (int dy = -r; dy <= r; dy++) {
        const int half = (int)sqrtf((float)(r * r - dy * dy));
        clear(cx - half, cy + dy, 2 * half + 1, 1);
    }
}

static void render_face(uint32_t now_ms, Mood mood) {
    Face &f = s_face_anim;
    if (now_ms >= f.blink_at) {
        f.blink_start = now_ms;
        f.blink_at = now_ms + (rnd(0, 4) == 0 ? 350 : rnd(2500, 6000));  // now and then a double blink
    }
    if (now_ms >= f.glance_at) {
        const bool back = f.tx != 0 || f.ty != 0;
        f.tx = back ? 0 : (float)rnd(0, 24) - 12;
        f.ty = back ? 0 : (float)rnd(0, 10) - 5;
        f.glance_at = now_ms + (back ? rnd(2000, 5000) : rnd(600, 2500));
    }
    f.dx += (f.tx - f.dx) * 0.35f;
    f.dy += (f.ty - f.dy) * 0.35f;

    const uint32_t since_blink = now_ms - f.blink_start;
    const float open = since_blink < BLINK_MS ? fabsf(1.0f - 2.0f * since_blink / BLINK_MS) : 1.0f;
    const int gx = (int)lroundf(f.dx), gy = (int)lroundf(f.dy);
    const int left_x = W / 2 - EYE_GAP / 2 - EYE_W;
    const int right_x = W / 2 + EYE_GAP / 2;

    switch (mood) {
        case Mood::AWAKE: {
            const int h = std::max(2, (int)lroundf(EYE_H * open));
            for (int x : {left_x, right_x}) fill_round(x + gx, EYE_CY - h / 2 + gy, EYE_W, h, EYE_R);
            break;
        }
        case Mood::HAPPY: {
            const int bob = (int)lroundf(2.0f * sinf(now_ms * 6.2831853f / 700.0f));
            constexpr int HAPPY_H = 22, ARCH = 7;  // arch thickness at the top
            const int top = EYE_CY - HAPPY_H / 2 + bob;
            for (int x : {left_x, right_x}) {
                fill_round(x, top, EYE_W, HAPPY_H, EYE_R + 4);
                clear_disc(x + EYE_W / 2, top + ARCH + 15, 15);  // leaves an arch: ^ ^
                clear(x, top + HAPPY_H - 3, EYE_W, 3);           // blunt ends
            }
            for (int i = 0; i < EQ_BARS; i++) {
                f.eq[i] += ((float)rnd(1, 12) - f.eq[i]) * 0.4f;
                const int h = std::max(1, (int)lroundf(f.eq[i]));
                fill(2 + i * 8, BAND_Y + 16 - h, 5, h);  // in the yellow band
            }
            break;
        }
        case Mood::SLEEPY: {
            const int breathe = (int)lroundf(1.5f * sinf(now_ms * 6.2831853f / 4000.0f));
            for (int x : {left_x, right_x}) fill_round(x + 2, EYE_CY + 6 + breathe, EYE_W - 4, 4, 2);
            for (int i = 0; i < 2; i++) {  // two z's rise from the right eye, half a cycle apart
                const uint32_t p = (now_ms + i * 1500) % 3000;
                draw_text(right_x + EYE_W - 6 + (int)(p / 300), EYE_CY - (int)(p / 120), U"z", p < 1500 ? 1 : 2);
            }
            break;
        }
    }
}

// --- screen -------------------------------------------------------------------

static const char *const WEEKDAYS[] = {"So", "Mo", "Di", "Mi", "Do", "Fr", "Sa"};

static std::string hhmm(const struct tm &t) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
    return buf;
}

static std::string alarm_line(const settings::Config &cfg, time_t now) {
    using alarm_clock::State;
    if (alarm_clock::ringing()) return "Wecker klingelt!";
    struct tm t;
    if (alarm_clock::state() == State::SNOOZED) {
        const time_t until = alarm_clock::snoozed_until();
        localtime_r(&until, &t);
        return "Snooze bis " + hhmm(t);
    }
    if (!alarm_clock::time_ok()) return "Uhrzeit fehlt";
    const time_t next = alarm_clock::next_alarm(cfg, now);
    if (next == 0) return "Kein Wecker";
    localtime_r(&next, &t);
    return std::string("Wecker ") + WEEKDAYS[t.tm_wday] + " " + hhmm(t);
}

static std::atomic<uint32_t> s_interaction_ms{0};
static std::atomic<bool> s_face{false};
static std::string s_last_track;

static uint32_t now_ms() {
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

void wake() {
    s_interaction_ms = now_ms();
    s_face = false;
}

// The info screen stays while something needs attention.
static bool face_allowed(const net::Status &ns) {
    return ns.sta_connected && !ns.ap_active && alarm_clock::time_ok() && !alarm_clock::ringing() &&
           alarm_clock::state() != alarm_clock::State::SNOOZED;
}

static void render_info(uint32_t now_ms, const settings::Config &cfg, const net::Status &ns,
                        const spotify_connect::Track &track) {
    const time_t now = time(nullptr);

    // Top band, 16 px
    struct tm t;
    localtime_r(&now, &t);
    draw_text(0, 1, net::time_synced() ? hhmm(t) : "--:--", 2);
    if (ns.sta_connected) {
        const int bars = ns.rssi >= -55 ? 4 : ns.rssi >= -65 ? 3 : ns.rssi >= -75 ? 2 : 1;
        draw_wifi_bars(W - 15, 1, bars);
    } else {
        draw_wifi_bars(W - 15, 1, 0);
    }

    // Network and alarm
    std::string l0, l1;
    if (ns.sta_connected) {
        l0 = "IP " + ns.ip;
        l1 = "WLAN " + ns.ssid;
    } else if (ns.ap_active) {
        l0 = std::string("Setup: ") + net::AP_SSID;
        l1 = std::string("Passwort ") + net::setup_password();
    } else {
        using net::WifiError;
        l0 = ns.error == WifiError::WRONG_PASSWORD ? "WLAN-Passwort falsch"
             : ns.error == WifiError::NOT_FOUND    ? "WLAN nicht gefunden"
             : cfg.wifi_ssid.empty()               ? "WLAN fehlt"
                                                   : "WLAN verbindet...";
        l1 = cfg.wifi_ssid.empty() ? "" : "WLAN " + cfg.wifi_ssid;
    }
    s_lines[0].draw(0, 18, l0, now_ms);
    s_lines[1].draw(0, 27, l1, now_ms);
    s_lines[2].draw(0, 36, alarm_line(cfg, now), now_ms);
    fill(0, 46, W, 1);  // above row 48, where the band starts when it is at the bottom

    // Spotify
    std::string title, artist;
    if (spotify_connect::active() && !track.name.empty()) {
        if (spotify_connect::remote_paused()) {
            draw_pause(0, 48);
        } else {
            draw_play(0, 48);
        }
        title = track.name;
        artist = track.artist;
    } else if (spotify_connect::logged_in()) {
        title = "Spotify bereit";
        artist = spotify_connect::user_name();
    } else {
        title = "Spotify nicht";
        artist = "verbunden";
    }
    s_title.draw(9, 48, title, now_ms);
    s_artist.draw(9, 57, artist, now_ms);
}

static bool night(const settings::Config &cfg, int hour) {
    const int start = cfg.night_start_h, end = cfg.night_end_h;
    return start <= end ? hour >= start && hour < end : hour >= start || hour < end;
}

static void render(uint32_t now_ms, const settings::Config &cfg) {
    memset(s_fb, 0, sizeof(s_fb));
    const net::Status ns = net::status();
    const spotify_connect::Track track = spotify_connect::now_playing();
    if (track.name != s_last_track) {  // a new track counts as interaction
        s_last_track = track.name;
        s_interaction_ms = now_ms;
    }
    const bool face = cfg.face_enabled && face_allowed(ns) && now_ms - s_interaction_ms >= cfg.face_after_s * 1000u;
    s_face = face;
    if (!face) {
        render_info(now_ms, cfg, ns, track);
        return;
    }
    const time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    const Mood mood = spotify_connect::active() && !spotify_connect::remote_paused() ? Mood::HAPPY
                      : night(cfg, t.tm_hour)                                       ? Mood::SLEEPY
                                                                                    : Mood::AWAKE;
    render_face(now_ms, mood);
}

// Percent to the SSD1306 contrast register, 0..255.
static void apply_brightness(int percent) {
    const uint8_t contrast[] = {(uint8_t)std::clamp(percent * 255 / 100, 1, 255)};
    esp_lcd_panel_io_tx_param(s_io, 0x81, contrast, 1);
}

static void display_task(void *) {
    static uint8_t shown[sizeof(s_fb)];
    bool first = true;
    bool on = true;  // init() switched the panel on
    int brightness = -1;
    for (;;) {
        const settings::Config cfg = settings::get();
        if (!cfg.display_enabled) {
            if (on && esp_lcd_panel_disp_on_off(s_panel, false) == ESP_OK) on = false;
            vTaskDelay(FRAME);
            continue;
        }
        if (!on && esp_lcd_panel_disp_on_off(s_panel, true) == ESP_OK) {
            on = true;
            first = true;  // redraw: the panel kept the old picture
        }
        if (cfg.display_brightness != brightness) {
            brightness = cfg.display_brightness;
            apply_brightness(brightness);
        }
        render(now_ms(), cfg);
        if (first || memcmp(shown, s_fb, sizeof(s_fb)) != 0) {
            if (esp_lcd_panel_draw_bitmap(s_panel, 0, 0, W, H, s_fb) == ESP_OK) {
                memcpy(shown, s_fb, sizeof(s_fb));
                first = false;
            }
        }
        vTaskDelay(FRAME);
    }
}

esp_err_t init() {
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = I2C_NUM_0;
    bus_cfg.sda_io_num = pins::OLED_SDA;
    bus_cfg.scl_io_num = pins::OLED_SCL;
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;  // the module has its own; these only help
    i2c_master_bus_handle_t bus;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &bus), TAG, "i2c bus");
    if (i2c_master_probe(bus, I2C_ADDR, 50) != ESP_OK) {
        ESP_LOGW(TAG, "no display at 0x%02x (SDA %d, SCL %d), running without", I2C_ADDR,
                 pins::OLED_SDA, pins::OLED_SCL);
        i2c_del_master_bus(bus);
        return ESP_OK;
    }

    esp_lcd_panel_io_i2c_config_t io_cfg = {};
    io_cfg.dev_addr = I2C_ADDR;
    io_cfg.scl_speed_hz = I2C_HZ;
    io_cfg.control_phase_bytes = 1;
    io_cfg.lcd_cmd_bits = 8;
    io_cfg.lcd_param_bits = 8;
    io_cfg.dc_bit_offset = 6;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(bus, &io_cfg, &s_io), TAG, "panel io");

    esp_lcd_panel_ssd1306_config_t ssd1306_cfg = {};
    ssd1306_cfg.height = H;
    esp_lcd_panel_dev_config_t panel_cfg = {};
    panel_cfg.bits_per_pixel = 1;
    panel_cfg.reset_gpio_num = -1;
    panel_cfg.vendor_config = &ssd1306_cfg;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_ssd1306(s_io, &panel_cfg, &s_panel), TAG, "panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init");
    if (ROTATE_180) ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, true, true), TAG, "mirror");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "display on");

    s_interaction_ms = now_ms();
    settings::on_change([](const settings::Config &, const settings::Config &) { wake(); });
    BaseType_t ok = xTaskCreate(display_task, "display", 4096, nullptr, 2, nullptr);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task");
    ESP_LOGI(TAG, "SSD1306 at 0x%02x", I2C_ADDR);
    return ESP_OK;
}

}  // namespace display
