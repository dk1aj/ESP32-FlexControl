#pragma once

#include <stdint.h>

namespace KeyLighting
{
void clearForStartup();
void begin();
void startRadioConnectedAnimation(uint32_t nowMs);
void selectBlinkingKey(uint8_t key, uint32_t nowMs);
void clearBlinkingKey();
void update(uint32_t nowMs);
} // namespace KeyLighting
