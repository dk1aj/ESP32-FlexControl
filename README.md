# ESP32-S3 Handwheel FlexRadio Controller

PlatformIO-Firmware für ein Handrad und ein Adafruit NeoKey Snap 3x4 am
FLEX AU-510M. Die spätere Bedienung soll direkt über die SmartSDR-TCP/IP-API
erfolgen; FRStack oder ein PC sind dafür nicht erforderlich.

Der Netzwerkzugriff ist derzeit **read-only**: Die Firmware findet das Funkgerät,
verbindet sich mit SmartSDR und liest `rfpower`, verändert aber noch keine
Leistungs- oder Bedienwerte.

## Funktionen

- ESP32-S3-PCNT-Encoder mit x4-Auswertung und 64-Bit-Position
- entprellte 3x4-Tastenmatrix mit dezenter Grundbeleuchtung und Tastenanimationen
- WLAN-Einrichtungsportal und gespeicherte Zugangsdaten
- FLEX-Discovery, SmartSDR-TCP, Keepalive und automatische Wiederverbindung
- RGB-Statusanzeige auf GPIO48
- serielle Diagnose mit 115200 Baud

Es gibt genau ein PlatformIO-Environment: `esp32-s3-n16r8`.

## Anschlussbelegung

| Funktion | Pin |
| --- | --- |
| Encoder A / B | GPIO4 / GPIO5 |
| NeoKey COL1 / COL2 / COL3 | GPIO2 / GPIO42 / GPIO41 |
| NeoKey ROW1 / ROW2 / ROW3 / ROW4 | GPIO40 / GPIO39 / GPIO38 / GPIO47 |
| NeoPixel Data IN | GPIO21 |
| RGB-/Status-LED | GPIO48 |
| USB D- / D+ | GPIO19 / GPIO20 |

GPIO35 bis GPIO37 sind beim N16R8-Modul durch das Octal-PSRAM belegt. Der
ESP32-S3 ist nicht 5-V-tolerant; alle Komponenten benötigen eine sichere
3,3-V-Schnittstelle und gemeinsame Masse.

Das NeoKey ist um 180 Grad gedreht montiert:

```text
        COL1  COL2  COL3
ROW1     12    11    10
ROW2      9     8     7
ROW3      6     5     4
ROW4      3     2     1
```

## WLAN und Funkgerät

Zuerst wird `include/WifiSecrets.example.h` als
`include/WifiSecrets.local.h` kopiert. Dort werden lokale Zugangsdaten
eingetragen:

```cpp
constexpr char SSID[] = "SYNOLOGY";
constexpr char PASSWORD[] = "Dein Passwort";
constexpr char SETUP_AP_PASSWORD[] = "Dein Portal-Passwort";
```

Die ignorierte Datei bleibt lokal. Fehlen gültige Zugangsdaten oder scheitert
die Verbindung, startet das Portal:

```text
SSID: ESP32-FlexRadio-Setup
Adresse: http://192.168.4.1
```

Die Radio-Fallbackadresse steht in `include/RadioConfig.h`. Discovery und
SmartSDR verwenden Port 4992. Das Frontpanel ist kein API-Ziel.

## Build und Diagnose

```text
pio run -e esp32-s3-n16r8
pio run -e esp32-s3-n16r8 -t upload
pio device monitor -b 115200
```

GPIO48 zeigt den Verbindungsstand: blau = WLAN, orange = Portal, cyan = Suche,
violett = TCP-Aufbau, gelb = TCP bereit und grün = `rfpower` empfangen.

## Projektstand

Die zwölf Tasten besitzen vorläufige Leistungsprofile von 45 bis 500 W. Diese
werden beim Loslassen nur seriell ausgegeben. Ein SmartSDR-Schreibzugriff folgt
erst nach Klärung von Skalierung, Clientbindung und Rücklesebestätigung.

Planung und nachgewiesener Fortschritt werden getrennt gepflegt:

- [Entwicklungsplan](PLAN.md)
- [Umsetzungs- und Teststand](DEVELOPMENT_PROGRESS.md)
