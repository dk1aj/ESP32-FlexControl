#pragma once

#include <stdint.h>

namespace KeyLighting
{
void clearForStartup();
void begin();
void update(uint32_t nowMs);
} // namespace KeyLighting
