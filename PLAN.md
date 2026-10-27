# Kallsup Spotify Alarm Clock — Implementation Plan

Turn an IKEA KALLSUP speaker into a stand-alone alarm clock that plays one specific Spotify track at a scheduled time. No phone is needed after a one-time setup.

> **For Claude Code:** Work through the phases **in order**. Each phase ends with a **HUMAN CHECK**. At that point, stop and ask the user to flash the board and confirm the result before continuing. You can build (`idf.py build`), but flashing and anything involving the soldering iron or multimeter is done by the user. Never commit secrets (Wi-Fi password, Spotify client secret, refresh token).

---

## 1. Hardware

| Part | Notes |
|---|---|
| ESP32-S3 **N16R8** devkit | 16 MB flash, 8 MB **octal** PSRAM |
| MAX98357A breakout | I2S class-D amp, mono |
| IKEA KALLSUP PCB | Only used as the **USB-C 5 V power input** in v1. Its Bluetooth chip and amp stay idle. |
| KALLSUP speaker | **4 Ω, 3 W** |
| 470–1000 µF electrolytic cap (≥10 V) | Across the MAX98357A VIN/GND, to prevent brownouts on bass hits |
| 2 push buttons (optional) | Snooze / Stop. New buttons, or the Kallsup ones once verified (see 1.3). |

### 1.1 Pin map (ESP32-S3 N16R8)

Pins that must **not** be used on N16R8:
- GPIO 26–32: internal flash
- GPIO 33–37: octal PSRAM
- GPIO 19/20: native USB
- Strapping pins 0, 3, 45, 46

| Signal | ESP32-S3 GPIO | MAX98357A pin |
|---|---|---|
| I2S BCLK | GPIO 5 | BCLK |
| I2S WS / LRCLK | GPIO 6 | LRC |
| I2S DATA | GPIO 7 | DIN |
| Amp enable | GPIO 8 | SD (LOW = shutdown, HIGH = on, left channel) |
| — | — | GAIN: leave unconnected (9 dB) |
| 5 V | 5V pin | VIN |
| GND | GND | GND |
| Snooze button | GPIO 9 (internal pull-up, active LOW) | — |
| Stop button | GPIO 10 (internal pull-up, active LOW) | — |
| OLED SDA / SCL (optional) | GPIO 11 / GPIO 12, module VCC on 3V3 | — |
| Status LED | onboard RGB, usually GPIO 48 (**check your board**, some use 38) | — |

The firmware downmixes stereo to mono and writes the same signal to both I2S channels. That way the SD pin's channel-select behaviour doesn't matter.

### 1.2 Speaker and power — HUMAN TASKS
1. Unplug the speaker from the Kallsup board's **P2** connector. Wire it to the MAX98357A **OUT+ / OUT−** screw terminals.
   **Never connect either amp output to GND** — they are bridge-tied (BTL).
2. With a multimeter, check the Kallsup test pad **VCC_5V** (to **GND1**):
   - ≈5 V with USB-C plugged in
   - ≈0 V with USB unplugged and the battery connected

   If both hold, VCC_5V is raw USB VBUS. Use it to feed the ESP32 **5V** pin and the MAX98357A **VIN**.
   If it shows battery voltage without USB, **stop** and report back. In that case, power everything from the devkit's own USB port instead.
3. v1 is USB-powered only. The Kallsup battery can stay plugged into its board, but it isn't used.
4. Use a power supply of at least 1 A. Peak current at 3 W into 4 Ω is ~1 A.

### 1.3 Reusing the Kallsup buttons (optional) — HUMAN TASK
The Kallsup buttons have test pads **S1** and **S2** on the back of the board. They are wired to the Bluetooth chip, and may switch to GND, to battery voltage, or into a resistor ladder.
- Measure the voltage on each pad, released and pressed, with USB connected.
- Only wire a pad to an ESP GPIO if it switches between a level **≤3.3 V** and GND.
- Otherwise, use separate buttons.

---

## 2. Software stack

- **ESP-IDF v5.5** in C++17. Not Arduino, because cspot is an ESP-IDF component.
- **cspot** (https://github.com/feelfreelinux/cspot) as a git submodule, pinned to a specific commit. It makes the ESP a Spotify Connect device (Premium required).
  - Reference: https://github.com/jetstarlp/music_streamer_esp32 builds cspot for an ESP32-S3 with IDF 5.5. Use it for build and integration hints, but note it is untested on real hardware.
- **Spotify Web API** (HTTPS via `esp_http_client`) to start the track on this device at alarm time.
- `esp_http_server` for a small configuration page, `mdns` (hostname `kallsup-alarm.local`), SNTP for time, and NVS for settings.

### 2.1 sdkconfig.defaults (key settings)
```
CONFIG_IDF_TARGET="esp32s3"
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_LWIP_SNTP_UPDATE_DELAY=21600000   # re-sync every 6 h
```

### 2.2 Partitions (16 MB)
`nvs` 64 KB, `otadata`, `ota_0` 4 MB, `ota_1` 4 MB, `storage` (LittleFS) for the web UI and fallback audio.

### 2.3 Repository layout
```
kallsup-alarm/
├── PLAN.md
├── docs/WIRING.md
├── tools/get_refresh_token.py      # one-time OAuth helper, runs on a PC
└── firmware/
    ├── CMakeLists.txt
    ├── sdkconfig.defaults
    ├── partitions.csv
    ├── components/cspot/           # git submodule (+ bell)
    └── main/
        ├── app_main.cpp
        ├── audio_out.{h,cpp}       # I2S driver, amp SD pin, mono downmix, gain/fade, tone generator
        ├── spotify_connect.{h,cpp} # cspot wrapper, AudioSink → audio_out, credential persistence
        ├── spotify_api.{h,cpp}     # token refresh, list devices, play, volume, repeat, pause
        ├── alarm.{h,cpp}           # scheduler + state machine
        ├── settings.{h,cpp}        # NVS-backed config
        ├── net.{h,cpp}             # Wi-Fi (STA + fallback AP), SNTP, mDNS
        ├── web_ui.{h,cpp}          # HTTP config page + JSON API
        ├── buttons.{h,cpp}
        └── www/index.html          # embedded via EMBED_TXTFILES
```

### 2.4 Settings stored in NVS
- **Wi-Fi:** SSID, password
- **Time zone:** default `CET-1CEST,M3.5.0,M10.5.0/3` (Europe/Berlin)
- **Alarms:** list of up to 4 entries, each with `{enabled, hh, mm, weekday mask}`
- **Track:** `track_uri`, e.g. `spotify:track:XXXXXXXXXXXXXXXXXXXXXX` (user fills in)
- **Volume:** `max_gain` (software cap, default 0.6), `fade_in_s` (default 30)
- **Behaviour:** `snooze_min` (default 9), `auto_stop_min` (default 30), `repeat_track` (default true)
- **Spotify:** `spotify_client_id`, `spotify_client_secret`, `spotify_refresh_token`
- **Device:** `device_name` (default "Kallsup Alarm"); cspot's stored login credentials

---

## 3. Spotify setup (one-time) — HUMAN TASKS, Claude Code writes the tooling
1. **Create the developer app.** At https://developer.spotify.com/dashboard, create an app with the **Web API** enabled.
   - Redirect URI: `http://127.0.0.1:8888/callback`. Spotify only accepts plain `http` for loopback **IP** addresses, not `localhost`.
   - The app owner needs **Premium**. Development Mode allows up to 5 users, which is plenty here.
2. **Get a refresh token.** Run `tools/get_refresh_token.py` on a PC.
   - What it does: starts a local server on 127.0.0.1:8888, opens the authorize URL with scopes `user-read-playback-state user-modify-playback-state`, exchanges the code, and prints the **refresh token**.
   - Requirements: Python standard library only (`http.server`, `urllib`, `webbrowser`, `secrets`, `base64`).
3. **Enter credentials.** In the ESP web UI, enter the client ID, client secret and refresh token.
4. **Pair once from a phone.** Open Spotify, choose "Kallsup Alarm" under *Connect to a device*, and play anything.
   cspot only registers with Spotify's servers after a local app has connected once. The firmware must then **persist cspot's credentials to NVS** and log in with them on every later boot, without the phone.

Token handling in firmware:
- Refresh the access token before each alarm, and whenever a call returns HTTP 401.
- If the token response contains a new `refresh_token`, **overwrite the stored one**.

---

## 4. Alarm logic

States: `IDLE → PREPARING → RINGING_SPOTIFY | RINGING_FALLBACK → SNOOZED → … → IDLE`

1. **PREPARING** (90 s before alarm time):
   - Ensure Wi-Fi is connected and the cspot session is logged in. Reconnect if not.
   - Refresh the access token.
   - `GET /v1/me/player/devices` and find this device by `device_name` to get its `device_id`.
2. **At alarm time:**
   - Enable the amp (SD HIGH) and set the sink to "alarm mode": ignore Spotify's volume and use the local fade-in from 0 to `max_gain` over `fade_in_s`.
   - `PUT /v1/me/player/play?device_id=<id>` with body `{"uris":["<track_uri>"],"position_ms":0}`
   - If `repeat_track`: `PUT /v1/me/player/repeat?state=track&device_id=<id>`
   - `PUT /v1/me/player/volume?volume_percent=100&device_id=<id>` (loudness is then controlled by the local gain)
   - If the device isn't found or `play` fails: try `PUT /v1/me/player` (transfer playback to this device), then `play` again.
3. **Watchdog:**
   - If no PCM data reaches the sink within **15 s** of the play request, go to **RINGING_FALLBACK**.
   - RINGING_FALLBACK plays a locally generated beep pattern (sine bursts, also faded in). This must work with no Wi-Fi at all.
4. **Snooze button:**
   - `PUT /v1/me/player/pause`, mute the sink, amp SD LOW.
   - Re-ring after `snooze_min` minutes.
5. **Stop button**, or `auto_stop_min` reached:
   - Pause, mute, amp SD LOW, return to IDLE.
6. **If time was never synced** (no NTP since boot): do not guess. Show an error state on the LED and the web UI.

**Outside alarms:** the device works as a normal Spotify Connect speaker, using Spotify's volume × `max_gain`. Turn the amp on when audio starts, and off after 10 s of silence to avoid hiss.

---

## 5. Phases

### Phase 0 — Skeleton
- ESP-IDF project, `sdkconfig.defaults`, partitions, empty modules, `.gitignore` (build/, sdkconfig, secrets).
- Add cspot as a submodule; do not wire it in yet.
- **Done when:** `idf.py build` succeeds and the boot log shows 8 MB PSRAM.
- **HUMAN CHECK:** flash, read the log.

### Phase 1 — Audio output
- `audio_out`: I2S std mode, 44.1 kHz, 16-bit, stereo frames carrying mono content. Uses the amp SD pin and gain with a hard cap of `max_gain`.
- Ring buffer in PSRAM. Tone generator. Debug command that plays 440 Hz for 3 s at 30 % volume.
- **HUMAN CHECK:** clean tone, no clicks at start and stop (ramp 20 ms), no brownout reset at `max_gain`.

### Phase 2 — Network and time
- Wi-Fi station mode with credentials from NVS. If none are stored, or the connection fails for 60 s, start an AP `Kallsup-Setup` with the web UI.
- SNTP and time zone, mDNS.
- **HUMAN CHECK:** the log shows the correct local time, and `kallsup-alarm.local` resolves.

### Phase 3 — Settings and web UI
- Single-page UI to edit all settings from 2.4, show status (time, Wi-Fi, Spotify login, next alarm), and buttons for "Test alarm now" and "Test fallback beep".
- JSON endpoints: `GET/POST /api/settings`, `GET /api/status`, `POST /api/test/{alarm,fallback,stop}`.
- Hide secrets on read (show only "set" / "not set").
- **HUMAN CHECK:** settings survive a reboot.

### Phase 4 — Offline alarm clock (fallback only)
- `alarm` state machine plus buttons, using the fallback beep only.
- **Done when:** the clock rings at the set time, snooze and stop work, and auto-stop works. This is already a working alarm clock.
- **HUMAN CHECK.**

### Phase 5 — Spotify Connect (cspot)
- Integrate cspot. Implement an AudioSink that feeds `audio_out` (downmix to mono) and applies Spotify's volume callback.
- Persist cspot credentials after the first zeroconf login, and use them on boot.
- Adapt cspot and bell for IDF 5.5 if needed; check the reference repo for patches. Keep any patches small and documented in `docs/`.
- **HUMAN CHECK:**
  - The device appears in the Spotify app and plays.
  - After a power cycle **with the phone off**, it logs in again by itself. Check the log for a successful login with stored credentials.

### Phase 6 — Web API client
- `spotify_api`: token refresh (with rotation), list devices, play URIs, repeat, volume, pause, transfer.
- `tools/get_refresh_token.py`.
- **HUMAN CHECK:** "Test alarm now" in the web UI starts the configured track on the Kallsup, without a phone.

### Phase 7 — Integration and hardening
- Hook Spotify playback into the alarm state machine, with the 15 s fallback and fade-in.
- Wi-Fi reconnect with backoff; task watchdog; a daily reconnect of the cspot session if it is stale.
- LED status colours: idle / ringing / error.
- OTA update via the web UI (`POST /api/ota`).
- Write `docs/WIRING.md` and a README with the setup steps from section 3.
- **HUMAN CHECK:** a 3-day soak test with a daily alarm. Also check that pulling the router power makes the fallback beep ring.

---

## 6. Known risks
- **cspot can break when Spotify changes its protocol or authentication.** Pin the commit, and keep the offline fallback alarm working no matter what.
- **Spotify Web API Development Mode** requires the app owner to have Premium. Access stops if Premium lapses; the firmware should log a clear error in that case.
- **Speaker protection:** the MAX98357A at 5 V can exceed 3 W into 4 Ω. Keep `max_gain` ≤ 0.7 unless the user tests higher.
- **Memory:** cspot plus two TLS sessions needs PSRAM. Allocate large buffers with `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`, and log free internal heap every 60 s during development.
