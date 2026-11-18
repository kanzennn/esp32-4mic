# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Research project that records audio from 4 INMP441 I2S microphones via ESP32, for
building a dataset for Arabic makhraj (articulation-point) research — see
"Research goal" below. Current workflow (default): ESP32 writes 4-channel WAV
directly to a local SD card and hosts a WiFi Access Point + web UI so a phone can
pick a Responden/Huruf/Variasi recording target, start/stop takes, and browse/
download/delete recordings — no PC required. A legacy PC-tethered workflow
(`recorder.py`, Serial streaming) still exists in the repo but is unused by the
current firmware.

**Research goal:** respondents read hijaiyah letters (28 standard letters), in
different pronunciation variants, each possibly repeated multiple times ("takes"),
across the 4 mics — this is the reason recordings are organized as
Responden → Huruf → Variasi → Take rather than a flat anonymous session list.

## Workflow (current: SD card + phone web UI)

**Upload firmware:**
Use Arduino IDE to upload the sketch folder `esp32.txt` (multi-tab: `esp32.txt.ino` +
`sd_recorder.ino` + `session_model.ino` + `wifi_ap.ino` + `web_ui.ino` — Arduino IDE
compiles all `.ino` files in the folder together automatically, shown as tabs). No
PlatformIO/build config is tracked in-repo — flash size / partition scheme is whatever
"Tools > Partition Scheme" is currently selected locally in Arduino IDE. The default app
partition (~1.2-1.3MB) is enough; no SPIFFS/LittleFS partition needed since the web UI
HTML is embedded in PROGMEM, not stored as a filesystem.

**Record audio:**
1. Power the ESP32 — OLED shows a boot splash with the WiFi AP SSID (`ESP32-4MIC`,
   password in `wifi_ap.ino`) and its IP address for a few seconds, then switches to the
   idle waveform screen.
2. Connect a phone to that WiFi network, open a browser to the IP shown.
3. In the "Rekam Baru" tab: pick or create a Responden, pick a Huruf (1 of 28 hijaiyah
   letters), pick or create a Variasi — this locks in the recording target (`POST
   /select`).
4. Start/stop a take from the web/app button only — the physical ESP32 button no
   longer controls recording at all (previously a short press started/repeated the
   last-selected target; that behavior was removed). The physical button is now
   dedicated to two housekeeping actions, regardless of recording state: a **short
   press** (<600ms) shows the WiFi SSID/IP + SD status on the OLED for a few seconds
   (same content as the boot splash) — handy for reconnecting a phone without
   rebooting; a **long press** (≥600ms, held) restarts the device (`ESP.restart()`,
   with a brief "Restarting..." OLED message first). Restarting while a take is in
   progress is safe but abrupt — the take is left exactly as an unexpected power-cut
   would leave it (recovered at next boot per the header-repatch/`.done`-marker
   mechanism below), so prefer stopping the take from the app first.
5. The "Lihat Data" tab browses/downloads/deletes existing recordings (read-only —
   it never changes the recording target). Takes currently being recorded can't be
   deleted (HTTP 409).
6. Alternatively, just pull the SD card and read it directly on a PC — nested folders
   per respondent/huruf/variasi,
   `/RSPDN{r}/HRF{h}/VRS{v}/RSPDN{r}_HRF{h}_VRS{v}_{take}_MIC{m}.wav`.

Files saved as `/RSPDN{r}/HRF{h}/VRS{v}/RSPDN{r}_HRF{h}_VRS{v}_{take}_MIC{m}.wav` —
`r`=respondent index, `h`=huruf index (1-28), `v`=variasi (pronunciation variant)
number, `take`=repetition number, `m`=mic 1-4. The `HRF{h}`/`VRS{v}` folders are only
created (`mkdir`, one level at a time — `SD.mkdir()` does **not** auto-create parents)
the moment a take is first recorded for that combo, never just because a huruf/variasi
was selected in the UI — this is what keeps empty letters/variants from ever showing up
in the browse view. There's no per-combo counter file — respondent index and next take
number are derived by scanning; huruf/variasi "existence" is just `SD.exists()` on
their folder path (see `session_model.ino`). No RTC/NTP available
(standalone AP mode has no internet uplink), so nothing here is calendar-based.
If power is lost mid-take, the WAV header is re-patched every ~2s during recording (and
`flush()`-ed to physical disk each time — see `sd_recorder.ino`) so at most ~2s of size
accuracy is lost; at boot, any take missing a `<base>.done` marker file is treated as
unclean, has its WAV headers recomputed from actual file size, and stays permanently
flagged incomplete in the web UI (its filenames are never renamed, unlike the old
per-folder scheme, so downstream scripts can rely on stable names).

## Workflow (legacy: PC + Serial, currently unused)

`recorder.py` + the old Serial-streaming path are kept in the repo but the current
firmware no longer streams audio over Serial during recording (that path was replaced
by direct SD writes — see Architecture below). To resurrect this workflow you'd need to
re-add the `Serial.write()` calls that used to live in `startRecording()`/`loop()`.

```bash
pip install pyserial   # one-time setup
python recorder.py COM3   # adjust COM port as needed
```
Historically saved to `recording/<YYYY-MM-DD_HH-MM-SS>/mic1-4.wav` (folder name came
from the PC's clock, not the ESP32's `millis()`-based timestamp).

## Architecture

### Firmware (`esp32.txt.ino`)
- Two independent I2S buses (I2S_NUM_0 and I2S_NUM_1), each handling 2 mics via L/R channel selection
- Bus 0 (Mic 1 & 2): SCK→D26, WS→D25, SD→D33. Bus 1 (Mic 3 & 4): SCK→D18, WS→D5, SD→D19
- Mic L/R=GND → LEFT channel (odd buffer index `idx+1`); L/R=3V3 → RIGHT channel (even index `idx`)
- Per loop iteration, samples are split into 4 per-channel buffers (`mic1Buf`..`mic4Buf`) and, while recording, appended directly to 4 open WAV files on the SD card (see `sd_recorder.ino` below) — no longer streamed over Serial as an interleaved marker-delimited protocol (that was the old `recorder.py` path, now legacy/unused; see Workflow section)
- Bus 1 must use `use_apll = false` — ESP32 APLL is shared and both buses cannot both use it
- Both i2s_read calls must use `portMAX_DELAY` (not 0) or Bus 1 returns zeros intermittently
- I2S data pins must be output-capable GPIOs (0–33); input-only pins GPIO34–39 cause `i2s_set_pin` to return `ESP_ERR_INVALID_ARG` (258)
- `i2s_pin_config_t` must explicitly set `.mck_io_num = I2S_PIN_NO_CHANGE`. If omitted, it defaults to 0 (GPIO0) instead of -1, and `i2s_set_pin` on I2S_NUM_1 returns `ESP_ERR_INVALID_ARG` (258) because I2S1 doesn't support MCLK output on classic ESP32 (I2S0 does, so bus 0 works fine even without this field set — the bug only manifests on bus 1)
- Bus 1's second `i2s_read` must use `portMAX_DELAY`, not `0` — a `0` (non-blocking) timeout returns zero/stale data from bus 1 intermittently
- Bus 1 lacking APLL means its actual sample rate drifts slightly from bus 0's. Since the main loop reads a fixed `BUFFER_LEN` from both buses per iteration (paced by bus 0's precise clock), this drift eventually overflows bus 1's DMA ring buffer, dropping a batch of samples at once — audible as a periodic "knock/tap" on mic 3 & 4 only. Mitigated (not eliminated) by giving bus 1 a larger `dma_buf_count` (24 vs bus 0's 8) so drift takes longer to overflow it. **Do not raise this further** — pushing it to 48 exhausted internal DRAM, silently failing `i2s_driver_install` on bus 1, which made its `i2s_read(portMAX_DELAY)` hang forever and froze the whole `loop()` (button/LED/OLED all unresponsive). WiFi now also competes for the same limited internal DRAM (see below) — if that bug ever resurfaces, check `[HEAP]` lines on Serial before suspecting `dma_buf_count` again.

### SD card recording (`sd_recorder.ino` + `session_model.ino`)
- SD is on **custom SPI pins**, not ESP32's default VSPI/HSPI: SCK=D14, MOSI=D27, CS=D13, MISO=D34. Default VSPI (18/19/23/5) collides with I2S Bus 1 wiring; GPIO12 (HSPI default MISO) is avoided because it's a strapping pin that sets flash voltage at boot — an SD module's MISO pull-up biasing it high risks a brownout. GPIO34 is input-only, which is fine since MISO is ESP32-input-only anyway; it has no internal pull-up, so add an external 10kΩ pull-up to 3.3V if reads are flaky.
- The SD breakout module's **VCC must go to VIN/5V, not 3.3V** — confirmed by board photo to have an onboard 8-pin IC + SOT-23 regulator (a 5V-in/3.3V-out design, not a bare pass-through). Powering it from 3.3V puts the regulator in dropout and undervolts the card enough that `SD.begin()` fails 100% of the time (not intermittent) regardless of pins/card/format — this cost a full debugging session before the wiring was traced back to this. The board runs warm to the touch on 5V; that's normal LDO dissipation, not a fault.
- Bus is GPIO-matrix-routed (non-native pins), which caps reliable SPI clock below native-pin speeds — `SD.begin()` runs at a conservative 4MHz; only raise it after confirming stability in isolation.
- `sd_recorder.ino` is the low-level primitive layer: `writeWavHeader()`, `writeFrameToSD()` (writes samples + periodic header repatch), the `sessionFile[4]`/`sessionSampleCount`/`lastHeaderPatchMs` globals, `initSDCard()`/`remountSDCard()`/`checkSDPresent()` (real-time SD-removal detection — forces a physical read of `/sd_marker.txt`, since `SD.cardType()`/`cardSize()` only report a value cached at mount time). `session_model.ino` builds the Responden/Huruf/Variasi/Take data model on top of it (folder/filename construction, take-number scanning, target selection) — see below.
- Folders nest 3 levels deep: `/RSPDN{r}/HRF{h}/VRS{v}/RSPDN{r}_HRF{h}_VRS{v}_{take}_MIC{m}.wav`. `HRF{h}`/`VRS{v}` are only `mkdir`-ed (one level at a time, via `ensureComboFolder()` — `SD.mkdir()` doesn't auto-create parents like `mkdir -p`) the moment a take is first recorded for that combo, never on mere UI selection, so an untouched letter/variant never gets an empty folder. There's no counter file for respondent index or take number (derived by scanning — `findNextRespondentIndex()`, `findNextTakeNumber()`); huruf/variasi "existence" is just `SD.exists()` on the folder path (`findHurufWithData()`, `findExistingVariasi()`, both in `session_model.ino`). `SD.begin()` is called with `max_files=10` (not the default 5) — the nested traversal in `repairIncompleteTakesAtBoot()` needs several directory handles open at once. No RTC/NTP available (standalone WiFi AP has no internet uplink), so nothing here is calendar-based.
- The current recording target (`currentRespondenIdx`/`HurufIdx`/`VariasiIdx`, `targetSelected`) is set via `POST /select` and lives in RAM in `esp32.txt.ino` — deliberately **not** reset when a take finishes (`closeCurrentTake()`), so the app can call `/start` repeatedly to "repeat the last-selected target" across multiple takes without re-selecting each time. The physical button no longer starts/stops recording at all (see button behavior below), so this no longer concerns it.
- WAV headers are written as 44-byte placeholders at file open, **re-patched every ~2s during recording, followed by an explicit `flush()`** (not only at close) — the `flush()` is load-bearing, not optional: `File::write()` only fills an in-RAM stdio buffer, and FAT doesn't persist the file-size directory entry to physical disk until synced, so without it a power cut can lose both the buffered audio *and* the size metadata (this was observed directly: an 8s recording survived a power cut as a ~1KB silent file before `flush()` was added). `closeCurrentTake()` does a final patch, closes all 4 files, then writes a `<take-base-path>.done` marker — `repairIncompleteTakesAtBoot()` (called from `setup()` and after a hot-swap remount in `sdPresenceCheckTask()`) scans for takes missing `.done`, recomputes real WAV sizes from actual file length, and repatches headers, but deliberately never renames the WAV files and never writes `.done` itself — a repaired take stays permanently flagged incomplete in the web UI, and filenames stay stable for downstream scripts.
- All `SD.*`/`File.*` calls, from both the recording loop (Core 1) and the web server (Core 0), **must** hold `sdMutex` — ESP32's FAT/VFS layer isn't safe for uncoordinated concurrent access across cores.
- **Every open `File` costs a stdio buffer from internal DRAM — shrink it.** By default each `fopen` on FAT allocates a ~4KB buffer, so opening the 4 WAV files at take start cost ~19KB of *contiguous* heap: `largestFreeBlock` dropped from ~18KB to ~1.1KB for the whole recording while total free heap was still ~41KB. `WebServer` needs ~1.4KB contiguous per new connection, so requests from the app failed and it showed "belum tersambung" (diagnosed 2026-10-26 with `[HEAP-REC]` logs). Fix: `sessionFile[n].setBufferSize(512)` right after `SD.open()` and before any I/O (`startTakeForCurrentTarget()`), and the same for the read in `handleDownload()`. After the fix `largestFreeBlock` stays ~14-15KB while recording (~1KB per file instead of ~4.7KB). The loop writes 512B per channel per iteration, so a 512B buffer costs no throughput. Judge heap health by `largestFreeBlock`, not `freeHeap`.
- **Known gap:** `qc_analyze.py` still expects the old `recording/<session>/mic{1-4}.wav` layout from the pre-Responden/Huruf/Variasi/Take scheme — it hasn't been updated yet to parse `RSPDN{r}_HRF{h}_VRS{v}_{take}_MIC{m}.wav`. Update it before running QC on data collected under the new structure.

### WiFi AP + web UI (`wifi_ap.ino`, `web_ui.ino`)
- ESP32 hosts its own WiFi Access Point (`WiFi.softAP()`, SSID/password in `wifi_ap.ino`) — standalone, no dependency on an existing network.
- Web server uses ESP32 Arduino core's built-in `WebServer.h` (not ESPAsyncWebServer, to avoid an extra third-party dependency), run via a dedicated FreeRTOS task **pinned to Core 0** (`xTaskCreatePinnedToCore`) so it can't block Core 1's timing-sensitive I2S read loop. `/start` and `/stop` don't mutate SD state directly from the web task — they set `webStartRequested`/`webStopRequested` flags that `loop()` checks and actions on Core 1, keeping all SD file-handle mutation on one core.
- The page has two tabs: "Rekam Baru" (3-column Responden/Huruf/Variasi picker feeding `POST /select`, then `/start`/`/stop`) and "Lihat Data" (read-only browse/download/delete of existing takes — **must never call `/select`**, since browsing shouldn't silently change what the next `/start` will record). The Flutter companion app (`recorder_app/`, separate repo) talks to the same endpoints and is now the primary client; the embedded web UI still works standalone from any browser.
- Endpoints: `GET /` (embedded HTML/JS, PROGMEM), `GET /status` (recording state, current target + `nextTake` preview, live heap/SD-space diagnostics), `GET /respondents` + `POST /respondents` (list / create-next, no free-text name), `GET /huruf?responden=` (which of the 28 letters have data — the 28 names themselves only live in the page's JS, firmware only tracks numeric index), `GET /variasi?responden=&huruf=` (existing variant numbers, defaulting to `[1]` if none), `POST /select?responden=&huruf=&variasi=` (locks the recording target), `GET /takes?responden=&huruf=&variasi=` (per-take completeness + sizes, streamed JSON like the old `/sessions` did), `GET /download?path=...`, `POST /delete?scope=take|respondent&path=...` (409 if the target is currently recording), `POST /start` (409 if no target selected)/`POST /stop`.
- Downloading a file holds `sdMutex` for the whole transfer — if a download happens *while* a recording is active, the recording loop's SD writes will block waiting for the mutex, which can cause audio glitches. Prefer downloading after stopping a recording.
- `handleDownload()` sends the file with a manual chunked loop (`f.read()` into a static 2KB buffer + `select()`-gated `client.write()` + `delay(1)` per chunk, file opened with `setBufferSize(512)`) instead of the library's `server.streamFile()`. Reason: `streamFile()` reads the SD file 64 bytes at a time with no yield, and that whole read-write loop runs inside a single `server.handleClient()` call — `webServerTask`'s `vTaskDelay(2)` only runs *between* `handleClient()` calls, not during one. For a multi-MB WAV over the deliberately-conservative 4MHz custom SPI bus, that starves Core 0's IDLE0 task long enough to trip the ESP32 task watchdog and reboot — which looks from the phone like "the ESP32's WiFi just died" mid-download. The explicit `delay(1)` per chunk is load-bearing, same spirit as the `dma_buf_count` Core-1-starvation bug documented above for I2S.
- **Internal-DRAM exhaustion was the cause of "phone can't join the AP until the ESP32 is restarted"** (diagnosed 2026-10-26 with `[HEAP-IDLE]`/`[WIFI-EV]` Serial logs). I2S DMA buffers take ~64KB (bus 1: 24×2KB, bus 0: 8×2KB), leaving only ~58KB free at idle. With 802.11n enabled, the first station join dropped the largest free block to ~292 bytes and it never recovered after disconnect, so every later 4-way handshake failed (tablet logs: `WRONG_KEY` / `assoc_reject`, even with the correct password). Fix in `initSoftAP()`: `esp_wifi_set_protocol(WIFI_IF_AP, 11B|11G)` (no 11n → no AMPDU buffers), plus `Serial.setTxBufferSize(2048)` (was 8192) and a 2KB download buffer. Result: ~67KB free at idle, ~60KB / 17KB-largest while a station is connected, no leak across connect/disconnect cycles. Don't re-enable 11n or grow these buffers without re-checking `[HEAP-IDLE]` (logged every 5s idle / 2s recording, with `apStations`); `[WIFI-EV]` logs station join/leave with reason codes.
- **A stalled download blocks the whole web server.** `NetworkClient::write()` waits for the socket to become writable up to ~10s (1s `select` × 10 retries) when the phone stops reading/ACKing. `WebServer` is single-threaded, so `/status` isn't served meanwhile and the app shows "belum tersambung". `handleDownload()` therefore polls `select()` every 200ms and aborts after 3s without progress (`[DL] MACET/batal` with heap figures) instead of calling `write()` blind. Seen in practice: a download stalled after 8KB while largest free block was ~1.5KB (WiFi TX buffers can't be allocated); the same file downloaded fine at other times.
- **Open issue (as of 2026-10-26): `GET /takes` sometimes never returns.** Seen 3×: `[WEB>] /takes mulai` with no matching `[WEB] /takes` line, free heap drops ~15–18KB and `largestFreeBlock` collapses to ~1.4KB and doesn't recover until the tablet disconnects ~10–15s later (`STA disconnected reason=3`, tablet side). Other endpoints recover their heap normally, and some `/takes` calls completed in ~0.3s. Cause not found. Stage logs `[TAKES]` were added in `handleGetTakes()`/`listTakes()` to show whether it hangs on `sdMutex`, inside `listTakes()` (4× `SD.open(..., FILE_READ)` per take, each with a ~4KB stdio buffer — first candidate: `setBufferSize(512)` there too), or while streaming the chunked response. Next step: record 3–4 takes in one combo, open Lihat Data in the app with Serial attached, read the `[TAKES]` lines. Diagnostic Serial logs left in on purpose until this is resolved: `[WEB>]`/`[WEB]` (per-request start/end with heap before→after, via `onLogged()` in `web_ui.ino`; `/status` skipped), `[DL]`, `[TAKES]`, `[HEAP-OPEN]` — remove afterwards.
- WiFi + the web task add meaningful RAM pressure on top of the existing I2S DMA buffers — `[HEAP]` lines are logged to Serial at boot (pre-WiFi, post-WiFi) and live via `/status` (`freeHeap`, `largestFreeBlock`), since there's no Serial Monitor attached during real standalone phone-controlled use.

### Recorder (`recorder.py`) — legacy, currently unused
- Uses sliding window marker detection to safely split audio frames from the binary serial stream
- Splits interleaved bytes into 4 mono WAV files per session (8 bytes per frame = 4 × int16 little-endian)
- The firmware no longer sends `START_MARKER`/audio/`END_MARKER` over Serial during recording (replaced by direct SD writes), so this script currently has nothing to consume during a recording session

## Key Constants

| Constant | Value | Still used by |
|----------|-------|----------------|
| SAMPLE_RATE | 8000 Hz | firmware WAV header, `qc_analyze.py` |
| BAUD_RATE | 921600 | Serial debug prints only (audio no longer streams over Serial) |
| START_MARKER | `0xDE 0xAD 0xBE 0xEF` × 2 | `recorder.py` only (legacy path, not sent by current firmware) |
| END_MARKER | `0xCA 0xFE 0xBA 0xBE` × 2 | `recorder.py` only (legacy path, not sent by current firmware) |

## Hardware Wiring Summary

```
Mic 1: Bus0 LEFT  — L/R→GND, SD shared on D33
Mic 2: Bus0 RIGHT — L/R→3V3, SD shared on D33
Mic 3: Bus1 LEFT  — L/R→GND, SD→D19
Mic 4: Bus1 RIGHT — L/R→3V3, SD→D19 (shared with Mic 3)

OLED: SDA→D21, SCL→D22 (I2C address 0x3C)
LED:  GPIO 2
BTN:  D4→GND (INPUT_PULLUP, active LOW)

SD Card (SPI, custom GPIO-matrix pins — see sd_recorder.ino for why):
  SCK→D14 | MISO→D34 | MOSI→D27 | CS→D13 | VCC→VIN(5V) | GND→GND
  (D34 has no internal pull-up — add external 10k pull-up to 3V3 if flaky)
  (VCC must be 5V, not 3.3V — this SD module has an onboard regulator that
  needs 5V in; on 3.3V it browns out and SD.begin() fails 100% of the time)
```

## Known hardware issue: mic 1 & 2 (Bus 0) noise (open, 2026-10-26)

Mic 1/2 recordings from 9 and 16 Jul were clean (DC ≈ −2, noise floor 11–60 in int16 units, voice band present). Since then bus 0 reads garbage: raw I2S words sit at ~+69% of full scale with large slow wander (isolation sketch reading ONLY bus 0: |DC| ≈ 3k–10k and AC ≈ 4k–10k per second), mic 1 clips (≈41–46% of samples at the ×4 `MIC1_GAIN`), and 75–90% of the energy is below 20Hz. Bus 1 is normal.

Ruled out by direct tests: WiFi (same with it off), APLL on bus 0 (same with `use_apll=false`), OLED/SD/WiFi being switched on (in the isolation sketch the noise is already there at stage A, bus 0 alone), anything recording-specific (present at idle). DC of mic 1 and 2 moves together (correlation ≈0.53, same sign in 24/33 s), which points to a shared cause (supply/ground, D33 data line, an L/R pin) rather than two independent mic failures — inference only, not proven. Mic 3/4 respond clearly to taps and are isolated from each other, so they are alive; their low level in the 26 Oct recording was probably speaking distance (unverified).

**Next: physical checks, not firmware** — VDD at mic 1/2 pins (should be 3.3V), L/R pins not floating (mic 1→GND, mic 2→3V3), unplug one bus-0 mic at a time, re-seat D26/D25/D33/3V3/GND.

## Bring-up Order for SD + WiFi (test on real hardware, each stage before the next)

0. Wire SD physically, no firmware changes — confirm Bus 1 knock-glitch rate is unchanged from baseline (rules out physical crosstalk before software is suspect)
1. SD in isolation (scratch sketch, no I2S/WiFi): mount at 4MHz, write/read test file, confirm GPIO34 MISO reliability; raise SPI clock only if stable
2. SD + existing I2S, no WiFi: real recording-to-SD, multi-minute sessions, watch for *new* Bus 1 overflow patterns vs. baseline, confirm files pass `qc_analyze.py` unmodified; deliberately power-cycle mid-recording to exercise header repatch/boot-repair
3. Only if Stage 2 shows a new timing regression: add a RAM ring buffer + separate SD-writer task to decouple `SD.write()` latency from the I2S loop (not built preemptively)
4. WiFi AP alone, no web server: confirm OLED splash shows SSID/IP, log `[HEAP]` checkpoints, confirm no brownouts
5. Add the Core-0 web task serving a trivial static page only — confirm Core 1 recording is unaffected by WiFi/web-task presence alone
6. Wire up `/sessions`, `/download`, `/delete`; stress-test downloading a large file *while a recording is in progress* to confirm the mutex prevents corruption (glitches during simultaneous download are expected — see web_ui note above)
7. Wire up `/start`/`/stop` (at the time this was written, also test interleaving with the physical button for races — no longer applicable now that the physical button doesn't control recording, see button behavior above)
8. Multi-hour soak test with repeated record/stop cycles, watching `/status` heap/largest-free-block trend for slow leaks/fragmentation
