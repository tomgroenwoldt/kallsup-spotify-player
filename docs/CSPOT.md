# cspot integration

The firmware uses [cspot](https://github.com/philippe44/cspot) to act as a Spotify Connect device.

## Which cspot

| Part | Source | Pinned at |
|---|---|---|
| cspot | `philippe44/cspot` (fork used by squeezelite-esp32) | `99e5c95` (2026-10-09) |
| bell | `philippe44/bell`, submodule of cspot | `46a4a8f` |

**Why the fork and not `feelfreelinux/cspot`** (as PLAN.md first said): upstream's last change is from July 2024, and Spotify has changed its login and token flow since. The fork is maintained and shipped in squeezelite-esp32. It fetches the CDN access token with the client-credentials grant, so **cspot needs the Spotify client ID and secret** from the web UI, the same ones the Web API uses.

The submodule lives at `firmware/components/cspot` and stays **unmodified**. ESP-IDF doesn't build it directly (`EXCLUDE_COMPONENTS cspot` in `firmware/CMakeLists.txt`). The component `firmware/components/cspot_lib` compiles the needed sources from it instead.

## How it's built (`components/cspot_lib`)

- **cspot:** all of `cspot/src/*.cpp`.
- **bell:** only what cspot needs, chosen the way squeezelite-esp32 does it:
  - Compiled: nanopb, utilities, I/O (HTTP client, TLS socket), `WrappedSemaphore`, and tremor for Vorbis.
  - Left out: codecs, audio sinks, MQTT, web server (civetweb), fmt, regex, bell's mDNS, nlohmann/json.
  - cJSON comes from ESP-IDF's `json` component (`BELL_ONLY_CJSON`).
- **Protobuf:** the nanopb output is checked in under `generated/protobuf/`, so building doesn't need protoc or Python protobuf. At configure time, CMake compares the SHA-256 of the submodule's `.proto`/`.options` files with `generated/SOURCES.sha256` and stops if they changed.

### Regenerating the protobuf files

This is only needed after updating the submodule, when CMake reports a changed `.proto`.

```sh
python3 -m venv /tmp/pbvenv && /tmp/pbvenv/bin/pip install "protobuf>=7.36,<7.37" "setuptools<81"
cp -r firmware/components/cspot/cspot/bell/external/nanopb/generator /tmp/nanopb-gen
# nanopb 0.4.6-dev uses an API removed from current protobuf:
sed -i 's/msg = reflection.MakeClass(desc)()/from google.protobuf import message_factory; msg = message_factory.GetMessageClass(desc)()/' /tmp/nanopb-gen/nanopb_generator.py
cd firmware/components/cspot/cspot
/tmp/pbvenv/bin/python /tmp/nanopb-gen/nanopb_generator.py -I . -D ../../cspot_lib/generated protobuf/*.proto
sha256sum protobuf/*.proto protobuf/*.options > ../../cspot_lib/generated/SOURCES.sha256
```

- **Python protobuf version:** it must be at least the version of the system `protoc` (here 36.1, so Python protobuf 7.36).
- **Why a copy:** the generator is run from a copy because it writes `nanopb_pb2.py` next to itself, which would otherwise land inside the submodule.
- **Check:** the output at `99e5c95` was byte-identical to an independently generated set from the reference project.

## Patches (source overlays)

Each fix is a text replacement applied at CMake configure time to a **copy** of the file (`build/cspot_overlay/`). The copy is compiled instead of the original (`cspot_lib/overlays.cmake`). If a search text no longer matches after a submodule update, configuration fails with a clear message, so a fix can't silently stop applying.

| # | File | Kind | Change |
|---|---|---|---|
| 1 | bell `main/io/URLParser.cpp` | build fix | `#include <cstring>` (GCC 14 error: `strstr` not declared) |
| 2 | cspot `src/ApResolve.cpp` | crash fix | Check the apresolve JSON before using `ap_list[0]`; throw otherwise. A captive portal or error page used to cause a NULL dereference, i.e. a reboot loop. |
| 3 | cspot `src/MercurySession.cpp` | robustness | `mercury_dispatcher` stack 4 → 16 kB (PSRAM). It runs reconnects, including a TLS handshake. |
| 4 | bell `main/io/TLSSocket.cpp` | robustness | 15 s `SO_RCVTIMEO`/`SO_SNDTIMEO`, so a dead connection fails instead of hanging. |
| 5 | bell `main/io/TLSSocket.cpp` | **security** | Verify server certificates against ESP-IDF's CA bundle (`esp_crt_bundle_attach`). Upstream verifies nothing unless a bundle file was loaded, which never happens here. |
| 6 | cspot `src/AccessKeyFetcher.cpp` | crash fix | `try` around the token request and checks on the JSON. A network error there was an uncaught exception, i.e. a reboot. |
| 7 | cspot `src/MercurySession.cpp` | **crash fix** | One mutex for the `callbacks`, `audioKeyCallbacks` and `subscriptions` tables. They were filled under `connectingMutex` but read, called and erased without any lock in `handlePacket()`. Skipping tracks quickly corrupted the `unordered_map` (`LoadProhibited` in `handlePacket`, **reproduced on hardware**). Callbacks are now copied out under the lock and run unlocked, since they may send new requests. |
| 8 | cspot `src/PlaybackState.cpp` | **crash fix** | `syncWithRemote()` passed `context_uri` to `strlen()`/`strcpy()` unchecked. A Load frame without a context is what the Web API sends for `"uris": [track]`, i.e. exactly the alarm's request. That was a NULL dereference (**reproduced on hardware**). A missing context is now treated as `""`. |

Fixes 3 and 7 share one overlay call, because each file can only be overlaid once.

Fixes 1–3 follow the findings of [jetstarlp/music_streamer_esp32](https://github.com/jetstarlp/music_streamer_esp32) (`docs/SPIKE_REPORT.md`), reimplemented here; no code was copied. Fixes 5–8 are new.

## Runtime notes

- **PSRAM stacks:** cspot's tasks (player 48 kB, track queue 32 kB, dispatcher 16 kB) and the wrapper's session task (32 kB) have their stacks in PSRAM. Such tasks **must not touch flash**, so the wrapper writes credentials to NVS from the `esp_timer` task.
- **Internal RAM:** `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=2048`. With the default of 16 kB, cspot's many mid-sized allocations left only 39 kB of internal RAM while streaming; now it's 87–136 kB.
- **I2S DMA:** 8 × 1023 frames (186 ms). While Spotify streams, the audio writer task measurably runs 60–110 ms late at times; with 33 ms of DMA every late run was an audible click.
- **Tick rate:** `CONFIG_FREERTOS_HZ=1000`, because cspot sleeps in 1 ms steps.
- **Logger:** `bell::setDefaultLogger()` must run before any cspot code (`CSPOT_LOG` dereferences the global logger unchecked).
- **Credentials:** after every successful login, Spotify's *reusable credentials* are stored in NVS (namespace `spotify`, key `creds`) and used on the next boot. If Spotify declines them, they are erased and the device waits for the app again. `spotify forget` on the console does the same by hand.
- **Zeroconf:** served by the web UI's HTTP server (`/spotify_info`, port 80) and the firmware's mDNS (`_spotify-connect._tcp`). It is only advertised while not logged in.

## License

cspot is **GPLv3**. Firmware linked with it is a GPLv3 work: if you distribute the firmware, you must offer its complete source under GPLv3. This doesn't matter for private use.
