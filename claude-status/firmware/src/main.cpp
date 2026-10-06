// Claude Code status display for the Waveshare ESP32-S3-LCD-1.28.
//
// The Mac bridge (../bridge) POSTs a JSON summary to http://claude-status.local/state.
// When a session needs input the screen shows a full-screen alert; otherwise it shows
// session status, plan usage rings (5-hour outer, weekly inner) and today's activity.
// The BOOT button (if your case leaves it reachable) dismisses the current alert;
// holding it for 5 seconds forgets the Wi-Fi network and reopens Wi-Fi setup.
// Without the button: power the display on 3 times in a row (each within 10 s of
// the last), or POST http://claude-status.local/wifi/reset from the Mac.
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
#include <time.h>

#include <vector>

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
  String name, state, kind, detail;
  uint32_t since = 0;
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
};

static State st;
static String dismissedKey;
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
               (st.valid ? String((millis() - st.rxMillis) / 1000) + "s ago" : String("never")) + "\n";
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

  time_t now = time(nullptr);
  String foot = s.since && now > 1700000000 ? "waiting " + duration(now - s.since) : String("");
  if (others > 0) foot += (foot.length() ? "  +" : "+") + String(others) + " more";
  text(foot, CX, 192, COL_FAINT, &fonts::Font2);
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

  if (!live) {
    text(st.valid ? "Mac offline" : "Waiting for Mac", CX, 110, COL_DIM, &fonts::FreeSansBold9pt7b);
    String ip = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("Wi-Fi...");
    text(ip, CX, 136, COL_FAINT, &fonts::Font2);
    text(String(HOSTNAME) + ".local", CX, 154, COL_FAINT, &fonts::Font2);
    return;
  }

  // Plan usage labels (5h left, weekly right) with time to reset underneath
  if (usage) {
    auto pct = [](float v) { return v < 0 ? String("--") : String((int)lroundf(v)) + "%"; };
    text("5h " + pct(st.h5), 88, 60, usageColor(st.h5, COL_CLAUDE), &fonts::Font2);
    text("wk " + pct(st.d7), 152, 60, usageColor(st.d7, COL_WEEK), &fonts::Font2);
    if (now > 1700000000) {
      if (st.h5Reset) text(duration(st.h5Reset - now), 88, 76, COL_FAINT, &fonts::Font2);
      if (st.d7Reset) text(duration(st.d7Reset - now), 152, 76, COL_FAINT, &fonts::Font2);
    }
  }

  // Headline
  String head;
  uint32_t headCol;
  if (st.waiting > 0) {  // alert was dismissed with the button
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

  // Up to three active sessions, each with a status dot
  int y = 130, shown = 0;
  frame.setFont(&fonts::FreeSans9pt7b);
  for (const Session &s : st.sessions) {
    if (shown == 3 || s.state == "idle") break;
    uint32_t dot = s.state == "waiting" ? COL_CLAUDE
                 : s.state == "busy"    ? lerpColor(COL_TRACK, COL_CLAUDE, pulse(1200))
                 : s.state == "error"   ? COL_ERR
                                        : COL_OK;
    String name = fit(s.name, 140);
    int w = frame.textWidth(name) + 12;
    int x0 = CX - w / 2;
    frame.fillSmoothCircle(x0 + 3, y, 3, dot);
    text(name, x0 + 12, y, s.state == "idle" ? COL_FAINT : COL_DIM, &fonts::FreeSans9pt7b, textdatum_t::middle_left);
    y += 19;
    shown++;
  }
  if (shown == 0) {
    int total = st.idle + st.done + st.busy + st.waiting;
    text(String(total) + (total == 1 ? " session open" : " sessions open"), CX, 132, COL_FAINT, &fonts::Font2);
  }

  // Today's activity, alternating with an update notice when there's a newer release
  if (st.update.length() && (millis() / 4000) % 2) {
    text("Update: " + st.update, CX, 190, COL_WEEK, &fonts::Font2);
  } else if (st.haveToday) {
    frame.setFont(&fonts::Font2);
    String today = String(st.prompts) + " prompts - " + compact(st.tokens);
    if (frame.textWidth(today) > 128) today = String(st.prompts) + "p - " + compact(st.tokens) + " tok";
    text(today, CX, 190, COL_FAINT, &fonts::Font2);
  }
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

// Identifies the current set of waiting sessions, so a dismissal only lasts until something new happens.
static String waitingKey() {
  String k;
  for (const Session &s : st.sessions)
    if (s.state == "waiting") k += s.name + "|" + String(s.since) + ";";
  return k;
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

// BOOT: a short press dismisses the alert; holding it forgets Wi-Fi and restarts into setup.
static bool handleBootButton(bool alertShowing) {
  static uint32_t pressedAt = 0;
  bool down = digitalRead(PIN_BOOT) == LOW;
  if (down && !pressedAt) pressedAt = millis() | 1;
  if (!down && pressedAt) {
    if (millis() - pressedAt < 1500 && alertShowing) dismissedKey = waitingKey();
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
      if (!handleBootButton(false)) {
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

  bool stale = st.valid && millis() - st.rxMillis > STALE_MS;
  const Session *first = nullptr;
  int waiting = 0;
  if (!stale)
    for (const Session &s : st.sessions)
      if (s.state == "waiting" && waiting++ == 0) first = &s;

  if (handleBootButton(first != nullptr)) {
    delay(15);
    return;
  }

  bool alert = first && waitingKey() != dismissedKey;
  if (alert) drawAlert(*first, waiting - 1);
  else drawStatus(stale);
  frame.pushSprite(0, 0);

  static int brightness = -1;
  int want = alert ? 255 : 150;
  if (want != brightness) lcd.setBrightness(brightness = want);

  delay(15);
}
