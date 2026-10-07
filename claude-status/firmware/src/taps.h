// Taps on the display's case, detected with the board's QMI8658 motion sensor.
#pragma once

#include <stdint.h>

// Starts watching for taps; each tap must be at least tapG (in g) to count.
// Returns false if the board has no motion sensor, so tap gestures are unavailable.
bool tapsBegin(float tapG);

// How often each gesture has happened so far: compare with earlier counts to spot new ones.
uint32_t doubleTapCount();  // exactly two quick taps
uint32_t fiveTapCount();    // five quick taps (more in the same run don't count again)
