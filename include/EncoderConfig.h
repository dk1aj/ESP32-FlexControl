#pragma once

#include <driver/gpio.h>
#include <stdint.h>

namespace EncoderConfig
{
constexpr gpio_num_t ENCODER_PIN_A = GPIO_NUM_4;
constexpr gpio_num_t ENCODER_PIN_B = GPIO_NUM_5;

constexpr uint32_t SERIAL_BAUD_RATE = 115200;
constexpr int ENCODER_PULSES_PER_REVOLUTION = 100;

// Generic ESP32-S3-DevKitC-1 boards commonly use GPIO48 for the WS2812 RGB LED.
// Change this pin if the actual N16R8 board routes its onboard LED differently.
constexpr uint8_t RGB_LED_PIN = 48;
constexpr bool HEARTBEAT_ENABLED = false;
constexpr uint32_t HEARTBEAT_PERIOD_MS = 1600;
constexpr uint32_t HEARTBEAT_UPDATE_INTERVAL_MS = 20;
constexpr uint8_t HEARTBEAT_MAX_BRIGHTNESS = 8;

// PCNT uses x4 quadrature decoding: four counts per A/B pulse period.
// Change this to 100 or 200 only if the selected decoding/wheel differs.
constexpr int ENCODER_COUNTS_PER_REVOLUTION = 400;
constexpr int ENCODER_COUNTS_PER_DETENT = 4;

constexpr bool CLOCKWISE_TUNES_UP = true;

// PCNT's legacy filter is expressed in APB clock cycles (normally 12.5 ns).
// 800 cycles suppress pulses shorter than about 10 us. Valid range: 0..1023;
// set to 0 to disable. Mechanical contacts may still need external conditioning.
constexpr uint16_t ENCODER_GLITCH_FILTER_CYCLES = 800;

// Suitable for dry contacts or open-collector outputs. Do not enable an
// internal pull-up if an external level shifter already provides one.
constexpr bool ENCODER_ENABLE_INTERNAL_PULLUPS = true;

static_assert(ENCODER_COUNTS_PER_REVOLUTION > 0,
              "Encoder counts per revolution must be positive");
static_assert(ENCODER_COUNTS_PER_DETENT > 0,
              "Encoder counts per detent must be positive");
static_assert(ENCODER_COUNTS_PER_REVOLUTION % ENCODER_COUNTS_PER_DETENT == 0,
              "Encoder counts per revolution must contain complete detents");
static_assert(ENCODER_GLITCH_FILTER_CYCLES <= 1023,
              "ESP32-S3 PCNT filter accepts at most 1023 APB cycles");
} // namespace EncoderConfig
