#pragma once

#include <stdint.h>

namespace NeoKeyConfig
{
// Fixed physical wiring: COL1..3 = D2..D4 (GPIO3..5),
// ROW1..4 = D5..D8 (GPIO6, GPIO43, GPIO44, GPIO7).
// Keep both matrix axes in physical order so the installed keypad is numbered
// row-wise with key 1 at top-left and key 12 at bottom-right.
constexpr uint8_t COLUMN_PINS[] = {3, 4, 5};
constexpr uint8_t ROW_PINS[] = {6, 43, 44, 7};

constexpr uint8_t COLUMN_COUNT = 3;
constexpr uint8_t ROW_COUNT = 4;
constexpr uint8_t KEY_COUNT = COLUMN_COUNT * ROW_COUNT;

constexpr const char *KEY_NAMES[KEY_COUNT] = {
    "STEP", "RIT", "MUTE",
    "2%", "4%", "10%",
    "20%", "40%", "60%",
    "80%", "90%", "RND kHz"
};

constexpr uint32_t SCAN_INTERVAL_US = 1000;
constexpr uint32_t COLUMN_SETTLE_US = 3;
constexpr uint32_t DEBOUNCE_MS = 20;

static_assert(sizeof(COLUMN_PINS) / sizeof(COLUMN_PINS[0]) == COLUMN_COUNT,
              "NeoKey column pin count does not match COLUMN_COUNT");
static_assert(sizeof(ROW_PINS) / sizeof(ROW_PINS[0]) == ROW_COUNT,
              "NeoKey row pin count does not match ROW_COUNT");
static_assert(sizeof(KEY_NAMES) / sizeof(KEY_NAMES[0]) == KEY_COUNT,
              "NeoKey name count does not match KEY_COUNT");
static_assert(KEY_COUNT == 12, "NeoKey matrix must contain 12 keys");
static_assert(SCAN_INTERVAL_US > 0,
              "NeoKey scan interval must be positive");
} // namespace NeoKeyConfig
