// The cat on the status screen: a brown tabby whose mood follows your sessions.
//
// Each pose is a 20 x 16 frame of pixel art, drawn at 2x with one character per pixel (see
// paletteColor). The animations switch between poses and add z's, stars and confetti on top.
// At power-on, a bigger side view of the cat walks across the screen.

#include "cat.h"

#include <Arduino.h>

namespace {

constexpr int H = 16, S = 2;          // frame height, in pixels; each one is drawn S x S
constexpr uint32_t BG = 0x0B0F1A;     // the screen's background, for fading out

uint32_t paletteColor(char c) {
  switch (c) {
    case 'B': return 0x8F7B64;  // greyish-brown fur
    case 'K': return 0x241C17;  // black stripes
    case 'D': return 0x5C4D3F;  // shading
    case 'W': return 0xF3F0EA;  // white muzzle, chest and paws
    case 'V': return 0xA9A49C;  // white paws on the far side
    case 'T': return 0xBF9A6A;  // gold around the eyes
    case 'E': return 0xA8B556;  // green eyes
    case 'P': return 0xEBA2AA;  // pink nose and ears
    case 'G': return 0xAEB6C6;  // keyboard keys
    case 'H': return 0x646D80;  // keyboard
    case 'O': return 0xD97757;  // the key being pressed
    case 'Y': return 0xFFD60A;  // stars
    default: return 0;
  }
}

const char *const SIT[] = {
  ".....B........B.....",
  ".....BB......BB.....",
  ".....BPBBKKBBPB.....",
  "....BBBKBKKBKBBB....",
  "....BBBTTBBTTBBB....",
  "....BKEKEBBEKEKB....",
  "....BBEKEWWEKEBB....",
  "....BKBWWPPWWBKB....",
  "....BBKWWWWWWKBB....",
  ".....BBBWWWWBBB.....",
  ".....BKBWWWWBKB..BK.",
  "....BBKBWWWWBKBB.BB.",
  "....BKBBWWWWBBKB.KB.",
  "....BBKBWWWWBKBB.BB.",
  "....BBKWWBBWWKBBBKB.",
  "....BBBWWBBWWBBBBB..",
};

// Typing: the paws take turns on the keys
const char *const TYPE_A[] = {
  ".....B........B.....",
  ".....BB......BB.....",
  ".....BPBBKKBBPB.....",
  "....BBBKBKKBKBBB....",
  "....BBBTTBBTTBBB....",
  "....BKDDDBBDDDKB....",
  "....BBEKEWWEKEBB....",
  "....BKBWWPPWWBKB....",
  "....BBKWWWWWWKBB....",
  ".....BBBWWWWBBB.....",
  "....BKWWBWWBBBKB....",
  "....BBWWKWWKBBBB....",
  "....BKBBBBBBWWKB....",
  "..HHHHHHHHHHWWHHHH..",
  "..HGHGHGHGHGOOHGHH..",
  "..HHHHHHHHHHHHHHHH..",
};

const char *const TYPE_B[] = {
  ".....B........B.....",
  ".....BB......BB.....",
  ".....BPBBKKBBPB.....",
  "....BBBKBKKBKBBB....",
  "....BBBTTBBTTBBB....",
  "....BKDDDBBDDDKB....",
  "....BBEKEWWEKEBB....",
  "....BKBWWPPWWBKB....",
  "....BBKWWWWWWKBB....",
  ".....BBBWWWWBBB.....",
  "....BKBBBWWBWWKB....",
  "....BBBBKWWKWWBB....",
  "....BKWWBBBBBBKB....",
  "..HHHHWWHHHHHHHHHH..",
  "..HGHGOOHGHGHGHGHH..",
  "..HHHHHHHHHHHHHHHH..",
};

// Curled up asleep, breathing slowly
const char *const SLEEP_A[] = {
  "....................",
  "....................",
  "....................",
  "....................",
  "....................",
  "....................",
  ".....B........B.....",
  ".....BB......BB.....",
  ".....BPBBKKBBPB.....",
  "...DBBBKBKKBKBBBD...",
  "..BDBBBTTBBTTBBBDB..",
  "..KDBBKKKWWKKKBBDK..",
  "..BDBKBWWPPWWBKBDB..",
  "..KBDBKWWWWWWKBDBKB.",
  "..BKBWWDWWWWDWWBKBKB",
  "...BBBBBBBBBBBBBBBB.",
};

const char *const SLEEP_B[] = {
  "....................",
  "....................",
  "....................",
  "....................",
  "....................",
  "....................",
  ".....B........B.....",
  ".....BB......BB.....",
  "...B.BPBBKKBBPB.B...",
  "..BDBBBKBKKBKBBBDB..",
  ".BBDBBBTTBBTTBBBDBB.",
  ".BKDBBKKKWWKKKBBDKB.",
  ".BBDBKBWWPPWWBKBDBB.",
  "..KBDBKWWWWWWKBDBKB.",
  "..BKBWWDWWWWDWWBKBKB",
  "...BBBBBBBBBBBBBBBB.",
};

// Waving a paw to get your attention
const char *const WAVE_A[] = {
  ".....B........B.....",
  ".....BB......BB.....",
  ".....BPBBKKBBPB.....",
  "....BBBKBKKBKBBB....",
  "....BBBTTBBTTBBB.WW.",
  "....BKEKEBBEKEKB.WW.",
  "....BBEKEWWEKEBB.BB.",
  "....BKBWWPPWWBKBBK..",
  "....BBKWWKKWWKBBB...",
  ".....BBBWWWWBBBB....",
  ".....BKBWWWWBKB.....",
  "....BBKBWWWWBKBB....",
  "....BKBBWWWWBBKB.KB.",
  "....BBKBWWWWBKBB.BB.",
  "....BBKWWBBWWKBBBKB.",
  "....BBBWWBBWWBBBBB..",
};

const char *const WAVE_B[] = {
  ".....B........B.....",
  ".....BB......BB.....",
  ".....BPBBKKBBPB.....",
  "....BBBKBKKBKBBB.WW.",
  "....BBBTTBBTTBBBWW..",
  "....BKEKEBBEKEKBBB..",
  "....BBEKEWWEKEBBK...",
  "....BKBWWPPWWBKBB...",
  "....BBKWWWWWWKBBB...",
  ".....BBBWWWWBBBB....",
  ".....BKBWWWWBKB.....",
  "....BBKBWWWWBKBB....",
  "....BKBBWWWWBBKB.KB.",
  "....BBKBWWWWBKBB.BB.",
  "....BBKWWBBWWKBBBKB.",
  "....BBBWWBBWWBBBBB..",
};

// Paws in the air
const char *const CHEER[] = {
  "....................",
  "..WW............WW..",
  "..WW.B........B.WW..",
  "..BB.BB......BB.BB..",
  "..BB.BPBBKKBBPB.BB..",
  "...DBBBKBKKBKBBBD...",
  "...DBBBBBBBBBBBBD...",
  "....BBBKBBBBKBBB....",
  "....BBKBKWWKBKBB....",
  "....BKBWWPPWWBKB....",
  "....BBKWWKKWWKBB....",
  ".....BBBWWWWBBB.....",
  "....BBKBWWWWBKBB....",
  "....BKBBWWWWBBKB....",
  "....BBKWWBBWWKBB....",
  "....BBBWWBBWWBBB....",
};

// Seeing stars
const char *const DIZZY[] = {
  "....................",
  "....................",
  ".....B........B.....",
  ".....BB......BB.....",
  ".....BPBBKKBBPB.....",
  "....BBBBBKKBBBBB....",
  "....BBKBKBBKBKBB....",
  "....BBBKBBBBKBBB....",
  "....BBKBKWWKBKBB....",
  "....BKBWWPPWWBKB....",
  "....BBKWWKKWWKBB....",
  ".....BBBWWWWBBB.....",
  "....BBKBWWWWBKBB....",
  "....BKBBWWWWBBKB....",
  "....BBKWWBBWWKBB....",
  "....BBBWWBBWWBBB....",
};

// Walking across the screen at power-on, seen from the side and drawn bigger. The body is
// shared; the legs take turns between these poses.
constexpr int WALK_W = 24;

const char *const WALK_BODY[] = {
  "..KK...........B...B....",
  ".BBK...........BB.BBB...",
  ".BB...........BBBBBPB...",
  ".KK...........BBKBKBBB..",
  ".BB..........BBBBBBBBBB.",
  "..BBBBBBBBBBBBBKBTEKBBB.",
  "...BBBKBBKBBKBBBKTEKBBB.",
  "...BBBKBBKBBKBBBBBBBBWWP",
  "...BBBKBBKBBKBBBBBBWWWWW",
  "...DBBBBBBBBBBBBBWWWWWW.",
  "....DBBBBBBBBBBWWWWW....",
  ".....BBBBBBBBBBBWWW.....",
};

const char *const WALK_STRIDE[] = {
  "....BB..DD...DD..WW.....",
  "...BB...DD...DD...WW....",
  "...BB....DD.DD.....WW...",
  "..WW.....VV.VV.....WW...",
};

const char *const WALK_PASS[] = {
  "....BBDD......DDWW......",
  "....BBDD......DDWW......",
  "....BBDD......DDWW......",
  "....WWVV......VVWW......",
};

const char *const WALK_STRIDE2[] = {
  "....DD..BB...WW..DD.....",
  "...DD...BB...WW...DD....",
  "...DD....BB.WW.....DD...",
  "..VV.....WW.WW.....VV...",
};

const char *const ZEE[] = {"####", "..#.", ".#..", "####"};
const char *const STAR[] = {".Y.", "YWY", ".Y."};
const char *const DOT[] = {"#"};

uint32_t mix(uint32_t a, uint32_t b, float t) {
  auto ch = [&](int shift) {
    int x = (a >> shift) & 0xFF, y = (b >> shift) & 0xFF;
    return (uint32_t)(x + (y - x) * t) & 0xFF;
  };
  return ch(16) << 16 | ch(8) << 8 | ch(0);
}

// Draws rows of pixel art with their top-left pixel at column c, row r of the cat's frame, which
// starts at (x, y) on screen. '#' pixels take the colour given; the rest come from the palette.
void art(LGFX_Sprite &g, int x, int y, float c, float r, const char *const *rows, int h, uint32_t col = 0,
         int scale = S) {
  int c0 = lroundf(c), r0 = lroundf(r);
  for (int i = 0; i < h; i++)
    for (int j = 0; rows[i][j]; j++) {
      char p = rows[i][j];
      if (p != '.') g.fillRect(x + (c0 + j) * scale, y + (r0 + i) * scale, scale, scale, p == '#' ? col : paletteColor(p));
    }
}

void pose(LGFX_Sprite &g, int x, int y, const char *const *frame) { art(g, x, y, 0, 0, frame, H); }

}  // namespace

void drawCat(LGFX_Sprite &g, int x, int y, CatMood mood, int busy) {
  uint32_t t = millis();
  switch (mood) {
    case CatMood::Sleeping: {  // breathing slowly, with a z floating up from its head
      pose(g, x, y, (t / 1600) % 2 ? SLEEP_B : SLEEP_A);
      float p = (t % 2800) / 2800.0f;
      float fade = p < 0.25f ? p / 0.25f : p > 0.7f ? (1 - p) / 0.3f : 1;
      art(g, x, y, 14 + 2 * p, 3 - 4 * p, ZEE, 4, mix(BG, 0xA6AEBF, fade));
      break;
    }

    case CatMood::Working:
      pose(g, x, y, (t / max(130, 400 - 100 * busy)) % 2 ? TYPE_B : TYPE_A);
      break;

    case CatMood::Waving: {  // waves for a bit, then waits
      uint32_t p = t % 3000;
      pose(g, x, y, p >= 1600 ? SIT : (p / 200) % 2 ? WAVE_B : WAVE_A);
      break;
    }

    case CatMood::Cheering: {  // hops now and then, with confetti falling around it
      uint32_t p = t % 1800;
      int lift = p < 500 ? lroundf(2 * sinf(p / 500.0f * PI)) * S : 0;
      pose(g, x, y - lift, CHEER);
      static const uint32_t colors[] = {0x34C759, 0xD97757, 0x6A9BCC, 0xFFD60A};
      static const float cols[] = {1, 18, 3, 16};
      for (int i = 0; i < 4; i++) {
        float q = fmodf(t / (1900.0f + 350 * i) + i * 0.3f, 1);
        art(g, x, y, cols[i] + sinf(q * 4 * PI), q * (H - 1), DOT, 1, colors[i]);
      }
      break;
    }

    case CatMood::Dizzy: {  // two stars circle its head; the one going round the back is behind it
      float a = t / 1500.0f * TWO_PI;
      int sway = lroundf(sinf(t / 1100.0f * TWO_PI)) * S;
      for (int front = 0; front < 2; front++) {
        if (front) pose(g, x + sway, y, DIZZY);
        for (int k = 0; k < 2; k++) {
          float b = a + k * PI;
          if ((sinf(b) > 0) == (bool)front) art(g, x + sway, y, 8.5f + 7 * cosf(b), 0.5f + sinf(b), STAR, 3);
        }
      }
      break;
    }
  }
}

bool drawCatWalk(LGFX_Sprite &g, uint32_t ms) {
  constexpr int scale = 4, w = WALK_W * scale, h = H * scale;
  int x = -w + (int)(ms * 0.09f);  // about 3.7 s to cross the screen
  if (x >= g.width()) return false;
  static const char *const *const legs[] = {WALK_STRIDE, WALK_PASS, WALK_STRIDE2, WALK_PASS};
  int step = (ms / 100) % 4;
  int y = (g.height() - h) / 2 - (step % 2) * 2;  // a little bob as the legs pass each other
  art(g, x, y, 0, 0, WALK_BODY, 12, 0, scale);
  art(g, x, y, 0, 12, legs[step], 4, 0, scale);
  return true;
}
