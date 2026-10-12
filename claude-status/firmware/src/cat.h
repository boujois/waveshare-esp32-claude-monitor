// The cat on the status screen: a brown tabby whose mood follows your sessions.
#pragma once

#include <LovyanGFX.hpp>

enum class CatMood {
  Sleeping,  // nothing's happening, or the Mac is offline
  Working,   // sessions are busy: it types, faster the more there are
  Waving,    // a session is waiting for you
  Cheering,  // sessions have finished
  Dizzy,     // a session's last turn ended with an API error
};

constexpr int CAT_W = 40, CAT_H = 32;  // size on screen, in pixels

// Draws the cat with its top-left corner at (x, y). busy is how many sessions are working.
void drawCat(LGFX_Sprite &gfx, int x, int y, CatMood mood, int busy = 0);

// Draws the cat walking across the screen from left to right, ms into its walk. Returns false
// once it has walked off the other side.
bool drawCatWalk(LGFX_Sprite &gfx, uint32_t ms);
