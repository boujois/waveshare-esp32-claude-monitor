// Claude Code status display for the Waveshare ESP32-S3-LCD-1.28.
//
// The Mac bridge (../bridge) POSTs a JSON summary to http://claude-status.local/state.
// When a session needs input the screen shows a full-screen alert; otherwise it shows
// session status, plan usage rings (5-hour outer, weekly inner) and today's activity.
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

#include <vector>

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
  int busy = 0, idle = 0, waiting = 0, done = 0;
  bool haveUsage = false;
  float h5 = -1, d7 = -1;
  uint32_t h5Reset = 0, d7Reset = 0;
  bool haveToday = false;
  double tokens = 0;
  uint32_t prompts = 0;
  String update;  // newer release available, e.g. "v1.1.0"
  CI ci;
};

static State st;
static String dismissedWaiting, dismissedDone;  // sessions on screen when you last dismissed each card
static String dismissedCI;                      // the CI card's key when you last dismissed it
static bool tapReady = false;                   // the motion sensor was found, so taps on the case work
static WiFiManager wifiManager;
static Preferences prefs;
static String currentTz;
static uint8_t powerCycles = 0;  // consecutive quick power-ons, for the no-button Wi-Fi reset
static String pairKey;           // shared with the Mac helper; required for firmware updates

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
  st = std::move(s);

  // The Mac helper sends its timezone as a POSIX TZ string; remember it across reboots
  String tz = (const char *)(doc["tz"] | "");
  if (tz.length() && tz != currentTz) {
    applyTimezone(tz);
    prefs.putString("tz", tz);
    Serial.println("Timezone set to " + tz);
  }
  server.send(200, "application/json", "{\"ok\":true}");
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

  ring(108, 118, usage ? st.h5 : 0, usageColor(st.h5, COL_CLAUDE));
  ring(96, 104, usage ? st.d7 : 0, usageColor(st.d7, COL_WEEK));

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
    text(st.valid ? "Mac offline" : "Waiting for Mac", CX, 110, COL_DIM, &fonts::FreeSansBold9pt7b);
    String ip = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("Wi-Fi...");
    text(ip, CX, 136, COL_FAINT, &fonts::Font2);
    text(String(HOSTNAME) + ".local", CX, 154, COL_FAINT, &fonts::Font2);
    return;
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

  // Headline
  String head;
  uint32_t headCol;
  if (st.waiting > 0) {  // the alert was dismissed
    head = String(st.waiting) + " waiting";
    headCol = lerpColor(COL_CLAUDE, COL_BRIGHT, pulse(1600));
  } else if (st.busy > 0) {
    head = String(st.busy) + " working";
    headCol = COL_TEXT;
  } else {
    head = st.done > 0 ? "All done" : "All idle";
    headCol = st.done > 0 ? COL_OK : COL_DIM;
  }
  text(head, CX, 104, headCol, &fonts::FreeSansBold12pt7b);

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
  frame.pushSprite(0, 0);
}

// ---- Pairing and firmware updates over Wi-Fi ----

static bool authorized() {
  return pairKey.length() && server.header("X-Claude-Monitor-Key") == pairKey;
}

static void handleInfo() {
  JsonDocument d;
  d["version"] = FW_VERSION;
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
  frame.pushSprite(0, 0);
}

static bool otaAuthed = false, otaOk = false;
static size_t otaTotal = 0;

// Called repeatedly while the firmware file streams in
static void handleUpdateUpload() {
  HTTPUpload &up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    otaAuthed = authorized();
    otaOk = false;
    if (!otaAuthed) return;
    otaTotal = server.header("X-Firmware-Size").toInt();
    drawUpdateProgress(0);
    otaOk = Update.begin(otaTotal ? otaTotal : UPDATE_SIZE_UNKNOWN);
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (!otaAuthed || !otaOk) return;
    if (Update.write(up.buf, up.currentSize) != up.currentSize) otaOk = false;
    static uint32_t lastDraw = 0;
    if (otaTotal && millis() - lastDraw > 250) {
      lastDraw = millis();
      drawUpdateProgress(min(99, (int)(100.0 * up.totalSize / otaTotal)));
    }
  } else if (up.status == UPLOAD_FILE_END) {
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
    server.send(500, "text/plain", String("Update failed: ") + Update.errorString() + "\n");
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

  tapReady = tapsBegin(TAP_G);
  Serial.println(tapReady ? "Motion sensor found: taps on the case work" : "No motion sensor: taps on the case don't work");

  prefs.begin("claude-status", false);
  pairKey = prefs.getString("key", "");
  String savedTz = prefs.getString("tz", DEFAULT_TZ);
  configTzTime(savedTz.c_str(), "pool.ntp.org", "time.google.com");
  applyTimezone(savedTz);

  checkPowerCycleReset();
  if (powerCycles == RESET_POWER_CYCLES - 1) {
    drawMessage("Reset Wi-Fi?", "Unplug and replug once more", COL_WARN);
    delay(2500);
  }

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setAutoReconnect(true);
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
        frame.pushSprite(0, 0);
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
      if (MDNS.begin(HOSTNAME)) MDNS.addService("http", "tcp", 80);
      Serial.printf("mDNS: %s.local\n", HOSTNAME);
      serverStarted = true;
    }
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
  frame.pushSprite(0, 0);

  static int brightness = -1;
  int want = card.alert && !diagnosticsSince ? 255 : 150;
  if (want != brightness) lcd.setBrightness(brightness = want);

  delay(15);
}
