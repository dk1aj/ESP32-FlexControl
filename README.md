# ESP32-S3 Radio Handwheel Controller

This is firmware for an ESP32-S3 handwheel with a 3x4 Adafruit NeoKey keypad. It controls one Radio through the SmartSDR TCP/IP API over Wi-Fi.

## Hardware

- Board: ESP32-S3-WROOM-1-N16R8 DevKitC-1 with 16 MB QSPI flash and 8 MB OPI PSRAM. See the [parts list](DOKU/BOM_CNC_Rotary_Macropad.txt).
- Encoder A/B: GPIO4/GPIO5. NeoKey columns: GPIO2/GPIO42/GPIO41. NeoKey rows: GPIO40/GPIO39/GPIO38/GPIO47.
- NeoKey NeoPixel data: GPIO21. Connection-status RGB LED: GPIO48. Native USB D-/D+: GPIO19/GPIO20.
- Use 3.3 V logic and a common ground. GPIO35 to GPIO37 are reserved by PSRAM.

## Controls

- Turn the encoder to tune by the selected step. Key 1 cycles through 1, 5, 10, 20, 50, 100, 250, 500, and 1000 Hz. Double-click selects 50 Hz; a long press selects 100 Hz.
- Keys 2 and 3 have no action. Key 12 rounds the Slice frequency to the nearest full kHz; 500 Hz rounds up.
- Keys 4 to 11 request RF-power settings of 2, 4, 10, 20, 40, 60, 80, or 90 percent when pressed. With rapid presses, the latest valid selection wins.

## Network behavior

- Complete credentials in `include/WifiSecrets.local.h` are tried first. Otherwise, saved credentials are used. If none work within 20 seconds, the setup portal starts at `http://192.168.4.1` with SSID `ESP32-Radio-Setup`.
- The controller listens for Radio discovery on UDP port 4992. After 5 seconds without discovery, it uses `192.168.178.70:4992`.
- It connects directly with the SmartSDR TCP/IP API, enables keepalive, and reconnects after a lost connection. Normal operation does not require FRStack or a PC.
- A session becomes ready only after all five required setup commands are accepted and fresh `rfpower` status arrives.
- RF-power requests require exactly one active Slice and are blocked from 50 through 54 MHz. Confirmation uses the command response and matching `rfpower` status with a 3-second timeout. There is no automatic retry or startup/reconnect power command.

## Build and setup

Copy `include/WifiSecrets.example.h` to the ignored `include/WifiSecrets.local.h` and enter the local Wi-Fi settings.

```text
pio run -e esp32-s3-n16r8
pio run -e esp32-s3-n16r8 -t upload
pio device monitor -b 115200
```

Upload and monitoring use `COM11` as configured in `platformio.ini`.

## Current limitations

- Parser-to-radio-state integration tests and the full hardware regression test are still open.
