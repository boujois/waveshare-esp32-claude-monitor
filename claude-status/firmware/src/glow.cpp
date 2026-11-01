#ifdef GLOW_PIN

#include "glow.h"

#include <Adafruit_NeoPixel.h>
#include <Arduino.h>

#ifndef GLOW_LEDS
#define GLOW_LEDS 6  // LEDs on the strip (60/m: about 10 cm)
#endif
#ifndef GLOW_MAX
#define GLOW_MAX 60  // brightness cap, 0-255: keeps the strip's draw well within USB power
#endif
#ifdef GLOW_RGB
#define GLOW_TYPE (NEO_GRB + NEO_KHZ800)  // RGB strip
#else
#define GLOW_TYPE (NEO_GRBW + NEO_KHZ800)  // RGBW strip (default)
#endif

static Adafruit_NeoPixel strip(GLOW_LEDS, GLOW_PIN, GLOW_TYPE);

struct Rgbw {
  uint8_t r, g, b, w;
};

// Colours at full level; GLOW_MAX scales them all
static Rgbw colour(Glow g) {
  switch (g) {
    case Glow::Idle:    return {0, 0, 0, 90};       // dim warm white (white channel)
    case Glow::Working: return {0, 60, 255, 0};     // soft blue
    case Glow::Waiting: return {255, 110, 0, 0};    // amber
    case Glow::Done:    return {37, 194, 31, 0};     // green
    case Glow::Warn:    return {255, 0, 0, 20};     // faint red
    default:            return {0, 0, 0, 0};
  }
}

void glowBegin() {
  strip.begin();
  strip.setBrightness(GLOW_MAX);
  strip.clear();
  strip.show();
}

void glowShow(Glow g) {
  static Glow shown = Glow::Off;
  static uint32_t lastWrite = 0;
  static bool first = true;
  uint32_t now = millis();
  bool animated = g == Glow::Waiting || g == Glow::Working;
  if (!first && g == shown && (!animated || now - lastWrite < 30)) return;
  first = false;
  shown = g;
  lastWrite = now;

  Rgbw c = colour(g);
  float k = 1.0f;
  if (g == Glow::Waiting) k = 0.15f + 0.85f * (0.5f + 0.5f * sinf(now * TWO_PI / 2400.0f));  // 2.4 s pulse: catches the eye
  if (g == Glow::Working) k = 0.45f + 0.55f * (0.5f + 0.5f * sinf(now * TWO_PI / 4000.0f));  // 4 s soft pulse: calmer
  uint32_t px = strip.Color(c.r * k, c.g * k, c.b * k, c.w * k);
  strip.fill(px);
  strip.show();
}

#endif  // GLOW_PIN
