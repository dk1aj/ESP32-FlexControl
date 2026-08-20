#pragma once

#include <Arduino.h>

namespace SmartSdrConnection
{
enum class State : uint8_t
{
    Idle,
    Discovering,
    RadioFound,
    Connecting,
    Connected,
    Ready
};

enum class RfPowerRequestState : uint8_t
{
    Idle,
    Pending,
    Confirmed,
    Failed
};

void begin();
void update();
State state();
bool hasRfPower();
uint16_t rfPowerSetting();
bool activeSliceFrequencyHz(uint64_t &frequencyHz);
bool requestRfPowerPercent(uint8_t percent);
RfPowerRequestState rfPowerRequestState();
void clearRfPowerRequestResult();
bool tuneActiveSliceByHz(int64_t deltaHz);
IPAddress radioIp();
uint16_t radioPort();
const char *radioModel();
const char *radioName();
} // namespace SmartSdrConnection
