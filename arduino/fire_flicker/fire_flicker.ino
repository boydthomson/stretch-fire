/*
 * =============================================================================
 *  fire_flicker.ino: WS2812B "firelight" effect for ESP-WROOM-32
 * =============================================================================
 *
 *  Two effects:
 *    MODE_HEARTH   (0): every pixel glows & flickers independently, like a
 *                        bed of embers / candle cluster. Best for strips laid
 *                        horizontally, behind diffusers, in lanterns, etc.
 *    MODE_FIRE2012 (1): classic rising-flame simulation (Mark Kriegsman's
 *                        Fire2012). Best for strips mounted VERTICALLY with
 *                        the data-in end at the bottom.
 *
 *  Hardware (see wiring diagram):
 *    GPIO18 -> 74AHCT125 (1A, pin 2) -> 1Y (pin 3) -> 330R -> strip DIN
 *    74AHCT125: pin 14 = 5V, pin 7 = GND, pin 1 (1OE) = GND
 *               unused inputs (5, 9, 12) -> GND, unused OEs (4, 10, 13) -> 5V
 *    1000uF cap across 5V/GND at the strip's input end
 *    All grounds common. ESP32 powered from the same 5V into VIN.
 *
 *  Library: FastLED (recent version: 3.9+ recommended for ESP32 core 3.x)
 *
 *  Serial monitor @ 115200 baud. Type "h" + Enter for runtime commands.
 * =============================================================================
 */

#include <FastLED.h>
#include <esp_system.h>   // esp_reset_reason(), esp_random(), heap info

// =============================================================================
//  USER CONFIGURATION: edit these for your build
// =============================================================================
#define DATA_PIN        18        // GPIO feeding the level shifter
#define NUM_LEDS        60        // number of pixels on the strip
#define LED_TYPE        WS2812B
#define COLOR_ORDER     GRB       // if the self-test colours are wrong, change this

#define PSU_VOLTS       5
#define PSU_MAX_MA      2000      // set ~80% of your supply's real rating
#define START_BRIGHTNESS 180      // 0-255 global brightness at boot
#define TARGET_FPS      60        // animation frame rate

#define DEFAULT_MODE    0         // 0 = hearth, 1 = fire2012

// --- Hearth mode tuning ---
#define HEARTH_NOISE_SCALE  40    // spatial "grain": lower = smoother between pixels
#define HEARTH_SPEED        9     // how fast the flicker evolves
#define GUST_CHANCE         6     // 0-255 per frame: chance of a draft dimming the fire
#define GUST_MAX_DIP        70    // how far a gust can dim (0-255)

// --- Fire2012 mode tuning ---
#define FIRE_COOLING    55        // higher = shorter flames (20-100)
#define FIRE_SPARKING   120       // higher = more roaring fire (50-200)

// --- Debug ---
//  0 = errors only, 1 = + warnings, 2 = + info, 3 = + verbose per-frame spam
#define DEBUG_LEVEL     2
#define STATS_INTERVAL_MS 5000    // how often to print the stats summary
#define RUN_SELF_TEST   1         // flash R/G/B at boot to verify wiring

// =============================================================================
//  DEBUG / LOGGING MACROS
// =============================================================================
//  Each log line is prefixed with uptime in ms and severity, e.g.:
//    [   1234][INFO ] Strip initialised
#define LOG_AT(level, tag, fmt, ...)                                          \
  do {                                                                        \
    if (DEBUG_LEVEL >= (level)) {                                             \
      Serial.printf("[%7lu][" tag "] " fmt "\n", millis(), ##__VA_ARGS__);    \
    }                                                                         \
  } while (0)

#define LOG_ERROR(fmt, ...)   LOG_AT(0, "ERROR", fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)    LOG_AT(1, "WARN ", fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)    LOG_AT(2, "INFO ", fmt, ##__VA_ARGS__)
#define LOG_VERBOSE(fmt, ...) LOG_AT(3, "VERB ", fmt, ##__VA_ARGS__)

// =============================================================================
//  COMPILE-TIME SANITY CHECKS: catch config mistakes before flashing
// =============================================================================

// Returns true if a GPIO is a sane choice for LED data on an ESP-WROOM-32.
//  - 0, 2, 5, 12, 15 are strapping pins (can break boot / flash voltage)
//  - 1, 3 are UART0 TX/RX (we use Serial for debug)
//  - 6-11 are wired to the module's internal SPI flash
//  - 34-39 are input-only
constexpr bool isSafeDataPin(int p) {
  return p >= 0 && p < 34 &&
         p != 0 && p != 1 && p != 2 && p != 3 && p != 5 &&
         p != 12 && p != 15 &&
         !(p >= 6 && p <= 11);
}

static_assert(isSafeDataPin(DATA_PIN),
              "DATA_PIN is a strapping/flash/UART/input-only pin. Use 4, 13, 14, 16-19, 21-23, 25-27, 32 or 33.");
static_assert(NUM_LEDS >= 3,         "NUM_LEDS must be at least 3 (Fire2012 needs 3 pixels).");
static_assert(NUM_LEDS <= 1000,      "NUM_LEDS > 1000: frame rate will collapse; split into multiple outputs.");
static_assert(TARGET_FPS > 0 && TARGET_FPS <= 200, "TARGET_FPS out of range (1-200).");
static_assert(PSU_MAX_MA >= 100,     "PSU_MAX_MA looks too low: the ESP32 alone draws ~100-250mA.");
static_assert(DEFAULT_MODE == 0 || DEFAULT_MODE == 1, "DEFAULT_MODE must be 0 or 1.");

// WS2812B takes ~30us per pixel to clock out. Warn if the strip alone can't hit the FPS target.
static_assert((NUM_LEDS * 30UL) < (1000000UL / TARGET_FPS),
              "Strip too long for TARGET_FPS: lower the FPS or split the strip.");

// =============================================================================
//  GLOBAL STATE
// =============================================================================
enum FireMode : uint8_t { MODE_HEARTH = 0, MODE_FIRE2012 = 1 };

CRGB     leds[NUM_LEDS];          // pixel buffer sent to the strip
uint8_t  heat[NUM_LEDS];          // per-pixel "temperature" for Fire2012

FireMode currentMode    = (FireMode)DEFAULT_MODE;
uint8_t  userBrightness = START_BRIGHTNESS;
uint32_t noiseTime      = 0;      // time axis for the Perlin noise in hearth mode
uint8_t  gustDip        = 0;      // current draft-dimming amount (decays each frame)

const uint32_t FRAME_INTERVAL_US = 1000000UL / TARGET_FPS;

// Warm firelight palette: black -> deep ember -> red -> orange -> amber -> pale yellow.
// (No blues or whites: real wood fire tops out around 1800-2000K.)
DEFINE_GRADIENT_PALETTE(firelight_gp) {
    0,    0,   0,   0,
   50,   40,   1,   0,
  110,  160,  15,   0,
  170,  255,  60,   0,
  220,  255, 130,   8,
  255,  255, 180,  50
};
CRGBPalette16 firePalette = firelight_gp;

// --- Stats (reset every STATS_INTERVAL_MS) ---
struct Stats {
  uint32_t frames        = 0;
  uint32_t overruns      = 0;   // frames that took longer than the frame budget
  uint32_t maxFrameUs    = 0;
  uint64_t totalFrameUs  = 0;
  uint32_t powerLimited  = 0;   // frames where FastLED throttled brightness for the PSU
  uint32_t lastPrintMs   = 0;
} stats;

// --- Serial command buffer ---
char     cmdBuf[32];
uint8_t  cmdLen = 0;

// =============================================================================
//  BOOT DIAGNOSTICS
// =============================================================================

// Report why we (re)booted. A BROWNOUT reset almost always means the LEDs pulled
// the 5V rail down: bigger supply, power injection, or lower brightness needed.
void logResetReason() {
  esp_reset_reason_t r = esp_reset_reason();
  switch (r) {
    case ESP_RST_POWERON:  LOG_INFO("Reset reason: power-on"); break;
    case ESP_RST_SW:       LOG_INFO("Reset reason: software restart"); break;
    case ESP_RST_EXT:      LOG_INFO("Reset reason: external pin"); break;
    case ESP_RST_PANIC:    LOG_ERROR("Reset reason: PANIC (crash): check previous serial output"); break;
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      LOG_ERROR("Reset reason: WATCHDOG: something blocked the loop too long"); break;
    case ESP_RST_BROWNOUT:
      LOG_ERROR("Reset reason: BROWNOUT: 5V rail sagged. Check PSU size, wire gauge, "
                "the 1000uF cap, or lower START_BRIGHTNESS / PSU_MAX_MA.");
      break;
    default:               LOG_WARN("Reset reason: other (%d)", (int)r); break;
  }
}

// Quick visual test: R, G, B, then off. If you see the wrong colours in the
// wrong order, fix COLOR_ORDER. If nothing lights, check data path & ground.
void runSelfTest() {
#if RUN_SELF_TEST
  LOG_INFO("Self-test: you should see RED, then GREEN, then BLUE");
  const CRGB testColours[] = { CRGB(64, 0, 0), CRGB(0, 64, 0), CRGB(0, 0, 64) };
  const char* names[]      = { "RED", "GREEN", "BLUE" };
  for (uint8_t c = 0; c < 3; c++) {
    LOG_INFO("  Self-test -> %s", names[c]);
    fill_solid(leds, NUM_LEDS, testColours[c]);
    FastLED.show();
    delay(400);
  }
  FastLED.clear(true);
  LOG_INFO("Self-test complete. If colours were wrong, change COLOR_ORDER.");
#else
  LOG_VERBOSE("Self-test disabled");
#endif
}

// =============================================================================
//  EFFECT: HEARTH (independent per-pixel ember flicker)
// =============================================================================
void renderHearth() {
  noiseTime += HEARTH_SPEED;

  // Random "draft": occasionally the whole fire dips, then recovers smoothly.
  if (random8() < GUST_CHANCE) {
    uint8_t newDip = random8(GUST_MAX_DIP / 3, GUST_MAX_DIP);
    if (newDip > gustDip) {
      gustDip = newDip;
      LOG_VERBOSE("Gust! dip=%u", gustDip);
    }
  }
  gustDip = scale8(gustDip, 235);   // exponential recovery (~0.92 per frame)

  for (uint16_t i = 0; i < NUM_LEDS; i++) {
    // Two octaves of Perlin noise: slow broad glow + faster fine flicker.
    uint8_t slow = inoise8(i * HEARTH_NOISE_SCALE, noiseTime);
    uint8_t fast = inoise8(i * HEARTH_NOISE_SCALE * 3 + 10000, noiseTime * 3);

    // Blend: mostly slow body, some fast flicker on top.
    uint8_t h = qadd8(scale8(slow, 190), scale8(fast, 90));

    // Stretch contrast so embers sometimes go nearly dark and sometimes flare.
    h = (h < 60) ? 0 : (uint8_t)min(255, (h - 60) * 3 / 2);
    h = qsub8(h, gustDip);

    // Keep a floor so the fire never fully dies (feels like coals underneath).
    h = max(h, (uint8_t)25);

    // Cap index at 240 so LINEARBLEND doesn't wrap palette end back to black.
    leds[i] = ColorFromPalette(firePalette, scale8(h, 240), 255, LINEARBLEND);
  }
}

// =============================================================================
//  EFFECT: FIRE2012 (rising flames: mount strip vertically, DIN at bottom)
// =============================================================================
void renderFire2012() {
  // 1. Cool every cell a little.
  const uint8_t maxCool = ((FIRE_COOLING * 10) / NUM_LEDS) + 2;
  for (uint16_t i = 0; i < NUM_LEDS; i++) {
    heat[i] = qsub8(heat[i], random8(0, maxCool));
  }

  // 2. Heat drifts upward and diffuses.
  for (uint16_t k = NUM_LEDS - 1; k >= 2; k--) {
    heat[k] = (heat[k - 1] + heat[k - 2] + heat[k - 2]) / 3;
  }

  // 3. Randomly ignite new sparks near the bottom.
  if (random8() < FIRE_SPARKING) {
    const uint8_t sparkZone = min((uint16_t)7, (uint16_t)NUM_LEDS);
    uint8_t y = random8(sparkZone);
    heat[y] = qadd8(heat[y], random8(160, 255));
    LOG_VERBOSE("Spark at %u -> heat %u", y, heat[y]);
  }

  // 4. Map heat to colour.
  for (uint16_t j = 0; j < NUM_LEDS; j++) {
    leds[j] = ColorFromPalette(firePalette, scale8(heat[j], 240), 255, LINEARBLEND);
  }
}

// =============================================================================
//  SERIAL COMMANDS (non-blocking)
// =============================================================================
void printHelp() {
  Serial.println(F("\nCommands (type then Enter):"));
  Serial.println(F("  m 0       hearth/ember mode"));
  Serial.println(F("  m 1       Fire2012 rising flames"));
  Serial.println(F("  b <0-255> set brightness"));
  Serial.println(F("  s         print stats now"));
  Serial.println(F("  t         re-run colour self-test"));
  Serial.println(F("  h         this help\n"));
}

void printStats(); // forward declaration

void handleCommand(const char* cmd) {
  LOG_VERBOSE("Command received: '%s'", cmd);

  if (cmd[0] == '\0') return;  // empty line, ignore silently

  int value = -1;
  switch (cmd[0]) {
    case 'h': case '?':
      printHelp();
      break;

    case 'm':
      if (sscanf(cmd + 1, "%d", &value) != 1 || (value != 0 && value != 1)) {
        LOG_WARN("Bad mode command '%s': use 'm 0' or 'm 1'", cmd);
        return;
      }
      currentMode = (FireMode)value;
      memset(heat, 0, sizeof(heat));  // fresh start so modes don't bleed into each other
      LOG_INFO("Mode -> %s", currentMode == MODE_HEARTH ? "HEARTH" : "FIRE2012");
      break;

    case 'b':
      if (sscanf(cmd + 1, "%d", &value) != 1 || value < 0 || value > 255) {
        LOG_WARN("Bad brightness '%s': use 'b 0' to 'b 255'", cmd);
        return;
      }
      userBrightness = (uint8_t)value;
      FastLED.setBrightness(userBrightness);
      LOG_INFO("Brightness -> %u", userBrightness);
      break;

    case 's':
      printStats();
      break;

    case 't':
      runSelfTest();
      break;

    default:
      LOG_WARN("Unknown command '%s': type 'h' for help", cmd);
      break;
  }
}

// Reads whatever bytes are available without blocking the animation.
void pollSerial() {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();

    if (c == '\r') continue;               // ignore CR from Windows terminals
    if (c == '\n') {
      cmdBuf[cmdLen] = '\0';
      handleCommand(cmdBuf);
      cmdLen = 0;
      continue;
    }
    if (cmdLen >= sizeof(cmdBuf) - 1) {    // overflow protection
      LOG_WARN("Command too long (max %u chars): discarded", (unsigned)(sizeof(cmdBuf) - 1));
      cmdLen = 0;
      // drain the rest of this line so it isn't parsed as a new command
      while (Serial.available() && Serial.read() != '\n') {}
      continue;
    }
    cmdBuf[cmdLen++] = c;
  }
}

// =============================================================================
//  STATS
// =============================================================================
void printStats() {
  const uint32_t now     = millis();
  const uint32_t elapsed = max<uint32_t>(1, now - stats.lastPrintMs);
  const float    fps     = stats.frames * 1000.0f / elapsed;
  const uint32_t avgUs   = stats.frames ? (uint32_t)(stats.totalFrameUs / stats.frames) : 0;

  // Estimated draw of the current frame at the current brightness.
  const uint32_t rawMw = calculate_unscaled_power_mW(leds, NUM_LEDS);
  const uint32_t estMw = (uint32_t)((uint64_t)rawMw * userBrightness / 255);
  const uint32_t estMa = estMw / PSU_VOLTS;

  LOG_INFO("STATS mode=%s fps=%.1f/%d avgFrame=%luus maxFrame=%luus overruns=%lu",
           currentMode == MODE_HEARTH ? "HEARTH" : "FIRE2012",
           fps, TARGET_FPS, (unsigned long)avgUs,
           (unsigned long)stats.maxFrameUs, (unsigned long)stats.overruns);
  LOG_INFO("      LEDs~%lumA (limit %dmA) powerLimitedFrames=%lu heapFree=%lu minHeap=%lu",
           (unsigned long)estMa, PSU_MAX_MA, (unsigned long)stats.powerLimited,
           (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap());

  // Health warnings
  if (stats.frames > 0 && fps < TARGET_FPS * 0.8f) {
    LOG_WARN("Running below 80%% of target FPS: reduce NUM_LEDS or TARGET_FPS");
  }
  if (stats.powerLimited > stats.frames / 2) {
    LOG_WARN("Power limiter active on most frames: fire will look dimmer than "
             "START_BRIGHTNESS. Bigger PSU or lower brightness.");
  }
  if (ESP.getFreeHeap() < 20000) {
    LOG_WARN("Low heap (%lu bytes): possible leak", (unsigned long)ESP.getFreeHeap());
  }

  stats = Stats();          // reset counters
  stats.lastPrintMs = now;
}

// =============================================================================
//  SETUP
// =============================================================================
void setup() {
  Serial.begin(115200);
  delay(300);  // give the USB-serial bridge a moment so early logs aren't lost

  Serial.println();
  LOG_INFO("=== Fire flicker booting ===");
  LOG_INFO("Chip: %s rev %d, %d cores @ %lu MHz",
           ESP.getChipModel(), ESP.getChipRevision(), ESP.getChipCores(),
           (unsigned long)ESP.getCpuFreqMHz());
  logResetReason();

  LOG_INFO("Config: pin=GPIO%d leds=%d order=%s fps=%d psu=%dV/%dmA bright=%d",
           DATA_PIN, NUM_LEDS, "see COLOR_ORDER", TARGET_FPS,
           PSU_VOLTS, PSU_MAX_MA, START_BRIGHTNESS);

  // Worst case WS2812B draw is ~60mA per pixel (full white). Fire never goes
  // white, but warn if the PSU is badly undersized for the strip length anyway.
  const uint32_t worstCaseMa = (uint32_t)NUM_LEDS * 60;
  if (worstCaseMa > PSU_MAX_MA) {
    LOG_WARN("Worst-case strip draw %lumA > PSU_MAX_MA %dmA. FastLED will cap "
             "brightness to protect the supply: expected and OK for fire effects.",
             (unsigned long)worstCaseMa, PSU_MAX_MA);
  }

  // Seed FastLED's RNG from the ESP32 hardware RNG so every boot flickers differently.
  random16_set_seed((uint16_t)esp_random());
  random16_add_entropy((uint16_t)esp_random());
  noiseTime = esp_random();
  LOG_VERBOSE("RNG seeded");

  // Register the strip with FastLED.
  FastLED.addLeds<LED_TYPE, DATA_PIN, COLOR_ORDER>(leds, NUM_LEDS)
         .setCorrection(TypicalLEDStrip);

  if (FastLED.count() == 0) {
    // Should never happen, but if it does nothing else will work: halt loudly.
    LOG_ERROR("FastLED registered zero controllers! Halting.");
    while (true) { delay(1000); LOG_ERROR("Halted: LED controller init failed"); }
  }
  LOG_INFO("FastLED: %d controller(s), %d pixels", FastLED.count(), FastLED.size());

  FastLED.setMaxPowerInVoltsAndMilliamps(PSU_VOLTS, PSU_MAX_MA);
  FastLED.setBrightness(userBrightness);
  FastLED.setDither(BINARY_DITHER);   // smoother low-brightness embers
  FastLED.clear(true);

  memset(heat, 0, sizeof(heat));

  runSelfTest();

  LOG_INFO("Starting in %s mode. Type 'h' for commands.",
           currentMode == MODE_HEARTH ? "HEARTH" : "FIRE2012");
  stats.lastPrintMs = millis();
}

// =============================================================================
//  LOOP: fixed-rate, non-blocking
// =============================================================================
void loop() {
  static uint32_t lastFrameUs = micros();

  pollSerial();

  const uint32_t nowUs = micros();
  if (nowUs - lastFrameUs < FRAME_INTERVAL_US) {
    delay(1);           // yield so the idle task can feed the watchdog
    return;
  }
  lastFrameUs = nowUs;

  const uint32_t t0 = micros();

  // Render the selected effect into leds[]
  switch (currentMode) {
    case MODE_HEARTH:   renderHearth();   break;
    case MODE_FIRE2012: renderFire2012(); break;
    default:
      // Corrupted state: recover rather than crash
      LOG_ERROR("Invalid mode %d: resetting to HEARTH", (int)currentMode);
      currentMode = MODE_HEARTH;
      renderHearth();
      break;
  }

  // Track whether the PSU power limiter will clamp this frame.
  const uint8_t allowed = calculate_max_brightness_for_power_mW(
      leds, NUM_LEDS, userBrightness, (uint32_t)PSU_VOLTS * PSU_MAX_MA);
  if (allowed < userBrightness) {
    stats.powerLimited++;
    LOG_VERBOSE("Power limit: %u -> %u", userBrightness, allowed);
  }

  FastLED.show();

  // Frame timing stats
  const uint32_t frameUs = micros() - t0;
  stats.frames++;
  stats.totalFrameUs += frameUs;
  if (frameUs > stats.maxFrameUs) stats.maxFrameUs = frameUs;
  if (frameUs > FRAME_INTERVAL_US) {
    stats.overruns++;
    LOG_VERBOSE("Frame overrun: %luus > %luus budget",
                (unsigned long)frameUs, (unsigned long)FRAME_INTERVAL_US);
  }

  if (millis() - stats.lastPrintMs >= STATS_INTERVAL_MS) {
    printStats();
  }
}
