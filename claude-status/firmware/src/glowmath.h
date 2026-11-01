// Dithering for the glow strip. At a low brightness cap each colour channel has only a handful of
// levels, so a fade steps visibly. Each LED rounds a fractional level up or down against its own
// fixed threshold, so the strip as a whole moves in steps GLOW_LEDS times finer, one LED at a time,
// with no flicker. Hardware-free, so it's unit tested on the Mac (pio test -e native).
#pragma once

#include <math.h>
#include <stdint.h>

inline uint16_t glowGcd(uint16_t a, uint16_t b) {
  while (b) {
    uint16_t t = a % b;
    a = b;
    b = t;
  }
  return a;
}

// The fraction (0-1) above which LED led of leds rounds up. The thresholds are (k + 0.5) / leds for
// k = 0 to leds - 1, so the strip's average is never more than half an LED's step out. They're dealt
// round the strip with a stride near leds * 0.618 that shares no factor with leds, so the LEDs that
// round up together are spread out rather than side by side.
inline float glowThreshold(uint16_t led, uint16_t leds) {
  if (leds < 2) return 0.5f;
  uint16_t stride = leds * 0.618034f;
  while (glowGcd(stride, leds) != 1) stride++;
  return ((uint32_t)led * stride % leds + 0.5f) / leds;
}

// level (0-255, may be fractional) as LED led of leds shows it
inline uint8_t glowDither(float level, uint16_t led, uint16_t leds) {
  if (!(level > 0)) return 0;  // also catches NaN
  if (level >= 255) return 255;
  float whole = floorf(level);
  return (uint8_t)whole + (level - whole > glowThreshold(led, leds) ? 1 : 0);
}
