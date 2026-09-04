#include "WifiProvisioning.h"

#include <DNSServer.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>

#include <cstddef>
#include <cstring>

#include "WifiSecrets.local.h"

namespace
{
constexpr char PREFERENCES_NAMESPACE[] = "flexwifi";
constexpr char SSID_KEY[] = "ssid";
constexpr char PASSWORD_KEY[] = "password";
constexpr char CREDENTIAL_SLOT_KEYS[][7] = {"cred_0", "cred_1"};
constexpr char ACTIVE_CREDENTIAL_SLOT_KEY[] = "cred_active";
constexpr uint8_t CREDENTIAL_SLOT_COUNT = 2;
constexpr uint8_t INVALID_CREDENTIAL_SLOT = UINT8_MAX;
constexpr uint32_t CREDENTIAL_MAGIC = 0x57494649UL;
constexpr uint16_t CREDENTIAL_VERSION = 1;
constexpr size_t MAX_SSID_LENGTH = 32;
constexpr size_t MAX_PASSWORD_LENGTH = 64;
constexpr size_t MIN_SETUP_AP_PASSWORD_LENGTH = 8;
constexpr size_t MAX_SETUP_AP_PASSWORD_LENGTH = 63;
constexpr char SETUP_AP_NAME[] = "ESP32-Radio-Setup";
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 20000;
constexpr uint32_t RESTART_DELAY_MS = 1500;
constexpr uint16_t DNS_PORT = 53;

DNSServer dnsServer;
WebServer webServer(80);
WifiProvisioning::State currentState = WifiProvisioning::State::Idle;
String configuredSsid;
String configuredPassword;
uint32_t connectionStartedMs = 0;
uint32_t restartRequestedMs = 0;
bool restartRequested = false;
bool portalHandlersConfigured = false;
bool stationConnectedOnce = false;
uint32_t stationAttempt = 0;

struct CredentialRecord
{
    uint32_t magic = CREDENTIAL_MAGIC;
    uint16_t version = CREDENTIAL_VERSION;
    uint8_t ssidLength = 0;
    uint8_t passwordLength = 0;
    char ssid[MAX_SSID_LENGTH + 1] = {};
    char password[MAX_PASSWORD_LENGTH + 1] = {};
    uint8_t reserved[2] = {};
    uint32_t checksum = 0;
};

static_assert(offsetof(CredentialRecord, checksum) == 108,
              "Credential record layout changed");
static_assert(sizeof(CredentialRecord) == 112,
              "Credential record size changed");
static_assert(sizeof(WifiSecrets::SETUP_AP_PASSWORD) - 1U >=
                  MIN_SETUP_AP_PASSWORD_LENGTH &&
              sizeof(WifiSecrets::SETUP_AP_PASSWORD) - 1U <=
                  MAX_SETUP_AP_PASSWORD_LENGTH,
              "The setup AP password must contain 8 to 63 characters");

uint32_t credentialChecksum(const CredentialRecord &record)
{
    constexpr uint32_t FNV_OFFSET_BASIS = 2166136261UL;
    constexpr uint32_t FNV_PRIME = 16777619UL;
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&record);
    uint32_t checksum = FNV_OFFSET_BASIS;
    for (size_t index = 0; index < offsetof(CredentialRecord, checksum); ++index)
    {
        checksum ^= bytes[index];
        checksum *= FNV_PRIME;
    }
    return checksum;
}

bool makeCredentialRecord(const String &ssid,
                          const String &password,
                          CredentialRecord &record)
{
    if (ssid.isEmpty() || ssid.length() > MAX_SSID_LENGTH ||
        password.length() > MAX_PASSWORD_LENGTH)
    {
        return false;
    }

    record = CredentialRecord{};
    record.ssidLength = static_cast<uint8_t>(ssid.length());
    record.passwordLength = static_cast<uint8_t>(password.length());
    memcpy(record.ssid, ssid.c_str(), record.ssidLength);
    memcpy(record.password, password.c_str(), record.passwordLength);
    record.checksum = credentialChecksum(record);
    return true;
}

bool credentialRecordValid(const CredentialRecord &record)
{
    return record.magic == CREDENTIAL_MAGIC &&
           record.version == CREDENTIAL_VERSION &&
           record.ssidLength > 0 &&
           record.ssidLength <= MAX_SSID_LENGTH &&
           record.passwordLength <= MAX_PASSWORD_LENGTH &&
           record.ssid[record.ssidLength] == '\0' &&
           record.password[record.passwordLength] == '\0' &&
           strlen(record.ssid) == record.ssidLength &&
           strlen(record.password) == record.passwordLength &&
           record.checksum == credentialChecksum(record);
}

bool readCredentialSlot(Preferences &preferences,
                        const uint8_t slot,
                        CredentialRecord &record)
{
    if (slot >= CREDENTIAL_SLOT_COUNT ||
        preferences.getBytesLength(CREDENTIAL_SLOT_KEYS[slot]) !=
            sizeof(CredentialRecord))
    {
        return false;
    }

    return preferences.getBytes(CREDENTIAL_SLOT_KEYS[slot],
                                &record,
                                sizeof(record)) == sizeof(record) &&
           credentialRecordValid(record);
}

bool saveStoredCredentialRecord(const String &ssid, const String &password)
{
    CredentialRecord stagedRecord;
    if (!makeCredentialRecord(ssid, password, stagedRecord))
    {
        return false;
    }

    Preferences preferences;
    if (!preferences.begin(PREFERENCES_NAMESPACE, false))
    {
        Serial.println("[WIFI] Preferences open failed while saving");
        return false;
    }

    const uint8_t activeSlot =
        preferences.getUChar(ACTIVE_CREDENTIAL_SLOT_KEY,
                             INVALID_CREDENTIAL_SLOT);
    const uint8_t stagedSlot = activeSlot == 0 ? 1 : 0;
    const size_t written = preferences.putBytes(
        CREDENTIAL_SLOT_KEYS[stagedSlot],
        &stagedRecord,
        sizeof(stagedRecord));

    CredentialRecord verifiedRecord;
    const bool stagedRecordVerified =
        written == sizeof(stagedRecord) &&
        readCredentialSlot(preferences, stagedSlot, verifiedRecord) &&
        memcmp(&stagedRecord, &verifiedRecord, sizeof(stagedRecord)) == 0;
    if (!stagedRecordVerified)
    {
        preferences.end();
        Serial.println("[WIFI] Staged credential record verification failed");
        return false;
    }

    const bool activated =
        preferences.putUChar(ACTIVE_CREDENTIAL_SLOT_KEY, stagedSlot) ==
        sizeof(stagedSlot);
    preferences.end();
    if (!activated)
    {
        Serial.println("[WIFI] Credential activation failed; previous record preserved");
        return false;
    }

    Serial.printf("[WIFI] Credential record version=%u activated in slot=%u\n",
                  CREDENTIAL_VERSION,
                  stagedSlot);
    return true;
}

bool eraseStoredCredentialRecord()
{
    Preferences preferences;
    if (!preferences.begin(PREFERENCES_NAMESPACE, false))
    {
        Serial.println("[WIFI] Preferences open failed while erasing");
        return false;
    }

    const bool erased = preferences.clear();
    preferences.end();
    if (!erased)
    {
        Serial.println("[WIFI] Stored credential erase failed");
        return false;
    }

    Serial.println("[WIFI] Stored credentials erased");
    return true;
}

String configurationPage()
{
    return F(
        "<!doctype html><html lang='en'><head>"
        "<meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Radio Wi-Fi Setup</title><style>"
        "body{font-family:sans-serif;max-width:34rem;margin:2rem auto;padding:0 1rem}"
        "label{display:block;margin-top:1rem}input{box-sizing:border-box;width:100%;padding:.7rem}"
        "button{margin-top:1.2rem;padding:.8rem 1.2rem}</style></head><body>"
        "<h1>Radio Wi-Fi Setup</h1>"
        "<p>Replace the stored Wi-Fi credentials. The device will restart after saving.</p>"
        "<form method='post' action='/save'>"
        "<label>Wi-Fi network name (SSID)<input name='ssid' maxlength='32' required></label>"
        "<label>Password<input name='password' type='password' maxlength='64'></label>"
        "<button type='submit'>Replace and restart</button></form>"
        "<hr><h2>Erase stored credentials</h2>"
        "<p>After restart, compiled fallback credentials are used when available.</p>"
        "<form method='post' action='/erase'>"
        "<label>Type ERASE to confirm<input name='confirm' pattern='ERASE' autocomplete='off' required></label>"
        "<button type='submit'>Erase and restart</button></form></body></html>");
}

void handleRoot()
{
    webServer.send(200, "text/html; charset=utf-8", configurationPage());
}

void handleSave()
{
    const String ssid = webServer.arg("ssid");
    const String password = webServer.arg("password");
    Serial.printf("[WIFI] Configuration received for SSID=\"%s\"\n",
                  ssid.c_str());
    if (ssid.isEmpty() || ssid.length() > MAX_SSID_LENGTH ||
        password.length() > MAX_PASSWORD_LENGTH)
    {
        Serial.println("[WIFI] Configuration rejected: invalid field length");
        webServer.send(400,
                       "text/plain; charset=utf-8",
                       "Invalid Wi-Fi credentials.");
        return;
    }

    if (!saveStoredCredentialRecord(ssid, password))
    {
        Serial.println("[WIFI] Preferences write failed");
        webServer.send(500,
                       "text/plain; charset=utf-8",
                       "Could not save the Wi-Fi credentials.");
        return;
    }

    webServer.send(200,
                   "text/html; charset=utf-8",
                   "<!doctype html><html lang='en'><meta charset='utf-8'>"
                   "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                   "<body><h1>Saved</h1><p>The device is restarting.</p></body></html>");
    restartRequested = true;
    restartRequestedMs = millis();
    Serial.println("[WIFI] Configuration saved; restart scheduled");
}

void handleErase()
{
    if (webServer.arg("confirm") != "ERASE")
    {
        Serial.println("[WIFI] Credential erase rejected: confirmation missing");
        webServer.send(400,
                       "text/plain; charset=utf-8",
                       "Type ERASE to confirm deletion.");
        return;
    }

    if (!eraseStoredCredentialRecord())
    {
        webServer.send(500,
                       "text/plain; charset=utf-8",
                       "Could not erase the stored Wi-Fi credentials.");
        return;
    }

    webServer.send(200,
                   "text/html; charset=utf-8",
                   "<!doctype html><html lang='en'><meta charset='utf-8'>"
                   "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                   "<body><h1>Erased</h1><p>The device is restarting.</p></body></html>");
    restartRequested = true;
    restartRequestedMs = millis();
    Serial.println("[WIFI] Stored credentials erased; restart scheduled");
}

void configurePortalHandlers()
{
    if (portalHandlersConfigured)
    {
        return;
    }
    webServer.on("/", HTTP_GET, handleRoot);
    webServer.on("/save", HTTP_POST, handleSave);
    webServer.on("/erase", HTTP_POST, handleErase);
    webServer.onNotFound(handleRoot);
    portalHandlersConfigured = true;
}

void startPortal()
{
    Serial.println("[WIFI] Starting setup access point");
    Serial.println("[WIFI] Portal step 1/4: enabling AP-only mode");
    const bool accessPointModeEnabled = WiFi.mode(WIFI_AP);
    Serial.printf("[WIFI] Portal AP-only mode: %s\n",
                  accessPointModeEnabled ? "ready" : "failed");
    yield();

    Serial.println("[WIFI] Portal step 2/4: starting SoftAP");
    const bool accessPointStarted =
        WiFi.softAP(SETUP_AP_NAME, WifiSecrets::SETUP_AP_PASSWORD);
    Serial.printf("[WIFI] Portal SoftAP: %s\n",
                  accessPointStarted ? "ready" : "failed");

    Serial.println("[WIFI] Portal step 3/4: starting captive DNS");
    dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
    configurePortalHandlers();

    Serial.println("[WIFI] Portal step 4/4: starting web server");
    webServer.begin();
    currentState = WifiProvisioning::State::Portal;
    Serial.printf("[WIFI] Setup portal %s: SSID=\"%s\" IP=%s\n",
                  accessPointStarted ? "ready" : "start failed",
                  SETUP_AP_NAME,
                  WiFi.softAPIP().toString().c_str());
}

void startStationConnection()
{
    ++stationAttempt;
    Serial.printf("[WIFI] Station connection attempt %lu: SSID=\"%s\"\n",
                  static_cast<unsigned long>(stationAttempt),
                  configuredSsid.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.persistent(false);
    WiFi.begin(configuredSsid.c_str(), configuredPassword.c_str());
    connectionStartedMs = millis();
    currentState = WifiProvisioning::State::Connecting;
}

bool loadLocalCredentials()
{
    static_assert(sizeof(WifiSecrets::SSID) - 1U <= MAX_SSID_LENGTH,
                  "The local WiFi SSID must not exceed 32 characters");
    static_assert(sizeof(WifiSecrets::PASSWORD) - 1U <= MAX_PASSWORD_LENGTH,
                  "The local WiFi password must not exceed 64 characters");

    if (WifiSecrets::SSID[0] == '\0' || WifiSecrets::PASSWORD[0] == '\0')
    {
        Serial.println("[WIFI] Compiled fallback credentials incomplete");
        return false;
    }

    configuredSsid = WifiSecrets::SSID;
    configuredPassword = WifiSecrets::PASSWORD;
    Serial.printf("[WIFI] Compiled fallback credentials selected: SSID=\"%s\"\n",
                  configuredSsid.c_str());
    return true;
}

bool loadStoredCredentials()
{
    Preferences preferences;
    if (!preferences.begin(PREFERENCES_NAMESPACE, true))
    {
        Serial.println("[WIFI] Stored credentials not available");
        return false;
    }
    const uint8_t activeSlot =
        preferences.getUChar(ACTIVE_CREDENTIAL_SLOT_KEY,
                             INVALID_CREDENTIAL_SLOT);
    CredentialRecord record;
    if (readCredentialSlot(preferences, activeSlot, record))
    {
        preferences.end();
        configuredSsid = String(record.ssid);
        configuredPassword = String(record.password);
        Serial.printf("[WIFI] Stored credential record version=%u slot=%u selected: SSID=\"%s\"\n",
                      record.version,
                      activeSlot,
                      configuredSsid.c_str());
        return true;
    }

    const String storedSsid = preferences.getString(SSID_KEY, "");
    const String storedPassword = preferences.getString(PASSWORD_KEY, "");
    preferences.end();

    if (storedSsid.isEmpty() || storedSsid.length() > MAX_SSID_LENGTH ||
        storedPassword.length() > MAX_PASSWORD_LENGTH)
    {
        Serial.println("[WIFI] Stored credentials missing or invalid");
        return false;
    }

    configuredSsid = storedSsid;
    configuredPassword = storedPassword;
    Serial.printf("[WIFI] Legacy stored credentials selected: SSID=\"%s\"\n",
                  configuredSsid.c_str());
    if (saveStoredCredentialRecord(configuredSsid, configuredPassword))
    {
        Serial.println("[WIFI] Legacy credentials migrated to versioned record");
    }
    else
    {
        Serial.println("[WIFI] Legacy credential migration failed; using legacy data for this boot");
    }
    return true;
}
} // namespace

namespace WifiProvisioning
{
void begin()
{
    Serial.println("[WIFI] Provisioning service initializing");
    restartRequested = false;
    stationConnectedOnce = false;
    stationAttempt = 0;
    currentState = State::Idle;
    if (loadStoredCredentials() || loadLocalCredentials())
    {
        startStationConnection();
    }
    else
    {
        startPortal();
    }
}

bool openSetupPortal()
{
    if (currentState == State::Portal)
    {
        return true;
    }

    restartRequested = false;
    Serial.println("[WIFI] Setup portal requested by local key gesture");
    startPortal();
    return currentState == State::Portal;
}

void update()
{
    if (restartRequested && millis() - restartRequestedMs >= RESTART_DELAY_MS)
    {
        Serial.println("[WIFI] Restarting after configuration update");
        ESP.restart();
    }

    if (currentState == State::Portal)
    {
        dnsServer.processNextRequest();
        webServer.handleClient();
        return;
    }

    if (WiFi.status() == WL_CONNECTED)
    {
        stationConnectedOnce = true;
        currentState = State::Connected;
        return;
    }

    if (currentState == State::Connected)
    {
        Serial.printf("[WIFI] Connection lost, status=%d; reconnecting\n",
                      static_cast<int>(WiFi.status()));
        startStationConnection();
        return;
    }

    if (currentState == State::Connecting &&
        millis() - connectionStartedMs >= WIFI_CONNECT_TIMEOUT_MS)
    {
        if (!stationConnectedOnce)
        {
            Serial.printf("[WIFI] Initial connection timeout after %lu ms\n",
                          static_cast<unsigned long>(WIFI_CONNECT_TIMEOUT_MS));
            startPortal();
            return;
        }

        ++stationAttempt;
        Serial.printf("[WIFI] Reconnect timeout after %lu ms; attempt %lu\n",
                      static_cast<unsigned long>(WIFI_CONNECT_TIMEOUT_MS),
                      static_cast<unsigned long>(stationAttempt));
        const bool reconnectStarted = WiFi.reconnect();
        Serial.printf("[WIFI] Station reconnect request: %s\n",
                      reconnectStarted ? "accepted" : "not accepted");
        connectionStartedMs = millis();
    }
}

State state()
{
    return currentState;
}

bool isConnected()
{
    return currentState == State::Connected && WiFi.status() == WL_CONNECTED;
}

IPAddress localIp()
{
    return isConnected() ? WiFi.localIP() : IPAddress();
}

const char *setupAccessPointName()
{
    return SETUP_AP_NAME;
}
} // namespace WifiProvisioning
