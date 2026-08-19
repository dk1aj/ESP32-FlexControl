#pragma once

#include <stdint.h>

namespace NeoKey
{
void begin();
void update();

bool isKeyPressed(uint8_t key);
bool wasKeyPressed(uint8_t key);
bool wasKeyReleased(uint8_t key);
const char *keyName(uint8_t key);
} // namespace NeoKey
