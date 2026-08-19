#pragma once

#include <stdint.h>

namespace NeoKeyConfig
{
// The NeoKey board is installed upside down. Reversing both axes rotates the
// logical matrix by 180 degrees while leaving the wiring unchanged.
constexpr uint8_t COLUMN_PINS[] = {41, 42, 2};
constexpr uint8_t ROW_PINS[] = {47, 38, 39, 40};

constexpr uint8_t COLUMN_COUNT = 3;
constexpr uint8_t ROW_COUNT = 4;
constexpr uint8_t KEY_COUNT = COLUMN_COUNT * ROW_COUNT;

constexpr const char *KEY_NAMES[KEY_COUNT] = {
    "Taste 1", "Taste 2", "Taste 3",
    "Taste 4", "Taste 5", "Taste 6",
    "Taste 7", "Taste 8", "Taste 9",
    "Taste 10", "Taste 11", "Taste 12"
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
