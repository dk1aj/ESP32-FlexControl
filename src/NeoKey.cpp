#include "NeoKey.h"

#include <Arduino.h>

#include "NeoKeyConfig.h"

namespace
{
bool rawKeyStates[NeoKeyConfig::KEY_COUNT] = {};
bool stableKeyStates[NeoKeyConfig::KEY_COUNT] = {};
bool pressedEvents[NeoKeyConfig::KEY_COUNT] = {};
bool releasedEvents[NeoKeyConfig::KEY_COUNT] = {};
uint32_t rawStateChangedMs[NeoKeyConfig::KEY_COUNT] = {};

uint32_t previousScanUs = 0;
bool initialized = false;

bool isValidKey(const uint8_t key)
{
    return key >= 1 && key <= NeoKeyConfig::KEY_COUNT;
}

void deactivateColumn(const uint8_t column)
{
    pinMode(NeoKeyConfig::COLUMN_PINS[column], INPUT);
}

void scanMatrix(bool (&snapshot)[NeoKeyConfig::KEY_COUNT])
{
    for (uint8_t column = 0; column < NeoKeyConfig::COLUMN_COUNT; ++column)
    {
        const uint8_t columnPin = NeoKeyConfig::COLUMN_PINS[column];

        digitalWrite(columnPin, LOW);
        pinMode(columnPin, OUTPUT);
        delayMicroseconds(NeoKeyConfig::COLUMN_SETTLE_US);

        for (uint8_t row = 0; row < NeoKeyConfig::ROW_COUNT; ++row)
        {
            const uint8_t keyIndex =
                static_cast<uint8_t>(row * NeoKeyConfig::COLUMN_COUNT + column);
            snapshot[keyIndex] =
                digitalRead(NeoKeyConfig::ROW_PINS[row]) == LOW;
        }

        deactivateColumn(column);
    }
}

bool consumeEvent(bool (&events)[NeoKeyConfig::KEY_COUNT], const uint8_t key)
{
    if (!isValidKey(key))
    {
        return false;
    }

    const uint8_t keyIndex = static_cast<uint8_t>(key - 1U);
    const bool occurred = events[keyIndex];
    events[keyIndex] = false;
    return occurred;
}
} // namespace

namespace NeoKey
{
void begin()
{
    for (uint8_t row = 0; row < NeoKeyConfig::ROW_COUNT; ++row)
    {
        pinMode(NeoKeyConfig::ROW_PINS[row], INPUT_PULLUP);
    }

    for (uint8_t column = 0; column < NeoKeyConfig::COLUMN_COUNT; ++column)
    {
        digitalWrite(NeoKeyConfig::COLUMN_PINS[column], LOW);
        deactivateColumn(column);
    }

    const uint32_t nowMs = millis();
    for (uint8_t keyIndex = 0; keyIndex < NeoKeyConfig::KEY_COUNT; ++keyIndex)
    {
        rawKeyStates[keyIndex] = false;
        stableKeyStates[keyIndex] = false;
        pressedEvents[keyIndex] = false;
        releasedEvents[keyIndex] = false;
        rawStateChangedMs[keyIndex] = nowMs;
    }

    previousScanUs = micros();
    initialized = true;
}

void update()
{
    if (!initialized)
    {
        return;
    }

    const uint32_t nowUs = micros();
    const uint32_t elapsedUs = nowUs - previousScanUs;
    if (elapsedUs < NeoKeyConfig::SCAN_INTERVAL_US)
    {
        return;
    }

    const uint32_t elapsedScans =
        elapsedUs / NeoKeyConfig::SCAN_INTERVAL_US;
    previousScanUs += elapsedScans * NeoKeyConfig::SCAN_INTERVAL_US;

    bool snapshot[NeoKeyConfig::KEY_COUNT] = {};
    scanMatrix(snapshot);

    const uint32_t nowMs = millis();
    for (uint8_t keyIndex = 0; keyIndex < NeoKeyConfig::KEY_COUNT; ++keyIndex)
    {
        if (snapshot[keyIndex] != rawKeyStates[keyIndex])
        {
            rawKeyStates[keyIndex] = snapshot[keyIndex];
            rawStateChangedMs[keyIndex] = nowMs;
            continue;
        }

        if (rawKeyStates[keyIndex] == stableKeyStates[keyIndex] ||
            nowMs - rawStateChangedMs[keyIndex] < NeoKeyConfig::DEBOUNCE_MS)
        {
            continue;
        }

        stableKeyStates[keyIndex] = rawKeyStates[keyIndex];
        if (stableKeyStates[keyIndex])
        {
            pressedEvents[keyIndex] = true;
        }
        else
        {
            releasedEvents[keyIndex] = true;
        }
    }
}

bool isKeyPressed(const uint8_t key)
{
    return isValidKey(key) && stableKeyStates[key - 1U];
}

bool wasKeyPressed(const uint8_t key)
{
    return consumeEvent(pressedEvents, key);
}

bool wasKeyReleased(const uint8_t key)
{
    return consumeEvent(releasedEvents, key);
}

const char *keyName(const uint8_t key)
{
    return isValidKey(key) ? NeoKeyConfig::KEY_NAMES[key - 1U]
                           : "Unbekannte Taste";
}
} // namespace NeoKey
