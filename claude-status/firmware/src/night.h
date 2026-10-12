// Night mode: between two times of day the screen goes dark, or very dim, unless something needs you.
#pragma once

#include <stdint.h>
#include <time.h>

struct NightMode {
  bool enabled = false;
  uint16_t start = 22 * 60, end = 7 * 60;  // minutes since local midnight
  bool dim = false;                        // very dim instead of off
  bool alerts = true;                      // needs-input alerts still light the screen
};

// Backlight levels, 0-255
constexpr int BRIGHTNESS = 150;              // normal
constexpr int ALERT_BRIGHTNESS = 255;        // a needs-input alert by day
constexpr int NIGHT_ALERT_BRIGHTNESS = 150;  // ...and at night
constexpr int NIGHT_PEEK_BRIGHTNESS = 80;    // a tap on the dark screen lights it this much
constexpr uint32_t NIGHT_PEEK_MS = 15000;    // ...for this long
constexpr int NIGHT_DIM_BRIGHTNESS = 10;     // "dim" rather than off

// A double tap is recognised up to this long after it (taps.cpp), so one recognised within this
// long of the screen going dark or lit is judged by how it looked before
constexpr uint32_t TAP_DELAY_MS = 600;

// Whether minute (0-1439) is in [start, end). The window wraps past midnight when start > end, and
// is empty when start == end.
bool nightWindow(int minute, int start, int end);

// Whether it's night at now: night mode is on, the clock has been set and the local time is in the window.
bool nightActive(const NightMode &n, time_t now);

// Whether the screen is dark (off or dimmed) for the night, so a tap should only light it up.
// diagnostics: the five-tap screen is open; peek: a peek is running; alert: a needs-input alert is on screen.
bool nightDark(const NightMode &n, bool night, bool diagnostics, bool peek, bool alert);

// The backlight to use, by day or night.
int backlight(const NightMode &n, bool night, bool diagnostics, bool peek, bool alert);
