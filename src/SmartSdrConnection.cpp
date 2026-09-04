#include "SmartSdrConnection.h"
#include "SmartSdrLineParser.h"
#include "SmartSdrProtocolParser.h"

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
constexpr uint32_t COMMAND_RESPONSE_TIMEOUT_MS = 3000;
constexpr size_t DISCOVERY_BUFFER_SIZE = 512;
constexpr size_t LINE_BUFFER_SIZE = 2048;
constexpr size_t COMMAND_BUFFER_SIZE = 160;
constexpr uint8_t MAX_TRACKED_SLICES = 8;
constexpr uint8_t MAX_TRACKED_CLIENTS = 8;
constexpr uint8_t MAX_PENDING_COMMANDS = 32;
constexpr size_t CLIENT_ID_SIZE = 37;
constexpr const char *INITIAL_SESSION_COMMANDS[] = {
    "name ESP32_Handwheel",
    "info",
    "sub client all",
    "sub slice all",
    "keepalive enable"};
constexpr uint8_t INITIAL_SESSION_COMMAND_COUNT =
    sizeof(INITIAL_SESSION_COMMANDS) /
    sizeof(INITIAL_SESSION_COMMANDS[0]);
constexpr uint8_t REQUIRED_SESSION_COMMAND_COUNT =
    INITIAL_SESSION_COMMAND_COUNT + 2;

using ApiLineParser = SmartSdrLineParser<LINE_BUFFER_SIZE>;

enum class CommandType : uint8_t
{
    Session,
    ClientBind,
    TxSubscription,
    Keepalive,
    Tuning,
    OperatorMessage,
    FrequencyStepSpot,
    Rit,
    Mute,
    RfPower
};

enum class ResponseClass : uint8_t
{
    Success,
    Informational,
    Error
};

struct CommandLedgerEntry
{
    bool inUse = false;
    uint32_t sequence = 0;
    CommandType type = CommandType::Session;
    uint32_t sentMs = 0;
};

struct SliceState
{
    bool inUse = false;
    bool active = false;
    bool frequencyAvailable = false;
    bool clientHandleAvailable = false;
    uint64_t frequencyHz = 0;
    uint64_t reportedFrequencyHz = 0;
    uint32_t clientHandle = 0;
    uint32_t latestTuningSequence = 0;
    bool ritOnAvailable = false;
    bool ritOn = false;
    bool ritFrequencyAvailable = false;
    int32_t ritFrequencyHz = 0;
    bool audioMuteAvailable = false;
    bool audioMute = false;
    uint32_t ritSequence = 0;
    uint32_t muteSequence = 0;
};

struct ClientState
{
    bool inUse = false;
    uint32_t handle = 0;
    char clientId[CLIENT_ID_SIZE] = {};
};

struct RfPowerRequest
{
    SmartSdrConnection::RfPowerRequestState state =
        SmartSdrConnection::RfPowerRequestState::Idle;
    uint32_t sequence = 0;
    uint32_t startedMs = 0;
    uint32_t statusGenerationAtStart = 0;
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
uint16_t confirmedRfPower = 0;
bool confirmedRfPowerAvailable = false;
uint32_t rfPowerStatusGeneration = 0;
bool discoveryStarted = false;
bool discoveryFailureLogged = false;
uint32_t discoveryPhaseStartedMs = 0;
uint32_t lastConnectAttemptMs = 0;
uint32_t lastPingMs = 0;
uint32_t lastReceiveMs = 0;
uint32_t nextSequence = 1;
ApiLineParser apiLineParser;
SliceState slices[MAX_TRACKED_SLICES] = {};
ClientState clients[MAX_TRACKED_CLIENTS] = {};
RfPowerRequest rfPowerRequest;
CommandLedgerEntry commandLedger[MAX_PENDING_COMMANDS] = {};
uint8_t acceptedSessionCommandCount = 0;
bool freshTransmitterStatusReceived = false;
bool clientBindAccepted = false;
bool txSubscriptionAccepted = false;
uint32_t targetClientHandle = 0;
uint32_t boundClientHandle = 0;
uint32_t clientBindSequence = 0;
uint32_t txSubscriptionSequence = 0;

void invalidateClientBinding(const char *reason);

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
        confirmedRfPower = rfPowerRequest.requestedPercent;
        confirmedRfPowerAvailable = true;
        rfPowerRequest.state =
            SmartSdrConnection::RfPowerRequestState::Confirmed;
        Serial.printf("[RF POWER] Confirmed: %u%%\n",
                      rfPowerRequest.requestedPercent);
    }
}

void resetSessionReadiness()
{
    acceptedSessionCommandCount = 0;
    freshTransmitterStatusReceived = false;
    clientBindAccepted = false;
    txSubscriptionAccepted = false;
    targetClientHandle = 0;
    boundClientHandle = 0;
    clientBindSequence = 0;
    txSubscriptionSequence = 0;
}

void updateSessionReadiness()
{
    const uint8_t acceptedCommandCount =
        acceptedSessionCommandCount +
        (clientBindAccepted ? 1U : 0U) +
        (txSubscriptionAccepted ? 1U : 0U);
    if (currentState != SmartSdrConnection::State::Connected ||
        acceptedCommandCount != REQUIRED_SESSION_COMMAND_COUNT ||
        !freshTransmitterStatusReceived)
    {
        return;
    }

    currentState = SmartSdrConnection::State::Ready;
    Serial.printf("[SMARTSDR] Session ready: commands=%u/%u fresh-rfpower=yes bound-client=0x%08lX\n",
                  acceptedCommandCount,
                  REQUIRED_SESSION_COMMAND_COUNT,
                  static_cast<unsigned long>(boundClientHandle));
}

void resetSlices()
{
    for (SliceState &slice : slices)
    {
        slice = SliceState{};
    }
}

void resetClients()
{
    for (ClientState &client : clients)
    {
        client = ClientState{};
    }
}


const char *commandTypeName(const CommandType type)
{
    switch (type)
    {
    case CommandType::Session:
        return "session";
    case CommandType::ClientBind:
        return "client-bind";
    case CommandType::TxSubscription:
        return "tx-subscription";
    case CommandType::Keepalive:
        return "keepalive";
    case CommandType::Tuning:
        return "tuning";
    case CommandType::OperatorMessage:
        return "operator-message";
    case CommandType::FrequencyStepSpot:
        return "frequency-step-spot";
    case CommandType::Rit:
        return "rit";
    case CommandType::Mute:
        return "mute";
    case CommandType::RfPower:
        return "rf-power";
    }
    return "unknown";
}

ResponseClass classifyResponseCode(const uint32_t responseCode)
{
    if (responseCode == 0)
    {
        return ResponseClass::Success;
    }
    if ((responseCode & 0xF0000000UL) == 0x10000000UL)
    {
        return ResponseClass::Informational;
    }
    return ResponseClass::Error;
}

const char *responseClassName(const ResponseClass responseClass)
{
    switch (responseClass)
    {
    case ResponseClass::Success:
        return "success";
    case ResponseClass::Informational:
        return "informational";
    case ResponseClass::Error:
        return "error";
    }
    return "unknown";
}

void clearCommandLedger()
{
    for (CommandLedgerEntry &entry : commandLedger)
    {
        entry = CommandLedgerEntry{};
    }
}

CommandLedgerEntry *freeCommandLedgerEntry()
{
    for (CommandLedgerEntry &entry : commandLedger)
    {
        if (!entry.inUse)
        {
            return &entry;
        }
    }
    return nullptr;
}

bool takeCommandFromLedger(const uint32_t sequence, CommandType &type)
{
    for (CommandLedgerEntry &entry : commandLedger)
    {
        if (entry.inUse && entry.sequence == sequence)
        {
            type = entry.type;
            entry = CommandLedgerEntry{};
            return true;
        }
    }
    return false;
}

void finishTuningCommand(const uint32_t sequence, const bool accepted)
{
    for (uint8_t index = 0; index < MAX_TRACKED_SLICES; ++index)
    {
        SliceState &slice = slices[index];
        if (slice.latestTuningSequence != sequence)
        {
            continue;
        }

        slice.latestTuningSequence = 0;
        if (!accepted)
        {
            slice.frequencyHz = slice.reportedFrequencyHz;
            Serial.printf("[TUNING] Sequence %lu failed; slice=%u resynchronized=%llu.%06llu MHz\n",
                          static_cast<unsigned long>(sequence),
                          index,
                          static_cast<unsigned long long>(
                              slice.frequencyHz / 1000000ULL),
                          static_cast<unsigned long long>(
                              slice.frequencyHz % 1000000ULL));
        }
        return;
    }
}

void finishSliceToggleCommand(const uint32_t sequence,
                              const CommandType type,
                              const bool accepted)
{
    for (uint8_t index = 0; index < MAX_TRACKED_SLICES; ++index)
    {
        SliceState &slice = slices[index];
        uint32_t &pendingSequence =
            type == CommandType::Rit
                ? slice.ritSequence
                : slice.muteSequence;
        if (pendingSequence != sequence)
        {
            continue;
        }

        pendingSequence = 0;
        if (type == CommandType::Rit)
        {
            if (accepted)
            {
                slice.ritOn = !slice.ritOn;
            }
            slice.ritOnAvailable = true;
        }
        else
        {
            if (accepted)
            {
                slice.audioMute = !slice.audioMute;
            }
            slice.audioMuteAvailable = true;
        }
        Serial.printf("[SLICE CONTROL] %s sequence=%lu slice=%u result=%s\n",
                      commandTypeName(type),
                      static_cast<unsigned long>(sequence),
                      index,
                      accepted ? "accepted" : "failed");
        return;
    }
}

void expireCommandLedger(const uint32_t nowMs)
{
    for (CommandLedgerEntry &entry : commandLedger)
    {
        if (!entry.inUse ||
            nowMs - entry.sentMs < COMMAND_RESPONSE_TIMEOUT_MS)
        {
            continue;
        }

        Serial.printf("[SMARTSDR] Command response timeout: sequence=%lu type=%s\n",
                      static_cast<unsigned long>(entry.sequence),
                      commandTypeName(entry.type));
        if (entry.type == CommandType::RfPower &&
            rfPowerRequest.sequence == entry.sequence)
        {
            failRfPowerRequest("command response timeout");
        }
        else if (entry.type == CommandType::Tuning)
        {
            finishTuningCommand(entry.sequence, false);
        }
        else if (entry.type == CommandType::Rit ||
                 entry.type == CommandType::Mute)
        {
            finishSliceToggleCommand(entry.sequence, entry.type, false);
        }
        else if (entry.type == CommandType::ClientBind ||
                 entry.type == CommandType::TxSubscription)
        {
            invalidateClientBinding("client setup response timeout");
        }
        entry = CommandLedgerEntry{};
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
    confirmedRfPower = 0;
    confirmedRfPowerAvailable = false;
    rfPowerStatusGeneration = 0;
    rfPowerRequest = RfPowerRequest{};
    resetSessionReadiness();
    apiLineParser.reset();
    nextSequence = 1;
    clearCommandLedger();
    resetSlices();
    resetClients();
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

uint32_t sendCommand(const char *command, const CommandType type)
{
    if (!apiClient.connected())
    {
        return 0;
    }
    CommandLedgerEntry *ledgerEntry = freeCommandLedgerEntry();
    if (ledgerEntry == nullptr)
    {
        Serial.println("[SMARTSDR TX] Command ledger full; not sent");
        return 0;
    }

    const uint32_t sequence = nextSequence;
    char commandLine[COMMAND_BUFFER_SIZE] = {};
    const int commandLength = snprintf(
        commandLine,
        sizeof(commandLine),
        "C%lu|%s\n",
        static_cast<unsigned long>(sequence),
        command);
    if (commandLength <= 0 ||
        static_cast<size_t>(commandLength) >= sizeof(commandLine))
    {
        Serial.println("[SMARTSDR TX] Command is too long; not sent");
        return 0;
    }

    const size_t expectedBytes = static_cast<size_t>(commandLength);
    const size_t writtenBytes = apiClient.write(
        reinterpret_cast<const uint8_t *>(commandLine),
        expectedBytes);
    if (writtenBytes != expectedBytes)
    {
        Serial.printf("[SMARTSDR TX] TCP write failed: wrote %u of %u bytes; reconnect scheduled\n",
                      static_cast<unsigned>(writtenBytes),
                      static_cast<unsigned>(expectedBytes));
        failRfPowerRequest("TCP write failed");
        clearCommandLedger();
        apiClient.stop();
        resetSessionReadiness();
        rfPowerAvailable = false;
        currentState = SmartSdrConnection::State::RadioFound;
        lastConnectAttemptMs = millis();
        return 0;
    }

    ++nextSequence;
    if (nextSequence == 0)
    {
        nextSequence = 1;
    }
    ledgerEntry->inUse = true;
    ledgerEntry->sequence = sequence;
    ledgerEntry->type = type;
    ledgerEntry->sentMs = millis();
    Serial.printf("[SMARTSDR TX] C%lu|%s\n",
                  static_cast<unsigned long>(sequence),
                  command);
    return sequence;
}

ClientState *clientForHandle(const uint32_t handle)
{
    for (ClientState &client : clients)
    {
        if (client.inUse && client.handle == handle)
        {
            return &client;
        }
    }
    return nullptr;
}

ClientState *freeClientState()
{
    for (ClientState &client : clients)
    {
        if (!client.inUse)
        {
            return &client;
        }
    }
    return nullptr;
}

bool singleActiveSliceClientHandle(uint32_t &clientHandle)
{
    uint8_t activeSliceCount = 0;
    uint32_t selectedHandle = 0;
    bool selectedHandleAvailable = false;
    for (const SliceState &slice : slices)
    {
        if (!slice.inUse || !slice.active)
        {
            continue;
        }
        ++activeSliceCount;
        selectedHandle = slice.clientHandle;
        selectedHandleAvailable = slice.clientHandleAvailable;
    }

    if (activeSliceCount != 1 || !selectedHandleAvailable)
    {
        return false;
    }
    clientHandle = selectedHandle;
    return true;
}

void invalidateClientBinding(const char *reason)
{
    if (currentState == SmartSdrConnection::State::Ready)
    {
        currentState = SmartSdrConnection::State::Connected;
        Serial.printf("[SMARTSDR] Action readiness cleared: %s\n", reason);
    }
    failRfPowerRequest(reason);
    rfPowerAvailable = false;
    currentRfPower = 0;
    confirmedRfPowerAvailable = false;
    confirmedRfPower = 0;
    freshTransmitterStatusReceived = false;
    clientBindAccepted = false;
    txSubscriptionAccepted = false;
    targetClientHandle = 0;
    boundClientHandle = 0;
    clientBindSequence = 0;
    txSubscriptionSequence = 0;
}

void startClientBindingIfPossible()
{
    if (!apiClient.connected() ||
        (currentState != SmartSdrConnection::State::Connected &&
         currentState != SmartSdrConnection::State::Ready))
    {
        return;
    }

    uint32_t desiredHandle = 0;
    if (!singleActiveSliceClientHandle(desiredHandle))
    {
        if (targetClientHandle != 0 || boundClientHandle != 0)
        {
            invalidateClientBinding("active Slice context changed");
        }
        return;
    }

    if (desiredHandle == boundClientHandle &&
        clientBindAccepted && txSubscriptionAccepted)
    {
        return;
    }
    if (desiredHandle == targetClientHandle &&
        (clientBindSequence != 0 || txSubscriptionSequence != 0))
    {
        return;
    }

    ClientState *client = clientForHandle(desiredHandle);
    if (client == nullptr)
    {
        return;
    }

    if (desiredHandle != targetClientHandle)
    {
        invalidateClientBinding("active SmartSDR client changed");
        targetClientHandle = desiredHandle;
    }

    char command[80] = {};
    const int length = snprintf(command,
                                sizeof(command),
                                "client bind client_id=%s",
                                client->clientId);
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(command))
    {
        Serial.println("[SMARTSDR] Client bind command is too long");
        return;
    }

    clientBindSequence = sendCommand(command, CommandType::ClientBind);
    if (clientBindSequence != 0)
    {
        Serial.printf("[SMARTSDR] Binding to active SmartSDR client: handle=0x%08lX\n",
                      static_cast<unsigned long>(desiredHandle));
    }
}

void processClientStatus(const char *payload)
{
    uint32_t handle = 0;
    if (!SmartSdrProtocolParser::parseHexField(payload, "client ", handle))
    {
        return;
    }

    ClientState *client = clientForHandle(handle);
    if (strstr(payload, " disconnected") != nullptr)
    {
        if (client != nullptr)
        {
            *client = ClientState{};
        }
        if (handle == targetClientHandle || handle == boundClientHandle)
        {
            invalidateClientBinding("bound SmartSDR client disconnected");
        }
        startClientBindingIfPossible();
        return;
    }

    char clientId[CLIENT_ID_SIZE] = {};
    if (!SmartSdrProtocolParser::parseTextField(
            payload, "client_id=", clientId, sizeof(clientId)))
    {
        return;
    }

    if (client == nullptr)
    {
        client = freeClientState();
    }
    if (client == nullptr)
    {
        Serial.println("[SMARTSDR] Client table full; status ignored");
        return;
    }

    client->inUse = true;
    client->handle = handle;
    memcpy(client->clientId, clientId, sizeof(client->clientId));
    Serial.printf("[SMARTSDR] SmartSDR client discovered: handle=0x%08lX\n",
                  static_cast<unsigned long>(handle));
    startClientBindingIfPossible();
}

void startApiSession()
{
    Serial.println("[SMARTSDR] TCP connected; starting session");
    apiLineParser.reset();
    resetSessionReadiness();
    rfPowerAvailable = false;
    currentRfPower = 0;
    confirmedRfPower = 0;
    confirmedRfPowerAvailable = false;
    rfPowerStatusGeneration = 0;
    rfPowerRequest = RfPowerRequest{};
    resetSlices();
    resetClients();
    nextSequence = 1;
    clearCommandLedger();
    lastReceiveMs = millis();
    lastPingMs = millis();
    for (const char *command : INITIAL_SESSION_COMMANDS)
    {
        if (sendCommand(command, CommandType::Session) == 0)
        {
            return;
        }
    }
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

void processSliceStatus(const char *payload)
{
    uint8_t sliceNumber = 0;
    if (!SmartSdrProtocolParser::parseSliceNumber(
            payload, MAX_TRACKED_SLICES, sliceNumber))
    {
        return;
    }

    SliceState &slice = slices[sliceNumber];
    uint16_t value = 0;
    if (SmartSdrProtocolParser::parseUnsignedField(payload, "in_use=", value))
    {
        if (value == 0)
        {
            slice = SliceState{};
            Serial.printf("[SMARTSDR] Slice %u no longer in use\n", sliceNumber);
            startClientBindingIfPossible();
            return;
        }
        slice.inUse = true;
    }

    if (SmartSdrProtocolParser::parseUnsignedField(payload, "active=", value))
    {
        slice.active = value != 0;
        slice.inUse = true;
    }

    if (SmartSdrProtocolParser::parseUnsignedField(payload, "rit_on=", value))
    {
        slice.ritOn = value != 0;
        slice.ritOnAvailable = true;
        slice.ritSequence = 0;
        slice.inUse = true;
        Serial.printf("[SMARTSDR] Slice %u RIT=%s\n",
                      sliceNumber,
                      slice.ritOn ? "on" : "off");
    }

    int32_t signedValue = 0;
    if (SmartSdrProtocolParser::parseSignedField(
            payload, "rit_freq=", signedValue))
    {
        slice.ritFrequencyHz = signedValue;
        slice.ritFrequencyAvailable = true;
        slice.inUse = true;
        Serial.printf("[SMARTSDR] Slice %u RIT offset=%ld Hz\n",
                      sliceNumber,
                      static_cast<long>(slice.ritFrequencyHz));
    }

    if (SmartSdrProtocolParser::parseUnsignedField(payload, "audio_mute=", value))
    {
        slice.audioMute = value != 0;
        slice.audioMuteAvailable = true;
        slice.muteSequence = 0;
        slice.inUse = true;
        Serial.printf("[SMARTSDR] Slice %u audio mute=%s\n",
                      sliceNumber,
                      slice.audioMute ? "on" : "off");
    }

    uint32_t clientHandle = 0;
    if (SmartSdrProtocolParser::parseHexField(
            payload, "client_handle=", clientHandle))
    {
        slice.clientHandle = clientHandle;
        slice.clientHandleAvailable = true;
        slice.inUse = true;
    }

    uint64_t frequencyHz = 0;
    if (SmartSdrProtocolParser::parseFrequencyHz(payload, frequencyHz))
    {
        slice.frequencyHz = frequencyHz;
        slice.reportedFrequencyHz = frequencyHz;
        slice.frequencyAvailable = true;
        slice.inUse = true;
        Serial.printf("[SMARTSDR] Slice %u frequency=%llu.%06llu MHz active=%s\n",
                      sliceNumber,
                      static_cast<unsigned long long>(frequencyHz / 1000000ULL),
                      static_cast<unsigned long long>(frequencyHz % 1000000ULL),
                      slice.active ? "yes" : "no");
    }
    startClientBindingIfPossible();
}

int actionReadySliceNumber()
{
    if (!apiClient.connected() ||
        currentState != SmartSdrConnection::State::Ready)
    {
        return -1;
    }

    int selectedSlice = -1;
    uint8_t activeSliceCount = 0;
    for (uint8_t index = 0; index < MAX_TRACKED_SLICES; ++index)
    {
        const SliceState &slice = slices[index];
        if (!slice.inUse || !slice.active)
        {
            continue;
        }
        selectedSlice = index;
        ++activeSliceCount;
    }

    return activeSliceCount == 1 &&
                   slices[selectedSlice].frequencyAvailable
               ? selectedSlice
               : -1;
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

    const ResponseClass responseClass = classifyResponseCode(responseCode);
    if (responseClass != ResponseClass::Success)
    {
        Serial.printf("[RF POWER] Radio did not accept sequence %lu: class=%s code=%08lX\n",
                      static_cast<unsigned long>(sequence),
                      responseClassName(responseClass),
                      static_cast<unsigned long>(responseCode));
        failRfPowerRequest(
            responseClass == ResponseClass::Informational
                ? "Radio returned informational response"
                : "Radio rejected command");
        return;
    }

    rfPowerRequest.responseReceived = true;
    Serial.printf("[RF POWER] Command accepted: sequence=%lu requested=%u%%\n",
                  static_cast<unsigned long>(sequence),
                  rfPowerRequest.requestedPercent);
    completeRfPowerRequestIfConfirmed();
}

void processCommandResponse(const uint32_t sequence,
                            const uint32_t responseCode)
{
    const ResponseClass responseClass = classifyResponseCode(responseCode);
    CommandType type = CommandType::Session;
    if (!takeCommandFromLedger(sequence, type))
    {
        Serial.printf("[SMARTSDR] Response has no pending command: sequence=%lu class=%s code=%08lX\n",
                      static_cast<unsigned long>(sequence),
                      responseClassName(responseClass),
                      static_cast<unsigned long>(responseCode));
        return;
    }

    if (type == CommandType::RfPower)
    {
        processRfPowerResponse(sequence, responseCode);
        return;
    }

    if (type == CommandType::Rit || type == CommandType::Mute)
    {
        finishSliceToggleCommand(
            sequence,
            type,
            responseClass == ResponseClass::Success);
    }

    if (responseClass != ResponseClass::Success &&
        (type == CommandType::ClientBind ||
         type == CommandType::TxSubscription))
    {
        invalidateClientBinding(
            type == CommandType::ClientBind
                ? "SmartSDR client bind rejected"
                : "TX subscription rejected");
    }

    if (responseClass == ResponseClass::Informational)
    {
        if (type == CommandType::Tuning)
        {
            finishTuningCommand(sequence, false);
        }
        Serial.printf("[SMARTSDR] Informational response: sequence=%lu type=%s code=%08lX\n",
                      static_cast<unsigned long>(sequence),
                      commandTypeName(type),
                      static_cast<unsigned long>(responseCode));
        return;
    }

    if (responseClass == ResponseClass::Error)
    {
        if (type == CommandType::Tuning)
        {
            finishTuningCommand(sequence, false);
        }
        Serial.printf("[SMARTSDR] Command rejected: sequence=%lu type=%s code=%08lX\n",
                      static_cast<unsigned long>(sequence),
                      commandTypeName(type),
                      static_cast<unsigned long>(responseCode));
        return;
    }

    if (type == CommandType::Tuning)
    {
        finishTuningCommand(sequence, true);
    }
    if (type == CommandType::ClientBind)
    {
        if (sequence != clientBindSequence || targetClientHandle == 0)
        {
            return;
        }
        clientBindSequence = 0;
        clientBindAccepted = true;
        boundClientHandle = targetClientHandle;
        txSubscriptionSequence =
            sendCommand("sub tx all", CommandType::TxSubscription);
    }
    if (type == CommandType::TxSubscription)
    {
        if (sequence != txSubscriptionSequence || !clientBindAccepted)
        {
            return;
        }
        txSubscriptionSequence = 0;
        txSubscriptionAccepted = true;
        updateSessionReadiness();
    }
    if (type == CommandType::Session)
    {
        if (acceptedSessionCommandCount < INITIAL_SESSION_COMMAND_COUNT)
        {
            ++acceptedSessionCommandCount;
        }
        Serial.printf("[SMARTSDR] Initial session command accepted: %u/%u\n",
                      acceptedSessionCommandCount,
                      INITIAL_SESSION_COMMAND_COUNT);
        updateSessionReadiness();
    }
    if (type != CommandType::Keepalive)
    {
        Serial.printf("[SMARTSDR] Command accepted: sequence=%lu type=%s\n",
                      static_cast<unsigned long>(sequence),
                      commandTypeName(type));
    }
}

void processApiLine(char *line)
{
    Serial.printf("[SMARTSDR RX] %s\n", line);

    uint32_t responseSequence = 0;
    uint32_t responseCode = 0;
    if (SmartSdrProtocolParser::parseResponse(line, responseSequence, responseCode))
    {
        processCommandResponse(responseSequence, responseCode);
        return;
    }

    const char *payload = strchr(line, '|');
    if (payload == nullptr)
    {
        return;
    }
    ++payload;

    if (strncmp(payload, "client ", 7) == 0)
    {
        processClientStatus(payload);
        return;
    }

    if (strncmp(payload, "slice ", 6) == 0)
    {
        processSliceStatus(payload);
        return;
    }

    if (strncmp(payload, "transmit ", 9) != 0 ||
        strncmp(payload, "transmit band ", 14) == 0)
    {
        return;
    }

    uint16_t value = 0;
    if (SmartSdrProtocolParser::parseUnsignedField(line, "rfpower=", value))
    {
        ++rfPowerStatusGeneration;
        if (!rfPowerAvailable || currentRfPower != value)
        {
            Serial.printf("[SMARTSDR] Parsed rfpower raw value=%u\n", value);
        }
        currentRfPower = value;
        rfPowerAvailable = true;
        freshTransmitterStatusReceived = true;
        updateSessionReadiness();
        if (rfPowerRequest.state ==
                SmartSdrConnection::RfPowerRequestState::Pending &&
            rfPowerStatusGeneration !=
                rfPowerRequest.statusGenerationAtStart &&
            value == rfPowerRequest.requestedPercent)
        {
            rfPowerRequest.statusConfirmed = true;
            completeRfPowerRequestIfConfirmed();
        }
        else if (rfPowerRequest.state !=
                 SmartSdrConnection::RfPowerRequestState::Pending &&
                 rfPowerRequest.state !=
                 SmartSdrConnection::RfPowerRequestState::Failed)
        {
            confirmedRfPower = value;
            confirmedRfPowerAvailable = true;
        }
    }
}

void processApiInput()
{
    while (apiClient.available() > 0)
    {
        const char character = static_cast<char>(apiClient.read());
        lastReceiveMs = millis();
        const ApiLineParser::Result result = apiLineParser.push(character);
        if (result == ApiLineParser::Result::LineReady)
        {
            processApiLine(apiLineParser.line());
        }
        else if (result == ApiLineParser::Result::Overflow)
        {
            Serial.printf("[SMARTSDR] RX line exceeded %u bytes; discarding through newline\n",
                          static_cast<unsigned>(ApiLineParser::maxLineLength()));
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
        clearCommandLedger();
        resetSessionReadiness();
        rfPowerAvailable = false;
        currentState = SmartSdrConnection::State::RadioFound;
        lastConnectAttemptMs = millis();
        return;
    }

    processApiInput();
    const uint32_t nowMs = millis();
    expireCommandLedger(nowMs);
    if (nowMs - lastPingMs >= PING_INTERVAL_MS)
    {
        if (sendCommand("ping", CommandType::Keepalive) == 0)
        {
            return;
        }
        lastPingMs = nowMs;
    }
    if (nowMs - lastReceiveMs >= RX_TIMEOUT_MS)
    {
        Serial.printf("[SMARTSDR] RX timeout after %lu ms; closing TCP\n",
                      static_cast<unsigned long>(RX_TIMEOUT_MS));
        failRfPowerRequest("receive timeout");
        clearCommandLedger();
        resetSessionReadiness();
        apiClient.stop();
        rfPowerAvailable = false;
        currentState = SmartSdrConnection::State::RadioFound;
        lastConnectAttemptMs = nowMs;
        return;
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

bool confirmedRfPowerPercent(uint16_t &percent)
{
    if (!confirmedRfPowerAvailable)
    {
        return false;
    }

    percent = confirmedRfPower;
    return true;
}

bool activeSliceFrequencyHz(uint64_t &frequencyHz)
{
    const int sliceNumber = actionReadySliceNumber();
    if (sliceNumber < 0)
    {
        return false;
    }

    frequencyHz = slices[sliceNumber].frequencyHz;
    return true;
}

bool showFrequencyStepMessage(const uint16_t stepHz)
{
    if (stepHz == 0)
    {
        Serial.println("[MESSAGE TX] Skipped: step is invalid");
        return false;
    }

    if (actionReadySliceNumber() < 0)
    {
        Serial.println("[MESSAGE TX] Skipped: action is not ready");
        return false;
    }

    char command[80] = {};
    snprintf(command,
             sizeof(command),
             "message severity=info code=0x000010 \"Tuning step: %u Hz\"",
             static_cast<unsigned>(stepHz));
    const uint32_t sequence =
        sendCommand(command, CommandType::OperatorMessage);
    if (sequence == 0)
    {
        Serial.println("[MESSAGE TX] Failed: TCP command was not written");
        return false;
    }

    Serial.printf("[MESSAGE TX] Tuning step: %u Hz\n",
                  static_cast<unsigned>(stepHz));
    return true;
}

bool showFrequencyStepSpot(const uint16_t stepHz)
{
    if (stepHz == 0)
    {
        Serial.println("[SPOT TX] Skipped: step is invalid");
        return false;
    }

    const int sliceNumber = actionReadySliceNumber();
    if (sliceNumber < 0)
    {
        Serial.println("[SPOT TX] Skipped: action is not ready");
        return false;
    }

    const uint64_t frequencyHz = slices[sliceNumber].frequencyHz;
    char command[160] = {};
    snprintf(command,
             sizeof(command),
             "spot add rx_freq=%llu.%06llu callsign=STEP\177%u\177Hz lifetime_seconds=10 priority=1 trigger_action=None",
             static_cast<unsigned long long>(frequencyHz / 1000000ULL),
             static_cast<unsigned long long>(frequencyHz % 1000000ULL),
             static_cast<unsigned>(stepHz));
    const uint32_t sequence =
        sendCommand(command, CommandType::FrequencyStepSpot);
    if (sequence == 0)
    {
        Serial.println("[SPOT TX] Failed: TCP command was not written");
        return false;
    }

    Serial.printf("[SPOT TX] slice=%d frequency=%llu.%06llu MHz text=\"STEP %u Hz\" lifetime=10 s trigger=None\n",
                  sliceNumber,
                  static_cast<unsigned long long>(frequencyHz / 1000000ULL),
                  static_cast<unsigned long long>(frequencyHz % 1000000ULL),
                  static_cast<unsigned>(stepHz));
    return true;
}

bool activeSliceRitEnabled(bool &enabled)
{
    const int sliceNumber = actionReadySliceNumber();
    if (sliceNumber < 0 || !slices[sliceNumber].ritOnAvailable)
    {
        return false;
    }

    enabled = slices[sliceNumber].ritOn;
    return true;
}

bool activeSliceMuteEnabled(bool &enabled)
{
    const int sliceNumber = actionReadySliceNumber();
    if (sliceNumber < 0 || !slices[sliceNumber].audioMuteAvailable)
    {
        return false;
    }

    enabled = slices[sliceNumber].audioMute;
    return true;
}

bool toggleActiveSliceRit()
{
    const int sliceNumber = actionReadySliceNumber();
    if (sliceNumber < 0)
    {
        Serial.println("[RIT] Toggle skipped: action is not ready");
        return false;
    }

    SliceState &slice = slices[sliceNumber];
    if (!slice.ritOnAvailable || !slice.ritFrequencyAvailable ||
        slice.ritSequence != 0)
    {
        Serial.println("[RIT] Toggle skipped: current state/offset is unavailable or pending");
        return false;
    }

    const bool target = !slice.ritOn;
    char command[64] = {};
    snprintf(command,
             sizeof(command),
             "slice set %d rit_on=%u rit_freq=%ld",
             sliceNumber,
             target ? 1U : 0U,
             static_cast<long>(slice.ritFrequencyHz));
    const uint32_t sequence = sendCommand(command, CommandType::Rit);
    if (sequence == 0)
    {
        return false;
    }

    slice.ritSequence = sequence;
    slice.ritOnAvailable = false;
    Serial.printf("[RIT] Toggle pending: sequence=%lu slice=%d target=%s\n",
                  static_cast<unsigned long>(sequence),
                  sliceNumber,
                  target ? "on" : "off");
    return true;
}

bool toggleActiveSliceMute()
{
    const int sliceNumber = actionReadySliceNumber();
    if (sliceNumber < 0)
    {
        Serial.println("[MUTE] Toggle skipped: action is not ready");
        return false;
    }

    SliceState &slice = slices[sliceNumber];
    if (!slice.audioMuteAvailable || slice.muteSequence != 0)
    {
        Serial.println("[MUTE] Toggle skipped: current state is unavailable or pending");
        return false;
    }

    const bool target = !slice.audioMute;
    char command[48] = {};
    snprintf(command,
             sizeof(command),
             "slice set %d audio_mute=%u",
             sliceNumber,
             target ? 1U : 0U);
    const uint32_t sequence = sendCommand(command, CommandType::Mute);
    if (sequence == 0)
    {
        return false;
    }

    slice.muteSequence = sequence;
    slice.audioMuteAvailable = false;
    Serial.printf("[MUTE] Toggle pending: sequence=%lu slice=%d target=%s\n",
                  static_cast<unsigned long>(sequence),
                  sliceNumber,
                  target ? "on" : "off");
    return true;
}

bool requestRfPowerPercent(const uint8_t percent)
{
    if (percent > 100)
    {
        Serial.println("[RF POWER] Request skipped: percentage is out of range");
        return false;
    }
    const int sliceNumber = actionReadySliceNumber();
    if (sliceNumber < 0)
    {
        Serial.println("[RF POWER] Request skipped: action is not ready");
        return false;
    }

    const uint64_t frequencyHz = slices[sliceNumber].frequencyHz;
    if (frequencyHz >= 50000000ULL && frequencyHz <= 54000000ULL)
    {
        Serial.println("[RF POWER] Request skipped: power keys are disabled on 6 m");
        return false;
    }

    if (rfPowerRequest.state != RfPowerRequestState::Pending &&
        confirmedRfPowerAvailable &&
        confirmedRfPower == percent)
    {
        Serial.printf("[RF POWER] No-op: %u%% is already confirmed\n", percent);
        return false;
    }

    char command[48] = {};
    snprintf(command,
             sizeof(command),
             "transmit set rfpower=%u",
             static_cast<unsigned>(percent));

    const uint32_t sequence = sendCommand(command, CommandType::RfPower);
    if (sequence == 0)
    {
        return false;
    }

    if (rfPowerRequest.state == RfPowerRequestState::Pending)
    {
        Serial.printf("[RF POWER] Superseding sequence=%lu requested=%u%%\n",
                      static_cast<unsigned long>(rfPowerRequest.sequence),
                      rfPowerRequest.requestedPercent);
    }

    rfPowerRequest = RfPowerRequest{};
    rfPowerRequest.state = RfPowerRequestState::Pending;
    rfPowerRequest.sequence = sequence;
    rfPowerRequest.startedMs = millis();
    rfPowerRequest.statusGenerationAtStart = rfPowerStatusGeneration;
    rfPowerRequest.requestedPercent = percent;

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
    const int sliceNumber = actionReadySliceNumber();
    if (sliceNumber < 0)
    {
        Serial.println("[ENCODER TX] Skipped: action is not ready");
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
    const uint32_t sequence = sendCommand(command, CommandType::Tuning);
    if (sequence == 0)
    {
        Serial.println("[ENCODER TX] Failed: TCP command was not written");
        return false;
    }

    // Keep an optimistic target so rapid turns build on the last command even
    // before the corresponding asynchronous slice status arrives.
    slice.latestTuningSequence = sequence;
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
