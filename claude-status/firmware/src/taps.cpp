// Taps on the display's case, detected with the board's QMI8658 motion sensor.
//
// A task on core 0 reads the accelerometer 1000 times a second and measures the jolt: how far
// each reading is from the slowly changing direction of gravity. A hit starts when the jolt jumps
// well above the fading echo of earlier hits, and is judged 200 ms later: it's a tap if it was
// strong enough and the display didn't tilt (it wasn't being picked up or moved).
//
// Taps 80-450 ms apart make a run. A run of exactly two is a double tap, counted once the run is
// over (so it isn't the start of five taps); the fifth tap of a run counts as five taps straight away.
//
// Measured on a display in a desk stand: taps on the case 0.26-1.7 g; putting a mug down or
// knocking on the desk 0.10-0.19 g; typing on the same desk doesn't register at all; picking the
// display up builds up too gradually to start a hit.

#include "taps.h"

#include <Arduino.h>
#include <Wire.h>

#include <algorithm>

namespace {

constexpr int PIN_SDA = 6, PIN_SCL = 7;
constexpr uint8_t REG_WHO_AM_I = 0x00, REG_CTRL1 = 0x02, REG_CTRL2 = 0x03, REG_CTRL5 = 0x06,
                  REG_CTRL7 = 0x08, REG_AX_L = 0x35, REG_RST_RESULT = 0x4D, REG_RESET = 0x60;
constexpr float COUNTS_PER_G = 4096;  // +-8 g range

// Times are in samples, about 1 ms each
constexpr float HIT_G = 0.05f;           // smallest jolt that can start a hit
constexpr float ONSET_RATIO = 2.5f;      // ...and it must jump this far above the echo of earlier hits
constexpr float ECHO_DECAY = 0.975f;     // per sample: the echo fades over ~40 ms
constexpr float GRAVITY_FOLLOW = 0.01f;  // per sample: gravity follows slow changes over ~100 ms
constexpr uint32_t LOCKOUT = 40;         // a hit's own ringing can't start another hit
constexpr uint32_t SETTLE = 200;         // judge a hit this long after it starts
constexpr float MAX_TILT_DEG = 10;       // tilted more than this: moved, not tapped
constexpr uint32_t GAP_MIN = 80;         // a tap closer than this to the last one is the case rattling
constexpr uint32_t GAP_MAX = 450;        // taps further apart than this start a new run
constexpr uint32_t NONE = UINT32_MAX;
constexpr int RING = 1024;
constexpr int MAX_PENDING = 8;

uint8_t addr = 0;
float minTapG = 0.25f;
volatile uint32_t doubles = 0, fives = 0;
int16_t recent[RING][3];  // recent readings, for measuring tilt around a hit

bool writeReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

bool readRegs(uint8_t reg, uint8_t *buf, uint8_t n) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom(addr, n) != n) return false;
  for (uint8_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

int16_t median3(int16_t a, int16_t b, int16_t c) { return std::max(std::min(a, b), std::min(std::max(a, b), c)); }

// Direction of gravity over samples [from, to): the median on each axis, so a tap inside doesn't skew it
void gravity(uint32_t from, uint32_t to, float g[3]) {
  int16_t v[100];
  const int n = to - from;
  for (int k = 0; k < 3; k++) {
    for (int i = 0; i < n; i++) v[i] = recent[(from + i) % RING][k];
    std::nth_element(v, v + n / 2, v + n);
    g[k] = v[n / 2];
  }
}

// How far the display tilted between just before a hit and just after it, in degrees
float tiltDeg(uint32_t hit) {
  float a[3], b[3];
  gravity(hit - 200, hit - 100, a);
  gravity(hit + 100, hit + 200, b);
  float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
  float na = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
  float nb = sqrtf(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
  if (na == 0 || nb == 0) return 0;
  return acosf(constrain(dot / (na * nb), -1.0f, 1.0f)) * RAD_TO_DEG;
}

void watch(void *) {
  int16_t older[3] = {}, old[3] = {};
  float base[3] = {}, echo = 0;
  uint32_t reads = 0, n = 0;
  uint32_t hitAt[MAX_PENDING];  // hits waiting to be judged, oldest first
  float hitPeak[MAX_PENDING];
  int hits = 0;
  uint32_t lastTap = NONE;  // when the latest tap of the current run started
  int run = 0;              // taps in the current run
  TickType_t wake = xTaskGetTickCount();
  for (;;) {
    vTaskDelayUntil(&wake, 1);
    uint8_t b[6];
    if (!readRegs(REG_AX_L, b, 6)) continue;

    // The sensor updates faster than we read it, so now and then a value's low and high bytes
    // come from different samples: a one-sample spike of 256 counts. A median of three removes
    // those; real movement lasts several samples.
    int16_t s[3];
    for (int k = 0; k < 3; k++) {
      int16_t cur = (int16_t)(b[2 * k] | b[2 * k + 1] << 8);
      s[k] = median3(older[k], old[k], cur);
      older[k] = old[k];
      old[k] = cur;
    }
    if (++reads < 3) continue;

    float jolt2 = 0;
    for (int k = 0; k < 3; k++) {
      float a = s[k] / COUNTS_PER_G;
      if (n == 0) base[k] = a;
      float d = a - base[k];
      base[k] += GRAVITY_FOLLOW * d;
      jolt2 += d * d;
    }
    float jolt = sqrtf(jolt2), echoBefore = echo;
    echo = std::max(jolt, echo * ECHO_DECAY);
    uint32_t i = n++;
    memcpy(recent[i % RING], s, sizeof(s));
    if (i < 500) continue;  // let gravity settle after power-on

    if ((hits == 0 || i - hitAt[hits - 1] >= LOCKOUT) && hits < MAX_PENDING && jolt >= HIT_G &&
        jolt >= ONSET_RATIO * echoBefore) {
      hitAt[hits] = i;
      hitPeak[hits++] = jolt;
    }
    for (int h = 0; h < hits; h++)
      if (i - hitAt[h] < LOCKOUT) hitPeak[h] = std::max(hitPeak[h], jolt);

    if (hits && i - hitAt[0] >= SETTLE) {
      uint32_t at = hitAt[0];
      float peak = hitPeak[0];
      hits--;
      for (int h = 0; h < hits; h++) {
        hitAt[h] = hitAt[h + 1];
        hitPeak[h] = hitPeak[h + 1];
      }
      uint32_t gap = lastTap == NONE ? NONE : at - lastTap;
      if (peak >= minTapG && gap >= GAP_MIN && tiltDeg(at) <= MAX_TILT_DEG) {
        run = gap <= GAP_MAX ? run + 1 : 1;
        lastTap = at;
        if (run == 5) fives++;
      }
    }

    // The run is over once no more taps can join it (any hit still being judged started too late)
    if (run && i - lastTap > GAP_MAX && (hits == 0 || hitAt[0] - lastTap > GAP_MAX)) {
      if (run == 2) doubles++;
      run = 0;
    }
  }
}

}  // namespace

bool tapsBegin(float tapG) {
  minTapG = tapG;
  Wire.begin(PIN_SDA, PIN_SCL, 400000);
  for (uint8_t a : {0x6B, 0x6A}) {  // depends on how the board wires the sensor
    addr = a;
    uint8_t id = 0;
    if (readRegs(REG_WHO_AM_I, &id, 1) && id == 0x05) break;
    addr = 0;
  }
  if (!addr) return false;

  writeReg(REG_RESET, 0xB0);
  delay(15);
  uint8_t status = 0;
  for (int i = 0; i < 50 && !(readRegs(REG_RST_RESULT, &status, 1) && status == 0x80); i++) delay(2);
  writeReg(REG_CTRL1, 0x40);                // registers auto-increment, little-endian
  writeReg(REG_CTRL2, (0x02 << 4) | 0x02);  // accelerometer: +-8 g, ~1.8 kHz
  writeReg(REG_CTRL5, (0x03 << 1) | 0x01);  // its low-pass filter, at 13% of that rate
  writeReg(REG_CTRL7, 0x01);                // accelerometer on, gyroscope off
  xTaskCreatePinnedToCore(watch, "taps", 4096, nullptr, 5, nullptr, 0);
  return true;
}

uint32_t doubleTapCount() { return doubles; }
uint32_t fiveTapCount() { return fives; }
