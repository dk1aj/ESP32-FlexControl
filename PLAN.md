# Entwicklungsplan: ESP32-S3 Handrad, NeoKey und FLEX AU-510M

## 1. Zweck und Geltungsbereich

Dieser Plan ist ab dem 19. August 2026 der verbindliche Gesamtplan für das
PlatformIO-Projekt `ESP32_S3_Encoder`.

Er ersetzt als operative Arbeitsgrundlage die älteren Einzelpläne:

- `CODEX_PLAN_DETAILED_SOFTWARE_DEVELOPMENT.md`;
- `CODEX_PLAN_ESP32_S3_Handwheel_NeoKey.md`.

Der frühere FlexControl-Plan und der zugehörige USB-Legacy-Code wurden am
19. August 2026 vollständig entfernt. Die beiden verbleibenden älteren Dateien
dienen nur noch als historische Dokumentation. Bei Widersprüchen gelten:

1. der erfolgreich gebaute aktuelle Quellstand;
2. dieser `PLAN.md`;
3. `DEVELOPMENT_PROGRESS.md` als chronologisches Prüfprotokoll;
4. die beiden historischen Codex-Pläne.

## 2. Hauptziel

Der ESP32-S3 soll das Handrad und alle zwölf NeoKey-Tasten auswerten und den
FLEX AU-510M direkt über das lokale WLAN bedienen.

Das Hauptziel ist:

```text
Handrad + zwölf Tasten + Tastenbeleuchtung
                    ->
                ESP32-S3
                    ->
             WLAN / SmartSDR API
                    ->
              FLEX AU-510M
```

Eine PC-Software oder nachgebildete USB-Geräteidentität ist für den
Normalbetrieb nicht erforderlich. Das Projekt besitzt nur noch den direkten
WLAN-/SmartSDR-Pfad.

### 2.1 Einordnung der verfügbaren FlexRadio-Schnittstellen

Für die Anbindung kommen grundsätzlich vier Wege infrage:

| Lösung | ESP32 direkt geeignet | zusätzlicher PC/Dienst | Umfang |
| --- | --- | --- | --- |
| FRStack Web API | sehr einfach per HTTP/REST | ja | hoch |
| SmartSDR TCP/IP API direkt | sehr gut | nein | sehr hoch |
| FlexLib/FlexAPI | nein, .NET-/Windows-orientiert | ja | vollständig |
| Hamlib | für diesen ESP32-Zweck eher ungeeignet | normalerweise ja | mittel |

#### FRStack Web API

FRStack ist keine auf dem ESP32 eingebundene Funkgerätebibliothek, sondern eine
zusätzliche REST-Schicht über der FlexRadio-API. Endpunkte für Funktionen wie
`RFPOWER` oder `AUDIOGAIN` sind für Stream Deck, Node-RED und einfache
HTTP-Steuerungen bequem. Dafür muss FRStack jedoch dauerhaft auf einem PC oder
einem anderen Rechner im Netzwerk laufen.

FRStack kann während der Entwicklung als Vergleichs- und Diagnosewerkzeug
nützlich sein. Es darf für den vorgesehenen eigenständigen Betrieb dieses
Controllers aber keine zwingende Laufzeitabhängigkeit werden.

#### SmartSDR TCP/IP API direkt

Der ESP32 verbindet sich unmittelbar mit dem FLEX-Gerät. Discovery,
Statusmeldungen und Bedienkommandos werden ohne FRStack-PC verarbeitet. Diese
plattformunabhängige Schnittstelle ist der verbindliche Hauptweg dieses
Projekts.

#### FlexLib/FlexAPI

FlexLib ist die offizielle .NET-Abstraktion über der SmartSDR-API. Sie stellt
Klassen und Ereignisse für Discovery, Radio, Slices, Meter, Panadapter und
Streaming bereit. Die am 19. August 2026 offiziell angebotene v4.x-Ausgabe ist
FlexLib 4.2.20. FlexLib ist eine sinnvolle Referenz für Windows-/C#-Programme,
aber keine geeignete direkte Abhängigkeit für die Arduino-Firmware des
ESP32-S3.

#### Hamlib

Hamlib ist eine allgemeine Amateurfunkbibliothek und unterstützt typische
Rig-Funktionen sowie SmartSDR-Slices. Für eine tiefe FlexRadio-Integration und
den direkten, schlanken Betrieb auf diesem ESP32 bietet Hamlib gegenüber der
SmartSDR-TCP/IP-API keinen Vorteil.

#### Waveform API

Die Waveform API ist für eigene digitale Betriebsarten und DSP-Funktionen in
der FlexRadio-Umgebung vorgesehen. Encoder, Tasten, Lautstärke und RF Power
benötigen sie nicht; sie bleibt außerhalb dieses Projekts.

### 2.2 Verbindliche Architekturentscheidung

```text
Produktivpfad:
ESP32-S3 -> WLAN -> SmartSDR TCP/IP API -> FLEX AU-510M

Optionale Entwicklungsreferenz:
Stream Deck / Testprogramm -> FRStack REST -> FlexRadio

Nicht im ESP32:
FlexLib/.NET, Hamlib und Waveform API
```

Erfahrungen und Messwerte aus einem vorhandenen Stream-Deck-/FRStack-Aufbau
können zum Vergleich verwendet werden. Die ESP32-Firmware muss trotzdem ohne
FRStack, FlexLib, Hamlib und PC funktionieren.

## 3. Geschützter funktionierender Bestand

Folgende Bestandteile dürfen nicht ohne einen nachgewiesenen technischen Grund
ersetzt oder grundlegend umgebaut werden:

- PCNT-Hardwarezähler für den Encoder;
- Encoder A auf GPIO4 und Encoder B auf GPIO5;
- x4-Quadraturauswertung mit normalerweise 400 Counts pro Umdrehung;
- Hardware-Glitch-Filter;
- PCNT-Limit-ISR und signed 64-bit-Position;
- `resetEncoderPosition()`;
- 3x4-Matrixscan des NeoKey;
- individuelle 20-ms-Entprellung aller zwölf Tasten;
- logische 180-Grad-Drehung der Tastenanordnung;
- bestehendes Zickzack-Mapping der Tastenpixel;
- eine zentrale NeoPixel-Kette mit 15 Pixeln auf GPIO21;
- einziges Build-Environment `esp32-s3-n16r8`;
- native USB-Pins GPIO19 und GPIO20;
- freie TX- und RX1-Anschlüsse.

Aus ISR-Kontext dürfen weiterhin weder USB-/Netzwerkdaten geschrieben noch
NeoPixel, Tastenaktionen oder Stringformatierungen verarbeitet werden.

## 4. Hardware- und Pinbelegung

| Funktion | Anschluss |
| --- | --- |
| Encoder A | GPIO4 |
| Encoder B | GPIO5 |
| NeoKey COL1, COL2, COL3 | GPIO2, GPIO42, GPIO41 |
| NeoKey ROW1, ROW2, ROW3, ROW4 | GPIO40, GPIO39, GPIO38, GPIO47 |
| externe NeoPixel-Kette | GPIO21 |
| Onboard-RGB-/Status-LED | GPIO48 |
| USB D- | GPIO19 |
| USB D+ | GPIO20 |

GPIO35 bis GPIO37 sind beim N16R8 durch das Octal-PSRAM belegt und bleiben
vollständig unbenutzt. NeoPixel Data IN wurde deshalb von GPIO36 auf GPIO21
und NeoKey ROW4 von GPIO37 auf GPIO47 verlegt.

Der ESP32-S3 arbeitet mit 3,3-V-Logik und ist nicht 5-V-tolerant. Encoder,
NeoKey und Pixelkette benötigen eine sichere Pegelanpassung beziehungsweise
Versorgung und eine gemeinsame Masse gemäß dem geprüften Hardwareaufbau.

## 5. Aktuell implementierter Stand

### 5.1 Encoder und NeoKey

- PCNT-Encoder mit x4-Auswertung und signed 64-bit-Position;
- Matrixscan im 1-ms-Raster und 3-µs-Einschwingzeit je aktiver Spalte;
- zwölf entprellte `PRESSED`- und `RELEASED`-Events;
- Tastenpixel 0 bis 11 mit konstanter Grundbeleuchtung: Taste 1 blau auf RGB
  0/0/1, Tasten 2 bis 12 weiß auf RGB 1/1/1 sowie grüner Druckimpuls, rotes
  Halteblinken und Abblenden nach dem Loslassen; der Atem-/Glow-Effekt ist
  entfernt;
- Pixel 12 bis 14 derzeit ausgeschaltet und ohne Anwendungsfunktion.

### 5.2 Vorläufige Leistungsprofile

Die zwölf Tasten besitzen folgende in 5-W-Schritten aufgerundete Standardwerte:

| Taste | Leistung | Taste | Leistung |
| ---: | ---: | ---: | ---: |
| 1 | 45 W | 7 | 295 W |
| 2 | 85 W | 8 | 335 W |
| 3 | 125 W | 9 | 375 W |
| 4 | 170 W | 10 | 420 W |
| 5 | 210 W | 11 | 460 W |
| 6 | 250 W | 12 | 500 W |

Diese Werte sind zunächst nur Vorgaben in `ButtonConfig.h`. Sie werden im
Diagnose-Build ausgegeben, aber noch nicht an das Funkgerät gesendet.

### 5.3 Einziger Buildmodus

```text
esp32-s3-n16r8
    serielle Detaildiagnose, WLAN, SmartSDR und Status-LED
```

Serielle Diagnose arbeitet einheitlich mit 115200 Baud. Ein zweites
USB-Protokoll-Environment existiert nicht mehr.

## 6. WLAN-Einbindung

### 6.1 Bereits implementierte WLAN-Basis

Die Firmware enthält bereits:

- lokale, über `.gitignore` ausgeschlossene Zugangsdaten in
  `include/WifiSecrets.local.h` mit festem 2,4-GHz-Netz `SYN_2G`;
- Vorrang vollständiger lokaler Zugangsdaten vor `Preferences`, ohne serielle
  Ausgabe des Passworts;
- Speicherung von SSID und Passwort mit `Preferences`;
- automatischen Verbindungsversuch mit gespeicherten Zugangsdaten;
- nicht blockierendes WLAN-Einrichtungsportal bei fehlenden Zugangsdaten oder
  nach einem Verbindungs-Timeout;
- Access Point `ESP32-FlexRadio-Setup`;
- vorläufiges Einrichtungskennwort `flexradio`;
- Portalbetrieb im reinen `WIFI_AP`-Modus ohne vorherigen STA-Disconnect;
- automatische Anlage des Preferences-Namensraums beim ersten Start;
- Captive DNS und Konfigurationsseite unter `http://192.168.4.1`;
- Neustart nach erfolgreichem Speichern;
- automatische Wiederverbindung beziehungsweise Rückkehr zum Portal;
- WLAN-Zustandsanzeige über GPIO48.

`SYN_5G` ist kein mögliches Ziel, da der ESP32-S3 nur 2,4-GHz-WLAN
unterstützt. Solange der lokale Passwortplatzhalter leer ist, wird die lokale
Quelle übersprungen. Nach 20 Sekunden ohne Verbindung bleibt das Portal der
Fallback.

### 6.2 Noch erforderliche WLAN-Erweiterungen

Diese Erweiterungen folgen erst nach dem ersten Hardwaretest:

1. definierter Weg zum erneuten Öffnen des Portals bei bereits gültigem WLAN;
2. Möglichkeit zum Löschen oder Ersetzen gespeicherter Zugangsdaten;
3. verständliche Anzeige von Verbindungs- und Eingabefehlern;
4. optional Auswahl eines gefundenen FLEX-Geräts über Seriennummer;
5. persistente Speicherung der ausgewählten Radioidentität;
6. spätere gemeinsame Konfigurationsseite für WLAN und Tastenbelegung.

WLAN-Zugangsdaten dürfen niemals über die normale serielle Diagnose ausgegeben
werden. Schreibende Funkgerätefunktionen dürfen nicht allein durch eine neu
hergestellte WLAN-Verbindung automatisch ausgelöst werden.

## 7. SmartSDR-LAN-Anbindung

### 7.1 Aufteilung der Netzwerkprotokolle

Die direkte SmartSDR-Anbindung besteht aus klar getrennten Datenwegen:

```text
UDP / VITA-49
    -> Radio-Discovery
    -> später optional Meter- und Streamingdaten

TCP
    -> Clientanmeldung und Keepalive
    -> Abonnements und Statusmeldungen
    -> Bedienkommandos wie RF Power
```

Für die erste Leistungssteuerung werden keine Panadapter-, Audio- oder
Meterstreams benötigt. Discovery läuft über UDP/VITA-49; der aktuelle
Transmitterstatus und die späteren RF-Power-Kommandos laufen über die
SmartSDR-TCP-Verbindung. UDP-Meterdaten werden erst ergänzt, wenn eine konkrete
Anzeige oder Schutzfunktion sie benötigt.

### 7.2 Verbindungsbasis und freigegebene Encoderabstimmung

- UDP-Discovery auf Port 4992;
- Auswertung von Modell, Name, IP-Adresse und API-Port;
- fünf Sekunden Discovery-Wartezeit;
- Fallback auf `192.168.178.70:4992`, wenn wegen der Subnetzgrenze
  kein Discovery-Broadcast eintrifft;
- Verbindung zum erkannten oder fest konfigurierten FLEX-Gerät;
- SmartSDR-TCP-Sitzung als eigener Programmclient;
- Abonnement des Transmitterstatus;
- Abonnement des Slice-Status einschließlich aktiver Slice und Frequenz;
- Keepalive per regelmäßigem `ping`;
- nicht blockierende Wiederverbindung;
- Einlesen vorhandener `rfpower`-Statuswerte;
- Abstimmung der eindeutigen aktiven Slice mit 1 Hz pro Encoder-Count über
  `slice t`; ohne eindeutiges Ziel wird kein Kommando gesendet;
- Statusfarben auf GPIO48 für Suche, Verbindung und Betriebsbereitschaft.

Die feste Fallbackadresse ist in `RadioConfig.h` zentral konfiguriert. Sie
verweist auf das Funkgerät. Das Frontpanel unter `192.168.178.80` ist kein
TCP-Ziel dieser Firmware.

Die aktuelle Stufe sendet keine Leistungswerte. Die ausdrücklich freigegebene
Ausnahme ist ausschließlich die Encoderabstimmung in 1-Hz-Schritten.

### 7.3 Abgrenzung zu FRStack

Die direkte SmartSDR-Implementierung soll nicht durch parallele REST-Aufrufe
an FRStack vermischt werden. Für einen kontrollierten Vergleich darf FRStack
vorübergehend denselben Wert lesen oder setzen, sofern eindeutig dokumentiert
ist, welcher Client die jeweilige Änderung ausgelöst hat.

Aus FRStack bekannte Funktionen können später als Anforderungen dienen, zum
Beispiel:

- RF Power;
- Audio Gain;
- Slice-Auswahl;
- TUNE Power;
- Mute und weitere Bedienfunktionen.

Neben der freigegebenen Encoderabstimmung bleibt der weitere Funktionsumfang
auf das Lesen und anschließend sichere Setzen von RF Power begrenzt. Weitere
Befehle benötigen jeweils eine eigene Freigabe und Abnahme.

### 7.4 Verbindliche Sicherheitsgrenze

Die in Watt gespeicherten Tastenwerte dürfen nicht direkt als rohe
SmartSDR-`rfpower`-Werte übertragen werden.

Vor dem ersten RF-Power-Schreibzugriff müssen geklärt und am AU-510M bestätigt
werden:

- Wertebereich und Einheit des SmartSDR-Felds;
- Zuordnung zwischen API-Wert und 0 bis 500 W Ausgangsleistung;
- Verhalten bei TUNE und normalem Sendebetrieb;
- erforderliche Bindung an den aktiven Maestro-/GUI-Client;
- Verhalten bei mehreren GUI-Clients beziehungsweise MultiFlex;
- Auswahl des richtigen Funkgeräts bei mehreren Discovery-Antworten;
- Rückmeldung des tatsächlich übernommenen Werts.

## 8. Entwicklungsphasen

### Phase 1 – Hardware-Inbetriebnahme und Encoder-Schreibtest

1. Diagnose-/LAN-Firmware auf den ESP32-S3 laden.
2. WLAN über das Einrichtungsportal konfigurieren.
3. Erkannten oder konfigurierten Modellnamen, Gerätenamen, IP-Adresse und Port
   kontrollieren.
4. Sicherstellen, dass das Ziel `192.168.178.70:4992` der gewünschte AU-510M
   und nicht das Frontpanel ist.
5. TCP-Verbindung mindestens 30 Minuten stabil beobachten.
6. Leistungsanzeige am Maestro verändern und den empfangenen rohen
   `rfpower`-Wert protokollieren.
7. Falls FRStack verfügbar ist, dessen angezeigten RF-Power-Wert nur als
   zusätzliche Referenz mit Maestro und direkter TCP-API vergleichen.
8. WLAN-Ausfall, Funkgerät-Neustart und Wiederverbindung testen.
9. Encoderabstimmung anschließend getrennt mit 1 Hz pro Count prüfen.

Abnahme:

- der richtige AU-510M wird reproduzierbar erkannt;
- die Verbindung bleibt stabil;
- gelesene Werte folgen eindeutig der Maestro-Einstellung;
- nach einem Verbindungsabbruch erfolgt keine Bedienaktion.

### Phase 2 – Radioauswahl, Clientkontext und Skalierung

1. Discovery um Seriennummer beziehungsweise eindeutige Radio-ID ergänzen.
2. Bei mehreren Geräten nicht automatisch das erste Gerät steuern.
3. aktiven GUI-/Maestro-Client ermitteln;
4. gegebenenfalls erforderliche Clientbindung implementieren;
5. rohe API-Werte ausdrücklich getrennt von Wattwerten speichern;
6. Skalierung beziehungsweise Kalibrierung für den AU-510M bestimmen;
7. Grenzwerte, Rundung und 5-W-Schritte zentral validieren;
8. gewünschte, gesendete und bestätigte Leistung als getrennte Zustände
   behandeln.

Abnahme:

- Radio und GUI-Kontext sind eindeutig;
- mehrere geprüfte API-Werte lassen sich reproduzierbar Wattwerten zuordnen;
- kein Wattwert kann ungeprüft in einen API-Befehl gelangen.

### Phase 3 – Abgesicherter SmartSDR-Schreibpfad

Eine zentrale Schnittstelle, beispielsweise `setRfPowerWatts()`, darf erst in
dieser Phase ergänzt werden.

Anforderungen:

- nur bei vollständig verbundener und bestätigter SmartSDR-Sitzung schreiben;
- nur für das ausgewählte Radio und den richtigen GUI-Kontext schreiben;
- Wertebereich und 5-W-Raster vor dem Senden prüfen;
- keine automatische Leistungsvorgabe beim Start oder Wiederverbinden;
- genau einen kontrollierten Befehl pro Benutzeraktion senden;
- bestätigenden Statuswert abwarten;
- Timeout und Fehlerzustand bereitstellen;
- keine unbegrenzte Wiederholung und keine Sendeschleife erzeugen;
- Ausfall oder unpassende Rückmeldung sichtbar melden.

Der erste Schreibtest erfolgt ohne HF-Aussendung und mit einer vorher
festgelegten kleinen Testvorgabe. Die reale Sender- und Verstärkerumgebung muss
dafür sicher vorbereitet sein.

### Phase 4 – Zwölf Tasten mit Leistungsprofilen verbinden

1. bestehende `RELEASED`-Events verwenden;
2. Tasten 1 bis 12 über eine zentrale Mapping-Tabelle anbinden;
3. nur im bestätigten `Ready`-Zustand eine Aktion auslösen;
4. bei fehlender Verbindung die Aktion ablehnen statt später nachzuholen;
5. ausgewählten, ausstehenden, bestätigten und fehlerhaften Zustand eindeutig
   anzeigen;
6. Tastenbeleuchtung und SmartSDR-Statusanzeige mit einem klaren
   Prioritätsmodell kombinieren;
7. alle zwölf Werte einzeln testen.

Abnahme:

- jede Taste setzt genau einen vorgesehenen Wert;
- keine Taste sendet doppelt;
- nach Verbindungsunterbrechung werden keine alten Tastenaktionen nachgeholt;
- der bestätigte Wert stimmt mit der Maestro-Anzeige überein.

### Phase 5 – Von außen konfigurierbare Zuordnung

Ein versioniertes Konfigurationsmodell wird ergänzt:

- zwölf Tasteneinträge;
- Aktionstyp, zunächst ausschließlich `RF_POWER_WATTS`;
- Leistungswert;
- optionale Bezeichnung;
- gültiger Bereich 0 bis 500 W;
- 5-W-Schritte;
- werkseitige Standardwerte aus `ButtonConfig.h`;
- persistente Speicherung mit Versionskennung;
- atomare Validierung vor dem Speichern;
- Wiederherstellung der Standardwerte.

Die Weboberfläche darf erst dann aktiviert werden, wenn ungültige Werte sicher
abgelehnt und bestehende gültige Konfigurationen bei einem Fehler erhalten
bleiben.

### Phase 6 – Robustheit und Regression

Verbindlich zu prüfen:

- alle zwölf Tasten einzeln;
- normale Mehrfachtasten und mögliches Matrix-Ghosting;
- schneller Encoderbetrieb in beiden Richtungen;
- gleichzeitige Tasten-, Encoder-, LED- und Netzwerkaktivität;
- WLAN-Abbruch und Wiederverbindung;
- DHCP-Adressänderung;
- Neustart des AU-510M;
- mehrere FLEX-Geräte im Netzwerk;
- mehrere GUI-Clients;
- Verlust und Wiederkehr des aktiven GUI-Clients;
- fehlerhafte, unvollständige und überlange SmartSDR-Zeilen;
- `millis()`- und `micros()`-Überlaufkonzept;
- Watchdog- und Speicherstabilität im Langzeittest;
- beide PlatformIO-Environments.

### Phase 7 – Dokumentation konsolidieren

Nach erfolgreichem Hardwaretest:

1. `README.md` an den tatsächlichen WLAN-/SmartSDR-Stand anpassen;
2. die beiden alten Codex-Pläne deutlich als historisch kennzeichnen;
3. WLAN-Einrichtung und Wiederherstellung dokumentieren;
4. Radioauswahl, Clientbindung und Leistungsskalierung dokumentieren;
5. direkte SmartSDR-API und optionale FRStack-Testreferenz klar unterscheiden;
6. LED-Farben und Fehlerzustände dokumentieren;
7. Build, Upload und Hardwaretests getrennt ausweisen;
8. `DEVELOPMENT_PROGRESS.md` nach jeder freigegebenen Stufe ergänzen.

## 9. GPIO48-Statusanzeige

GPIO48 wird als Statusanzeige verwendet:

| Zustand | Anzeige |
| --- | --- |
| WLAN-Verbindung läuft | blau blinkend |
| Einrichtungsportal aktiv | gelb/orange blinkend |
| Radiosuche | cyan blinkend |
| Radio gefunden/TCP-Verbindung | violett blinkend |
| TCP verbunden, Status noch offen | gelb |
| SmartSDR und `rfpower` bereit | grün |

Eine spätere Änderung dieser Zustände benötigt eine eigene Freigabe, weil
GPIO48 damit eine Bedien- und Diagnosefunktion besitzt.

## 10. Serielle Detaildiagnose

Die einzige Firmware gibt bei 115200 Baud ereignisbezogene Diagnose aus:

- `[BOOT]`: Buildzeit, Resetgrund, CPU, Flash, Heap und PSRAM;
- `[ENCODER]`: PCNT-Konfiguration, Position, Delta und Richtung;
- `[KEY]`: Matrixkonfiguration, Tastendruck, Loslassen und Leistungsprofil;
- `[LED]`: Status-LED und Start-Lauflicht;
- `[WIFI]`: gespeicherte SSID vorhanden, Verbindungsversuche, Zustände,
  Portal, IP-Konfiguration, RSSI und Fehler;
- `[DISCOVERY]`: UDP-/VITA-Port und gefundene Radioinformationen;
- `[SMARTSDR TX]`: gesendete API-Kommandos;
- `[SMARTSDR RX]`: vollständig empfangene API-Zeilen;
- `[SMARTSDR]`: Verbindungszustand, Timeouts und geparste Werte;
- `[FATAL]`: nicht behebbarer Initialisierungsfehler.

WLAN-Passwörter dürfen niemals ausgegeben werden. Dauerhafte Ausgaben ohne
Zustandsänderung sind zu vermeiden; die ausdrücklich gewünschte rohe
SmartSDR-RX-Ausgabe ist die Ausnahme für die Inbetriebnahme.

## 11. Allgemeine Qualitätsregeln

- Keine langen `delay()`-Aufrufe im normalen Betrieb.
- Keine Netzwerk- oder USB-Ausgabe aus ISR-Kontext.
- Keine dynamische Speicherverwaltung in zeitkritischen Encoder- oder
  Matrixpfaden.
- Zeitdifferenzen über vorzeichenlose Subtraktion berechnen.
- Keine Aufholschleifen mit vielen LED- oder Netzwerkübertragungen.
- NeoPixel nur bei sichtbaren Änderungen übertragen.
- Netzwerkparser müssen begrenzte Puffer besitzen und sich nach fehlerhaften
  Daten wieder synchronisieren.
- Verbindungsabbruch muss ausstehende Bedienaktionen verwerfen.
- Keine automatische Wiederholung eines Leistungsbefehls nach Reconnect.
- Keine Laufzeitabhängigkeit von FRStack, FlexLib oder Hamlib einführen.
- TCP-Status/Kommandos und optionale UDP-/VITA-Daten getrennt behandeln.
- Build und reale Hardwaretests immer getrennt dokumentieren.
- Vor jeder Quellcodeänderung Umfang und betroffene Dateien festlegen.
- Keine zusätzlichen Funktionen ohne vorherige Freigabe ergänzen.

## 12. Gesamtabnahme

Das Hauptziel ist erreicht, wenn:

- Encoder und alle zwölf Tasten zuverlässig funktionieren;
- die Tasten- und Statusbeleuchtung eindeutig bleibt;
- WLAN ohne PC eingerichtet und wiederhergestellt werden kann;
- der normale Betrieb ohne FRStack, FlexLib, Hamlib und PC möglich ist;
- ausschließlich der ausgewählte AU-510M gesteuert wird;
- der richtige Maestro-/GUI-Kontext verwendet wird;
- die Umrechnung von Watt zu SmartSDR-Wert bestätigt ist;
- jede Taste genau einen konfigurierten 5-W-Leistungswert setzt;
- der gesetzte Wert über SmartSDR zurückgelesen und bestätigt wird;
- kein Befehl beim Start oder nach einem Reconnect automatisch ausgelöst wird;
- Fehler und Verbindungsabbrüche keine veralteten Aktionen nachholen;
- das Environment `esp32-s3-n16r8` fehlerfrei baut;
- alle noch offenen Hardwaretests in `DEVELOPMENT_PROGRESS.md` dokumentiert
  sind.

## 13. Nächster Arbeitsschritt

Der nächste Schritt ist der kontrollierte Hardwaretest der Encoderabstimmung:

```text
Diagnose-/LAN-Firmware aufspielen
        ->
WLAN konfigurieren
        ->
AU-510M automatisch finden
        ->
SmartSDR verbinden und aktive Slice empfangen
        ->
Encoder langsam UP/DOWN drehen
        ->
1-Hz-Schritte und Rückmeldung am Maestro vergleichen
```

Bis dieser Hardwaretest ausgewertet ist, werden keine weiteren
SmartSDR-Schreibbefehle und keine Tastensteuerung des AU-510M ergänzt.
