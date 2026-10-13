/*
  Pomodoro timer for ESP32-S3-BOX-3 (320x240 touchscreen)
  LovyanGFX display + touch, WiFi HTTP API + WebSocket push

  First time only: copy secrets.example.h to secrets.h next to this file and
  put your WiFi + API token in it. See README.md for full setup.

  Libraries (Library Manager):
    "LovyanGFX" (lovyan03)
    "WebSockets" (Markus Sattler)
  Board settings (Tools menu):
    Board ESP32S3 Dev Module, Flash Size 16MB, PSRAM "OPI PSRAM",
    USB CDC On Boot Enabled, Partition Scheme "16M Flash (3MB APP/9.9MB FATFS)"

  Local control:
    tap the screen      -> start / pause
    hold ~0.8 s         -> skip to the next phase
    BOOT button (GPIO0) -> same thing
    Screen goes dark after 2 min stopped/paused; touch wakes it (and only wakes it).

  API (token via header  X-Api-Key: <token>  or  ?key=<token>):
    /status  /start  /pause  /toggle  /skip  /reset
    /phase?name=focus|short|long
    /config?focus=25&short=5&long=15      (minutes)
  WebSocket (port 81): ws://pomodoro.local:81/?key=<token>
    Pushes the status JSON plus an "event" field on connect and on every
    state change: started, paused, phase_changed, completed.
*/
#include <Arduino.h>
#define LGFX_ESP32_S3_BOX_V3
#include <LGFX_AUTODETECT.hpp>
#include <LovyanGFX.hpp>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <WebSocketsServer.h>

// WiFi name, password, API token and hostname live in secrets.h (a separate
// tab in the Arduino IDE), so replacing this file never overwrites them.
#include "secrets.h"

#define PIN_BTN 0   // BOOT button

static LGFX lcd;
static LGFX_Sprite timeSprite(&lcd);   // off-screen buffer for the time band

WebServer server(80);
// WebSocket server that refuses a wrong ?key= during the handshake (401),
// before the connection is upgraded and takes a client slot.
class AuthWebSocketsServer : public WebSocketsServer {
 public:
  using WebSocketsServer::WebSocketsServer;

  // True if a WebSocket URL such as "/?key=TOKEN" carries the right key.
  static bool urlKeyOk(const String &url) {
    int q = url.indexOf('?');
    if (q < 0) return false;
    String query = "&" + url.substring(q + 1) + "&";
    return query.indexOf(String("&key=") + API_TOKEN + "&") >= 0;
  }

 protected:
  // Runs for each ordinary header (Host, Origin, ...) of a handshake. The request
  // line with the URL always comes first, so every client mid-handshake that
  // already has its URL can be checked here.
  bool execHttpHeaderValidation(String, String) override {
    for (auto &c : _clients)
      if (c.status == WSC_HEADER && c.cUrl.length() && !urlKeyOk(c.cUrl)) c.cHttpHeadersValid = false;
    return true;
  }

  void handleNonWebsocketConnection(WSclient_t *client) override {
    if (!client->cUrl.length() || urlKeyOk(client->cUrl)) {
      WebSocketsServer::handleNonWebsocketConnection(client);
      return;
    }
    client->tcp->write(
        "HTTP/1.1 401 Unauthorized\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 24\r\n"
        "Connection: close\r\n"
        "\r\n"
        "{\"error\":\"unauthorized\"}");
    clientDisconnect(client);
  }
};

AuthWebSocketsServer ws(81);
void announce(const char *event);
bool serverStarted = false;

// ---- Timer settings (changeable through /config) ----
uint32_t focusSecs = 25 * 60;
uint32_t shortSecs = 5 * 60;
uint32_t longSecs  = 15 * 60;
const uint8_t SESSIONS_BEFORE_LONG = 4;

// ---- Layout (320x240 landscape) ----
const int W = 320;
const int BAR_X = 20, BAR_Y = 178, BAR_W = 280, BAR_H = 14;

enum Phase { FOCUS, SHORT_BREAK, LONG_BREAK };
Phase phase = FOCUS;
uint8_t sessionsDone = 0;
bool running = false;
int32_t remainingMs;
uint32_t lastTick;
uint32_t totalSecs;
int lastShownSecs = -1;
int lastElapsedPx = 0;

// RGB565 colours. Must be uint16_t: LovyanGFX reads a uint32_t colour as RGB888.
uint16_t C_BG, C_TRACK, C_FOCUS, C_SHORT, C_LONG, C_DIM;

// Per-phase properties, indexed by Phase
const char *const PHASE_NAMES[]     = {"FOCUS", "BREAK", "LONG BREAK"};
const char *const PHASE_API_NAMES[] = {"focus", "short", "long"};
uint32_t *const PHASE_SECS[]        = {&focusSecs, &shortSecs, &longSecs};

uint16_t phaseColor() {
  const uint16_t colors[] = {C_FOCUS, C_SHORT, C_LONG};
  return colors[phase];
}
const char *phaseName() { return PHASE_NAMES[phase]; }
uint32_t phaseSecs()    { return *PHASE_SECS[phase]; }

// True while the current phase hasn't counted down at all yet.
bool atFullLength() { return remainingMs == (int32_t)(totalSecs * 1000); }
int remainingSecs() { return (remainingMs + 999) / 1000; }

// ---- Drawing ----
// drawLabel/drawDots are only called from drawFull, right after fillScreen.
void drawLabel() {
  lcd.setFont(&fonts::FreeSansBold18pt7b);
  lcd.setTextSize(1);
  lcd.setTextColor(phaseColor());
  lcd.setTextDatum(middle_center);
  lcd.drawString(phaseName(), W / 2, 28);
}

void drawDots() {
  int filledCount = sessionsDone % SESSIONS_BEFORE_LONG;
  if (filledCount == 0 && sessionsDone > 0 && phase == LONG_BREAK) filledCount = SESSIONS_BEFORE_LONG;
  for (uint8_t i = 0; i < SESSIONS_BEFORE_LONG; i++) {
    int x = W / 2 - 30 + i * 20;
    if (i < filledCount) lcd.fillCircle(x, 218, 6, C_FOCUS);
    else        lcd.drawCircle(x, 218, 6, C_DIM);
  }
}

void drawIP() {
  lcd.fillRect(224, 6, 96, 10, C_BG);
  lcd.setFont(&fonts::Font0);
  lcd.setTextSize(1);
  lcd.setTextColor(C_DIM);
  lcd.setTextDatum(top_right);
  String s = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : String("no wifi");
  lcd.drawString(s, W - 4, 8);
}

void drawTime(uint32_t secs) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%02u:%02u", (unsigned)(secs / 60), (unsigned)(secs % 60));
  // The band between the phase label (ends y=48) and the hint text (starts
  // y=146) is drawn off-screen, then pushed in one go so it never flashes blank.
  timeSprite.fillScreen(C_BG);
  // Smooth (anti-aliased) built-in font at its natural size: no blocky scaling.
  // If ':' ever shows as a box, swap in fonts::DejaVu56 or FreeSansBold24pt7b.
  timeSprite.setFont(&fonts::DejaVu72);
  timeSprite.setTextSize(1);
  timeSprite.setTextColor(running ? phaseColor() : C_DIM);   // phase colour while running, grey when paused/stopped
  timeSprite.setTextDatum(middle_center);
  timeSprite.drawString(buf, W / 2, 98 - 50);
  timeSprite.pushSprite(0, 50);
  static bool logged = false;
  if (!logged) { logged = true; Serial.printf("drawTime: first frame %s pushed\n", buf); }
}

void drawHint() {
  lcd.fillRect(40, 146, 240, 24, C_BG);
  if (!running) {
    lcd.setFont(&fonts::FreeSans12pt7b);
    lcd.setTextSize(1);
    lcd.setTextColor(C_DIM);
    lcd.setTextDatum(middle_center);
    const char *t = atFullLength() ? "tap to start" : "paused";
    lcd.drawString(t, W / 2, 158);
  }
}

void drawFull() {
  lcd.fillScreen(C_BG);
  lcd.fillRect(BAR_X, BAR_Y, BAR_W, BAR_H, phaseColor());
  lastElapsedPx = 0;
  drawLabel();
  drawDots();
  drawIP();
  lastShownSecs = -1;
  drawHint();
}

// ---- Phase control ----
void startPhase(Phase p) {
  phase = p;
  totalSecs = phaseSecs();
  remainingMs = totalSecs * 1000;
  running = false;
  drawFull();          // loop() draws the time on its next pass
  announce("phase_changed");
}

void nextPhase(bool completed) {
  if (phase == FOCUS) {
    if (completed) sessionsDone++;
    startPhase((sessionsDone % SESSIONS_BEFORE_LONG == 0 && sessionsDone > 0) ? LONG_BREAK : SHORT_BREAK);
  } else {
    startPhase(FOCUS);
  }
}

// ---- Sound: ES8311 codec + speaker amp on the BOX-3 ----
#include <ESP_I2S.h>
#define PIN_I2S_MCLK 2
#define PIN_I2S_BCLK 17
#define PIN_I2S_WS   45
#define PIN_I2S_DOUT 15
#define PIN_PA       46          // speaker amplifier enable
#define CODEC_ADDR   0x18
#define AUDIO_RATE   16000

bool soundOn = true;             // /sound?on=0|1
TaskHandle_t chimeTask = nullptr;  // plays the chime; created by audioInit()
int  codecPort = 1;              // I2C port the touch controller already uses
I2SClass i2s;                    // standard (Philips) I2S, MCLK = 256 x fs, outputs silence when idle

void codecWrite(uint8_t reg, uint8_t v) {
  lgfx::i2c::writeRegister8(codecPort, CODEC_ADDR, reg, v, 0, 400000);
}

void audioInit() {
  auto t = lcd.touch();
  if (t) codecPort = t->config().i2c_port;   // share the touch controller's I2C bus

  pinMode(PIN_PA, OUTPUT);
  digitalWrite(PIN_PA, LOW);

  i2s.setPins(PIN_I2S_BCLK, PIN_I2S_WS, PIN_I2S_DOUT, -1, PIN_I2S_MCLK);
  if (!i2s.begin(I2S_MODE_STD, AUDIO_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) {
    Serial.println("audio: i2s begin failed");
    return;
  }
  delay(20);                                  // MCLK must be running before the codec is configured

  // ES8311 setup: slave mode, 16 kHz, MCLK = 256 x fs, 16-bit I2S, DAC only
  codecWrite(0x44, 0x08); codecWrite(0x44, 0x08);
  codecWrite(0x01, 0x30); codecWrite(0x02, 0x00); codecWrite(0x03, 0x10);
  codecWrite(0x16, 0x24); codecWrite(0x04, 0x10); codecWrite(0x05, 0x00);
  codecWrite(0x0B, 0x00); codecWrite(0x0C, 0x00); codecWrite(0x10, 0x1F);
  codecWrite(0x11, 0x7F); codecWrite(0x00, 0x80);
  delay(20);
  codecWrite(0x01, 0x3F);                     // clocks on, MCLK from pin
  codecWrite(0x06, 0x03);                     // BCLK divider
  codecWrite(0x07, 0x00); codecWrite(0x08, 0xFF);   // LRCK = MCLK/256
  codecWrite(0x09, 0x0C); codecWrite(0x0A, 0x0C);   // 16-bit I2S
  codecWrite(0x17, 0xBF);
  codecWrite(0x0E, 0x02); codecWrite(0x12, 0x00); codecWrite(0x14, 0x1A);
  codecWrite(0x0D, 0x01); codecWrite(0x15, 0x40); codecWrite(0x37, 0x08);
  codecWrite(0x45, 0x00);
  codecWrite(0x32, 0xA8);                     // DAC volume (0xBF = 0 dB; lower = quieter)
  xTaskCreatePinnedToCore(chimeTaskFn, "chime", 4096, nullptr, 1, &chimeTask, 0);
}

void playTone(int freq, int ms) {
  const int n = AUDIO_RATE * ms / 1000;
  int16_t buf[256 * 2];
  int done = 0;
  float phase = 0, step = 2.0f * PI * freq / AUDIO_RATE;
  while (done < n) {
    int chunk = min(256, n - done);
    for (int i = 0; i < chunk; i++) {
      // short fade in/out so the notes don't click
      int pos = done + i;
      float env = min(1.0f, min(pos, n - pos) / 160.0f);
      int16_t v = (int16_t)(sinf(phase) * 9000 * env);
      phase += step;
      buf[2 * i] = v; buf[2 * i + 1] = v;
    }
    i2s.write((uint8_t *)buf, chunk * 4);
    done += chunk;
  }
}

// The chime plays in its own task so loop() (UI, HTTP, WebSocket) keeps running.
void chimeTaskFn(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    digitalWrite(PIN_PA, HIGH);
    delay(30);
    playTone(784, 180);    // G5
    playTone(988, 180);    // B5
    playTone(1319, 380);   // E6
    delay(120);            // let the queued DMA audio (~90 ms) drain before the amp goes off
    digitalWrite(PIN_PA, LOW);
  }
}

void playChime() {
  if (soundOn && chimeTask) xTaskNotifyGive(chimeTask);
}

// Backlight flash, stepped from loop(): 6 off/on blinks, 120 ms each half.
uint8_t flashStepsLeft = 0;
uint32_t flashStepAt = 0;

void flashBacklight() {
  lcd.setBrightness(0);
  flashStepsLeft = 11;
  flashStepAt = millis();
}

void updateFlash() {
  if (!flashStepsLeft || millis() - flashStepAt < 120) return;
  flashStepsLeft--;
  flashStepAt = millis();
  lcd.setBrightness(flashStepsLeft % 2 ? 0 : 255);   // odd = dark; ends lit at 0
}

void setRunning(bool want) {
  if (running == want) return;
  running = want;
  lastTick = millis();
  drawHint();
  lastShownSecs = -1;
  announce(running ? "started" : "paused");
}
void toggleRunning() { setRunning(!running); }

// ---- Screen sleep ----
// The screen goes dark when the timer has been stopped or paused for this long.
// A touch, the BOOT button, or any API/WebSocket-driven change wakes it.
const uint32_t SLEEP_AFTER_MS = 2UL * 60UL * 1000UL;   // 2 minutes
bool screenOn = true;
uint32_t lastActivity = 0;
bool backlightPending = false;   // panel is waking; backlight goes on at backlightOnAt
uint32_t backlightOnAt = 0;
bool swallowInput = false;   // the touch that wakes the screen must not also start/pause

void wakeScreen() {
  lastActivity = millis();
  if (!screenOn) {
    screenOn = true;
    // Panel sleep-out now, backlight later: the panel shows white while it
    // settles (up to 120 ms), so updateBacklight() turns the light on after that.
    lcd.getPanel()->setSleep(false);
    backlightOnAt = millis() + 120;
    backlightPending = true;
  }
}

void updateBacklight() {
  if (!backlightPending || (int32_t)(millis() - backlightOnAt) < 0) return;
  backlightPending = false;
  if (!flashStepsLeft) lcd.setBrightness(255);   // a running flash sets it itself
}

// Backlight off and the panel itself in sleep mode. Only the display sleeps:
// WiFi, HTTP and WebSocket keep running, and drawing continues into panel memory.
void sleepScreen() {
  screenOn = false;
  backlightPending = false;
  lcd.sleep();
}

// ---- Input: touchscreen + BOOT button ----
bool touchActive = false;

// Swipe tracking
const int SWIPE_MIN_PX   = 60;    // horizontal travel needed
const int MOVE_SLOP_PX   = 15;    // more movement than this = not a tap
const uint32_t SWIPE_MAX_MS = 700;
bool touchMoved = false;
int pendingSwipe = 0;             // -1 = left, +1 = right, set on release

void pollTouch() {
  static uint32_t lastPoll = 0, lastTouchSeen = 0, touchStartMs = 0;
  static int touchStartX = 0, touchStartY = 0, touchLastX = 0, touchLastY = 0;
  uint32_t now = millis();
  if (now - lastPoll < 15) return;
  lastPoll = now;
  uint16_t x, y;
  if (lcd.getTouch(&x, &y)) {
    if (!touchActive) {
      touchStartX = x; touchStartY = y; touchStartMs = now; touchMoved = false;
    }
    touchLastX = x; touchLastY = y;
    if (abs(touchLastX - touchStartX) > MOVE_SLOP_PX || abs(touchLastY - touchStartY) > MOVE_SLOP_PX) touchMoved = true;
    touchActive = true; lastTouchSeen = now;
  } else if (touchActive && now - lastTouchSeen > 150) {
    touchActive = false;
    int dx = touchLastX - touchStartX, dy = touchLastY - touchStartY;
    if (abs(dx) >= SWIPE_MIN_PX && abs(dx) > 2 * abs(dy) && lastTouchSeen - touchStartMs <= SWIPE_MAX_MS)
      pendingSwipe = dx < 0 ? -1 : 1;
  }
}

// Swipe left = skip to next phase, swipe right = restart current phase.
void handleSwipe() {
  if (!pendingSwipe) return;
  int s = pendingSwipe; pendingSwipe = 0;
  if (s < 0) nextPhase(false);
  else       startPhase(phase);
}

// Short press = start/pause, long press = skip. `down` is true while pressed.
// A touch that moved (swipe/drag) never counts as a tap or long press.
void inputUpdate(bool down) {
  static bool was = false, longFired = false;
  static uint32_t at = 0;
  uint32_t now = millis();
  if (down && !was) {
    was = true; at = now; longFired = false;
  } else if (down && was && !longFired && !touchMoved && now - at > 800) {
    longFired = true;
    nextPhase(false);
  } else if (!down && was) {
    was = false;
    if (!longFired && !touchMoved && now - at > 30) toggleRunning();
  }
}

// ---- API ----
// "running"  = counting down
// "paused"   = not counting, but part of the phase has already elapsed
// "stopped"  = not counting, and the phase is still at its full length
const char *stateName() {
  if (running) return "running";
  return atFullLength() ? "stopped" : "paused";
}

// Status JSON; with an event name it also carries an "event" field.
String statusJson(const char *event = nullptr) {
  char b[240];
  int n = snprintf(b, sizeof(b),
           "{\"state\":\"%s\",\"phase\":\"%s\",\"remaining_sec\":%d,\"total_sec\":%lu,\"sessions_done\":%u",
           stateName(), phaseName(), remainingSecs(), (unsigned long)totalSecs, (unsigned)sessionsDone);
  if (event) snprintf(b + n, sizeof(b) - n, ",\"event\":\"%s\"}", event);
  else       snprintf(b + n, sizeof(b) - n, "}");
  return String(b);
}

// ---- WebSocket ----
void wsEvent(uint8_t num, WStype_t type, uint8_t *payload, size_t length) {
  if (type == WStype_CONNECTED) {
    // Normally rejected during the handshake already; this is the backstop for
    // a request that sent no ordinary headers. payload is the path, e.g. "/?key=TOKEN".
    if (!AuthWebSocketsServer::urlKeyOk(String((char *)payload))) {
      ws.disconnect(num);
      return;
    }
    String s = statusJson();         // sendTXT needs a variable, not a temporary
    ws.sendTXT(num, s);              // send current state right away
  }
}

// Push the current state plus an event name to every connected client.
void announce(const char *event) {
  wakeScreen();                      // any state change lights the screen
  if (!serverStarted) return;
  String s = statusJson(event);
  ws.broadcastTXT(s);
}

bool authed() {
  if (server.header("X-Api-Key") == API_TOKEN || server.arg("key") == API_TOKEN) return true;
  server.send(401, "application/json", "{\"error\":\"unauthorized\"}");
  return false;
}

void replyStatus() { server.send(200, "application/json", statusJson()); }

void setupRoutes() {
  const char *hdrs[] = {"X-Api-Key"};
  server.collectHeaders(hdrs, 1);

  server.on("/sound", []() {
    if (!authed()) return;
    if (server.hasArg("on")) soundOn = server.arg("on") != "0";
    server.send(200, "application/json", String("{\"sound\":") + (soundOn ? "true" : "false") + "}");
  });

  server.on("/status", []() { if (!authed()) return; replyStatus(); });

  server.on("/start",  []() { if (!authed()) return; setRunning(true);  replyStatus(); });
  server.on("/pause",  []() { if (!authed()) return; setRunning(false); replyStatus(); });
  server.on("/toggle", []() { if (!authed()) return; toggleRunning(); replyStatus(); });
  server.on("/skip",   []() { if (!authed()) return; nextPhase(false); replyStatus(); });
  server.on("/reset",  []() { if (!authed()) return; startPhase(phase); replyStatus(); });

  server.on("/phase", []() {
    if (!authed()) return;
    String n = server.arg("name");
    for (int p = FOCUS; p <= LONG_BREAK; p++) {
      if (n == PHASE_API_NAMES[p]) { startPhase((Phase)p); replyStatus(); return; }
    }
    server.send(400, "application/json", "{\"error\":\"name must be focus, short or long\"}");
  });

  server.on("/config", []() {
    if (!authed()) return;
    bool atStart = !running && atFullLength();
    for (int p = FOCUS; p <= LONG_BREAK; p++) {
      const char *k = PHASE_API_NAMES[p];
      if (server.hasArg(k)) *PHASE_SECS[p] = constrain(server.arg(k).toInt(), 1, 180) * 60;
    }
    if (atStart) startPhase(phase);  // not started yet, so apply right away
    replyStatus();
  });

  server.onNotFound([]() { server.send(404, "application/json", "{\"error\":\"not found\"}"); });
}

void maintainWifi() {
  static bool wifiWasUp = false;
  bool up = (WiFi.status() == WL_CONNECTED);
  if (up && !serverStarted) {
    MDNS.begin(HOSTNAME);
    MDNS.addService("http", "tcp", 80);
    setupRoutes();
    server.begin();
    ws.begin();
    ws.onEvent(wsEvent);
    serverStarted = true;
  }
  if (up != wifiWasUp) {
    wifiWasUp = up;
    drawIP();
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_BTN, INPUT_PULLUP);

  lcd.init();
  lcd.setBrightness(255);
  timeSprite.setPsram(true);
  timeSprite.setColorDepth(16);
  if (!timeSprite.createSprite(W, 95)) Serial.println("time sprite: allocation failed");
  else Serial.printf("time sprite: %dx%d ok, psram free %u\n", timeSprite.width(), timeSprite.height(), (unsigned)ESP.getFreePsram());
  audioInit();
  C_BG    = lcd.color565(0, 0, 0);
  C_TRACK = lcd.color565(30, 30, 36);
  C_FOCUS = lcd.color565(255, 90, 70);
  C_SHORT = lcd.color565(60, 200, 140);
  C_LONG  = lcd.color565(70, 140, 255);
  C_DIM   = lcd.color565(120, 120, 130);

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  startPhase(FOCUS);   // the timer works even if WiFi never connects
}

void loop() {
  pollTouch();
  bool down = touchActive || digitalRead(PIN_BTN) == LOW;
  if (down) {
    if (!screenOn) swallowInput = true;                // first touch only wakes
    wakeScreen();
  }
  if (swallowInput) {
    pendingSwipe = 0;                                  // a waking swipe does nothing
    if (!down) swallowInput = false;                   // wait for release
  } else {
    inputUpdate(down);
    handleSwipe();
  }
  maintainWifi();
  if (serverStarted) { server.handleClient(); ws.loop(); }

  if (running) {
    uint32_t now = millis();
    remainingMs -= (int32_t)(now - lastTick);
    lastTick = now;
    if (remainingMs <= 0) {
      remainingMs = 0;
      announce("completed");
      playChime();          // both run in the background; loop() keeps going
      flashBacklight();
      nextPhase(true);
      return;
    }
  }
  updateFlash();
  updateBacklight();

  // Pausing and completing both announce(), which refreshes lastActivity.
  if (!running && screenOn && millis() - lastActivity > SLEEP_AFTER_MS) sleepScreen();

  int secs = remainingSecs();
  if (secs != lastShownSecs) {
    lastShownSecs = secs;
    drawTime(secs);
    int elapsedPx = (int)((float)BAR_W * (1.0f - (float)remainingMs / (totalSecs * 1000.0f)));
    if (elapsedPx > lastElapsedPx) {
      lcd.fillRect(BAR_X + BAR_W - elapsedPx, BAR_Y, elapsedPx - lastElapsedPx, BAR_H, C_TRACK);
      lastElapsedPx = elapsedPx;
    }
  }
  delay(5);
}
