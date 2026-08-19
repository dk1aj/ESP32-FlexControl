#pragma once

#include <Arduino.h>

namespace WifiProvisioning
{
enum class State : uint8_t
{
    Idle,
    Connecting,
    Portal,
    Connected
};

void begin();
void update();
State state();
bool isConnected();
IPAddress localIp();
const char *setupAccessPointName();
} // namespace WifiProvisioning
