#pragma once

#include <stdint.h>

#include "NeoKeyConfig.h"

namespace ButtonConfig
{
enum class Action : uint8_t
{
    None,
    CycleFrequencyStep,
    ToggleRit,
    ToggleMute,
    RoundFrequencyToKhz,
    RfPowerPreset
};

// Button 1 controls the encoder tuning step. Buttons 2 and 3 toggle RIT and
// Slice audio mute. Buttons 4 through 11 select the eight HF power presets;
// button 12 rounds the active Slice frequency to the nearest full kHz.
constexpr Action BUTTON_ACTIONS[NeoKeyConfig::KEY_COUNT] = {
    Action::CycleFrequencyStep,
    Action::ToggleRit,
    Action::ToggleMute,
    Action::RfPowerPreset,
    Action::RfPowerPreset,
    Action::RfPowerPreset,
    Action::RfPowerPreset,
    Action::RfPowerPreset,
    Action::RfPowerPreset,
    Action::RfPowerPreset,
    Action::RfPowerPreset,
    Action::RoundFrequencyToKhz
};

constexpr uint16_t FREQUENCY_STEPS_HZ[] = {
    1, 5, 10, 20, 50, 100, 250, 500, 1000
};
constexpr uint8_t FREQUENCY_STEP_COUNT =
    sizeof(FREQUENCY_STEPS_HZ) / sizeof(FREQUENCY_STEPS_HZ[0]);
constexpr uint8_t FREQUENCY_STEP_KEY = 1;
constexpr uint8_t FREQUENCY_STEP_50_HZ_INDEX = 4;
constexpr uint8_t FREQUENCY_STEP_100_HZ_INDEX = 5;
constexpr uint32_t DOUBLE_CLICK_WINDOW_MS = 350;
constexpr uint32_t LONG_PRESS_MS = 700;

// SmartSDR rfpower uses fixed percentage values.
constexpr uint8_t DEFAULT_RF_POWER_PERCENT[NeoKeyConfig::KEY_COUNT] = {
    0, 0, 0,
    2, 4, 10,
    20, 40, 60,
    80, 90, 0
};

constexpr uint64_t SIX_METER_MIN_HZ = 50000000ULL;
constexpr uint64_t SIX_METER_MAX_HZ = 54000000ULL;

constexpr Action action(const uint8_t key)
{
    return key >= 1 && key <= NeoKeyConfig::KEY_COUNT
               ? BUTTON_ACTIONS[key - 1]
               : Action::None;
}

constexpr uint8_t defaultRfPowerPercent(const uint8_t key)
{
    return key >= 1 && key <= NeoKeyConfig::KEY_COUNT
               ? DEFAULT_RF_POWER_PERCENT[key - 1]
               : 0;
}

inline uint8_t rfPowerKeyForPercent(const uint16_t percent)
{
    for (uint8_t key = 1; key <= NeoKeyConfig::KEY_COUNT; ++key)
    {
        if (action(key) == Action::RfPowerPreset &&
            defaultRfPowerPercent(key) == percent)
        {
            return key;
        }
    }

    return 0;
}

constexpr bool isSixMeterFrequency(const uint64_t frequencyHz)
{
    return frequencyHz >= SIX_METER_MIN_HZ &&
           frequencyHz <= SIX_METER_MAX_HZ;
}

constexpr uint16_t frequencyStepHz(const uint8_t index)
{
    return FREQUENCY_STEPS_HZ[index % FREQUENCY_STEP_COUNT];
}

static_assert(sizeof(BUTTON_ACTIONS) / sizeof(BUTTON_ACTIONS[0]) ==
                  NeoKeyConfig::KEY_COUNT,
              "Each NeoKey button must have one action");
static_assert(FREQUENCY_STEP_COUNT > 0,
              "At least one encoder frequency step is required");
static_assert(FREQUENCY_STEPS_HZ[FREQUENCY_STEP_50_HZ_INDEX] == 50,
              "The double-click frequency step must be 50 Hz");
static_assert(FREQUENCY_STEPS_HZ[FREQUENCY_STEP_100_HZ_INDEX] == 100,
              "The long-press frequency step must be 100 Hz");

static_assert(sizeof(DEFAULT_RF_POWER_PERCENT) /
                      sizeof(DEFAULT_RF_POWER_PERCENT[0]) ==
                  NeoKeyConfig::KEY_COUNT,
              "Each NeoKey button must have one RF-power percentage");
static_assert(DEFAULT_RF_POWER_PERCENT[10] <= 100,
              "RF-power presets must not exceed 100 percent");
} // namespace ButtonConfig
