#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <WiFi.h>
#include <driver/pcnt.h>
#include <esp_err.h>
#include <esp_system.h>

#include "ButtonConfig.h"
#include "EncoderConfig.h"
#include "KeyLighting.h"
#include "NeoKey.h"
#include "NeoKeyConfig.h"
#include "NeoPixelConfig.h"
#include "SmartSdrConnection.h"
#include "WifiProvisioning.h"

namespace
{
constexpr pcnt_unit_t ENCODER_PCNT_UNIT = PCNT_UNIT_0;
constexpr int16_t PCNT_HIGH_LIMIT = 30000;
constexpr int16_t PCNT_LOW_LIMIT = -30000;

Adafruit_NeoPixel onboardHeartbeat(1,
                                   EncoderConfig::RGB_LED_PIN,
                                   NEO_GRB + NEO_KHZ800);

portMUX_TYPE encoderMux = portMUX_INITIALIZER_UNLOCKED;
volatile int64_t encoderLimitOffset = 0;
int64_t zeroOffset = 0;
uint8_t frequencyStepIndex = 0;
uint8_t pendingRfPowerKey = 0;

enum class StepKeyState : uint8_t
{
    Idle,
    FirstPressed,
    WaitingForSecondClick,
    SecondPressed,
    LongPressHandled
};

StepKeyState stepKeyState = StepKeyState::Idle;
uint32_t stepKeyPressedMs = 0;
uint32_t firstClickReleasedMs = 0;

void checkEspError(const esp_err_t error, const char *operation)
{
    if (error == ESP_OK)
    {
        return;
    }

    Serial.printf("[FATAL] %s failed: %s\n",
                  operation,
                  esp_err_to_name(error));
    while (true)
    {
        delay(1000);
    }
}

const char *resetReasonName(const esp_reset_reason_t reason)
{
    switch (reason)
    {
    case ESP_RST_POWERON:
        return "power-on";
    case ESP_RST_EXT:
        return "external reset";
    case ESP_RST_SW:
        return "software reset";
    case ESP_RST_PANIC:
        return "panic";
    case ESP_RST_INT_WDT:
        return "interrupt watchdog";
    case ESP_RST_TASK_WDT:
        return "task watchdog";
    case ESP_RST_WDT:
        return "other watchdog";
    case ESP_RST_DEEPSLEEP:
        return "deep-sleep wake";
    case ESP_RST_BROWNOUT:
        return "brownout";
    case ESP_RST_SDIO:
        return "SDIO reset";
    case ESP_RST_UNKNOWN:
    default:
        return "unknown";
    }
}

const char *wifiStateName(const WifiProvisioning::State state)
{
    switch (state)
    {
    case WifiProvisioning::State::Idle:
        return "Idle";
    case WifiProvisioning::State::Connecting:
        return "Connecting";
    case WifiProvisioning::State::Portal:
        return "Portal";
    case WifiProvisioning::State::Connected:
        return "Connected";
    }
    return "Unknown";
}

const char *radioStateName(const SmartSdrConnection::State state)
{
    switch (state)
    {
    case SmartSdrConnection::State::Idle:
        return "Idle";
    case SmartSdrConnection::State::Discovering:
        return "Discovering";
    case SmartSdrConnection::State::RadioFound:
        return "RadioFound";
    case SmartSdrConnection::State::Connecting:
        return "Connecting";
    case SmartSdrConnection::State::Connected:
        return "Connected";
    case SmartSdrConnection::State::Ready:
        return "Ready";
    }
    return "Unknown";
}

void IRAM_ATTR onPcntLimit(void *)
{
    uint32_t eventStatus = 0;
    pcnt_get_event_status(ENCODER_PCNT_UNIT, &eventStatus);

    portENTER_CRITICAL_ISR(&encoderMux);
    if ((eventStatus & PCNT_EVT_H_LIM) != 0U)
    {
        encoderLimitOffset += PCNT_HIGH_LIMIT;
    }
    if ((eventStatus & PCNT_EVT_L_LIM) != 0U)
    {
        encoderLimitOffset += PCNT_LOW_LIMIT;
    }
    portEXIT_CRITICAL_ISR(&encoderMux);

    // Resetting at each limit gives the 16-bit peripheral room to continue.
    pcnt_counter_clear(ENCODER_PCNT_UNIT);
}

pcnt_config_t makeChannelConfig(const gpio_num_t pulsePin,
                                const gpio_num_t controlPin,
                                const pcnt_channel_t channel,
                                const pcnt_count_mode_t positiveMode,
                                const pcnt_count_mode_t negativeMode)
{
    pcnt_config_t config = {};
    config.pulse_gpio_num = static_cast<int>(pulsePin);
    config.ctrl_gpio_num = static_cast<int>(controlPin);
    config.lctrl_mode = PCNT_MODE_REVERSE;
    config.hctrl_mode = PCNT_MODE_KEEP;
    config.pos_mode = positiveMode;
    config.neg_mode = negativeMode;
    config.counter_h_lim = PCNT_HIGH_LIMIT;
    config.counter_l_lim = PCNT_LOW_LIMIT;
    config.unit = ENCODER_PCNT_UNIT;
    config.channel = channel;
    return config;
}

void configureEncoder()
{
    const pcnt_config_t channelA =
        makeChannelConfig(EncoderConfig::ENCODER_PIN_A,
                          EncoderConfig::ENCODER_PIN_B,
                          PCNT_CHANNEL_0,
                          PCNT_COUNT_INC,
                          PCNT_COUNT_DEC);
    const pcnt_config_t channelB =
        makeChannelConfig(EncoderConfig::ENCODER_PIN_B,
                          EncoderConfig::ENCODER_PIN_A,
                          PCNT_CHANNEL_1,
                          PCNT_COUNT_DEC,
                          PCNT_COUNT_INC);

    checkEspError(pcnt_unit_config(&channelA), "PCNT channel A configuration");
    checkEspError(pcnt_unit_config(&channelB), "PCNT channel B configuration");

    const gpio_pullup_t pullup =
        EncoderConfig::ENCODER_ENABLE_INTERNAL_PULLUPS ? GPIO_PULLUP_ENABLE
                                                       : GPIO_PULLUP_DISABLE;
    checkEspError(gpio_set_pull_mode(EncoderConfig::ENCODER_PIN_A,
                                    pullup == GPIO_PULLUP_ENABLE
                                        ? GPIO_PULLUP_ONLY
                                        : GPIO_FLOATING),
                  "GPIO A pull mode");
    checkEspError(gpio_set_pull_mode(EncoderConfig::ENCODER_PIN_B,
                                    pullup == GPIO_PULLUP_ENABLE
                                        ? GPIO_PULLUP_ONLY
                                        : GPIO_FLOATING),
                  "GPIO B pull mode");

    if (EncoderConfig::ENCODER_GLITCH_FILTER_CYCLES > 0)
    {
        checkEspError(
            pcnt_set_filter_value(ENCODER_PCNT_UNIT,
                                  EncoderConfig::ENCODER_GLITCH_FILTER_CYCLES),
            "PCNT filter value");
        checkEspError(pcnt_filter_enable(ENCODER_PCNT_UNIT), "PCNT filter enable");
    }
    else
    {
        checkEspError(pcnt_filter_disable(ENCODER_PCNT_UNIT), "PCNT filter disable");
    }

    checkEspError(pcnt_event_enable(ENCODER_PCNT_UNIT, PCNT_EVT_H_LIM),
                  "PCNT high-limit event");
    checkEspError(pcnt_event_enable(ENCODER_PCNT_UNIT, PCNT_EVT_L_LIM),
                  "PCNT low-limit event");
    checkEspError(pcnt_isr_service_install(ESP_INTR_FLAG_IRAM),
                  "PCNT ISR service");
    checkEspError(pcnt_isr_handler_add(ENCODER_PCNT_UNIT, onPcntLimit, nullptr),
                  "PCNT ISR handler");
    checkEspError(pcnt_counter_clear(ENCODER_PCNT_UNIT), "PCNT counter clear");
    checkEspError(pcnt_counter_resume(ENCODER_PCNT_UNIT), "PCNT counter start");
}

int64_t readRawEncoderCount()
{
    int16_t hardwareCount = 0;
    int64_t limitOffset = 0;

    portENTER_CRITICAL(&encoderMux);
    pcnt_get_counter_value(ENCODER_PCNT_UNIT, &hardwareCount);
    limitOffset = encoderLimitOffset;
    portEXIT_CRITICAL(&encoderMux);

    return limitOffset + hardwareCount;
}

void updateHeartbeat()
{
    static uint32_t previousColor = UINT32_MAX;
    const bool blinkOn = ((millis() / 400U) % 2U) == 0U;
    uint8_t red = 0;
    uint8_t green = 0;
    uint8_t blue = 0;

    switch (WifiProvisioning::state())
    {
    case WifiProvisioning::State::Idle:
        break;
    case WifiProvisioning::State::Connecting:
        blue = blinkOn ? 8 : 0;
        break;
    case WifiProvisioning::State::Portal:
        red = blinkOn ? 8 : 0;
        green = blinkOn ? 3 : 0;
        break;
    case WifiProvisioning::State::Connected:
        switch (SmartSdrConnection::state())
        {
        case SmartSdrConnection::State::Idle:
        case SmartSdrConnection::State::Discovering:
            green = blinkOn ? 4 : 0;
            blue = blinkOn ? 8 : 0;
            break;
        case SmartSdrConnection::State::RadioFound:
        case SmartSdrConnection::State::Connecting:
            red = blinkOn ? 6 : 0;
            blue = blinkOn ? 8 : 0;
            break;
        case SmartSdrConnection::State::Connected:
            red = 6;
            green = 6;
            break;
        case SmartSdrConnection::State::Ready:
            green = 8;
            break;
        }
        break;
    }

    const uint32_t color = onboardHeartbeat.Color(red, green, blue);
    if (color != previousColor)
    {
        onboardHeartbeat.setPixelColor(0, color);
        onboardHeartbeat.show();
        previousColor = color;
    }
}

void selectFrequencyStep(const uint8_t index, const char *gesture)
{
    frequencyStepIndex = index;
    Serial.printf("[KEY] %u gesture=%s action=frequency-step step=%u Hz\n",
                  ButtonConfig::FREQUENCY_STEP_KEY,
                  gesture,
                  ButtonConfig::frequencyStepHz(frequencyStepIndex));
}

void cycleFrequencyStep()
{
    selectFrequencyStep(
        static_cast<uint8_t>((frequencyStepIndex + 1U) %
                             ButtonConfig::FREQUENCY_STEP_COUNT),
        "single-click");
}

void handleStepKeyPressed(const uint32_t nowMs)
{
    if (stepKeyState == StepKeyState::WaitingForSecondClick &&
        nowMs - firstClickReleasedMs <=
            ButtonConfig::DOUBLE_CLICK_WINDOW_MS)
    {
        stepKeyState = StepKeyState::SecondPressed;
    }
    else
    {
        if (stepKeyState == StepKeyState::WaitingForSecondClick)
        {
            cycleFrequencyStep();
        }
        stepKeyState = StepKeyState::FirstPressed;
    }
    stepKeyPressedMs = nowMs;
}

void handleStepKeyReleased(const uint32_t nowMs)
{
    if (stepKeyState == StepKeyState::LongPressHandled)
    {
        stepKeyState = StepKeyState::Idle;
        return;
    }

    if (nowMs - stepKeyPressedMs >= ButtonConfig::LONG_PRESS_MS)
    {
        selectFrequencyStep(ButtonConfig::FREQUENCY_STEP_100_HZ_INDEX,
                            "long-press");
        stepKeyState = StepKeyState::Idle;
    }
    else if (stepKeyState == StepKeyState::SecondPressed)
    {
        selectFrequencyStep(ButtonConfig::FREQUENCY_STEP_50_HZ_INDEX,
                            "double-click");
        stepKeyState = StepKeyState::Idle;
    }
    else
    {
        firstClickReleasedMs = nowMs;
        stepKeyState = StepKeyState::WaitingForSecondClick;
    }
}

void updateStepKeyGesture(const uint32_t nowMs)
{
    if ((stepKeyState == StepKeyState::FirstPressed ||
         stepKeyState == StepKeyState::SecondPressed) &&
        nowMs - stepKeyPressedMs >= ButtonConfig::LONG_PRESS_MS)
    {
        selectFrequencyStep(ButtonConfig::FREQUENCY_STEP_100_HZ_INDEX,
                            "long-press");
        stepKeyState = StepKeyState::LongPressHandled;
    }
    else if (stepKeyState == StepKeyState::WaitingForSecondClick &&
             nowMs - firstClickReleasedMs >
                 ButtonConfig::DOUBLE_CLICK_WINDOW_MS)
    {
        cycleFrequencyStep();
        stepKeyState = StepKeyState::Idle;
    }
}

void roundActiveSliceToNearestKhz()
{
    uint64_t frequencyHz = 0;
    if (!SmartSdrConnection::activeSliceFrequencyHz(frequencyHz))
    {
        Serial.println("[KEY] 12 round-to-kHz skipped: no unambiguous active slice");
        return;
    }

    const uint16_t remainderHz = static_cast<uint16_t>(frequencyHz % 1000ULL);
    const int64_t deltaHz = remainderHz < 500U
                                ? -static_cast<int64_t>(remainderHz)
                                : static_cast<int64_t>(1000U - remainderHz);
    if (deltaHz == 0)
    {
        Serial.printf("[KEY] 12 round-to-kHz unchanged: frequency=%llu kHz\n",
                      static_cast<unsigned long long>(frequencyHz / 1000ULL));
        return;
    }

    const bool transmitted =
        SmartSdrConnection::tuneActiveSliceByHz(deltaHz);
    Serial.printf("[KEY] 12 round-to-kHz direction=%s delta=%lld Hz transmitted=%s\n",
                  deltaHz > 0 ? "up" : "down",
                  static_cast<long long>(deltaHz),
                  transmitted ? "yes" : "no");
}

void processNeoKeyEvents(const uint32_t nowMs)
{
    for (uint8_t key = 1; key <= NeoKeyConfig::KEY_COUNT; ++key)
    {
        if (NeoKey::wasKeyPressed(key))
        {
            Serial.printf("[KEY] %u PRESSED name=\"%s\"\n",
                          key,
                          NeoKey::keyName(key));
            if (ButtonConfig::action(key) ==
                ButtonConfig::Action::CycleFrequencyStep)
            {
                handleStepKeyPressed(nowMs);
            }
            else if (ButtonConfig::action(key) ==
                     ButtonConfig::Action::RfPowerPreset)
            {
                KeyLighting::selectBlinkingKey(key, nowMs);
            }
            else if (ButtonConfig::action(key) ==
                     ButtonConfig::Action::RoundFrequencyToKhz)
            {
                roundActiveSliceToNearestKhz();
            }
        }
        if (NeoKey::wasKeyReleased(key))
        {
            if (ButtonConfig::action(key) ==
                ButtonConfig::Action::CycleFrequencyStep)
            {
                handleStepKeyReleased(nowMs);
            }
            else if (ButtonConfig::action(key) ==
                     ButtonConfig::Action::RfPowerPreset)
            {
                const uint8_t percent =
                    ButtonConfig::defaultRfPowerPercent(key);
                const bool requested = pendingRfPowerKey == 0 &&
                    SmartSdrConnection::requestRfPowerPercent(percent);
                if (requested)
                {
                    pendingRfPowerKey = key;
                }
                Serial.printf("[KEY] %u RELEASED name=\"%s\" RF-preset=%u W/%u%% requested=%s\n",
                              key,
                              NeoKey::keyName(key),
                              ButtonConfig::defaultRfPowerWatts(key),
                              percent,
                              requested ? "yes" : "no");
            }
        }
    }
    updateStepKeyGesture(nowMs);
}

void updateRfPowerRequest()
{
    const SmartSdrConnection::RfPowerRequestState requestState =
        SmartSdrConnection::rfPowerRequestState();
    if (requestState ==
        SmartSdrConnection::RfPowerRequestState::Confirmed)
    {
        if (pendingRfPowerKey != 0)
        {
            Serial.printf("[KEY] %u RF preset confirmed\n",
                          pendingRfPowerKey);
        }
        pendingRfPowerKey = 0;
        SmartSdrConnection::clearRfPowerRequestResult();
    }
    else if (requestState ==
             SmartSdrConnection::RfPowerRequestState::Failed)
    {
        Serial.printf("[KEY] RF preset failed; key indication retained\n");
        pendingRfPowerKey = 0;
        SmartSdrConnection::clearRfPowerRequestResult();
    }
}
} // namespace

void resetEncoderPosition()
{
    zeroOffset = readRawEncoderCount();
}

void setup()
{
    Serial.begin(EncoderConfig::SERIAL_BAUD_RATE);
    KeyLighting::clearForStartup();
    delay(1200); // One-time startup window for a USB CDC monitor.

    const esp_reset_reason_t resetReason = esp_reset_reason();
    Serial.println();
    Serial.println("[BOOT] ESP32-S3 Handwheel Radio Controller");
    Serial.printf("[BOOT] Build: %s %s\n", __DATE__, __TIME__);
    Serial.printf("[BOOT] Reset reason: %s (%d)\n",
                  resetReasonName(resetReason),
                  static_cast<int>(resetReason));
    Serial.printf("[BOOT] CPU=%u MHz flash=%u MB free-heap=%u bytes min-heap=%u bytes\n",
                  getCpuFrequencyMhz(),
                  ESP.getFlashChipSize() / (1024U * 1024U),
                  ESP.getFreeHeap(),
                  ESP.getMinFreeHeap());
    Serial.printf("[BOOT] PSRAM=%s size=%u MB free=%u bytes\n",
                  psramFound() ? "yes" : "no",
                  ESP.getPsramSize() / (1024U * 1024U),
                  ESP.getFreePsram());

    Serial.printf("[ENCODER] Configuring PCNT: A=GPIO%d B=GPIO%d PPR=%d counts/rev=%d filter=%u\n",
                  static_cast<int>(EncoderConfig::ENCODER_PIN_A),
                  static_cast<int>(EncoderConfig::ENCODER_PIN_B),
                  EncoderConfig::ENCODER_PULSES_PER_REVOLUTION,
                  EncoderConfig::ENCODER_COUNTS_PER_REVOLUTION,
                  EncoderConfig::ENCODER_GLITCH_FILTER_CYCLES);
    configureEncoder();
    resetEncoderPosition();
    Serial.printf("[ENCODER] PCNT ready; position reset to zero; tune step=%u Hz\n",
                  ButtonConfig::frequencyStepHz(frequencyStepIndex));

    onboardHeartbeat.begin();
    onboardHeartbeat.clear();
    onboardHeartbeat.show();
    Serial.printf("[LED] Connection status LED ready on GPIO%u\n",
                  EncoderConfig::RGB_LED_PIN);

    NeoKey::begin();
    Serial.printf("[KEY] Matrix ready: keys=%u scan=%lu us debounce=%lu ms\n",
                  NeoKeyConfig::KEY_COUNT,
                  static_cast<unsigned long>(NeoKeyConfig::SCAN_INTERVAL_US),
                  static_cast<unsigned long>(NeoKeyConfig::DEBOUNCE_MS));
    KeyLighting::begin();
    Serial.printf("[LED] Key lighting ready: pixels=%u data=GPIO%u brightness=%u background=%u hold=%lu ms fade=%lu ms\n",
                  NeoPixelConfig::PIXEL_COUNT,
                  NeoPixelConfig::DATA_PIN,
                  NeoPixelConfig::BRIGHTNESS,
                  NeoPixelConfig::BACKGROUND_LEVEL,
                  static_cast<unsigned long>(NeoPixelConfig::HOLD_THRESHOLD_MS),
                  static_cast<unsigned long>(NeoPixelConfig::RELEASE_FADE_MS));

    WifiProvisioning::begin();
    SmartSdrConnection::begin();
    Serial.printf("[BOOT] Initialization complete; setup AP=\"%s\"\n",
                  WifiProvisioning::setupAccessPointName());
}

void loop()
{
    WifiProvisioning::update();
    SmartSdrConnection::update();

    static WifiProvisioning::State previousWifiState =
        WifiProvisioning::State::Idle;
    static SmartSdrConnection::State previousRadioState =
        SmartSdrConnection::State::Idle;
    static bool previousRfPowerAvailable = false;
    static uint16_t previousRfPower = 0;

    const WifiProvisioning::State wifiState = WifiProvisioning::state();
    if (wifiState != previousWifiState)
    {
        Serial.printf("[WIFI] State: %s -> %s\n",
                      wifiStateName(previousWifiState),
                      wifiStateName(wifiState));
        previousWifiState = wifiState;
        if (wifiState == WifiProvisioning::State::Portal)
        {
            Serial.printf("[WIFI] Portal URL=http://192.168.4.1 SSID=\"%s\"\n",
                          WifiProvisioning::setupAccessPointName());
        }
        else if (wifiState == WifiProvisioning::State::Connected)
        {
            Serial.printf("[WIFI] Connected: SSID=\"%s\" IP=%s gateway=%s subnet=%s DNS=%s RSSI=%d dBm channel=%d MAC=%s\n",
                          WiFi.SSID().c_str(),
                          WiFi.localIP().toString().c_str(),
                          WiFi.gatewayIP().toString().c_str(),
                          WiFi.subnetMask().toString().c_str(),
                          WiFi.dnsIP().toString().c_str(),
                          WiFi.RSSI(),
                          WiFi.channel(),
                          WiFi.macAddress().c_str());
        }
    }

    const SmartSdrConnection::State radioState = SmartSdrConnection::state();
    if (radioState != previousRadioState)
    {
        Serial.printf("[SMARTSDR] State: %s -> %s\n",
                      radioStateName(previousRadioState),
                      radioStateName(radioState));
        if (radioState == SmartSdrConnection::State::Connected)
        {
            KeyLighting::startRadioConnectedAnimation(millis());
            Serial.println("[LED] Radio connected animation: left column keys=12,9,6,3 color=green step=100 ms");
        }
        previousRadioState = radioState;
        if (radioState == SmartSdrConnection::State::RadioFound ||
            radioState == SmartSdrConnection::State::Connecting ||
            radioState == SmartSdrConnection::State::Connected ||
            radioState == SmartSdrConnection::State::Ready)
        {
            Serial.printf("[SMARTSDR] Radio: model=%s name=\"%s\" API=%s:%u\n",
                          SmartSdrConnection::radioModel(),
                          SmartSdrConnection::radioName(),
                          SmartSdrConnection::radioIp().toString().c_str(),
                          SmartSdrConnection::radioPort());
        }
    }

    const bool rfPowerAvailable = SmartSdrConnection::hasRfPower();
    const uint16_t rfPower = SmartSdrConnection::rfPowerSetting();
    if (rfPowerAvailable &&
        (!previousRfPowerAvailable || rfPower != previousRfPower))
    {
        Serial.printf("[SMARTSDR] RF power status changed: raw=%u\n", rfPower);
    }
    else if (!rfPowerAvailable && previousRfPowerAvailable)
    {
        Serial.println("[SMARTSDR] RF power status unavailable");
    }
    previousRfPowerAvailable = rfPowerAvailable;
    previousRfPower = rfPower;

    updateRfPowerRequest();

    uint64_t activeFrequencyHz = 0;
    if (radioState != SmartSdrConnection::State::Ready ||
        !SmartSdrConnection::activeSliceFrequencyHz(activeFrequencyHz) ||
        ButtonConfig::isSixMeterFrequency(activeFrequencyHz))
    {
        KeyLighting::clearBlinkingKey();
    }

    updateHeartbeat();
    NeoKey::update();
    KeyLighting::update(millis());
    processNeoKeyEvents(millis());

    static int64_t previousCount = 0;
    static int64_t pendingDetentCounts = 0;
    const int64_t currentCount = readRawEncoderCount() - zeroOffset;

    if (currentCount != previousCount)
    {
        const int64_t rawDelta = currentCount - previousCount;
        pendingDetentCounts += rawDelta;
        const int64_t detentDelta =
            pendingDetentCounts / EncoderConfig::ENCODER_COUNTS_PER_DETENT;
        pendingDetentCounts %= EncoderConfig::ENCODER_COUNTS_PER_DETENT;
        const double revolutions =
            static_cast<double>(currentCount) /
            static_cast<double>(EncoderConfig::ENCODER_COUNTS_PER_REVOLUTION);

        if (detentDelta != 0)
        {
            const char *direction = detentDelta > 0 ? "CW" : "CCW";
            const int64_t tuneDeltaHz =
                detentDelta *
                ButtonConfig::frequencyStepHz(frequencyStepIndex) *
                (EncoderConfig::CLOCKWISE_TUNES_UP ? 1LL : -1LL);
            const bool transmitted =
                SmartSdrConnection::tuneActiveSliceByHz(tuneDeltaHz);

            Serial.printf("[ENCODER] count=%lld raw-delta=%lld detent-delta=%lld remainder=%lld revolutions=%.2f direction=%s tune=%s%lld Hz transmitted=%s\n",
                      static_cast<long long>(currentCount),
                          static_cast<long long>(rawDelta),
                          static_cast<long long>(detentDelta),
                          static_cast<long long>(pendingDetentCounts),
                          revolutions,
                          direction,
                          tuneDeltaHz > 0 ? "+" : "",
                          static_cast<long long>(tuneDeltaHz),
                          transmitted ? "yes" : "no");
        }
        previousCount = currentCount;
    }

    yield();
}
