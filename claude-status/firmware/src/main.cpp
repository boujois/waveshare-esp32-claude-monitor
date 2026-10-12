// Claude Code status display for the Waveshare ESP32-S3-LCD-1.28 or the LilyGO T-RGB
// (build with -e lilygo-t-rgb, see platformio.ini).
//
// The Mac bridge (../bridge) POSTs a JSON summary to http://claude-status.local/state.
// When a session needs input the screen shows a full-screen alert; otherwise it shows
// session status, plan usage rings (5-hour outer, weekly inner) and today's activity, with a
// tabby cat acting out what's going on (see cat.cpp). The cat also walks across at power-on.
// Double-tapping the case dismisses the alert or Done card on screen, as does a short press of the
// BOOT button if your case leaves it reachable; tapping it five times quickly shows diagnostics
// (see taps.cpp). Holding BOOT for 5 seconds forgets the Wi-Fi network and reopens Wi-Fi setup.
// Without the button: power the display on 3 times in a row (each within 10 s of the last), or
// POST http://claude-status.local/wifi/reset.
//
// Wi-Fi setup: with no saved network, the display opens its own "Claude-Monitor-Setup"
// network. Join it from a phone (scan the QR code on screen) and pick your Wi-Fi.
// The timezone comes from the Mac helper, so there's nothing to configure in code.
//
// Updates: the Mac helper pairs with the display once (POST /pair with a random key) and
// can then send new firmware over Wi-Fi (POST /update with that key). Powering on 3 times
// in a row also clears the pairing, for when the Mac's key is lost.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <LovyanGFX.hpp>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <Update.h>
#include <WiFiManager.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <time.h>

#include <utility>
#include <vector>

#include "cat.h"
#include "taps.h"

#if __has_include("secrets.h") && !defined(RELEASE_BUILD)  // never in release images
#include "secrets.h"  // optional: pre-fills Wi-Fi for development builds (git-ignored)
#endif

constexpr const char *HOSTNAME = "claude-status";
#ifndef FW_VERSION
#define FW_VERSION "dev"  // set by tools/build-release.sh
#endif
constexpr const char *SETUP_AP = "Claude-Monitor-Setup";      // Wi-Fi setup network name
constexpr const char *DEFAULT_TZ = "UTC0";                    // until the Mac helper sends its timezone
constexpr uint32_t RESET_HOLD_MS = 5000;                      // hold BOOT this long to reset Wi-Fi
constexpr uint8_t RESET_POWER_CYCLES = 3;                     // ...or power it on this many times in a row
constexpr uint32_t POWER_CYCLE_WINDOW_MS = 10000;             // each within this long of the last
constexpr uint32_t STALE_MS = 30000;                          // no push for this long = Mac offline
constexpr float TAP_G = 0.25f;                                // how firm each tap on the case must be, in g
constexpr uint32_t DIAGNOSTICS_MS = 30000;                    // five quick taps show diagnostics this long

#ifndef BOARD_LILYGO_T_RGB

// ---- Board pins (Waveshare wiki, non-touch version) ----
constexpr int PIN_LCD_DC   = 8;
constexpr int PIN_LCD_CS   = 9;
constexpr int PIN_LCD_SCLK = 10;
constexpr int PIN_LCD_MOSI = 11;
constexpr int PIN_LCD_RST  = 12;
constexpr int PIN_LCD_BL   = 40;
constexpr int PIN_BOOT     = 0;

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_GC9A01 _panel;
  lgfx::Bus_SPI _bus;
  lgfx::Light_PWM _light;

 public:
  LGFX() {
    {
      auto cfg = _bus.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 40000000;
      cfg.freq_read = 16000000;
      cfg.spi_3wire = true;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = PIN_LCD_SCLK;
      cfg.pin_mosi = PIN_LCD_MOSI;
      cfg.pin_miso = -1;
      cfg.pin_dc = PIN_LCD_DC;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs = PIN_LCD_CS;
      cfg.pin_rst = PIN_LCD_RST;
      cfg.pin_busy = -1;
      cfg.panel_width = 240;
      cfg.panel_height = 240;
      cfg.readable = false;
      cfg.invert = true;
      cfg.rgb_order = false;
      cfg.bus_shared = false;
      _panel.config(cfg);
    }
    {
      auto cfg = _light.config();
      cfg.pin_bl = PIN_LCD_BL;
      cfg.invert = false;
      cfg.freq = 44100;
      cfg.pwm_channel = 7;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    setPanel(&_panel);
  }
};

static LGFX lcd;
static LGFX_Sprite frame(&lcd);

constexpr const char *BOARD_NAME = "waveshare-s3-lcd128";
static void showFrame() { frame.pushSprite(0, 0); }
static uint32_t touchCount() { return 0; }  // no touch screen: wake from night hours with a double tap

#else  // BOARD_LILYGO_T_RGB

#include <esp_heap_caps.h>
#include "trgb/LilyGo_RGBPanel.h"

constexpr int PIN_BOOT = 0;

// The T-RGB has a 480x480 ST7701S panel on the ESP32-S3's RGB interface; LilyGO's driver
// (src/trgb, MIT) sets it up through the XL9555 I/O expander and picks the right init
// sequence for the 2.1" or 2.8" board from the touch controller it finds. The UI is still
// drawn into the 240x240 sprite and scaled up 2x with smoothing on the way to the panel.
// To force a panel type, build with e.g. -DTRGB_PANEL=LILYGO_T_RGB_2_8_INCHES.
#ifndef TRGB_WB_R  // starting white balance, in percent; adjust live with POST /whitebalance
#define TRGB_WB_R 100
#endif
#ifndef TRGB_WB_G
#define TRGB_WB_G 90
#endif
#ifndef TRGB_WB_B
#define TRGB_WB_B 70
#endif

class TRGBDisplay {
  LilyGo_RGBPanel panel;
  static constexpr int OUT = 480, BAND = 16;  // output rows per transfer
  uint16_t *band = nullptr;

  // White balance: per-channel gains in percent, applied to every pixel through small lookup
  // tables. This panel shows mixed colours too blue, so blue (and a little green) is turned down.
  uint8_t wb[3] = {TRGB_WB_R, TRGB_WB_G, TRGB_WB_B};
  uint16_t lutR[32], lutG[64], lutB[32];
  uint16_t rows[2][240];
  inline uint16_t balance(uint16_t v) const { return lutR[v >> 11] | lutG[(v >> 5) & 63] | lutB[v & 31]; }

  // Exact (floor) average of two RGB565 pixels.
  static inline uint16_t avg(uint16_t a, uint16_t b) { return (a & b) + (((a ^ b) & 0xF7DE) >> 1); }

 public:
  void init() {
#ifdef TRGB_BGR
    constexpr LilyGo_RGBPanel_Color_Order order = LILYGO_T_RGB_ORDER_BGR;
#else
    constexpr LilyGo_RGBPanel_Color_Order order = LILYGO_T_RGB_ORDER_RGB;
#endif
#ifdef TRGB_PANEL
    bool ok = panel.begin(TRGB_PANEL, order);
#else
    bool ok = panel.begin(order);
#endif
    if (!ok) Serial.println("T-RGB panel init failed");
    setWhiteBalance(wb[0], wb[1], wb[2]);
    Serial.printf("T-RGB: touch %s, model %d\n", panel.getTouchModelName(), (int)panel.getModel());
    band = (uint16_t *)heap_caps_malloc(OUT * BAND * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!band) band = (uint16_t *)heap_caps_malloc(OUT * BAND * 2, MALLOC_CAP_SPIRAM);
  }

  void setWhiteBalance(int r, int g, int b) {
    wb[0] = constrain(r, 0, 100);
    wb[1] = constrain(g, 0, 100);
    wb[2] = constrain(b, 0, 100);
    for (int i = 0; i < 32; i++) {
      lutR[i] = ((i * wb[0] + 50) / 100) << 11;
      lutB[i] = (i * wb[2] + 50) / 100;
    }
    for (int i = 0; i < 64; i++) lutG[i] = ((i * wb[1] + 50) / 100) << 5;
  }
  const uint8_t *whiteBalance() const { return wb; }

  const char *touchName() { return panel.getTouchModelName(); }
  bool touched() {
    int16_t x, y;
    return panel.getPoint(&x, &y, 1) > 0;
  }
  int model() { return (int)panel.getModel(); }

  // Diagnostic (build with -DTRGB_TEST_PATTERN): six colour stripes. The left half is written
  // straight to the panel as plain RGB565; the right half goes through the sprite like the UI.
  void testPattern(LGFX_Sprite &spr) {
    static const uint32_t cols[6] = {0xFF0000, 0x00FF00, 0x0000FF, 0xFFFFFF, 0xA6AEBF, 0x0B0F1A};
    for (int i = 0; i < 6; i++) spr.fillRect(0, i * 40, 240, 40, cols[i]);
    spr.setTextDatum(textdatum_t::middle_center);
    spr.setFont(&fonts::Font2);
    spr.setTextColor(0xFFFFFF);
    spr.drawString(String("touch ") + touchName() + "  model " + String(model()), 180, 220);
    present(spr);
    if (!band) return;
    for (int i = 0; i < 6; i++) {
      uint32_t c = cols[i];
      uint16_t v = ((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c & 0xFF) >> 3);  // RGB888 -> RGB565
      for (int k = 0; k < 240 * BAND; k++) band[k] = v;
      for (int y = i * 80; y < (i + 1) * 80; y += BAND) panel.pushColors(0, y, 240, y + BAND, band);
    }
  }

  // 0-255, like LovyanGFX; the T-RGB backlight driver has 16 steps.
  void setBrightness(uint8_t v) { panel.setBrightness(v ? max(1, (v * 16 + 127) / 255) : 0); }

  void fillScreen(uint32_t) {  // only ever used to clear to black at start-up
    if (!band) return;
    memset(band, 0, OUT * BAND * 2);
    for (int y = 0; y < OUT; y += BAND) panel.pushColors(0, y, OUT, y + BAND, band);
  }

  // Scales a 240x240 16-bit sprite to the full panel. Each source pixel becomes a 2x2 block
  // of itself and its averages with the neighbours to the right and below.
  void present(LGFX_Sprite &spr) {
    const uint16_t *src = (const uint16_t *)spr.getBuffer();
    if (!src || !band) return;
    const int W = spr.width(), H = spr.height();  // 240 x 240
    if (W > 240) return;
    // LovyanGFX keeps 16-bit sprites byte-swapped; the RGB panel wants native order.
    auto load = [&](int y, uint16_t *out) {
      const uint16_t *in = src + y * W;
      for (int x = 0; x < W; x++) out[x] = balance(__builtin_bswap16(in[x]));
    };
    uint16_t *r0 = rows[0], *r1 = rows[1];
    load(0, r0);
    int row = 0;
    for (int y = 0; y < H; y++) {
      load(y + 1 < H ? y + 1 : y, r1);
      uint16_t *o0 = band + row * OUT, *o1 = o0 + OUT;
      for (int x = 0; x < W; x++) {
        int xn = x + 1 < W ? x + 1 : x;
        uint16_t a = r0[x], b = r0[xn], c = r1[x], d = r1[xn];
        uint16_t ab = avg(a, b), ac = avg(a, c);
        o0[2 * x] = a;
        o0[2 * x + 1] = ab;
        o1[2 * x] = ac;
        o1[2 * x + 1] = avg(ab, avg(c, d));
      }
      std::swap(r0, r1);
      row += 2;
      if (row == BAND) {
        int oy = 2 * y + 2 - BAND;
        panel.pushColors(0, oy, OUT, oy + BAND, band);
        row = 0;
      }
    }
  }
};

static TRGBDisplay lcd;
static LGFX_Sprite frame;

constexpr const char *BOARD_NAME = "lilygo-t-rgb";
static void showFrame() { lcd.present(frame); }

// ---- Taps (taps.h) from the touch screen ----
// The T-RGB has no motion sensor, so the gestures that are taps on the Waveshare's case are taps
// on the screen here: a run of touches 60-450 ms apart; exactly two is a double tap (counted once
// the run is over), and the fifth touch of a run counts as five taps straight away.
namespace touchtaps {
constexpr uint32_t POLL_MS = 10, GAP_MIN = 60, GAP_MAX = 450;
volatile uint32_t doubles = 0, fives = 0, touches = 0;

void watch(void *) {
  bool wasDown = false;
  uint32_t lastTap = 0, run = 0;
  for (;;) {
    uint32_t now = millis();
    bool down = lcd.touched();
    if (down && !wasDown && (run == 0 || now - lastTap >= GAP_MIN)) {
      run = (run && now - lastTap <= GAP_MAX) ? run + 1 : 1;
      lastTap = now;
      touches = touches + 1;
      if (run == 5) fives = fives + 1;
    }
    wasDown = down;
    if (run && now - lastTap > GAP_MAX) {
      if (run == 2) doubles = doubles + 1;
      run = 0;
    }
    vTaskDelay(pdMS_TO_TICKS(POLL_MS));
  }
}
}  // namespace touchtaps

bool tapsBegin(float) {
  return xTaskCreatePinnedToCore(touchtaps::watch, "touchtaps", 4096, nullptr, 5, nullptr, 0) == pdPASS;
}
uint32_t doubleTapCount() { return touchtaps::doubles; }
uint32_t fiveTapCount() { return touchtaps::fives; }
static uint32_t touchCount() { return touchtaps::touches; }  // every touch, for waking from night hours

// Firmware for other boards would boot but drive the wrong screen, so Wi-Fi updates only
// accept images that carry this tag (every T-RGB build does, via this very string).
static const char OTA_BOARD_TAG[] = "claude-monitor-board=lilygo-t-rgb;";
#define REQUIRE_OTA_BOARD_TAG 1

#endif  // BOARD_LILYGO_T_RGB
static WebServer server(80);

constexpr int CX = 120, CY = 120;

// Colours
const uint32_t COL_BG      = 0x0B0F1A;
const uint32_t COL_TRACK   = 0x1C2230;
const uint32_t COL_TEXT    = 0xF2F4F8;
const uint32_t COL_DIM     = 0xA6AEBF;
const uint32_t COL_FAINT   = 0x5A6478;
const uint32_t COL_CLAUDE  = 0xD97757;
const uint32_t COL_BRIGHT  = 0xFF9B73;
const uint32_t COL_WEEK    = 0x6A9BCC;
const uint32_t COL_OK      = 0x34C759;
const uint32_t COL_WARN    = 0xFFD60A;
const uint32_t COL_ERR     = 0xFF453A;

// ---- State pushed from the Mac ----

struct Session {
  String id, name, state, kind, detail;
  uint32_t since = 0;
};

// CI checks on the pull request for the session open in the Claude app
struct CI {
  bool present = false;
  String id;           // that session's id
  String title;        // "#1252 Fix the thing"
  String status;       // "running", "passed", "failed" or "cancelled"
  String checks;       // a letter per check, in ring order: P passed, F failed, C cancelled, R running, Q queued
  String detail;       // the checks that failed, or that are running
  String key;          // changes when there's something new to show
  uint32_t since = 0;  // when the run started (while running) or finished
};

struct State {
  bool valid = false;
  uint32_t rxMillis = 0;
  std::vector<Session> sessions;
  int busy = 0, idle = 0, waiting = 0, done = 0, errors = 0;
  bool haveUsage = false;
  float h5 = -1, d7 = -1;
  uint32_t h5Reset = 0, d7Reset = 0;
  bool haveToday = false;
  double tokens = 0;
  uint32_t prompts = 0;
  String update;    // newer release available, e.g. "v1.1.0"
  String incident;  // an Anthropic incident affecting Claude Code, from status.claude.com
  CI ci;
};

static State st;
static String dismissedWaiting, dismissedDone;  // sessions on screen when you last dismissed each card
static String dismissedCI;                      // the CI card's key when you last dismissed it
static bool tapReady = false;                   // the motion sensor was found, so taps on the case work
static WiFiManager wifiManager;
static Preferences prefs;
#ifdef BOARD_LILYGO_T_RGB
constexpr uint8_t DEFAULT_BRIGHTNESS = 204;  // 80%: the T-RGB's backlight is dimmer
#else
constexpr uint8_t DEFAULT_BRIGHTNESS = 150;
#endif
static int idleBrightness = DEFAULT_BRIGHTNESS;  // normal screens; alerts are always full brightness

// Night hours: the backlight is off between these times (minutes after midnight, local time)
// unless Claude needs you. A tap wakes the screen for WAKE_MS. Equal times turn it off.
constexpr int DEFAULT_NIGHT_FROM = 23 * 60, DEFAULT_NIGHT_TO = 7 * 60;
constexpr uint32_t WAKE_MS = 60000;
static int nightFrom = DEFAULT_NIGHT_FROM, nightTo = DEFAULT_NIGHT_TO;

static bool nightNow() {
  if (nightFrom == nightTo) return false;
  time_t now = time(nullptr);
  if (now < 1700000000) return false;  // no clock yet
  struct tm t;
  localtime_r(&now, &t);
  int m = t.tm_hour * 60 + t.tm_min;
  return nightFrom < nightTo ? (m >= nightFrom && m < nightTo) : (m >= nightFrom || m < nightTo);
}

static String hhmm(int m) {
  char b[6];
  snprintf(b, sizeof(b), "%02d:%02d", m / 60, m % 60);
  return b;
}

static int parseHHMM(const String &v) {  // "23:00" or "23" -> minutes, or -1
  int colon = v.indexOf(':');
  int h = (colon < 0 ? v : v.substring(0, colon)).toInt();
  int m = colon < 0 ? 0 : v.substring(colon + 1).toInt();
  if (v.length() == 0 || h < 0 || h > 23 || m < 0 || m > 59) return -1;
  return h * 60 + m;
}
static String currentTz;
static uint8_t powerCycles = 0;  // consecutive quick power-ons, for the no-button Wi-Fi reset
static String pairKey;           // shared with the Mac helper; required for firmware updates
static bool showCat = true;      // the cat and its power-on walk ("cat" in the helper's config.json)

static void applyTimezone(const String &tz) {
  currentTz = tz;
  setenv("TZ", tz.c_str(), 1);
  tzset();
}

static void handleState() {
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "text/plain", "bad json");
    return;
  }
  State s;
  s.valid = true;
  s.rxMillis = millis();
  for (JsonObject o : doc["sessions"].as<JsonArray>()) {
    Session x;
    x.id = (const char *)(o["id"] | "");
    x.name = (const char *)(o["name"] | "");
    x.state = (const char *)(o["state"] | "idle");
    x.kind = (const char *)(o["kind"] | "");
    x.detail = (const char *)(o["detail"] | "");
    x.since = o["since"] | 0;
    s.sessions.push_back(x);
  }
  JsonObject c = doc["counts"];
  s.busy = c["busy"] | 0;
  s.idle = c["idle"] | 0;
  s.waiting = c["waiting"] | 0;
  s.done = c["done"] | 0;
  s.errors = c["error"] | 0;
  JsonObject u = doc["usage"];
  if (!u.isNull()) {
    s.haveUsage = true;
    s.h5 = u["h5"] | -1.0f;
    s.d7 = u["d7"] | -1.0f;
    s.h5Reset = u["h5_reset"] | 0;
    s.d7Reset = u["d7_reset"] | 0;
  }
  JsonObject t = doc["today"];
  if (!t.isNull()) {
    s.haveToday = true;
    s.tokens = t["tokens"] | 0.0;
    s.prompts = t["prompts"] | 0;
  }
  JsonObject ci = doc["ci"];
  if (!ci.isNull() && strlen(ci["checks"] | "")) {
    s.ci.present = true;
    s.ci.id = (const char *)(ci["id"] | "");
    s.ci.title = (const char *)(ci["title"] | "");
    s.ci.status = (const char *)(ci["status"] | "");
    s.ci.checks = (const char *)(ci["checks"] | "");
    s.ci.detail = (const char *)(ci["detail"] | "");
    s.ci.key = (const char *)(ci["key"] | "");
    s.ci.since = ci["since"] | 0;
  }
  s.update = (const char *)(doc["update"] | "");
  if (!doc["incident"].isNull()) s.incident = (const char *)(doc["incident"]["name"] | "Claude incident");
  st = std::move(s);

  // The Mac helper sends its timezone as a POSIX TZ string; remember it across reboots
  String tz = (const char *)(doc["tz"] | "");
  if (tz.length() && tz != currentTz) {
    applyTimezone(tz);
    prefs.putString("tz", tz);
    Serial.println("Timezone set to " + tz);
  }

  // Remembered too, so the power-on walk follows it before the Mac is heard from
  bool cat = doc["cat"] | true;
  if (cat != showCat) {
    showCat = cat;
    prefs.putBool("cat", cat);
  }
  server.send(200, "application/json", "{\"ok\":true}");
}

#ifdef BOARD_LILYGO_T_RGB
// GET shows the white balance; POST /whitebalance?r=100&g=90&b=70 (percent) changes and saves it.
static void handleWhiteBalance() {
  if (server.method() == HTTP_POST) {
    const uint8_t *cur = lcd.whiteBalance();
    int r = server.hasArg("r") ? server.arg("r").toInt() : cur[0];
    int g = server.hasArg("g") ? server.arg("g").toInt() : cur[1];
    int b = server.hasArg("b") ? server.arg("b").toInt() : cur[2];
    lcd.setWhiteBalance(r, g, b);
    const uint8_t *wb = lcd.whiteBalance();
    prefs.putUChar("wbR", wb[0]);
    prefs.putUChar("wbG", wb[1]);
    prefs.putUChar("wbB", wb[2]);
  }
  const uint8_t *wb = lcd.whiteBalance();
  server.send(200, "text/plain", "white balance r=" + String(wb[0]) + " g=" + String(wb[1]) + " b=" + String(wb[2]) + "\n");
}
#endif

// GET shows the brightness of the normal screens (alerts are always full); POST /brightness?level=80
// (percent) changes and saves it.
static void handleBrightness() {
  if (server.method() == HTTP_POST && server.hasArg("level")) {
    int pct = constrain(server.arg("level").toInt(), 5, 100);
    idleBrightness = (pct * 255 + 50) / 100;
    prefs.putUChar("bright", idleBrightness);
  }
  server.send(200, "text/plain", "brightness " + String((idleBrightness * 100 + 127) / 255) + "%\n");
}

// GET shows the night hours; POST /sleep?from=23:00&to=07:00 changes and saves them, POST /sleep?off=1
// turns them off.
static void handleSleep() {
  if (server.method() == HTTP_POST) {
    if (server.hasArg("off")) {
      nightFrom = nightTo = 0;
    } else {
      int from = server.hasArg("from") ? parseHHMM(server.arg("from")) : nightFrom;
      int to = server.hasArg("to") ? parseHHMM(server.arg("to")) : nightTo;
      if (from < 0 || to < 0) {
        server.send(400, "text/plain", "Use times like from=23:00&to=07:00\n");
        return;
      }
      nightFrom = from;
      nightTo = to;
    }
    prefs.putUShort("nightFrom", nightFrom);
    prefs.putUShort("nightTo", nightTo);
  }
  server.send(200, "text/plain",
              nightFrom == nightTo ? String("night hours off\n")
                                   : "night hours " + hhmm(nightFrom) + "-" + hhmm(nightTo) + (nightNow() ? " (now)\n" : "\n"));
}

static void handleWifiReset() {
  server.send(200, "text/plain", "Forgetting Wi-Fi and restarting into Wi-Fi setup\n");
  delay(300);
  wifiManager.resetSettings();
  ESP.restart();
}

static void handleRoot() {
  String out = "claude-status display " FW_VERSION "\nIP " + WiFi.localIP().toString() + "\nlast push " +
               (st.valid ? String((millis() - st.rxMillis) / 1000) + "s ago" : String("never")) + "\n" +
               (tapReady ? "taps seen: " + String(doubleTapCount()) + " double, " + String(fiveTapCount()) + " five"
                         : String("no motion sensor: taps on the case don't work")) + "\n";
  server.send(200, "text/plain", out);
}

// Streams the last rendered frame as a 24-bit BMP (handy for checking layouts remotely).
static void handleScreenshot() {
  const int W = 240, H = 240, rowBytes = W * 3, size = 54 + rowBytes * H;
  uint8_t hdr[54] = {'B', 'M'};
  auto put32 = [&](int off, uint32_t v) { memcpy(hdr + off, &v, 4); };
  put32(2, size);
  put32(10, 54);
  put32(14, 40);
  put32(18, W);
  put32(22, H);
  hdr[26] = 1;
  hdr[28] = 24;
  put32(34, rowBytes * H);
  WiFiClient client = server.client();
  server.setContentLength(size);
  server.send(200, "image/bmp", "");
  client.write(hdr, sizeof(hdr));
  static uint8_t row[W * 3];
  for (int y = H - 1; y >= 0; y--) {  // BMP rows are bottom-up, BGR
    for (int x = 0; x < W; x++) {
      auto c = frame.readPixelRGB(x, y);
      row[x * 3] = c.B8();
      row[x * 3 + 1] = c.G8();
      row[x * 3 + 2] = c.R8();
    }
    client.write(row, rowBytes);
  }
}

// ---- Drawing helpers ----

static uint32_t lerpColor(uint32_t a, uint32_t b, float t) {
  auto ch = [&](int shift) {
    int x = (a >> shift) & 0xFF, y = (b >> shift) & 0xFF;
    return (uint32_t)(x + (y - x) * t) & 0xFF;
  };
  return ch(16) << 16 | ch(8) << 8 | ch(0);
}

static uint32_t usageColor(float pct, uint32_t base) {
  if (pct >= 90) return COL_ERR;
  if (pct >= 75) return COL_WARN;
  return base;
}

// Usable width for a line of text centred at height y, inside a circle of radius r
// (the inner edge of a ring, minus a margin). h is about half the font's height.
static int chordWidth(int y, int r, int h = 8) {
  int dy = abs(y - CY) + h;
  return dy >= r ? 0 : (int)(2 * sqrtf((float)(r * r - dy * dy)));
}

constexpr int CARD_TEXT_R = 100;    // alert / Done cards: ring inner edge 106
constexpr int STATUS_TEXT_R = 90;   // status screen: inner usage ring's inner edge 96

static String fit(const String &s, int maxW);

// The first wording that fits (set the font first); the last one is truncated if none do.
static String firstFit(std::initializer_list<String> options, int maxW) {
  String last;
  for (const String &o : options) {
    if (frame.textWidth(o) <= maxW) return o;
    last = o;
  }
  return fit(last, maxW);
}

static String fit(const String &s, int maxW) {
  if (frame.textWidth(s) <= maxW) return s;
  String t = s;
  while (t.length() > 1 && frame.textWidth(t + "..") > maxW) t.remove(t.length() - 1);
  t.trim();
  return t + "..";
}

// Greedy word wrap into at most two lines; the second is truncated.
static void wrap2(const String &s, int maxW, String &l1, String &l2) {
  l2 = "";
  if (frame.textWidth(s) <= maxW) {
    l1 = s;
    return;
  }
  int cut = -1;
  for (int i = 1; i < (int)s.length(); i++)
    if (s[i] == ' ' && frame.textWidth(s.substring(0, i)) <= maxW) cut = i;
  if (cut < 0) {
    l1 = fit(s, maxW);
    return;
  }
  l1 = s.substring(0, cut);
  l2 = fit(s.substring(cut + 1), maxW);
}

static String duration(int32_t s) {
  if (s <= 0) return "now";
  if (s < 60) return String(s) + "s";
  if (s < 3600) return String(s / 60) + "m";
  if (s < 86400) return String(s / 3600) + "h" + String((s % 3600) / 60) + "m";
  return String(s / 86400) + "d" + String((s % 86400) / 3600) + "h";
}

static String compact(double v) {
  char b[16];
  if (v >= 1e9) snprintf(b, sizeof(b), "%.1fB", v / 1e9);
  else if (v >= 1e6) snprintf(b, sizeof(b), "%.1fM", v / 1e6);
  else if (v >= 1e3) snprintf(b, sizeof(b), "%.0fk", v / 1e3);
  else snprintf(b, sizeof(b), "%.0f", v);
  return b;
}

// Progress ring starting at 12 o'clock, clockwise, with rounded ends.
static void ring(int r0, int r1, float pct, uint32_t col) {
  frame.fillArc(CX, CY, r0, r1, 0, 360, COL_TRACK);
  if (pct <= 0) return;
  pct = min(pct, 100.0f);
  float end = -90 + 3.6f * pct;
  frame.fillArc(CX, CY, r0, r1, -90, end, col);
  float rm = (r0 + r1) / 2.0f, rw = (r1 - r0) / 2.0f;
  float a = end * DEG_TO_RAD;
  frame.fillSmoothCircle(CX, CY - rm, rw, col);
  frame.fillSmoothCircle(CX + rm * cosf(a), CY + rm * sinf(a), rw, col);
}

static void text(const String &s, int x, int y, uint32_t col, const lgfx::IFont *font,
                 textdatum_t datum = textdatum_t::middle_center) {
  frame.setFont(font);
  frame.setTextDatum(datum);
  frame.setTextColor(col);
  frame.drawString(s, x, y);
}

static float pulse(float periodMs) { return 0.5f + 0.5f * sinf(millis() * TWO_PI / periodMs); }

// A line of Font2 text centred at height y; if it's wider than w, it scrolls sideways through
// that width like a ticker, so all of it can be read
static void ticker(const String &s, int y, int w, uint32_t col) {
  frame.setFont(&fonts::Font2);
  int tw = frame.textWidth(s);
  if (tw <= w) {
    text(s, CX, y, col, &fonts::Font2);
    return;
  }
  const int gap = 48, x0 = CX - w / 2;
  int off = (millis() / 30) % (tw + gap);  // ~33 px a second
  frame.setClipRect(x0, y - 9, w, 18);
  text(s, x0 - off, y, col, &fonts::Font2, textdatum_t::middle_left);
  text(s, x0 - off + tw + gap, y, col, &fonts::Font2, textdatum_t::middle_left);
  frame.clearClipRect();
}

// ---- Screens ----

static const char *headline(const String &kind) {
  if (kind == "permission") return "Permission";
  if (kind == "question") return "Question";
  if (kind == "plan") return "Plan ready";
  return "Needs input";
}

// "waiting 3m  +1 more" at the bottom of a card, shortened to fit the narrow bottom of the circle
static void drawCardFooter(const char *before, const char *after, uint32_t since, int others) {
  const int y = 190;
  time_t now = time(nullptr);
  String t = since && now > 1700000000 ? duration(now - since) : String("");
  String more = others > 0 ? "+" + String(others) + " more" : String("");
  String sep = t.length() && more.length() ? "  " : "";
  String full = t.length() ? before + t + after : String("");
  String brief = t.length() ? t + after : String("");
  frame.setFont(&fonts::Font2);
  String foot = firstFit({full + sep + more, brief + sep + more,
                          brief + sep + (others > 0 ? "+" + String(others) : String("")), brief},
                         chordWidth(y, CARD_TEXT_R));
  text(foot, CX, y, COL_FAINT, &fonts::Font2);
}

static void drawAlert(const Session &s, int others) {
  frame.fillScreen(COL_BG);
  float p = pulse(1600);
  frame.fillArc(CX, CY, 106, 120, 0, 360, lerpColor(0x5A2414, COL_BRIGHT, p));

  frame.fillSmoothCircle(CX, 56, 19, COL_CLAUDE);
  text(s.kind == "question" ? "?" : "!", CX, 57, COL_BG, &fonts::FreeSansBold12pt7b);

  text(headline(s.kind), CX, 96, COL_TEXT, &fonts::FreeSansBold12pt7b);

  frame.setFont(&fonts::FreeSans9pt7b);
  String l1, l2;
  wrap2(s.name, 176, l1, l2);
  text(l1, CX, 124, COL_DIM, &fonts::FreeSans9pt7b);
  if (l2.length()) text(l2, CX, 144, COL_DIM, &fonts::FreeSans9pt7b);

  // Detail (e.g. the question): two lines when the session name fits on one
  frame.setFont(&fonts::Font2);
  if (l2.length()) {
    text(fit(s.detail, 150), CX, 168, COL_BRIGHT, &fonts::Font2);
  } else {
    String d1, d2;
    wrap2(s.detail, 160, d1, d2);
    text(d1, CX, d2.length() ? 152 : 158, COL_BRIGHT, &fonts::Font2);
    if (d2.length()) text(d2, CX, 168, COL_BRIGHT, &fonts::Font2);
  }

  drawCardFooter("waiting ", "", s.since, others);
}

// Green card for a session that finished; stays until you reply to it.
static void drawDone(const Session &s, int others) {
  frame.fillScreen(COL_BG);
  frame.fillArc(CX, CY, 106, 120, 0, 360, lerpColor(0x0F3D1E, COL_OK, 0.55f + 0.45f * pulse(4000)));

  frame.fillSmoothCircle(CX, 56, 19, COL_OK);
  frame.drawWedgeLine(CX - 9, 57, CX - 3, 63, 2.5, 2.5, COL_BG);  // check mark
  frame.drawWedgeLine(CX - 3, 63, CX + 9, 50, 2.5, 2.5, COL_BG);

  text("Done", CX, 96, COL_TEXT, &fonts::FreeSansBold12pt7b);

  frame.setFont(&fonts::FreeSans9pt7b);
  String l1, l2;
  wrap2(s.name, 176, l1, l2);
  text(l1, CX, 124, COL_DIM, &fonts::FreeSans9pt7b);
  if (l2.length()) text(l2, CX, 144, COL_DIM, &fonts::FreeSans9pt7b);

  // What it got done, when the helper's reply check provides a summary
  const uint32_t COL_DONE_TEXT = 0x7EE2A0;
  frame.setFont(&fonts::Font2);
  if (l2.length()) {
    text(fit(s.detail, 150), CX, 168, COL_DONE_TEXT, &fonts::Font2);
  } else {
    String d1, d2;
    wrap2(s.detail, 160, d1, d2);
    text(d1, CX, d2.length() ? 152 : 158, COL_DONE_TEXT, &fonts::Font2);
    if (d2.length()) text(d2, CX, 168, COL_DONE_TEXT, &fonts::Font2);
  }

  drawCardFooter("finished ", " ago", s.since, others);
}

// CI checks on the pull request for the session open in the Claude app: one ring segment per
// check, filling clockwise as they pass. Running checks pulse; queued ones stay dark.
static void drawCI(const CI &ci) {
  frame.fillScreen(COL_BG);
  int n = ci.checks.length(), passed = 0, failed = 0, active = 0;
  float seg = 360.0f / n, gap = n > 1 ? min(3.0f, seg / 4) : 0;
  for (int i = 0; i < n; i++) {
    char c = ci.checks[i];
    passed += c == 'P';
    failed += c == 'F';
    active += c == 'R';
    uint32_t col = c == 'P' ? COL_OK
                 : c == 'F' ? COL_ERR
                 : c == 'C' ? COL_FAINT
                 : c == 'R' ? lerpColor(COL_TRACK, COL_WARN, 0.3f + 0.7f * pulse(1400))
                            : COL_TRACK;
    frame.fillArc(CX, CY, 106, 119, -90 + i * seg + gap / 2, -90 + (i + 1) * seg - gap / 2, col);
  }

  bool running = ci.status == "running", red = ci.status == "failed", green = ci.status == "passed";
  frame.fillSmoothCircle(CX, 56, 19, running ? COL_WEEK : red ? COL_ERR : green ? COL_OK : COL_FAINT);
  if (running) {  // spinner
    float a = (millis() / 3) % 360;
    frame.fillArc(CX, 56, 7, 10, a, a + 270, COL_BG);
  } else if (red) {  // cross
    frame.drawWedgeLine(CX - 6, 50, CX + 6, 62, 2.5, 2.5, COL_BG);
    frame.drawWedgeLine(CX + 6, 50, CX - 6, 62, 2.5, 2.5, COL_BG);
  } else if (green) {  // check mark
    frame.drawWedgeLine(CX - 9, 57, CX - 3, 63, 2.5, 2.5, COL_BG);
    frame.drawWedgeLine(CX - 3, 63, CX + 9, 50, 2.5, 2.5, COL_BG);
  } else {  // cancelled
    frame.drawWedgeLine(CX - 8, 56, CX + 8, 56, 2.5, 2.5, COL_BG);
  }
  text(running ? "CI running" : red ? "Checks failed" : green ? "Checks passed" : "Checks cancelled", CX, 96, COL_TEXT,
       &fonts::FreeSansBold12pt7b);

  frame.setFont(&fonts::FreeSans9pt7b);
  text(fit(ci.title, 176), CX, 124, COL_DIM, &fonts::FreeSans9pt7b);

  frame.setFont(&fonts::Font2);
  String summary = failed ? String(failed) + " failed, " + String(passed) + " passed"
                          : String(passed) + " of " + String(n) + " passed";
  text(fit(summary, 170), CX, 150, red ? COL_ERR : 0x7EE2A0, &fonts::Font2);
  if (ci.detail.length()) {
    String d = !running ? ci.detail : active == 1 ? ci.detail + " running" : String(active) + " running: " + ci.detail;
    text(fit(d, 160), CX, 168, running ? COL_WARN : COL_ERR, &fonts::Font2);
  }

  drawCardFooter(running ? "started " : "finished ", " ago", ci.since, 0);
}

constexpr uint32_t H5_SECS = 5 * 3600, D7_SECS = 7 * 86400;  // usage window lengths

// How much of a usage window has gone by, 0-1 (-1 if unknown)
static float windowGone(uint32_t reset, uint32_t length, time_t now) {
  if (!reset || now < 1700000000 || (time_t)reset <= now || reset - now > length) return -1;
  return 1.0f - (float)(reset - now) / length;
}

// When you'd run out at the pace you've used it so far, if that's before the window resets: "out 16:40"
static String runsOut(float pct, uint32_t reset, uint32_t length, time_t now) {
  float gone = windowGone(reset, length, now);
  if (gone < 0.1f || pct <= 0 || pct >= 100 || pct / 100 <= gone) return "";  // too early to tell, or on pace
  time_t out = now + (time_t)((100 - pct) / pct * gone * length);
  if (out >= (time_t)reset) return "";
  struct tm t;
  localtime_r(&out, &t);
  char b[12];
  strftime(b, sizeof(b), out - now < 86400 ? "out %H:%M" : "out %a", &t);
  return b;
}

// A pace tick across a usage ring, at how much of the window has gone by
static void paceTick(int r0, int r1, float gone) {
  if (gone < 0) return;
  float a = (-90 + 360 * gone) * DEG_TO_RAD, c = cosf(a), s = sinf(a);
  frame.drawWedgeLine(CX + (r0 - 2) * c, CY + (r0 - 2) * s, CX + (r1 + 2) * c, CY + (r1 + 2) * s, 1.2f, 1.2f, COL_TEXT);
}

static void drawStatus(bool stale) {
  frame.fillScreen(COL_BG);
  bool live = st.valid && !stale;
  bool usage = live && st.haveUsage;

  // During an Anthropic incident, a red ring and the incident take the place of plan usage, so
  // red error dots can be read as Anthropic's problem rather than yours
  bool incident = live && st.incident.length();
  if (incident) {
    frame.fillArc(CX, CY, 106, 119, 0, 360, lerpColor(0x5A1612, COL_ERR, 0.3f + 0.5f * pulse(3000)));
    usage = false;
  } else {
    ring(108, 118, usage ? st.h5 : 0, usageColor(st.h5, COL_CLAUDE));
    ring(96, 104, usage ? st.d7 : 0, usageColor(st.d7, COL_WEEK));
  }

  time_t now = time(nullptr);
  if (now > 1700000000) {
    struct tm t;
    localtime_r(&now, &t);
    char hm[6];
    strftime(hm, sizeof(hm), "%H:%M", &t);
    text(hm, CX, 43, COL_DIM, &fonts::FreeSansBold9pt7b);
  }

  // Pace ticks: usage past its tick will run out before the window resets
  if (usage) {
    paceTick(108, 118, windowGone(st.h5Reset, H5_SECS, now));
    paceTick(96, 104, windowGone(st.d7Reset, D7_SECS, now));
  }

  if (!live) {
    if (showCat) drawCat(frame, CX - CAT_W / 2, 62, CatMood::Sleeping);
    text(st.valid ? "Mac offline" : "Waiting for Mac", CX, 110, COL_DIM, &fonts::FreeSansBold9pt7b);
    String ip = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("Wi-Fi...");
    text(ip, CX, 136, COL_FAINT, &fonts::Font2);
    text(String(HOSTNAME) + ".local", CX, 154, COL_FAINT, &fonts::Font2);
    return;
  }

  if (incident) {
    frame.setFont(&fonts::Font2);
    text(firstFit({"Anthropic incident", "Incident"}, chordWidth(62, STATUS_TEXT_R)), CX, 62, COL_ERR, &fonts::Font2);
    ticker(st.incident, 78, chordWidth(78, STATUS_TEXT_R), COL_DIM);
  }

  // Plan usage labels (5h left, weekly right). Underneath: when you'd run out at this pace, if
  // that's before the reset, otherwise the time to the reset.
  if (usage) {
    auto pct = [](float v) { return v < 0 ? String("--") : String((int)lroundf(v)) + "%"; };
    text("5h " + pct(st.h5), 88, 60, usageColor(st.h5, COL_CLAUDE), &fonts::Font2);
    text("wk " + pct(st.d7), 152, 60, usageColor(st.d7, COL_WEEK), &fonts::Font2);
    if (now > 1700000000) {
      String out5 = runsOut(st.h5, st.h5Reset, H5_SECS, now), outWk = runsOut(st.d7, st.d7Reset, D7_SECS, now);
      if (out5.length()) text(out5, 88, 76, COL_WARN, &fonts::Font2);
      else if (st.h5Reset) text(duration(st.h5Reset - now), 88, 76, COL_FAINT, &fonts::Font2);
      if (outWk.length()) text(outWk, 152, 76, COL_WARN, &fonts::Font2);
      else if (st.d7Reset) text(duration(st.d7Reset - now), 152, 76, COL_FAINT, &fonts::Font2);
    }
  }

  // Headline, with the cat acting it out beside it unless it's turned off
  String head;
  uint32_t headCol;
  CatMood mood;
  if (st.waiting > 0) {  // the alert was dismissed
    head = String(st.waiting) + " waiting";
    headCol = lerpColor(COL_CLAUDE, COL_BRIGHT, pulse(1600));
    mood = CatMood::Waving;
  } else if (st.busy > 0) {
    head = String(st.busy) + " working";
    headCol = COL_TEXT;
    mood = CatMood::Working;
  } else {
    head = st.done > 0 ? "All done" : "All idle";
    headCol = st.done > 0 ? COL_OK : COL_DIM;
    mood = st.errors > 0 ? CatMood::Dizzy : st.done > 0 ? CatMood::Cheering : CatMood::Sleeping;
  }
  if (showCat) {
    const int catGap = 4;
    frame.setFont(&fonts::FreeSansBold12pt7b);
    head = fit(head, chordWidth(104, STATUS_TEXT_R) - CAT_W - catGap);
    int headX = CX - (CAT_W + catGap + frame.textWidth(head)) / 2;
    drawCat(frame, headX, 104 - CAT_H / 2, mood, st.busy);
    text(head, headX + CAT_W + catGap, 104, headCol, &fonts::FreeSansBold12pt7b, textdatum_t::middle_left);
  } else {
    text(head, CX, 104, headCol, &fonts::FreeSansBold12pt7b);
  }

  // Up to three active sessions, each with a status dot; working ones show how long they've been at it
  int y = 130, shown = 0;
  frame.setFont(&fonts::FreeSans9pt7b);
  for (const Session &s : st.sessions) {
    if (shown == 3 || s.state == "idle") break;
    uint32_t dot = s.state == "waiting" ? COL_CLAUDE
                 : s.state == "busy"    ? lerpColor(COL_TRACK, COL_CLAUDE, pulse(1200))
                 : s.state == "error"   ? COL_ERR
                                        : COL_OK;
    String timer = s.state == "busy" && s.since && now > (time_t)s.since ? "  " + duration(now - s.since) : String("");
    int tw = frame.textWidth(timer);
    String name = fit(s.name, min(140, chordWidth(y, STATUS_TEXT_R, 9) - 12 - tw));
    int nw = frame.textWidth(name);
    int x0 = CX - (nw + 12 + tw) / 2;
    frame.fillSmoothCircle(x0 + 3, y, 3, dot);
    text(name, x0 + 12, y, s.state == "idle" ? COL_FAINT : COL_DIM, &fonts::FreeSans9pt7b, textdatum_t::middle_left);
    if (tw) text(timer, x0 + 12 + nw, y, COL_FAINT, &fonts::FreeSans9pt7b, textdatum_t::middle_left);
    y += 19;
    shown++;
  }
  if (shown == 0) {
    int total = st.idle + st.done + st.busy + st.waiting;
    text(String(total) + (total == 1 ? " session open" : " sessions open"), CX, 132, COL_FAINT, &fonts::Font2);
  }

  // Today's activity, alternating with an update notice when there's a newer release
  const int footY = 186;
  frame.setFont(&fonts::Font2);
  int footW = chordWidth(footY, STATUS_TEXT_R);
  if (st.update.length() && (millis() / 4000) % 2) {
    text(firstFit({"Update: " + st.update, st.update}, footW), CX, footY, COL_WEEK, &fonts::Font2);
  } else if (st.haveToday) {
    String p = String(st.prompts), tok = compact(st.tokens);
    text(firstFit({p + " prompts - " + tok, p + "p - " + tok + " tok", p + "p - " + tok, tok}, footW),
         CX, footY, COL_FAINT, &fonts::Font2);
  }
}

// "Label  value" centred at height y, the label dimmer, shortened to fit inside the ring
static void infoLine(const char *label, const String &value, int y) {
  frame.setFont(&fonts::Font2);
  String l = String(label) + "  ";
  int lw = frame.textWidth(l);
  String v = fit(value, chordWidth(y, CARD_TEXT_R) - lw);
  int x = CX - (lw + frame.textWidth(v)) / 2;
  text(l, x, y, COL_FAINT, &fonts::Font2, textdatum_t::middle_left);
  text(v, x + lw, y, COL_TEXT, &fonts::Font2, textdatum_t::middle_left);
}

// Five quick taps on the case: what you'd want to know when something isn't working.
// The ring counts down until it closes by itself.
static void drawDiagnostics(uint32_t shownMs) {
  frame.fillScreen(COL_BG);
  frame.fillArc(CX, CY, 112, 119, 0, 360, COL_TRACK);
  float left = 1.0f - min(1.0f, (float)shownMs / DIAGNOSTICS_MS);
  if (left > 0) frame.fillArc(CX, CY, 112, 119, -90, -90 + 360 * left, COL_WEEK);
  text("Diagnostics", CX, 40, COL_TEXT, &fonts::FreeSansBold9pt7b);

  bool online = WiFi.status() == WL_CONNECTED;
  int rssi = WiFi.RSSI();
  int32_t heard = st.valid ? (millis() - st.rxMillis) / 1000 : -1;
  infoLine("Firmware", FW_VERSION, 64);
  infoLine("Wi-Fi", online ? WiFi.SSID() : String("not connected"), 84);
  infoLine("Signal", online ? String(rssi) + " dBm, " + (rssi >= -67 ? "good" : rssi >= -80 ? "fair" : "weak") : String("-"), 104);
  infoLine("IP", online ? WiFi.localIP().toString() : String("-"), 124);
  infoLine("Name", String(HOSTNAME) + ".local", 144);
  infoLine("Mac", heard < 0 ? String("not seen yet") : heard == 0 ? String("seen just now") : "seen " + duration(heard) + " ago", 164);
  infoLine("Uptime", duration(esp_timer_get_time() / 1000000), 184);
}

static void drawWifiSetup() {
  frame.fillScreen(COL_BG);
  frame.fillArc(CX, CY, 112, 119, 0, 360, lerpColor(COL_TRACK, COL_WEEK, pulse(2400)));
  text("Wi-Fi setup", CX, 34, COL_TEXT, &fonts::FreeSansBold9pt7b);
  // Scanning this joins the setup network on most phones
  String qr = String("WIFI:T:nopass;S:") + SETUP_AP + ";;";
  frame.fillSmoothRoundRect(CX - 48, 50, 96, 96, 8, TFT_WHITE);
  frame.qrcode(qr.c_str(), CX - 44, 54, 88, 1, false);
  text("On your phone, join", CX, 160, COL_DIM, &fonts::Font2);
  text(SETUP_AP, CX, 178, COL_TEXT, &fonts::FreeSansBold9pt7b);
  text("then pick your Wi-Fi", CX, 196, COL_DIM, &fonts::Font2);
}

static void drawMessage(const char *title, const String &line, uint32_t ring) {
  frame.fillScreen(COL_BG);
  frame.fillArc(CX, CY, 112, 119, 0, 360, ring);
  text(title, CX, 108, COL_TEXT, &fonts::FreeSansBold9pt7b);
  text(line, CX, 134, COL_DIM, &fonts::Font2);
  showFrame();
}

// ---- Pairing and firmware updates over Wi-Fi ----

static bool authorized() {
  return pairKey.length() && server.header("X-Claude-Monitor-Key") == pairKey;
}

static void handleInfo() {
  JsonDocument d;
  d["version"] = FW_VERSION;
  d["board"] = BOARD_NAME;
  d["paired"] = pairKey.length() > 0;
  d["ip"] = WiFi.localIP().toString();
  String out;
  serializeJson(d, out);
  server.send(200, "application/json", out);
}

// The first Mac to pair owns the display; power on 3 times in a row to clear it.
static void handlePair() {
  String key = server.arg("plain");
  key.trim();
  if (pairKey.length()) {
    server.send(403, "text/plain", "Already paired. Power the display on 3 times in a row to reset pairing.\n");
    return;
  }
  if (key.length() < 16) {
    server.send(400, "text/plain", "Key too short\n");
    return;
  }
  pairKey = key;
  prefs.putString("key", key);
  server.send(200, "text/plain", "Paired\n");
  Serial.println("Paired with a Mac");
}

static void drawUpdateProgress(int pct) {
  frame.fillScreen(COL_BG);
  frame.fillArc(CX, CY, 112, 119, 0, 360, COL_TRACK);
  if (pct > 0) frame.fillArc(CX, CY, 112, 119, -90, -90 + 3.6f * pct, COL_WEEK);
  text("Updating", CX, 100, COL_TEXT, &fonts::FreeSansBold12pt7b);
  text(String(pct) + "%", CX, 128, COL_DIM, &fonts::FreeSansBold9pt7b);
  text("Don't unplug the display", CX, 156, COL_FAINT, &fonts::Font2);
  showFrame();
}

static bool otaAuthed = false, otaOk = false;
static size_t otaTotal = 0;
static String otaError;

#ifdef REQUIRE_OTA_BOARD_TAG
// Looks for OTA_BOARD_TAG anywhere in the incoming image, including across chunk boundaries.
static size_t otaTagMatched = 0;
static bool otaTagFound = false;
static void scanForBoardTag(const uint8_t *buf, size_t len) {
  const size_t n = sizeof(OTA_BOARD_TAG) - 1;
  for (size_t i = 0; i < len && !otaTagFound; i++) {
    char ch = (char)buf[i];
    while (otaTagMatched && ch != OTA_BOARD_TAG[otaTagMatched]) otaTagMatched = 0;  // tag has no repeated prefix
    if (ch == OTA_BOARD_TAG[otaTagMatched] && ++otaTagMatched == n) otaTagFound = true;
  }
}
#endif

// Called repeatedly while the firmware file streams in
static void handleUpdateUpload() {
  HTTPUpload &up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    otaAuthed = authorized();
    otaOk = false;
    otaError = "";
#ifdef REQUIRE_OTA_BOARD_TAG
    otaTagMatched = 0;
    otaTagFound = false;
#endif
    if (!otaAuthed) return;
    otaTotal = server.header("X-Firmware-Size").toInt();
    drawUpdateProgress(0);
    otaOk = Update.begin(otaTotal ? otaTotal : UPDATE_SIZE_UNKNOWN);
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (!otaAuthed || !otaOk) return;
#ifdef REQUIRE_OTA_BOARD_TAG
    scanForBoardTag(up.buf, up.currentSize);
#endif
    if (Update.write(up.buf, up.currentSize) != up.currentSize) otaOk = false;
    static uint32_t lastDraw = 0;
    if (otaTotal && millis() - lastDraw > 250) {
      lastDraw = millis();
      drawUpdateProgress(min(99, (int)(100.0 * up.totalSize / otaTotal)));
    }
  } else if (up.status == UPLOAD_FILE_END) {
#ifdef REQUIRE_OTA_BOARD_TAG
    if (otaAuthed && otaOk && !otaTagFound) {
      Update.abort();
      otaOk = false;
      otaError = String("this firmware isn't built for the ") + BOARD_NAME;
    }
#endif
    if (otaAuthed && otaOk) otaOk = Update.end(true);
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    otaOk = false;
  }
}

static void handleUpdateDone() {
  if (!otaAuthed) {
    server.send(403, "text/plain", "Not paired with this Mac\n");
    return;
  }
  if (!otaOk) {
    server.send(500, "text/plain",
                String("Update failed: ") + (otaError.length() ? otaError : String(Update.errorString())) + "\n");
    drawMessage("Update failed", "Still on " FW_VERSION, COL_ERR);
    delay(2000);
    return;
  }
  server.send(200, "text/plain", "Updated, restarting\n");
  drawMessage("Updated", "Restarting...", COL_OK);
  delay(800);
  ESP.restart();
}

// Identifies a session's current request or finish, so a dismissal only hides what was on screen:
// anything new still shows. stateKey() lists every session in a state.
static String sessionKey(const Session &s) { return ";" + s.name + "|" + String(s.since) + ";"; }
static String stateKey(const char *state) {
  String k;
  for (const Session &s : st.sessions)
    if (s.state == state) k += sessionKey(s);
  return k;
}

struct Pick {
  const Session *alert = nullptr, *done = nullptr;
  bool ci = false;
  int waiting = 0;
};

// Chooses the card to show. Needs-input alerts come first (the one waiting longest), except that
// the session you have open in the Claude app shows its CI checks instead of its own alert; then
// the newest finished session's green card, until you reply. Dismissed cards stay hidden until
// something new happens.
static Pick pickCard(bool stale) {
  Pick p;
  if (stale) return p;
  p.ci = st.ci.present && st.ci.key != dismissedCI;
  for (const Session &s : st.sessions) {
    if (s.state == "waiting") {
      p.waiting++;
      if (!p.alert && dismissedWaiting.indexOf(sessionKey(s)) < 0 && !(p.ci && s.id == st.ci.id)) p.alert = &s;
    } else if (s.state == "done" && !p.done && dismissedDone.indexOf(sessionKey(s)) < 0) {
      p.done = &s;  // the helper sends the newest first
    }
  }
  return p;
}

// ---- Setup / loop ----

static bool checkPowerCycleReset();

// At power-on, the cat walks across the empty screen before anything else appears
static void catWalk() {
  uint32_t start = millis();
  while (true) {
    frame.fillScreen(COL_BG);
    if (!drawCatWalk(frame, millis() - start)) break;
    frame.pushSprite(0, 0);
    delay(10);
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_BOOT, INPUT_PULLUP);

  lcd.init();
  lcd.setBrightness(160);
  lcd.fillScreen(TFT_BLACK);
  frame.setColorDepth(16);
  if (!frame.createSprite(240, 240)) {
    Serial.println("Sprite allocation failed");
    while (true) delay(1000);
  }
#if defined(BOARD_LILYGO_T_RGB) && defined(TRGB_TEST_PATTERN)
  lcd.setBrightness(255);
  lcd.testPattern(frame);
  for (int i = 0; i < 60; i++) {  // keep it up for a minute, repeating the log line for late monitors
    Serial.printf("T-RGB: touch %s, model %d\n", lcd.touchName(), lcd.model());
    delay(1000);
  }
#endif

  tapReady = tapsBegin(TAP_G);
#ifdef BOARD_LILYGO_T_RGB
  Serial.println(tapReady ? "Taps on the touch screen work" : "Couldn't start watching the touch screen");
#else
  Serial.println(tapReady ? "Motion sensor found: taps on the case work" : "No motion sensor: taps on the case don't work");
#endif

  prefs.begin("claude-status", false);
  idleBrightness = prefs.getUChar("bright", DEFAULT_BRIGHTNESS);
  nightFrom = prefs.getUShort("nightFrom", DEFAULT_NIGHT_FROM);
  nightTo = prefs.getUShort("nightTo", DEFAULT_NIGHT_TO);
#ifdef BOARD_LILYGO_T_RGB
  lcd.setWhiteBalance(prefs.getUChar("wbR", TRGB_WB_R), prefs.getUChar("wbG", TRGB_WB_G), prefs.getUChar("wbB", TRGB_WB_B));
#endif
  pairKey = prefs.getString("key", "");
  showCat = prefs.getBool("cat", true);
  String savedTz = prefs.getString("tz", DEFAULT_TZ);
  configTzTime(savedTz.c_str(), "pool.ntp.org", "time.google.com");
  applyTimezone(savedTz);

  bool wifiReset = checkPowerCycleReset();
  if (powerCycles == RESET_POWER_CYCLES - 1) {
    drawMessage("Reset Wi-Fi?", "Unplug and replug once more", COL_WARN);
    delay(2500);
  } else if (!wifiReset && showCat) {
    catWalk();
  }

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);  // Wi-Fi power saving makes the display miss pushes and mDNS queries
#ifdef WIFI_SSID
  if (!wifiManager.getWiFiIsSaved()) WiFi.begin(WIFI_SSID, WIFI_PASS);  // development builds only
#endif

  // Try the saved network; if there isn't one (or it fails) open the setup network.
  // Non-blocking, so the screen keeps updating while the portal is open.
  drawMessage("Connecting", "to Wi-Fi...", COL_TRACK);
  wifiManager.setConfigPortalBlocking(false);
  wifiManager.setConnectTimeout(20);
  wifiManager.setTitle("Claude Monitor");
  wifiManager.setShowInfoUpdate(false);
  if (!wifiManager.autoConnect(SETUP_AP)) Serial.printf("Wi-Fi setup open: join \"%s\"\n", SETUP_AP);

  server.on("/state", HTTP_POST, handleState);
  server.on("/", HTTP_GET, handleRoot);
  server.on("/screen.bmp", HTTP_GET, handleScreenshot);
  server.on("/wifi/reset", HTTP_POST, handleWifiReset);
  server.on("/brightness", handleBrightness);
  server.on("/sleep", handleSleep);
#ifdef BOARD_LILYGO_T_RGB
  server.on("/whitebalance", handleWhiteBalance);
#endif
  server.on("/info", HTTP_GET, handleInfo);
  server.on("/pair", HTTP_POST, handlePair);
  server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
  static const char *headers[] = {"X-Claude-Monitor-Key", "X-Firmware-Size"};
  server.collectHeaders(headers, 2);
}

// Power-on N times in a row (each within POWER_CYCLE_WINDOW_MS) to forget Wi-Fi,
// for cases that cover the BOOT button. Returns true if Wi-Fi was just reset.
static bool checkPowerCycleReset() {
  if (esp_reset_reason() != ESP_RST_POWERON) {
    prefs.putUChar("boots", 0);
    return false;
  }
  powerCycles = prefs.getUChar("boots", 0) + 1;
  if (powerCycles >= RESET_POWER_CYCLES) {
    prefs.putUChar("boots", 0);
    powerCycles = 0;
    drawMessage("Resetting Wi-Fi", "Opening Wi-Fi setup...", COL_ERR);
    wifiManager.resetSettings();
    prefs.remove("key");  // also forget the paired Mac, in case its key was lost
    pairKey = "";
    delay(1500);
    return true;
  }
  prefs.putUChar("boots", powerCycles);
  return false;
}

// Clears the power-on count once the display has stayed on long enough.
static void updatePowerCycleWindow() {
  if (powerCycles && millis() > POWER_CYCLE_WINDOW_MS) {
    prefs.putUChar("boots", 0);
    powerCycles = 0;
  }
}

// BOOT: a short press sets *pressed (to dismiss the card on screen); holding it forgets Wi-Fi and
// restarts into setup. Returns true while it's held long enough to show the countdown.
static bool handleBootButton(bool *pressed = nullptr) {
  static uint32_t pressedAt = 0;
  bool down = digitalRead(PIN_BOOT) == LOW;
  if (down && !pressedAt) pressedAt = millis() | 1;
  if (!down && pressedAt) {
    if (millis() - pressedAt < 1500 && pressed) *pressed = true;
    pressedAt = 0;
  }
  if (!pressedAt || millis() - pressedAt < 1500) return false;

  uint32_t held = millis() - pressedAt;
  if (held >= RESET_HOLD_MS) {
    drawMessage("Resetting Wi-Fi", "Restarting into setup...", COL_ERR);
    wifiManager.resetSettings();
    delay(1500);
    ESP.restart();
  }
  int secs = (RESET_HOLD_MS - held) / 1000 + 1;
  drawMessage("Keep holding", "to reset Wi-Fi (" + String(secs) + ")", lerpColor(COL_TRACK, COL_ERR, (float)held / RESET_HOLD_MS));
  return true;
}

void loop() {
  static bool serverStarted = false, wasConnected = false;
  bool connected = WiFi.status() == WL_CONNECTED;
  updatePowerCycleWindow();

  if (wifiManager.getConfigPortalActive()) {
    wifiManager.process();
    if (!connected) {
      if (!handleBootButton()) {
        drawWifiSetup();
        showFrame();
      }
      delay(15);
      return;
    }
    wifiManager.stopConfigPortal();  // joined a network: close the setup network
  }

  if (connected && !wasConnected) {
    Serial.println("Wi-Fi connected, IP " + WiFi.localIP().toString());
    if (!serverStarted) {  // the setup portal uses port 80 too, so start ours once it's closed
      server.begin();
      serverStarted = true;
    }
    // (Re)announce the name after every (re)connect: mDNS stops answering after a Wi-Fi drop
    // otherwise, and the Mac can't find the display any more.
    MDNS.end();
    if (MDNS.begin(HOSTNAME)) MDNS.addService("http", "tcp", 80);
    Serial.printf("mDNS: %s.local\n", HOSTNAME);
  }
  // Auto-reconnect occasionally gives up after a long outage; nudge it once a minute.
  static uint32_t offlineSince = 0;
  if (connected || !serverStarted) {
    offlineSince = 0;
  } else if (!offlineSince) {
    offlineSince = millis() | 1;
  } else if (millis() - offlineSince > 60000) {
    Serial.println("Wi-Fi still down, reconnecting");
    WiFi.reconnect();
    offlineSince = millis() | 1;
  }
  wasConnected = connected;

  if (serverStarted) server.handleClient();

  bool pressed = false;
  if (handleBootButton(&pressed)) {
    delay(15);
    return;
  }

  // Taps on the case: five quick taps open diagnostics, and a double tap (like a BOOT press)
  // closes them, or otherwise dismisses the card on screen
  static uint32_t doubles = 0, fives = 0, diagnosticsSince = 0;
  uint32_t d = doubleTapCount(), f = fiveTapCount();

  // Night hours: the screen sleeps unless Claude needs you. Any tap (or BOOT) wakes it, and the
  // gesture that woke it doesn't also dismiss a card or open diagnostics.
  static uint32_t touches = touchCount(), wokeAt = 0, swallowUntil = 0;
  uint32_t t = touchCount();
  bool poked = pressed || d != doubles || f != fives || t != touches;
  touches = t;
  bool night = nightNow();
  bool asleep = night && !(wokeAt && millis() - wokeAt < WAKE_MS);
  if (night && poked) {
    if (asleep) swallowUntil = millis() + 1000;  // a double tap is only counted once its run ends
    wokeAt = millis() | 1;
    asleep = false;
  }
  if ((int32_t)(swallowUntil - millis()) > 0) {
    pressed = false;
    doubles = d;
    fives = f;
  }

  bool dismiss = pressed || d != doubles;
  if (f != fives) {
    diagnosticsSince = millis() | 1;
  } else if (diagnosticsSince && (dismiss || millis() - diagnosticsSince > DIAGNOSTICS_MS)) {
    diagnosticsSince = 0;
    dismiss = false;  // closing diagnostics doesn't also dismiss the card underneath
  }
  doubles = d;
  fives = f;

  bool stale = st.valid && millis() - st.rxMillis > STALE_MS;
  Pick card = pickCard(stale);
  if (dismiss && !diagnosticsSince) {
    if (card.alert) dismissedWaiting = stateKey("waiting");
    else if (card.ci) dismissedCI = st.ci.key;
    else if (card.done) dismissedDone = stateKey("done");
    card = pickCard(stale);
  }

  if (diagnosticsSince) drawDiagnostics(millis() - diagnosticsSince);
  else if (card.alert) drawAlert(*card.alert, card.waiting - 1);
  else if (card.ci) drawCI(st.ci);
  else if (card.done) drawDone(*card.done, st.done - 1);
  else drawStatus(stale);
  showFrame();

  static int brightness = -1;
  int want = card.alert && !diagnosticsSince ? 255 : asleep ? 0 : idleBrightness;
  if (want != brightness) lcd.setBrightness(brightness = want);

  delay(15);
}
