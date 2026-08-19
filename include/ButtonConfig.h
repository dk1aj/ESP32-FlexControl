#pragma once

#include <stdint.h>

#include "NeoKeyConfig.h"

namespace ButtonConfig
{
enum class Action : uint8_t
{
    CycleFrequencyStep,
    RfPowerPreset
};

// Button assignment. Button 1 cycles the encoder tuning step; all remaining
// buttons retain their provisional RF-power presets.
constexpr Action BUTTON_ACTIONS[NeoKeyConfig::KEY_COUNT] = {
    Action::CycleFrequencyStep,
    Action::RfPowerPreset,
    Action::RfPowerPreset,
    Action::RfPowerPreset,
    Action::RfPowerPreset,
    Action::RfPowerPreset,
    Action::RfPowerPreset,
    Action::RfPowerPreset,
    Action::RfPowerPreset,
    Action::RfPowerPreset,
    Action::RfPowerPreset,
    Action::RfPowerPreset
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

// Initial RF-power presets for the twelve NeoKey buttons. These are defaults
// only; a later persistent/external configuration can override them without
// changing the matrix scanner or the radio transport.
constexpr uint16_t DEFAULT_RF_POWER_WATTS[NeoKeyConfig::KEY_COUNT] = {
    45, 85, 125,
    170, 210, 250,
    295, 335, 375,
    420, 460, 500
};

constexpr uint16_t RF_POWER_STEP_WATTS = 5;
constexpr uint16_t RF_POWER_MAX_WATTS = 500;

constexpr uint16_t defaultRfPowerWatts(const uint8_t key)
{
    return key >= 1 && key <= NeoKeyConfig::KEY_COUNT
               ? DEFAULT_RF_POWER_WATTS[key - 1]
               : 0;
}

constexpr Action action(const uint8_t key)
{
    return key >= 1 && key <= NeoKeyConfig::KEY_COUNT
               ? BUTTON_ACTIONS[key - 1]
               : Action::RfPowerPreset;
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

static_assert(sizeof(DEFAULT_RF_POWER_WATTS) /
                      sizeof(DEFAULT_RF_POWER_WATTS[0]) ==
                  NeoKeyConfig::KEY_COUNT,
              "Each NeoKey button must have one RF-power preset");
static_assert(DEFAULT_RF_POWER_WATTS[0] % RF_POWER_STEP_WATTS == 0 &&
                  DEFAULT_RF_POWER_WATTS[1] % RF_POWER_STEP_WATTS == 0 &&
                  DEFAULT_RF_POWER_WATTS[2] % RF_POWER_STEP_WATTS == 0 &&
                  DEFAULT_RF_POWER_WATTS[3] % RF_POWER_STEP_WATTS == 0 &&
                  DEFAULT_RF_POWER_WATTS[4] % RF_POWER_STEP_WATTS == 0 &&
                  DEFAULT_RF_POWER_WATTS[5] % RF_POWER_STEP_WATTS == 0 &&
                  DEFAULT_RF_POWER_WATTS[6] % RF_POWER_STEP_WATTS == 0 &&
                  DEFAULT_RF_POWER_WATTS[7] % RF_POWER_STEP_WATTS == 0 &&
                  DEFAULT_RF_POWER_WATTS[8] % RF_POWER_STEP_WATTS == 0 &&
                  DEFAULT_RF_POWER_WATTS[9] % RF_POWER_STEP_WATTS == 0 &&
                  DEFAULT_RF_POWER_WATTS[10] % RF_POWER_STEP_WATTS == 0 &&
                  DEFAULT_RF_POWER_WATTS[11] % RF_POWER_STEP_WATTS == 0,
              "RF-power presets must use 5 W steps");
static_assert(DEFAULT_RF_POWER_WATTS[11] <= RF_POWER_MAX_WATTS,
              "RF-power presets must not exceed the radio maximum");
} // namespace ButtonConfig
