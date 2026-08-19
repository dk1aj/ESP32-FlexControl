#pragma once

#include <stdint.h>

namespace RadioConfig
{
constexpr uint8_t STATIC_IP_OCTETS[4] = {192, 168, 178, 70};
constexpr uint16_t STATIC_API_PORT = 4992;
constexpr uint32_t DISCOVERY_WAIT_MS = 5000;

static_assert(STATIC_API_PORT > 0,
              "The configured SmartSDR TCP port must be valid");
static_assert(DISCOVERY_WAIT_MS > 0,
              "The discovery wait interval must be positive");
} // namespace RadioConfig
