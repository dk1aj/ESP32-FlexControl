# ESP32-S3 Radio Handwheel Controller

This firmware turns a Seeed Studio XIAO ESP32S3, a CNC encoder, and a 3x4 Adafruit NeoKey keypad into a direct Wi-Fi controller for one FlexRadio. The final hardware and Radio regression tests have passed.

## Hardware

- Board: Seeed Studio XIAO ESP32S3. Use 3.3 V logic and common ground. See the [parts list](DOKU/BOM_CNC_Rotary_Macropad.txt).
- Fixed wiring: encoder D0/D1 (GPIO1/2); NeoKey columns D2/D3/D4 (GPIO3/4/5); rows D5/D6/D7/D8 (GPIO6/43/44/7); NeoKey NeoPixel data D9 (GPIO8). D10/GPIO9 remains free.
- Keys 1 to 12 run row-wise from top-left to bottom-right. Their LEDs follow the physical zigzag chain; pixels 12 to 14 stay off.
- GPIO3 is deliberately used for NeoKey COL1 despite being an ESP32-S3 strapping pin. The onboard active-low LED on GPIO21 shows connection progress.

## Controls

- Turn the encoder to tune the one active Slice by the selected step. Key 1 cycles through 1, 5, 10, 20, 50, 100, 250, 500, and 1000 Hz. Double-click selects 50 Hz; a long press selects 100 Hz.
- Key 2 toggles RIT, key 3 toggles Slice audio mute, and key 12 rounds the Slice frequency to the nearest full kHz; 500 Hz rounds up.
- Keys 4 to 11 request RF-power settings of 2, 4, 10, 20, 40, 60, 80, or 90 percent when pressed. With rapid presses, the latest valid selection wins.
- Every Radio action requires exactly one active Slice with a known frequency. Rejected actions are never queued or replayed after reconnect.

## Status behavior

- Keys have dim background lighting, flash green when pressed, blink red while held, and fade after release. Enabled RIT and mute indicators glow dim red.
- An RF-power key blinks blue while pending and stays blue after Radio confirmation. Power keys are blocked from 50 through 54 MHz, and no power command is sent at startup or reconnect.
- A green animation runs down the left keypad column after Radio connection. The GPIO21 LED blinks during Wi-Fi, portal, discovery, and Radio connection work, then stays on for a connected or ready Radio session.

## Network behavior

- Valid stored credentials are tried before the compiled fallback in `include/WifiSecrets.local.h`. If neither connects within 20 seconds, the English setup portal starts as `ESP32-Radio-Setup` at `http://192.168.4.1`.
- Hold keys 1 and 12 together for two seconds to reopen the portal. It can safely replace or erase the stored credential record and then restarts the controller.
- The controller listens for Radio discovery on UDP port 4992. After 5 seconds without discovery, it uses `192.168.178.70:4992`.
- It connects directly through the SmartSDR TCP/IP API, subscribes to status, uses keepalive, and reconnects after a loss. SmartSDR or AetherSDR may be used separately; no FRStack service or PC-side runtime is required.

## Build and setup

Copy `include/WifiSecrets.example.h` to the ignored `include/WifiSecrets.local.h` and enter the local Wi-Fi settings.
```text
pio run -e xiao-esp32-s3
pio run -e xiao-esp32-s3 -t upload
pio device monitor -p COM12 -b 115200
```
XIAO upload and monitoring are fixed to `COM12` in `platformio.ini`.
