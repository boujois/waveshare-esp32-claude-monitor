// Night mode: between two times of day the screen goes dark, or very dim, unless something needs you.
//
// The times come from the Mac helper (night_start / night_end in its config.json) and are
// remembered, so night mode keeps working while the Mac is asleep. Until the clock has been set,
// from the network or the Mac's pushes, it's always day. At night a needs-input alert still lights
// the screen (unless that's turned off), a double tap or BOOT press on the dark screen lights it
// for a peek, and the five-tap diagnostics show at normal brightness.
//
// Nothing here touches the hardware, so it can be tested on a computer.

#include "night.h"

bool nightWindow(int minute, int start, int end) {
  if (start < end) return minute >= start && minute < end;
  if (start > end) return minute >= start || minute < end;  // past midnight
  return false;
}

bool nightActive(const NightMode &n, time_t now) {
  if (!n.enabled || now <= 1600000000) return false;
  struct tm t;
  localtime_r(&now, &t);
  return nightWindow(t.tm_hour * 60 + t.tm_min, n.start, n.end);
}

bool nightDark(const NightMode &n, bool night, bool diagnostics, bool peek, bool alert) {
  return night && !diagnostics && !peek && !(alert && n.alerts);
}

int backlight(const NightMode &n, bool night, bool diagnostics, bool peek, bool alert) {
  if (!night) return alert && !diagnostics ? ALERT_BRIGHTNESS : BRIGHTNESS;
  if (diagnostics) return BRIGHTNESS;
  if (alert && n.alerts) return NIGHT_ALERT_BRIGHTNESS;  // an alert arriving during a peek lights up fully
  if (peek) return NIGHT_PEEK_BRIGHTNESS;
  return n.dim ? NIGHT_DIM_BRIGHTNESS : 0;
}
