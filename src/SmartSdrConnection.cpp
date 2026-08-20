#include "SmartSdrConnection.h"

#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiUdp.h>

#include "RadioConfig.h"
#include "WifiProvisioning.h"

namespace
{
constexpr uint16_t DISCOVERY_PORT = 4992;
constexpr uint16_t DEFAULT_API_PORT = RadioConfig::STATIC_API_PORT;
constexpr uint32_t CONNECT_RETRY_MS = 4000;
constexpr uint32_t TCP_CONNECT_TIMEOUT_MS = 300;
constexpr uint32_t PING_INTERVAL_MS = 1000;
constexpr uint32_t RX_TIMEOUT_MS = 20000;
constexpr uint32_t RF_POWER_CONFIRM_TIMEOUT_MS = 3000;
constexpr size_t DISCOVERY_BUFFER_SIZE = 512;
constexpr size_t LINE_BUFFER_SIZE = 2048;
constexpr uint8_t MAX_TRACKED_SLICES = 8;

struct SliceState
{
    bool inUse = false;
    bool active = false;
    bool frequencyAvailable = false;
    uint64_t frequencyHz = 0;
};

struct RfPowerRequest
{
    SmartSdrConnection::RfPowerRequestState state =
        SmartSdrConnection::RfPowerRequestState::Idle;
    uint32_t sequence = 0;
    uint32_t startedMs = 0;
    uint8_t requestedPercent = 0;
    bool responseReceived = false;
    bool statusConfirmed = false;
};

WiFiUDP discoveryUdp;
WiFiClient apiClient;
SmartSdrConnection::State currentState = SmartSdrConnection::State::Idle;
IPAddress discoveredRadioIp;
uint16_t discoveredRadioPort = DEFAULT_API_PORT;
String discoveredRadioModel;
String discoveredRadioName;
String discoveredRadioSerial;
uint16_t currentRfPower = 0;
bool rfPowerAvailable = false;
bool discoveryStarted = false;
bool discoveryFailureLogged = false;
uint32_t discoveryPhaseStartedMs = 0;
uint32_t lastConnectAttemptMs = 0;
uint32_t lastPingMs = 0;
uint32_t lastReceiveMs = 0;
uint32_t nextSequence = 1;
char lineBuffer[LINE_BUFFER_SIZE] = {};
size_t lineLength = 0;
SliceState slices[MAX_TRACKED_SLICES] = {};
RfPowerRequest rfPowerRequest;

void failRfPowerRequest(const char *reason)
{
    if (rfPowerRequest.state !=
        SmartSdrConnection::RfPowerRequestState::Pending)
    {
        return;
    }

    rfPowerRequest.state = SmartSdrConnection::RfPowerRequestState::Failed;
    Serial.printf("[RF POWER] Request failed: %s\n", reason);
}

void completeRfPowerRequestIfConfirmed()
{
    if (rfPowerRequest.state ==
            SmartSdrConnection::RfPowerRequestState::Pending &&
        rfPowerRequest.responseReceived &&
        rfPowerRequest.statusConfirmed)
    {
        rfPowerRequest.state =
            SmartSdrConnection::RfPowerRequestState::Confirmed;
        Serial.printf("[RF POWER] Confirmed: %u%%\n",
                      rfPowerRequest.requestedPercent);
    }
}

void resetSlices()
{
    for (SliceState &slice : slices)
    {
        slice = SliceState{};
    }
}

void resetRadioState()
{
    apiClient.stop();
    discoveredRadioIp = IPAddress();
    discoveredRadioPort = DEFAULT_API_PORT;
    discoveredRadioModel = "";
    discoveredRadioName = "";
    discoveredRadioSerial = "";
    rfPowerAvailable = false;
    currentRfPower = 0;
    lineLength = 0;
    nextSequence = 1;
    resetSlices();
    discoveryPhaseStartedMs = 0;
    currentState = SmartSdrConnection::State::Idle;
}

IPAddress configuredRadioIp()
{
    return IPAddress(RadioConfig::STATIC_IP_OCTETS[0],
                     RadioConfig::STATIC_IP_OCTETS[1],
                     RadioConfig::STATIC_IP_OCTETS[2],
                     RadioConfig::STATIC_IP_OCTETS[3]);
}

void selectConfiguredRadioFallback()
{
    discoveredRadioIp = configuredRadioIp();
    discoveredRadioPort = RadioConfig::STATIC_API_PORT;
    discoveredRadioModel = "configured target";
    discoveredRadioName = "Radio static fallback";
    discoveredRadioSerial = "";
    currentState = SmartSdrConnection::State::RadioFound;
    lastConnectAttemptMs = 0;
    Serial.printf("[DISCOVERY] No broadcast received after %lu ms; using configured target %s:%u\n",
                  static_cast<unsigned long>(RadioConfig::DISCOVERY_WAIT_MS),
                  discoveredRadioIp.toString().c_str(),
                  discoveredRadioPort);
}

String discoveryField(const String &payload, const char *field)
{
    const String prefix = String(field) + '=';
    int start = payload.startsWith(prefix) ? 0 : payload.indexOf(' ' + prefix);
    if (start < 0)
    {
        return "";
    }
    if (start > 0)
    {
        ++start;
    }
    start += prefix.length();
    int end = payload.indexOf(' ', start);
    if (end < 0)
    {
        end = payload.length();
    }
    return payload.substring(start, end);
}

bool parseDiscoveryPacket(const uint8_t *data,
                          const size_t length,
                          const IPAddress &senderIp)
{
    size_t payloadStart = length;
    for (size_t index = 0; index + 6 <= length; ++index)
    {
        if (memcmp(data + index, "model=", 6) == 0)
        {
            payloadStart = index;
            break;
        }
    }
    if (payloadStart == length)
    {
        return false;
    }

    size_t payloadLength = 0;
    while (payloadStart + payloadLength < length &&
           data[payloadStart + payloadLength] != '\0')
    {
        ++payloadLength;
    }
    const String payload(
        reinterpret_cast<const char *>(data + payloadStart),
        payloadLength);

    const String model = discoveryField(payload, "model");
    if (model.isEmpty())
    {
        return false;
    }

    IPAddress apiIp = senderIp;
    const String ipText = discoveryField(payload, "ip");
    if (!ipText.isEmpty())
    {
        IPAddress advertisedIp;
        if (advertisedIp.fromString(ipText))
        {
            apiIp = advertisedIp;
        }
    }

    uint16_t apiPort = DEFAULT_API_PORT;
    const long advertisedPort = discoveryField(payload, "port").toInt();
    if (advertisedPort > 0 && advertisedPort <= 65535)
    {
        apiPort = static_cast<uint16_t>(advertisedPort);
    }

    discoveredRadioIp = apiIp;
    discoveredRadioPort = apiPort;
    discoveredRadioModel = model;
    discoveredRadioName = discoveryField(payload, "name");
    discoveredRadioName.replace('_', ' ');
    discoveredRadioSerial = discoveryField(payload, "serial");
    return true;
}

void processDiscovery()
{
    const int packetSize = discoveryUdp.parsePacket();
    if (packetSize <= 0)
    {
        return;
    }

    uint8_t packet[DISCOVERY_BUFFER_SIZE] = {};
    const int bytesRead = discoveryUdp.read(
        packet,
        min<int>(packetSize, static_cast<int>(sizeof(packet))));
    if (bytesRead <= 0)
    {
        return;
    }

    if ((currentState == SmartSdrConnection::State::Idle ||
         currentState == SmartSdrConnection::State::Discovering) &&
        parseDiscoveryPacket(packet,
                             static_cast<size_t>(bytesRead),
                             discoveryUdp.remoteIP()))
    {
        Serial.printf("[DISCOVERY] Radio found: model=%s name=\"%s\" serial=%s API=%s:%u packet=%d bytes\n",
                      discoveredRadioModel.c_str(),
                      discoveredRadioName.c_str(),
                      discoveredRadioSerial.isEmpty()
                          ? "unknown"
                          : discoveredRadioSerial.c_str(),
                      discoveredRadioIp.toString().c_str(),
                      discoveredRadioPort,
                      bytesRead);
        currentState = SmartSdrConnection::State::RadioFound;
        lastConnectAttemptMs = 0;
    }
}

uint32_t sendCommand(const char *command)
{
    if (!apiClient.connected())
    {
        return 0;
    }
    const uint32_t sequence = nextSequence++;
    apiClient.printf("C%lu|%s\n",
                     static_cast<unsigned long>(sequence),
                     command);
    Serial.printf("[SMARTSDR TX] C%lu|%s\n",
                  static_cast<unsigned long>(sequence),
                  command);
    return sequence;
}

void startApiSession()
{
    Serial.println("[SMARTSDR] TCP connected; starting session");
    lineLength = 0;
    rfPowerAvailable = false;
    currentRfPower = 0;
    resetSlices();
    nextSequence = 1;
    lastReceiveMs = millis();
    lastPingMs = millis();
    sendCommand("name ESP32_Handwheel");
    sendCommand("info");
    sendCommand("sub tx all");
    sendCommand("sub slice all");
    sendCommand("keepalive enable");
    currentState = SmartSdrConnection::State::Connected;
}

void tryApiConnection()
{
    const uint32_t nowMs = millis();
    if (lastConnectAttemptMs != 0 &&
        nowMs - lastConnectAttemptMs < CONNECT_RETRY_MS)
    {
        return;
    }
    lastConnectAttemptMs = nowMs;
    currentState = SmartSdrConnection::State::Connecting;
    Serial.printf("[SMARTSDR] TCP connect attempt: %s:%u timeout=%lu ms\n",
                  discoveredRadioIp.toString().c_str(),
                  discoveredRadioPort,
                  static_cast<unsigned long>(TCP_CONNECT_TIMEOUT_MS));

    if (apiClient.connect(discoveredRadioIp,
                          discoveredRadioPort,
                          TCP_CONNECT_TIMEOUT_MS))
    {
        apiClient.setNoDelay(true);
        startApiSession();
    }
    else
    {
        Serial.println("[SMARTSDR] TCP connect failed; retry scheduled");
        currentState = SmartSdrConnection::State::RadioFound;
    }
}

bool parseUnsignedField(const char *line,
                        const char *field,
                        uint16_t &value)
{
    const char *fieldStart = strstr(line, field);
    if (fieldStart == nullptr)
    {
        return false;
    }
    fieldStart += strlen(field);
    if (*fieldStart < '0' || *fieldStart > '9')
    {
        return false;
    }

    unsigned long parsed = 0;
    while (*fieldStart >= '0' && *fieldStart <= '9')
    {
        parsed = parsed * 10UL + static_cast<unsigned long>(*fieldStart - '0');
        if (parsed > 65535UL)
        {
            return false;
        }
        ++fieldStart;
    }
    value = static_cast<uint16_t>(parsed);
    return true;
}

bool parseSliceNumber(const char *payload, uint8_t &sliceNumber)
{
    constexpr char PREFIX[] = "slice ";
    if (strncmp(payload, PREFIX, sizeof(PREFIX) - 1U) != 0)
    {
        return false;
    }

    const char *numberStart = payload + sizeof(PREFIX) - 1U;
    char *numberEnd = nullptr;
    const unsigned long parsed = strtoul(numberStart, &numberEnd, 10);
    if (numberEnd == numberStart ||
        (*numberEnd != ' ' && *numberEnd != '\0') ||
        parsed >= MAX_TRACKED_SLICES)
    {
        return false;
    }

    sliceNumber = static_cast<uint8_t>(parsed);
    return true;
}

bool parseFrequencyHz(const char *line, uint64_t &frequencyHz)
{
    constexpr char FIELD[] = "RF_frequency=";
    const char *frequencyStart = strstr(line, FIELD);
    if (frequencyStart == nullptr)
    {
        return false;
    }
    frequencyStart += sizeof(FIELD) - 1U;

    char *frequencyEnd = nullptr;
    const double frequencyMhz = strtod(frequencyStart, &frequencyEnd);
    if (frequencyEnd == frequencyStart || frequencyMhz <= 0.0)
    {
        return false;
    }

    frequencyHz = static_cast<uint64_t>(frequencyMhz * 1000000.0 + 0.5);
    return frequencyHz > 0;
}

void processSliceStatus(const char *payload)
{
    uint8_t sliceNumber = 0;
    if (!parseSliceNumber(payload, sliceNumber))
    {
        return;
    }

    SliceState &slice = slices[sliceNumber];
    uint16_t value = 0;
    if (parseUnsignedField(payload, "in_use=", value))
    {
        if (value == 0)
        {
            slice = SliceState{};
            Serial.printf("[SMARTSDR] Slice %u no longer in use\n", sliceNumber);
            return;
        }
        slice.inUse = true;
    }

    if (parseUnsignedField(payload, "active=", value))
    {
        slice.active = value != 0;
        slice.inUse = true;
    }

    uint64_t frequencyHz = 0;
    if (parseFrequencyHz(payload, frequencyHz))
    {
        slice.frequencyHz = frequencyHz;
        slice.frequencyAvailable = true;
        slice.inUse = true;
        Serial.printf("[SMARTSDR] Slice %u frequency=%llu.%06llu MHz active=%s\n",
                      sliceNumber,
                      static_cast<unsigned long long>(frequencyHz / 1000000ULL),
                      static_cast<unsigned long long>(frequencyHz % 1000000ULL),
                      slice.active ? "yes" : "no");
    }
}

int activeSliceNumber()
{
    int onlyUsableSlice = -1;
    int onlyActiveSlice = -1;
    uint8_t usableSliceCount = 0;
    uint8_t activeSliceCount = 0;

    for (uint8_t index = 0; index < MAX_TRACKED_SLICES; ++index)
    {
        const SliceState &slice = slices[index];
        if (!slice.inUse || !slice.frequencyAvailable)
        {
            continue;
        }
        if (slice.active)
        {
            onlyActiveSlice = index;
            ++activeSliceCount;
        }
        onlyUsableSlice = index;
        ++usableSliceCount;
    }

    if (activeSliceCount == 1)
    {
        return onlyActiveSlice;
    }
    if (activeSliceCount > 1)
    {
        return -1;
    }
    return usableSliceCount == 1 ? onlyUsableSlice : -1;
}

int exactlyOneActiveSliceNumber()
{
    int selectedSlice = -1;
    uint8_t activeSliceCount = 0;

    for (uint8_t index = 0; index < MAX_TRACKED_SLICES; ++index)
    {
        const SliceState &slice = slices[index];
        if (slice.inUse && slice.active && slice.frequencyAvailable)
        {
            selectedSlice = index;
            ++activeSliceCount;
        }
    }
    return activeSliceCount == 1 ? selectedSlice : -1;
}

bool parseResponse(const char *line,
                   uint32_t &sequence,
                   uint32_t &responseCode)
{
    if (line[0] != 'R')
    {
        return false;
    }

    char *sequenceEnd = nullptr;
    const unsigned long parsedSequence = strtoul(line + 1, &sequenceEnd, 10);
    if (sequenceEnd == line + 1 || *sequenceEnd != '|')
    {
        return false;
    }

    const char *responseStart = sequenceEnd + 1;
    char *responseEnd = nullptr;
    const unsigned long parsedResponse =
        strtoul(responseStart, &responseEnd, 16);
    if (responseEnd == responseStart ||
        (*responseEnd != '|' && *responseEnd != '\0'))
    {
        return false;
    }

    sequence = static_cast<uint32_t>(parsedSequence);
    responseCode = static_cast<uint32_t>(parsedResponse);
    return true;
}

void processRfPowerResponse(const uint32_t sequence,
                            const uint32_t responseCode)
{
    if (rfPowerRequest.state !=
            SmartSdrConnection::RfPowerRequestState::Pending ||
        sequence != rfPowerRequest.sequence)
    {
        return;
    }

    if (responseCode != 0)
    {
        Serial.printf("[RF POWER] Radio rejected sequence %lu: code=%08lX\n",
                      static_cast<unsigned long>(sequence),
                      static_cast<unsigned long>(responseCode));
        failRfPowerRequest("Radio rejected command");
        return;
    }

    rfPowerRequest.responseReceived = true;
    Serial.printf("[RF POWER] Command accepted: sequence=%lu requested=%u%%\n",
                  static_cast<unsigned long>(sequence),
                  rfPowerRequest.requestedPercent);
    completeRfPowerRequestIfConfirmed();
}

void processApiLine(char *line)
{
    Serial.printf("[SMARTSDR RX] %s\n", line);

    uint32_t responseSequence = 0;
    uint32_t responseCode = 0;
    if (parseResponse(line, responseSequence, responseCode))
    {
        processRfPowerResponse(responseSequence, responseCode);
        return;
    }

    const char *payload = strchr(line, '|');
    if (payload == nullptr)
    {
        return;
    }
    ++payload;

    if (strncmp(payload, "slice ", 6) == 0)
    {
        processSliceStatus(payload);
        return;
    }

    if (strncmp(payload, "transmit ", 9) != 0)
    {
        return;
    }

    uint16_t value = 0;
    if (parseUnsignedField(line, "rfpower=", value))
    {
        if (!rfPowerAvailable || currentRfPower != value)
        {
            Serial.printf("[SMARTSDR] Parsed rfpower raw value=%u\n", value);
        }
        currentRfPower = value;
        rfPowerAvailable = true;
        currentState = SmartSdrConnection::State::Ready;
        if (rfPowerRequest.state ==
                SmartSdrConnection::RfPowerRequestState::Pending &&
            value == rfPowerRequest.requestedPercent)
        {
            rfPowerRequest.statusConfirmed = true;
            completeRfPowerRequestIfConfirmed();
        }
    }
}

void processApiInput()
{
    while (apiClient.available() > 0)
    {
        const char character = static_cast<char>(apiClient.read());
        lastReceiveMs = millis();
        if (character == '\r' || character == '\n')
        {
            if (lineLength > 0)
            {
                lineBuffer[lineLength] = '\0';
                processApiLine(lineBuffer);
                lineLength = 0;
            }
            continue;
        }

        if (lineLength + 1 < sizeof(lineBuffer))
        {
            lineBuffer[lineLength++] = character;
        }
        else
        {
            Serial.printf("[SMARTSDR] RX line exceeded %u bytes; discarded\n",
                          static_cast<unsigned>(sizeof(lineBuffer) - 1U));
            lineLength = 0;
        }
    }
}

void updateApiConnection()
{
    if (!apiClient.connected())
    {
        failRfPowerRequest("TCP connection lost");
        Serial.println("[SMARTSDR] TCP connection lost; retry scheduled");
        apiClient.stop();
        rfPowerAvailable = false;
        currentState = SmartSdrConnection::State::RadioFound;
        lastConnectAttemptMs = millis();
        return;
    }

    processApiInput();
    const uint32_t nowMs = millis();
    if (nowMs - lastPingMs >= PING_INTERVAL_MS)
    {
        sendCommand("ping");
        lastPingMs = nowMs;
    }
    if (nowMs - lastReceiveMs >= RX_TIMEOUT_MS)
    {
        Serial.printf("[SMARTSDR] RX timeout after %lu ms; closing TCP\n",
                      static_cast<unsigned long>(RX_TIMEOUT_MS));
        failRfPowerRequest("receive timeout");
        apiClient.stop();
        rfPowerAvailable = false;
        currentState = SmartSdrConnection::State::RadioFound;
        lastConnectAttemptMs = nowMs;
    }

    if (rfPowerRequest.state ==
            SmartSdrConnection::RfPowerRequestState::Pending &&
        nowMs - rfPowerRequest.startedMs >= RF_POWER_CONFIRM_TIMEOUT_MS)
    {
        failRfPowerRequest("confirmation timeout");
    }
}
} // namespace

namespace SmartSdrConnection
{
void begin()
{
    Serial.println("[SMARTSDR] Client initializing");
    discoveryUdp.stop();
    discoveryStarted = false;
    discoveryFailureLogged = false;
    rfPowerRequest = RfPowerRequest{};
    resetRadioState();
}

void update()
{
    if (!WifiProvisioning::isConnected())
    {
        if (discoveryStarted)
        {
            discoveryUdp.stop();
            discoveryStarted = false;
        }
        if (currentState != State::Idle)
        {
            Serial.println("[SMARTSDR] WiFi unavailable; radio state cleared");
            failRfPowerRequest("WiFi unavailable");
            resetRadioState();
        }
        return;
    }

    if (discoveryPhaseStartedMs == 0)
    {
        discoveryPhaseStartedMs = millis();
        Serial.printf("[DISCOVERY] Waiting %lu ms before static fallback to %s:%u\n",
                      static_cast<unsigned long>(RadioConfig::DISCOVERY_WAIT_MS),
                      configuredRadioIp().toString().c_str(),
                      RadioConfig::STATIC_API_PORT);
    }

    if (!discoveryStarted &&
        (currentState == State::Idle || currentState == State::Discovering))
    {
        discoveryStarted = discoveryUdp.begin(DISCOVERY_PORT) == 1;
        currentState = State::Discovering;
        if (discoveryStarted)
        {
            discoveryFailureLogged = false;
            Serial.printf("[DISCOVERY] Listening for VITA-49 broadcasts on UDP port %u\n",
                          DISCOVERY_PORT);
        }
        else if (!discoveryFailureLogged)
        {
            discoveryFailureLogged = true;
            Serial.printf("[DISCOVERY] Failed to open UDP port %u; retrying\n",
                          DISCOVERY_PORT);
        }
    }

    processDiscovery();

    if (currentState == State::Discovering &&
        millis() - discoveryPhaseStartedMs >= RadioConfig::DISCOVERY_WAIT_MS)
    {
        selectConfiguredRadioFallback();
    }

    if (currentState == State::RadioFound ||
        currentState == State::Connecting)
    {
        tryApiConnection();
    }
    else if (currentState == State::Connected ||
             currentState == State::Ready)
    {
        updateApiConnection();
    }
}

State state()
{
    return currentState;
}

bool hasRfPower()
{
    return rfPowerAvailable;
}

uint16_t rfPowerSetting()
{
    return currentRfPower;
}

bool activeSliceFrequencyHz(uint64_t &frequencyHz)
{
    const int sliceNumber = activeSliceNumber();
    if (sliceNumber < 0)
    {
        return false;
    }

    frequencyHz = slices[sliceNumber].frequencyHz;
    return true;
}

bool requestRfPowerPercent(const uint8_t percent)
{
    if (rfPowerRequest.state != RfPowerRequestState::Idle)
    {
        Serial.println("[RF POWER] Request skipped: another request is active");
        return false;
    }
    if (percent > 100)
    {
        Serial.println("[RF POWER] Request skipped: percentage is out of range");
        return false;
    }
    if (!apiClient.connected() || currentState != State::Ready)
    {
        Serial.println("[RF POWER] Request skipped: Radio is not ready");
        return false;
    }

    const int sliceNumber = exactlyOneActiveSliceNumber();
    if (sliceNumber < 0)
    {
        Serial.println("[RF POWER] Request skipped: not exactly one active slice");
        return false;
    }

    const uint64_t frequencyHz = slices[sliceNumber].frequencyHz;
    if (frequencyHz >= 50000000ULL && frequencyHz <= 54000000ULL)
    {
        Serial.println("[RF POWER] Request skipped: power keys are disabled on 6 m");
        return false;
    }

    char command[48] = {};
    snprintf(command,
             sizeof(command),
             "transmit set rfpower=%u",
             static_cast<unsigned>(percent));

    rfPowerRequest = RfPowerRequest{};
    rfPowerRequest.state = RfPowerRequestState::Pending;
    rfPowerRequest.startedMs = millis();
    rfPowerRequest.requestedPercent = percent;
    rfPowerRequest.statusConfirmed =
        rfPowerAvailable && currentRfPower == percent;
    rfPowerRequest.sequence = sendCommand(command);
    if (rfPowerRequest.sequence == 0)
    {
        failRfPowerRequest("TCP write unavailable");
        return false;
    }

    Serial.printf("[RF POWER] Request pending: sequence=%lu requested=%u%% slice=%d\n",
                  static_cast<unsigned long>(rfPowerRequest.sequence),
                  percent,
                  sliceNumber);
    return true;
}

RfPowerRequestState rfPowerRequestState()
{
    return rfPowerRequest.state;
}

void clearRfPowerRequestResult()
{
    if (rfPowerRequest.state == RfPowerRequestState::Pending)
    {
        return;
    }
    rfPowerRequest = RfPowerRequest{};
}

bool tuneActiveSliceByHz(const int64_t deltaHz)
{
    if (deltaHz == 0)
    {
        return true;
    }
    if (!apiClient.connected() ||
        (currentState != State::Connected && currentState != State::Ready))
    {
        Serial.println("[ENCODER TX] Skipped: SmartSDR is not connected");
        return false;
    }

    const int sliceNumber = activeSliceNumber();
    if (sliceNumber < 0)
    {
        Serial.println("[ENCODER TX] Skipped: no unambiguous active slice");
        return false;
    }

    SliceState &slice = slices[sliceNumber];
    const int64_t targetHz =
        static_cast<int64_t>(slice.frequencyHz) + deltaHz;
    if (targetHz <= 0)
    {
        Serial.println("[ENCODER TX] Skipped: target frequency is invalid");
        return false;
    }

    char command[96] = {};
    snprintf(command,
             sizeof(command),
             "slice t %d %lld.%06lld autopan=1",
             sliceNumber,
             static_cast<long long>(targetHz / 1000000LL),
             static_cast<long long>(targetHz % 1000000LL));
    sendCommand(command);

    // Keep an optimistic target so rapid turns build on the last command even
    // before the corresponding asynchronous slice status arrives.
    slice.frequencyHz = static_cast<uint64_t>(targetHz);
    Serial.printf("[ENCODER TX] slice=%d direction=%s delta=%lld Hz target=%lld.%06lld MHz\n",
                  sliceNumber,
                  deltaHz > 0 ? "UP" : "DOWN",
                  static_cast<long long>(deltaHz),
                  static_cast<long long>(targetHz / 1000000LL),
                  static_cast<long long>(targetHz % 1000000LL));
    return true;
}

IPAddress radioIp()
{
    return discoveredRadioIp;
}

uint16_t radioPort()
{
    return discoveredRadioPort;
}

const char *radioModel()
{
    return discoveredRadioModel.c_str();
}

const char *radioName()
{
    return discoveredRadioName.c_str();
}
} // namespace SmartSdrConnection
