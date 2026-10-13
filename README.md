# ESP32-S3-BOX-3 Pomodoro Timer

A Pomodoro timer for the [Espressif ESP32-S3-BOX-3](https://github.com/espressif/esp-box). It runs on the box's 320×240 touchscreen and plays a chime through the built-in speaker. It also has a small HTTP API and a WebSocket feed, so you can control and watch it from scripts, home automation or a dashboard.

## Features

- **Pomodoro cycle:** 25 min focus, 5 min short break, and a 15 min long break after every 4 focus sessions. All lengths can be changed over the API.
- **Display:** a large countdown, a phase label coloured per phase, a progress bar, session dots, and the device IP in the corner.
- **Touch controls:** tap to start or pause, hold to skip, and swipe to skip or restart. The BOOT button does the same as tap and hold.
- **End of phase:** a three-note chime and six backlight flashes. The next phase then waits for you to start it.
- **Screen sleep:** the panel and backlight go to sleep after 2 minutes stopped or paused. The first touch only wakes the screen.
- **HTTP API:** `/status`, `/start`, `/pause`, `/toggle`, `/skip`, `/reset`, `/phase`, `/config` and `/sound`, protected by an API token.
- **WebSocket:** pushes the full status on connect and on every state change.
- **Always works offline:** the timer runs without Wi-Fi and the network features start once it connects. The device is reachable as `pomodoro.local` over mDNS.

## Hardware

- **ESP32-S3-BOX-3** (ESP32-S3, 16 MB flash, 16 MB octal PSRAM, ILI9342C display, capacitive touch, ES8311 audio codec)
- A USB-C cable

No wiring is needed. Everything uses the box's built-in display, touch, speaker and BOOT button.

## Setup

### 1. Install the toolchain

Install the [Arduino IDE 2.x](https://www.arduino.cc/en/software) or [`arduino-cli`](https://arduino.github.io/arduino-cli/). Then install:

| Component | Where | Tested version |
|---|---|---|
| **esp32** board package (Espressif) | Boards Manager | 3.3.12 |
| **LovyanGFX** (lovyan03) | Library Manager | 1.2.32 |
| **WebSockets** (Markus Sattler) | Library Manager | 2.7.2 |

`ESP_I2S`, `WiFi`, `WebServer` and `ESPmDNS` come with the esp32 board package.

With `arduino-cli`:

```sh
arduino-cli core install esp32:esp32
arduino-cli lib install LovyanGFX WebSockets
```

### 2. Add your settings

Copy the template and fill it in:

```sh
cp secrets.example.h secrets.h
```

```cpp
const char *WIFI_SSID = "your-wifi-name";
const char *WIFI_PASS = "your-wifi-password";
const char *API_TOKEN = "change-me";   // e.g. output of: openssl rand -hex 24
const char *HOSTNAME  = "pomodoro";    // reachable as pomodoro.local
```

`secrets.h` is listed in `.gitignore`. **Never commit it.**

### 3. Board settings

In the Arduino IDE (**Tools** menu):

| Setting | Value |
|---|---|
| Board | ESP32S3 Dev Module |
| Flash Size | 16MB (128Mb) |
| PSRAM | OPI PSRAM |
| USB CDC On Boot | Enabled |
| Partition Scheme | 16M Flash (3MB APP/9.9MB FATFS) |

PSRAM must be set to **OPI PSRAM**. The countdown is drawn into a buffer in PSRAM, so with PSRAM off the time digits won't show.

### 4. Build and upload

In the IDE, pick the box's serial port and click **Upload**.

With `arduino-cli`, replacing `COM5` with your port (for example `/dev/ttyACM0` on Linux):

```sh
arduino-cli compile --upload -p COM5 \
  -b "esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,CDCOnBoot=cdc,PartitionScheme=app3M_fat9M_16MB" .
```

If the upload fails with *"port is busy"* or *"Access is denied"*, close any Serial Monitor or other program that has the port open.

Once it connects to Wi-Fi, the device's IP appears in the top-right corner of the screen.

## Using it

### On the device

| Action | Result |
|---|---|
| Tap the screen | Start / pause |
| Hold about 0.8 s | Skip to the next phase |
| Swipe left | Skip to the next phase |
| Swipe right | Restart the current phase |
| BOOT button (short / long press) | Same as tap / hold |
| Touch while the screen is dark | Wakes the screen only |

The time is grey while stopped or paused and turns the phase colour while running. Focus is red, short break green, long break blue.

### HTTP API

Every endpoint needs the API token, either as a header or as a query parameter:

```sh
curl -H "X-Api-Key: $TOKEN" http://pomodoro.local/status
curl "http://pomodoro.local/status?key=$TOKEN"
```

| Endpoint | Action |
|---|---|
| `/status` | Return the current status |
| `/start` | Start (no-op if already running) |
| `/pause` | Pause (no-op if not running) |
| `/toggle` | Start or pause |
| `/skip` | Go to the next phase |
| `/reset` | Restart the current phase |
| `/phase?name=focus\|short\|long` | Switch to a specific phase |
| `/config?focus=25&short=5&long=15` | Set phase lengths in minutes (1–180, any subset) |
| `/sound?on=0\|1` | Mute or unmute the chime; returns `{"sound":true}` or `{"sound":false}` |

Most endpoints reply with the status JSON:

```json
{"state":"running","phase":"FOCUS","remaining_sec":1342,"total_sec":1500,"sessions_done":2}
```

- `state` is `running`, `paused` (stopped part-way) or `stopped` (at the full length).
- `phase` is `FOCUS`, `BREAK` or `LONG BREAK`.
- Errors come back as JSON: `401 {"error":"unauthorized"}`, `400` for a bad phase name, `404` for unknown paths.
- `/config` takes effect straight away only if the current phase hasn't started yet. Otherwise it applies the next time that phase starts.
- Settings are kept in RAM only, so a reboot goes back to 25/5/15 with the sound on.

### WebSocket

Connect to port 81 with the token in the URL:

```
ws://pomodoro.local:81/?key=<token>
```

- **On connect:** the server sends the current status once.
- **On every change:** it sends the status plus an `event` field. The event is `started`, `paused`, `phase_changed` or `completed`.
- **Wrong or missing key:** the connection is refused with `401` before it's upgraded.

```json
{"state":"stopped","phase":"BREAK","remaining_sec":300,"total_sec":300,"sessions_done":1,"event":"phase_changed"}
```

Quick test with [`websocat`](https://github.com/vi/websocat):

```sh
websocat "ws://pomodoro.local:81/?key=$TOKEN"
```

### Security note

The API is meant for a trusted local network. It runs over plain HTTP and WS with no TLS, so anyone who can see your network traffic can read the token. Tokens passed as `?key=` can also end up in browser history and logs. Don't expose the device to the internet.

## How it works

- **Single sketch:** everything is in `pomodoro_box3.ino`, split into sections for timer state, drawing, sound, screen sleep, input, API, WebSocket, and `setup()`/`loop()`.
- **Display:** LovyanGFX's BOX-3 autodetect profile drives it. The countdown is drawn into an off-screen sprite and pushed in one go, so it doesn't flicker.
- **Sound:** the ES8311 codec is set up over the touch controller's I2C bus and fed 16 kHz stereo through `ESP_I2S`. The chime plays in its own FreeRTOS task, so the UI and network stay responsive.
- **Non-blocking:** the backlight flash and the screen wake are stepped from `loop()` rather than using `delay()`.

## Repository layout

| File | Purpose |
|---|---|
| `pomodoro_box3.ino` | The firmware |
| `secrets.example.h` | Template for your private `secrets.h` |
| `secrets.h` | Your Wi-Fi and token settings (git-ignored, you create it) |
| `README.md` | This file |
| `CONTRIBUTING.md` | How to build, test and submit changes |
| `LICENSE` | MIT licence |

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

[MIT](LICENSE)
