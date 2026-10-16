// Glow: an optional addressable LED strip (WS2812 / SK6812) that mirrors the display's state as a
// coloured light under or around the case. Built only when GLOW_PIN is defined, e.g. with
//   pio run -e waveshare-s3-touch-lcd128-glow -t upload
// Other builds get empty stubs, so nothing changes for them.
#pragma once

#include <stdint.h>

#ifndef GLOW_WARN_PCT
#define GLOW_WARN_PCT 80  // 5-hour usage (%) from which the strip turns faint red
#endif

enum class Glow {
  Off,      // no data, a stale Mac, or the screen is dark for the night
  Idle,     // all idle: a dim warm white
  Working,  // a session is running: a soft, slow blue pulse
  Waiting,  // a session needs you: a slow amber pulse
  Done,     // the "done" card is showing: green
  Warn,     // idle or working, but the 5-hour limit is past GLOW_WARN_PCT: a faint red
};

#ifdef GLOW_PIN
void glowBegin();
void glowShow(Glow g);  // call every loop; it animates and only writes to the strip when needed
#else
inline void glowBegin() {}
inline void glowShow(Glow) {}
#endif
