// Tests for the glow strip's dithering (src/glowmath.h):  pio test -e native
#include <math.h>
#include <unity.h>

#include "glowmath.h"

constexpr uint16_t LEDS = 24;

static float stripAverage(float level) {
  float sum = 0;
  for (uint16_t i = 0; i < LEDS; i++) sum += glowDither(level, i, LEDS);
  return sum / LEDS;
}

void setUp() {}
void tearDown() {}

// A whole level needs no rounding: every LED shows it exactly
static void test_whole_levels_are_exact() {
  for (int level = 0; level <= 255; level++)
    for (uint16_t i = 0; i < LEDS; i++) TEST_ASSERT_EQUAL_UINT8(level, glowDither(level, i, LEDS));
}

// The strip as a whole shows a fractional level, to within half an LED's step
static void test_strip_average_matches_fraction() {
  for (float level = 0; level <= 10; level += 0.01f)
    TEST_ASSERT_FLOAT_WITHIN(0.5f / LEDS + 1e-3f, level, stripAverage(level));
}

// A single LED has nothing to spread over, so it rounds to the nearest level
static void test_single_led_rounds_to_nearest() {
  TEST_ASSERT_EQUAL_UINT8(3, glowDither(3.4f, 0, 1));
  TEST_ASSERT_EQUAL_UINT8(4, glowDither(3.6f, 0, 1));
}

// Every LED rounds to the level just below or just above, never further
static void test_each_led_rounds_to_a_neighbour() {
  for (float level = 0.05f; level < 10; level += 0.1f)
    for (uint16_t i = 0; i < LEDS; i++) {
      uint8_t v = glowDither(level, i, LEDS);
      TEST_ASSERT_TRUE(v == (uint8_t)floorf(level) || v == (uint8_t)floorf(level) + 1);
    }
}

// As a fade rises, no LED ever gets dimmer, and only one LED steps at a time
static void test_rising_fade_steps_one_led_at_a_time() {
  uint8_t prev[LEDS] = {};
  for (int n = 1; n <= 10 * 1000; n++) {
    float level = n / 1000.0f;
    int changed = 0;
    for (uint16_t i = 0; i < LEDS; i++) {
      uint8_t v = glowDither(level, i, LEDS);
      TEST_ASSERT_TRUE(v >= prev[i]);
      changed += v != prev[i];
      prev[i] = v;
    }
    TEST_ASSERT_LESS_OR_EQUAL_INT(1, changed);
  }
}

// The LEDs that round up first are spread around the ring, not bunched together
static void test_first_leds_up_are_spread_out() {
  int ups = 0;
  for (uint16_t i = 0; i < LEDS; i++)
    if (glowDither(3.25f, i, LEDS) == 4) {
      ups++;
      TEST_ASSERT_EQUAL_UINT8(3, glowDither(3.25f, (i + 1) % LEDS, LEDS));  // its neighbour stays down
    }
  TEST_ASSERT_EQUAL_INT(6, ups);
}

// Out-of-range and invalid levels are clamped rather than wrapping round
static void test_out_of_range_levels_are_clamped() {
  TEST_ASSERT_EQUAL_UINT8(0, glowDither(-3.0f, 0, LEDS));
  TEST_ASSERT_EQUAL_UINT8(0, glowDither(NAN, 5, LEDS));
  TEST_ASSERT_EQUAL_UINT8(255, glowDither(300.0f, 7, LEDS));
  TEST_ASSERT_EQUAL_UINT8(255, glowDither(INFINITY, 7, LEDS));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_whole_levels_are_exact);
  RUN_TEST(test_strip_average_matches_fraction);
  RUN_TEST(test_single_led_rounds_to_nearest);
  RUN_TEST(test_each_led_rounds_to_a_neighbour);
  RUN_TEST(test_rising_fade_steps_one_led_at_a_time);
  RUN_TEST(test_first_leds_up_are_spread_out);
  RUN_TEST(test_out_of_range_levels_are_clamped);
  return UNITY_END();
}
