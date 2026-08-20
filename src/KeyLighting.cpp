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
constexpr Color OFF = {0, 0, 0};
constexpr Color ACTIVE_BLINK_BLUE = {
    0, 0, NeoPixelConfig::ACTIVE_KEY_BLINK_BLUE_LEVEL};
constexpr Color ACTIVE_STEADY_BLUE = {
    0, 0, NeoPixelConfig::ACTIVE_KEY_STEADY_BLUE_LEVEL};
constexpr uint8_t RADIO_ANIMATION_STEP_COUNT = NeoKeyConfig::ROW_COUNT;
constexpr uint32_t RADIO_ANIMATION_STEP_MS = 100;

Adafruit_NeoPixel pixels(NeoPixelConfig::PIXEL_COUNT,
                         NeoPixelConfig::DATA_PIN,
                         NEO_GRB + NEO_KHZ800);

bool previousPressed[NeoKeyConfig::KEY_COUNT] = {};
uint32_t pressedSinceMs[NeoKeyConfig::KEY_COUNT] = {};
uint32_t releasedSinceMs[NeoKeyConfig::KEY_COUNT] = {};
Color releaseStartColors[NeoKeyConfig::KEY_COUNT] = {};
Color displayedColors[NeoKeyConfig::KEY_COUNT] = {};

uint32_t previousUpdateMs = 0;
uint32_t radioAnimationStartedMs = 0;
uint8_t displayedRadioAnimationRow = UINT8_MAX;
bool radioAnimationActive = false;
uint8_t blinkingKey = 0;
uint32_t blinkingKeySelectedMs = 0;
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

Color restingColor(const uint8_t keyIndex, const uint32_t nowMs)
{
    if (blinkingKey != keyIndex + 1U)
    {
        return backgroundColor(keyIndex);
    }

    const uint32_t selectedMs = nowMs - blinkingKeySelectedMs;
    if (selectedMs >= NeoPixelConfig::ACTIVE_KEY_BLINK_DURATION_MS)
    {
        return ACTIVE_STEADY_BLUE;
    }

    const bool blinkOn =
        ((selectedMs /
          NeoPixelConfig::ACTIVE_KEY_BLINK_INTERVAL_MS) % 2U) == 0U;
    return blinkOn ? ACTIVE_BLINK_BLUE : OFF;
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

void showRadioAnimationStep(const uint8_t animationRow)
{
    pixels.clear();

    // Both logical axes are reversed because the NeoKey is mounted upside
    // down. Reverse the row for top-to-bottom motion and select the last
    // logical column, which is the physical left column.
    const uint8_t logicalRow = static_cast<uint8_t>(
        NeoKeyConfig::ROW_COUNT - 1U - animationRow);
    const uint8_t logicalColumn = static_cast<uint8_t>(
        NeoKeyConfig::COLUMN_COUNT - 1U);
    const uint8_t keyIndex = static_cast<uint8_t>(
        logicalRow * NeoKeyConfig::COLUMN_COUNT + logicalColumn);
    pixels.setPixelColor(
        NeoPixelConfig::KEY_PIXEL_MAP[keyIndex],
        pixels.Color(GREEN.red, GREEN.green, GREEN.blue));
    pixels.show();
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

void startRadioConnectedAnimation(const uint32_t nowMs)
{
    if (!initialized)
    {
        return;
    }

    radioAnimationStartedMs = nowMs;
    displayedRadioAnimationRow = UINT8_MAX;
    radioAnimationActive = true;
}

void selectBlinkingKey(const uint8_t key, const uint32_t nowMs)
{
    if (!initialized || key < 1 || key > NeoKeyConfig::KEY_COUNT)
    {
        return;
    }
    blinkingKey = key;
    blinkingKeySelectedMs = nowMs;
}

void clearBlinkingKey()
{
    blinkingKey = 0;
    blinkingKeySelectedMs = 0;
}

void update(const uint32_t nowMs)
{
    if (!initialized ||
        nowMs - previousUpdateMs < NeoPixelConfig::UPDATE_INTERVAL_MS)
    {
        return;
    }
    previousUpdateMs = nowMs;

    if (radioAnimationActive)
    {
        const uint32_t elapsedMs = nowMs - radioAnimationStartedMs;
        const uint8_t animationRow = static_cast<uint8_t>(
            elapsedMs / RADIO_ANIMATION_STEP_MS);
        if (animationRow < RADIO_ANIMATION_STEP_COUNT)
        {
            if (animationRow != displayedRadioAnimationRow)
            {
                showRadioAnimationStep(animationRow);
                displayedRadioAnimationRow = animationRow;
            }
            return;
        }

        radioAnimationActive = false;
        displayedRadioAnimationRow = UINT8_MAX;
        for (uint8_t keyIndex = 0;
             keyIndex < NeoKeyConfig::KEY_COUNT;
             ++keyIndex)
        {
            displayedColors[keyIndex] = {UINT8_MAX, UINT8_MAX, UINT8_MAX};
        }
    }

    bool changed = false;
    for (uint8_t keyIndex = 0;
         keyIndex < NeoKeyConfig::KEY_COUNT;
         ++keyIndex)
    {
        const uint8_t key = static_cast<uint8_t>(keyIndex + 1U);
        const bool pressed = NeoKey::isKeyPressed(key);
        const Color currentBackground = restingColor(keyIndex, nowMs);

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
