# ESP32-S3 Radio Handwheel Controller

This project is firmware for an ESP32-S3 handwheel controller with an
Adafruit NeoKey Snap 3x4 keypad. It connects directly to one Radio through the
SmartSDR TCP/IP API. It does not need FRStack or a PC during normal operation.

The current controls are implemented in the firmware. The verified hardware
state is listed at the end of this README.

## Main features

- Hardware quadrature encoder using the ESP32-S3 PCNT peripheral
- Direct tuning of the active Slice
- Nine selectable encoder step sizes
- Twelve debounced NeoKey buttons
- Eight RF power buttons for HF
- RF power command confirmation through the Radio API
- One-kHz frequency rounding with key 12
- Wi-Fi setup portal and saved Wi-Fi credentials
- UDP Radio discovery with a fixed-IP fallback
- TCP keepalive and automatic reconnection
- NeoPixel key lighting and a separate connection status LED
- Serial diagnostics at 115200 baud

There is one PlatformIO environment: `esp32-s3-n16r8`.

## Operating rules

The firmware is designed for this simple setup:

- Exactly one Radio is available on the network.
- Exactly one active Slice is used for control.
- RF power buttons are disabled on the 6-metre band.
- Failed or rejected actions are discarded. They are not repeated after a
  reconnect.
- No RF power value is sent automatically at startup or after a reconnect.

## Key layout

The NeoKey board is installed upside down. The physical key layout is:

```text
        COL1  COL2  COL3
ROW1     12    11    10
ROW2      9     8     7
ROW3      6     5     4
ROW4      3     2     1
```

## Controls

### Encoder

Turning the encoder changes the frequency of the active Slice. One mechanical
detent produces one tuning action. Clockwise movement tunes up and
counter-clockwise movement tunes down.

The selected step size is controlled by key 1.

### Key assignments

| Key | Action |
| ---: | --- |
| 1 | Select the encoder frequency step |
| 2 | No action |
| 3 | No action |
| 4 | Set RF power to 2% |
| 5 | Set RF power to 4% |
| 6 | Set RF power to 10% |
| 7 | Set RF power to 20% |
| 8 | Set RF power to 40% |
| 9 | Set RF power to 60% |
| 10 | Set RF power to 80% |
| 11 | Set RF power to 90% |
| 12 | Round the active Slice frequency to the nearest full kHz |

### Key 1 gestures

- Single click: cycle through 1, 5, 10, 20, 50, 100, 250, 500 and 1000 Hz
- Double click: select 50 Hz
- Long press: select 100 Hz

### Key 12 rounding

Key 12 acts when it is pressed.

- A remainder below 500 Hz is rounded down.
- A remainder of 500 Hz or more is rounded up.
- A frequency that is already on a full kHz does not change.

For example, 14,074,320 Hz becomes 14,074,000 Hz, while 14,074,500 Hz becomes
14,075,000 Hz.

### RF power buttons

Keys 4 through 11 start their visual indication when pressed. The RF power
command is sent once when the key is released.

The firmware sends:

```text
transmit set rfpower=<percent>
```

An RF power command is accepted only when:

- the Radio connection is ready;
- exactly one active Slice with a known frequency exists;
- the active Slice is outside the 6-metre band; and
- no other RF power request is waiting for confirmation.

The Radio command response and the matching asynchronous `rfpower` status must
both confirm the requested value. The confirmation timeout is three seconds.
There is no automatic retry. A rejected command does not change the previously
confirmed Radio value.

The Radio display and Stream Deck use the same percentage values shown in the
key table.

## Key lighting

Normal key lighting uses the lowest stable LED level:

- Key 1 has a dim blue background.
- Keys 2 through 12 have a dim white background.
- A pressed key flashes green.
- A held key flashes red after the hold threshold.
- A released key fades back to its normal state.

When a power key is pressed:

1. The selected key blinks pure blue for five seconds.
2. The blinking blue level is 15%.
3. After five seconds, the key stays pure blue at 10%.
4. Pressing another power key moves the indication and restarts the five-second
   sequence.

The power-key indication is cleared when the Radio is not ready, when there is
no unambiguous Slice frequency, or when the active Slice is on 6 metres.

After every successful Radio TCP connection, a short green animation runs once
down the physical left column:

```text
12 -> 9 -> 6 -> 3
```

Each animation step lasts 100 ms. No startup animation is shown before the
Radio connects. NeoPixel positions 12 through 14 are always off.

## Connection status LED

The RGB LED on GPIO48 shows the connection state:

| Colour | State |
| --- | --- |
| Off | Idle |
| Blinking blue | Connecting to Wi-Fi |
| Blinking orange | Wi-Fi setup portal |
| Blinking cyan | Searching for the Radio |
| Blinking violet | Connecting to the Radio API |
| Yellow | Radio TCP connection is ready |
| Green | RF power status has been received |

## Wiring

| Function | Pin |
| --- | --- |
| Encoder A | GPIO4 |
| Encoder B | GPIO5 |
| NeoKey COL1 | GPIO2 |
| NeoKey COL2 | GPIO42 |
| NeoKey COL3 | GPIO41 |
| NeoKey ROW1 | GPIO40 |
| NeoKey ROW2 | GPIO39 |
| NeoKey ROW3 | GPIO38 |
| NeoKey ROW4 | GPIO47 |
| NeoPixel data input | GPIO21 |
| RGB status LED | GPIO48 |
| USB D- | GPIO19 |
| USB D+ | GPIO20 |

GPIO35 through GPIO37 are used by the octal PSRAM on the N16R8 module. Do not
use them for this project.

The ESP32-S3 is not 5-V tolerant. All signals must be safe for 3.3 V, and all
parts must share a common ground.

## Wi-Fi setup

Copy the example credentials file:

```text
include/WifiSecrets.example.h
```

Create this local file:

```text
include/WifiSecrets.local.h
```

Enter the local 2.4-GHz Wi-Fi settings and a password for the setup access
point:

```cpp
constexpr char SSID[] = "YOUR_2_4_GHZ_WIFI";
constexpr char PASSWORD[] = "YOUR_WIFI_PASSWORD";
constexpr char SETUP_AP_PASSWORD[] = "CHANGE_ME";
```

`WifiSecrets.local.h` is ignored by Git.

At present, complete credentials in `WifiSecrets.local.h` have priority over
credentials saved through the portal. If the local credentials are incomplete,
the firmware uses saved credentials. If no credentials are available, or if a
connection attempt fails for 20 seconds, the setup portal starts.

```text
Wi-Fi name: ESP32-Radio-Setup
Address:    http://192.168.4.1
```

After new credentials are saved, the controller restarts.

## Radio connection

The controller listens for Radio discovery packets on UDP port 4992. If no
discovery packet is received within five seconds, it uses the address in
`include/RadioConfig.h`.

The default fallback is:

```text
192.168.178.70:4992
```

The controller then opens a SmartSDR TCP connection, identifies itself,
requests Radio information, subscribes to transmitter and Slice status, and
enables keepalive messages.

## Build, upload and monitor

Run these commands from the project directory:

```text
pio run -e esp32-s3-n16r8
pio run -e esp32-s3-n16r8 -t upload
pio device monitor -b 115200
```

The current PlatformIO configuration uses COM11 for upload and monitoring.
Change `platformio.ini` if the controller appears on another port.

## Current verified state

The current firmware builds and uploads successfully on the ESP32-S3 N16R8
hardware. The following functions have been tested on the real controller and
Radio:

- Wi-Fi connection and Radio TCP connection
- Radio information and status reception
- Encoder tuning in both directions
- Key 1 frequency-step selection
- RF power command, response and status readback
- Blue power-key indication
- Key 12 rounding to a full kHz

Detailed development notes are kept in `DEVELOPMENT_PROGRESS.md`. Planned work
and remaining tests are kept in `PLAN.md`.
