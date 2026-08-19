#include "KeyLighting.h"

#include <Adafruit_NeoPixel.h>

#include "NeoKey.h"
#include "NeoKeyConfig.h"
#include "NeoPixelConfig.h"

namespace
{
struct Color
{
    uint8_t red;
    uint8_t green;
    uint8_t blue;
};

constexpr Color GREEN = {0, NeoPixelConfig::PRESSED_GREEN, 0};
constexpr Color RED = {NeoPixelConfig::HELD_RED, 0, 0};
constexpr uint32_t STARTUP_STEP_MS = 100;

Adafruit_NeoPixel pixels(NeoPixelConfig::PIXEL_COUNT,
                         NeoPixelConfig::DATA_PIN,
                         NEO_GRB + NEO_KHZ800);

bool previousPressed[NeoKeyConfig::KEY_COUNT] = {};
uint32_t pressedSinceMs[NeoKeyConfig::KEY_COUNT] = {};
uint32_t releasedSinceMs[NeoKeyConfig::KEY_COUNT] = {};
Color releaseStartColors[NeoKeyConfig::KEY_COUNT] = {};
Color displayedColors[NeoKeyConfig::KEY_COUNT] = {};

uint32_t previousUpdateMs = 0;
bool pixelsPrepared = false;
bool initialized = false;

bool colorsEqual(const Color &left, const Color &right)
{
    return left.red == right.red &&
           left.green == right.green &&
           left.blue == right.blue;
}

Color backgroundColor(const uint8_t keyIndex)
{
    return {
        NeoPixelConfig::KEY_BACKGROUND_RGB[keyIndex][0],
        NeoPixelConfig::KEY_BACKGROUND_RGB[keyIndex][1],
        NeoPixelConfig::KEY_BACKGROUND_RGB[keyIndex][2]
    };
}

uint8_t interpolateChannel(const uint8_t from,
                           const uint8_t to,
                           const uint32_t elapsedMs,
                           const uint32_t durationMs)
{
    if (elapsedMs >= durationMs)
    {
        return to;
    }

    const int32_t difference = static_cast<int32_t>(to) - from;
    return static_cast<uint8_t>(
        static_cast<int32_t>(from) +
        (difference * static_cast<int32_t>(elapsedMs)) /
            static_cast<int32_t>(durationMs));
}

Color interpolateColor(const Color &from,
                       const Color &to,
                       const uint32_t elapsedMs,
                       const uint32_t durationMs)
{
    return {
        interpolateChannel(from.red, to.red, elapsedMs, durationMs),
        interpolateChannel(from.green, to.green, elapsedMs, durationMs),
        interpolateChannel(from.blue, to.blue, elapsedMs, durationMs)
    };
}

Color pressedColor(const uint32_t heldMs,
                   const Color &currentBackground)
{
    if (heldMs < NeoPixelConfig::GREEN_FLASH_MS)
    {
        return GREEN;
    }

    if (heldMs < NeoPixelConfig::HOLD_THRESHOLD_MS)
    {
        return interpolateColor(
            GREEN,
            currentBackground,
            heldMs - NeoPixelConfig::GREEN_FLASH_MS,
            NeoPixelConfig::HOLD_THRESHOLD_MS -
                NeoPixelConfig::GREEN_FLASH_MS);
    }

    const uint32_t blinkPhase =
        (heldMs - NeoPixelConfig::HOLD_THRESHOLD_MS) /
        NeoPixelConfig::HOLD_BLINK_INTERVAL_MS;
    return (blinkPhase % 2U) == 0U ? RED : currentBackground;
}

void setKeyColor(const uint8_t keyIndex, const Color &color)
{
    displayedColors[keyIndex] = color;
    pixels.setPixelColor(NeoPixelConfig::KEY_PIXEL_MAP[keyIndex],
                         pixels.Color(color.red, color.green, color.blue));
}

void runStartupLight()
{
    for (uint8_t keyIndex = 0;
         keyIndex < NeoKeyConfig::KEY_COUNT;
         ++keyIndex)
    {
        pixels.clear();
        pixels.setPixelColor(
            NeoPixelConfig::KEY_PIXEL_MAP[keyIndex],
            pixels.Color(GREEN.red, GREEN.green, GREEN.blue));
        pixels.show();
        delay(STARTUP_STEP_MS);
    }
}

void preparePixels()
{
    if (pixelsPrepared)
    {
        return;
    }

    pixels.begin();
    pixels.setBrightness(NeoPixelConfig::BRIGHTNESS);
    pixels.clear();
    pixels.show();
    pixelsPrepared = true;
}
} // namespace

namespace KeyLighting
{
void clearForStartup()
{
    preparePixels();
}

void begin()
{
    preparePixels();
    runStartupLight();
    pixels.clear();

    const uint32_t nowMs = millis();
    for (uint8_t keyIndex = 0;
         keyIndex < NeoKeyConfig::KEY_COUNT;
         ++keyIndex)
    {
        previousPressed[keyIndex] = false;
        pressedSinceMs[keyIndex] = nowMs;
        releasedSinceMs[keyIndex] = nowMs;
        const Color background = backgroundColor(keyIndex);
        releaseStartColors[keyIndex] = background;
        setKeyColor(keyIndex, background);
    }

    // Pixels without an associated key remain off.
    pixels.show();
    previousUpdateMs = nowMs;
    initialized = true;
}

void update(const uint32_t nowMs)
{
    if (!initialized ||
        nowMs - previousUpdateMs < NeoPixelConfig::UPDATE_INTERVAL_MS)
    {
        return;
    }
    previousUpdateMs = nowMs;

    bool changed = false;
    for (uint8_t keyIndex = 0;
         keyIndex < NeoKeyConfig::KEY_COUNT;
         ++keyIndex)
    {
        const uint8_t key = static_cast<uint8_t>(keyIndex + 1U);
        const bool pressed = NeoKey::isKeyPressed(key);
        const Color currentBackground = backgroundColor(keyIndex);

        if (pressed && !previousPressed[keyIndex])
        {
            pressedSinceMs[keyIndex] = nowMs;
        }
        else if (!pressed && previousPressed[keyIndex])
        {
            releasedSinceMs[keyIndex] = nowMs;
            releaseStartColors[keyIndex] = displayedColors[keyIndex];
        }

        Color nextColor = currentBackground;
        if (pressed)
        {
            nextColor = pressedColor(nowMs - pressedSinceMs[keyIndex],
                                     currentBackground);
        }
        else
        {
            nextColor = interpolateColor(
                releaseStartColors[keyIndex],
                currentBackground,
                nowMs - releasedSinceMs[keyIndex],
                NeoPixelConfig::RELEASE_FADE_MS);
        }

        previousPressed[keyIndex] = pressed;
        if (!colorsEqual(nextColor, displayedColors[keyIndex]))
        {
            setKeyColor(keyIndex, nextColor);
            changed = true;
        }
    }

    if (changed)
    {
        pixels.show();
    }
}
} // namespace KeyLighting
