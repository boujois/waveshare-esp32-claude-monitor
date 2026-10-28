// Taps on the display's case, detected with the board's QMI8658 motion sensor.
#pragma once

#include <stdint.h>

// Starts watching for taps; each tap must be at least tapG (in g) to count.
// Returns false if the board has no motion sensor, so tap gestures are unavailable.
bool tapsBegin(float tapG);

// How often each gesture has happened so far: compare with earlier counts to spot new ones.
uint32_t singleTapCount();  // a single tap on its own (touch screens only: 0 with the motion sensor)
uint32_t doubleTapCount();  // exactly two quick taps
uint32_t fiveTapCount();    // five quick taps (more in the same run don't count again)

// Swipes across the touch screen (the Waveshare touch board only: 0 elsewhere), by the direction
// the finger moved, and the last gesture code the touch controller reported (for diagnostics).
uint32_t swipeLeftCount();
uint32_t swipeRightCount();
uint8_t lastTouchGesture();

// Where the last single tap landed, in 240x240 screen pixels; false where taps aren't on the screen.
bool lastTapPoint(int *x, int *y);
