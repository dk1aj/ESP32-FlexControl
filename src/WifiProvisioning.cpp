#include "WifiProvisioning.h"

#include <DNSServer.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>

#include "WifiSecrets.local.h"

namespace
{
constexpr char PREFERENCES_NAMESPACE[] = "flexwifi";
constexpr char SSID_KEY[] = "ssid";
constexpr char PASSWORD_KEY[] = "password";
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
uint32_t stationAttempt = 0;

String configurationPage()
{
    return F(
        "<!doctype html><html lang='de'><head>"
        "<meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Radio WLAN</title><style>"
        "body{font-family:sans-serif;max-width:34rem;margin:2rem auto;padding:0 1rem}"
        "label{display:block;margin-top:1rem}input{box-sizing:border-box;width:100%;padding:.7rem}"
        "button{margin-top:1.2rem;padding:.8rem 1.2rem}</style></head><body>"
        "<h1>Radio WLAN</h1>"
        "<p>WLAN-Zugangsdaten eingeben. Das Geraet startet danach neu.</p>"
        "<form method='post' action='/save'>"
        "<label>WLAN-Name (SSID)<input name='ssid' maxlength='32' required></label>"
        "<label>Passwort<input name='password' type='password' maxlength='64'></label>"
        "<button type='submit'>Speichern</button></form></body></html>");
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
    if (ssid.isEmpty() || ssid.length() > 32 || password.length() > 64)
    {
        Serial.println("[WIFI] Configuration rejected: invalid field length");
        webServer.send(400,
                       "text/plain; charset=utf-8",
                       "Ungueltige WLAN-Zugangsdaten.");
        return;
    }

    Preferences preferences;
    if (!preferences.begin(PREFERENCES_NAMESPACE, false))
    {
        Serial.println("[WIFI] Preferences open failed while saving");
        webServer.send(500,
                       "text/plain; charset=utf-8",
                       "Speichern fehlgeschlagen.");
        return;
    }
    const size_t ssidBytes = preferences.putString(SSID_KEY, ssid);
    const size_t passwordBytes = preferences.putString(PASSWORD_KEY, password);
    preferences.end();

    if (ssidBytes == 0 || (!password.isEmpty() && passwordBytes == 0))
    {
        Serial.println("[WIFI] Preferences write failed");
        webServer.send(500,
                       "text/plain; charset=utf-8",
                       "Speichern fehlgeschlagen.");
        return;
    }

    webServer.send(200,
                   "text/html; charset=utf-8",
                   "<!doctype html><html lang='de'><meta charset='utf-8'>"
                   "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                   "<body><h1>Gespeichert</h1><p>Das Geraet startet neu.</p></body></html>");
    restartRequested = true;
    restartRequestedMs = millis();
    Serial.println("[WIFI] Configuration saved; restart scheduled");
}

void configurePortalHandlers()
{
    if (portalHandlersConfigured)
    {
        return;
    }
    webServer.on("/", HTTP_GET, handleRoot);
    webServer.on("/save", HTTP_POST, handleSave);
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
    static_assert(sizeof(WifiSecrets::SSID) - 1U <= 32U,
                  "The local WiFi SSID must not exceed 32 characters");
    static_assert(sizeof(WifiSecrets::PASSWORD) - 1U <= 64U,
                  "The local WiFi password must not exceed 64 characters");

    if (WifiSecrets::SSID[0] == '\0' || WifiSecrets::PASSWORD[0] == '\0')
    {
        Serial.println("[WIFI] Local credentials incomplete; checking Preferences");
        return false;
    }

    configuredSsid = WifiSecrets::SSID;
    configuredPassword = WifiSecrets::PASSWORD;
    Serial.printf("[WIFI] Local credentials selected: SSID=\"%s\"\n",
                  configuredSsid.c_str());
    return true;
}

bool loadStoredCredentials()
{
    Preferences preferences;
    if (!preferences.begin(PREFERENCES_NAMESPACE, false))
    {
        Serial.println("[WIFI] Preferences namespace open/create failed");
        return false;
    }
    configuredSsid = preferences.getString(SSID_KEY, "");
    configuredPassword = preferences.getString(PASSWORD_KEY, "");
    preferences.end();
    Serial.printf("[WIFI] Stored credentials: %s\n",
                  configuredSsid.isEmpty() ? "not found" : "SSID found");
    return !configuredSsid.isEmpty();
}
} // namespace

namespace WifiProvisioning
{
void begin()
{
    Serial.println("[WIFI] Provisioning service initializing");
    restartRequested = false;
    stationAttempt = 0;
    currentState = State::Idle;
    if (loadLocalCredentials() || loadStoredCredentials())
    {
        startStationConnection();
    }
    else
    {
        startPortal();
    }
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
        Serial.printf("[WIFI] Connection timeout after %lu ms\n",
                      static_cast<unsigned long>(WIFI_CONNECT_TIMEOUT_MS));
        startPortal();
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
