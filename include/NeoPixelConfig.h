#pragma once

#include <stdint.h>

#include "NeoKeyConfig.h"

namespace NeoPixelConfig
{
constexpr uint8_t DATA_PIN = 21;
constexpr uint16_t PIXEL_COUNT = 15;
constexpr uint8_t BRIGHTNESS = 255;

// NeoKey LEDs are wired in a row-wise zigzag chain. This mapping follows the
// matrix after its 180-degree rotation in NeoKeyConfig.h.
constexpr uint8_t KEY_PIXEL_MAP[NeoKeyConfig::KEY_COUNT] = {
    9, 10, 11,
    8, 7, 6,
    3, 4, 5,
    2, 1, 0
};

// Lowest stable white level directly above off; no breathing/glow animation.
constexpr uint8_t BACKGROUND_LEVEL = 1;
constexpr uint8_t KEY_BACKGROUND_RGB[NeoKeyConfig::KEY_COUNT][3] = {
    {0, 0, BACKGROUND_LEVEL},
    {BACKGROUND_LEVEL, BACKGROUND_LEVEL, BACKGROUND_LEVEL},
    {BACKGROUND_LEVEL, BACKGROUND_LEVEL, BACKGROUND_LEVEL},
    {BACKGROUND_LEVEL, BACKGROUND_LEVEL, BACKGROUND_LEVEL},
    {BACKGROUND_LEVEL, BACKGROUND_LEVEL, BACKGROUND_LEVEL},
    {BACKGROUND_LEVEL, BACKGROUND_LEVEL, BACKGROUND_LEVEL},
    {BACKGROUND_LEVEL, BACKGROUND_LEVEL, BACKGROUND_LEVEL},
    {BACKGROUND_LEVEL, BACKGROUND_LEVEL, BACKGROUND_LEVEL},
    {BACKGROUND_LEVEL, BACKGROUND_LEVEL, BACKGROUND_LEVEL},
    {BACKGROUND_LEVEL, BACKGROUND_LEVEL, BACKGROUND_LEVEL},
    {BACKGROUND_LEVEL, BACKGROUND_LEVEL, BACKGROUND_LEVEL},
    {BACKGROUND_LEVEL, BACKGROUND_LEVEL, BACKGROUND_LEVEL}
};
constexpr uint8_t PRESSED_GREEN = 255;
constexpr uint8_t HELD_RED = 255;

constexpr uint32_t UPDATE_INTERVAL_MS = 20;
constexpr uint32_t GREEN_FLASH_MS = 250;
constexpr uint32_t HOLD_THRESHOLD_MS = 700;
constexpr uint32_t HOLD_BLINK_INTERVAL_MS = 250;
constexpr uint32_t RELEASE_FADE_MS = 450;

static_assert(PIXEL_COUNT > 0, "At least one NeoPixel is required");
static_assert(sizeof(KEY_PIXEL_MAP) / sizeof(KEY_PIXEL_MAP[0]) ==
                  NeoKeyConfig::KEY_COUNT,
              "Each key must have one NeoPixel mapping");
static_assert(PIXEL_COUNT >= NeoKeyConfig::KEY_COUNT,
              "There must be at least one NeoPixel per key");
static_assert(sizeof(KEY_BACKGROUND_RGB) / sizeof(KEY_BACKGROUND_RGB[0]) ==
                  NeoKeyConfig::KEY_COUNT,
              "Each key must have one background color");
static_assert(UPDATE_INTERVAL_MS > 0,
              "The key-lighting update interval must be positive");
static_assert(GREEN_FLASH_MS < HOLD_THRESHOLD_MS,
              "The green flash must finish before hold mode starts");
static_assert(HOLD_BLINK_INTERVAL_MS > 0,
              "The hold blink interval must be positive");
static_assert(RELEASE_FADE_MS > 0,
              "The release fade duration must be positive");
} // namespace NeoPixelConfig
