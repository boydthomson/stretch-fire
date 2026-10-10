// fire_effects.h: the Hearth and Fire2012 effects from fire_flicker.ino,
// ported off FastLED so they run as ESPHome addressable_lambda effects.
#pragma once
#include "esphome.h"
#include <algorithm>
#include <vector>

namespace fire {

// --- Tuning (same values as the Arduino sketch) ---
static const uint16_t HEARTH_NOISE_SCALE = 40;  // spatial grain, lower = smoother
static const uint16_t HEARTH_SPEED = 9;         // how fast the flicker evolves
static const uint8_t GUST_CHANCE = 6;           // 0-255 per frame
static const uint8_t GUST_MAX_DIP = 70;         // 0-255
static const uint8_t FIRE_COOLING = 55;         // higher = shorter flames
static const uint8_t FIRE_SPARKING = 120;       // higher = more roaring fire
static const uint32_t PSU_MAX_MA = 2000;        // LED current budget

static inline uint8_t rand8() { return esphome::random_uint32() & 0xFF; }
static inline uint8_t rand8(uint8_t lo, uint8_t hi) {
  return hi <= lo ? lo : lo + (uint8_t) (esphome::random_uint32() % (hi - lo));
}
static inline uint8_t qadd8(int a, int b) { return std::min(255, a + b); }
static inline uint8_t qsub8(int a, int b) { return std::max(0, a - b); }
static inline uint8_t scale8(uint8_t v, uint8_t s) { return ((uint16_t) v * s) >> 8; }

// Smooth 2D value noise, coordinates in 1/256 lattice units like FastLED's inoise8.
static inline uint8_t lattice(uint32_t x, uint32_t y) {
  uint32_t h = x * 374761393u + y * 668265263u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return (h ^ (h >> 16)) & 0xFF;
}
static inline uint8_t noise8(uint32_t x, uint32_t y) {
  uint32_t xi = x >> 8, yi = y >> 8;
  float fx = (x & 0xFF) / 256.0f, fy = (y & 0xFF) / 256.0f;
  fx = fx * fx * (3 - 2 * fx);
  fy = fy * fy * (3 - 2 * fy);
  float a = lattice(xi, yi), b = lattice(xi + 1, yi);
  float c = lattice(xi, yi + 1), d = lattice(xi + 1, yi + 1);
  float top = a + (b - a) * fx, bot = c + (d - c) * fx;
  return (uint8_t) (top + (bot - top) * fy);
}

// Warm firelight palette: black, ember, red, orange, amber, pale yellow.
static inline esphome::Color palette(uint8_t idx) {
  static const uint8_t stops[][4] = {
      {0, 0, 0, 0},       {50, 40, 1, 0},     {110, 160, 15, 0},
      {170, 255, 60, 0},  {220, 255, 130, 8}, {255, 255, 180, 50},
  };
  for (int s = 1; s < 6; s++) {
    if (idx <= stops[s][0]) {
      const uint8_t *p = stops[s - 1], *q = stops[s];
      float t = (float) (idx - p[0]) / (q[0] - p[0]);
      return esphome::Color(p[1] + (q[1] - p[1]) * t, p[2] + (q[2] - p[2]) * t, p[3] + (q[3] - p[3]) * t);
    }
  }
  return esphome::Color(255, 180, 50);
}

// Scale the frame down if it would draw more than PSU_MAX_MA at full brightness
// (per-channel figures from FastLED's power model). HA brightness only lowers it further.
static inline void limit_power(esphome::light::AddressableLight &it) {
  uint32_t ma = 0;
  for (int i = 0; i < it.size(); i++) {
    esphome::Color c = it[i].get();
    ma += (c.r * 16 + c.g * 11 + c.b * 15) / 255 + 1;
  }
  if (ma <= PSU_MAX_MA) return;
  uint8_t s = (uint8_t) (255UL * PSU_MAX_MA / ma);
  for (int i = 0; i < it.size(); i++) {
    esphome::Color c = it[i].get();
    it[i] = esphome::Color(scale8(c.r, s), scale8(c.g, s), scale8(c.b, s));
  }
}

// --- Spark burst overlay: a flare plus embers drifting up the strip ---
// trigger_burst() queues one burst; bursts play back to back so a rush of
// events stays countable instead of merging into one flash.
struct Spark {
  float pos, vel;
  uint8_t life;
};
static std::vector<Spark> sparks;
static uint8_t bursts_pending = 0;
static uint8_t flare = 0;

static inline void trigger_burst() {
  if (bursts_pending < 20) bursts_pending++;
}

static inline void overlay_sparks(esphome::light::AddressableLight &it) {
  const int n = it.size();
  if (bursts_pending > 0 && flare < 40) {
    bursts_pending--;
    flare = 255;
    for (int k = 0, count = rand8(8, 14); k < count; k++)
      sparks.push_back({(float) rand8(0, n), 0.15f + rand8(0, 40) / 100.0f, rand8(200, 255)});
  }
  // Whole fire swells towards bright amber, then settles back over ~1 s.
  if (flare > 0) {
    const uint8_t amt = scale8(flare, 150);
    for (int i = 0; i < n; i++) {
      esphome::Color c = it[i].get();
      it[i] = esphome::Color(qadd8(c.r, scale8(255 - c.r, amt)), qadd8(c.g, scale8(200 - std::min<int>(c.g, 200), amt)),
                             qadd8(c.b, scale8(60 - std::min<int>(c.b, 60), amt)));
    }
    flare = scale8(flare, 240);
  }
  // Individual sparks: near-white hot, cooling to orange as they drift and fade.
  for (auto &s : sparks) {
    int p = (int) s.pos;
    if (p >= 0 && p < n) {
      esphome::Color c = it[p].get();
      it[p] = esphome::Color(std::max(c.r, s.life), std::max<uint8_t>(c.g, scale8(s.life, 230)),
                             std::max<uint8_t>(c.b, scale8(scale8(s.life, s.life), 160)));
    }
    s.pos += s.vel;
    s.life = qsub8(s.life, rand8(3, 8));
  }
  sparks.erase(std::remove_if(sparks.begin(), sparks.end(),
                              [n](const Spark &s) { return s.life == 0 || s.pos >= n; }),
               sparks.end());
}

// Every pixel glows and flickers independently, with occasional draughts.
static inline void hearth(esphome::light::AddressableLight &it) {
  static uint32_t t = esphome::random_uint32() >> 8;
  static uint8_t gust = 0;
  t += HEARTH_SPEED;
  if (rand8() < GUST_CHANCE) {
    uint8_t dip = rand8(GUST_MAX_DIP / 3, GUST_MAX_DIP);
    if (dip > gust) gust = dip;
  }
  gust = scale8(gust, 235);  // exponential recovery
  for (int i = 0; i < it.size(); i++) {
    uint8_t slow = noise8(i * HEARTH_NOISE_SCALE, t);
    uint8_t fast = noise8(i * HEARTH_NOISE_SCALE * 3 + 10000, t * 3);
    uint8_t h = qadd8(scale8(slow, 190), scale8(fast, 90));
    h = (h < 60) ? 0 : (uint8_t) std::min(255, (h - 60) * 3 / 2);
    h = qsub8(h, gust);
    h = std::max(h, (uint8_t) 25);  // coals never fully die
    it[i] = palette(scale8(h, 240));
  }
  overlay_sparks(it);
  limit_power(it);
}

// Mark Kriegsman's Fire2012 rising flames (strip vertical, data-in at the bottom).
static inline void fire2012(esphome::light::AddressableLight &it, bool initial_run) {
  static std::vector<uint8_t> heat;
  const int n = it.size();
  if (initial_run || (int) heat.size() != n) heat.assign(n, 0);
  const uint8_t max_cool = (FIRE_COOLING * 10) / n + 2;
  for (int i = 0; i < n; i++) heat[i] = qsub8(heat[i], rand8(0, max_cool));
  for (int k = n - 1; k >= 2; k--) heat[k] = (heat[k - 1] + heat[k - 2] + heat[k - 2]) / 3;
  if (rand8() < FIRE_SPARKING) {
    int y = rand8(0, std::min(7, n));
    heat[y] = qadd8(heat[y], rand8(160, 255));
  }
  for (int j = 0; j < n; j++) it[j] = palette(scale8(heat[j], 240));
  overlay_sparks(it);
  limit_power(it);
}

}  // namespace fire
