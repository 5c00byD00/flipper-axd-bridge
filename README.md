# AxD Recon

Flipper Zero application that drives the **orange-port** ESP32-C5 on an AWOK Dual C5 or Dual C5 Mini over the Flipper GPIO UART. The chip runs the headless **AxD bridge** build from [dagnazty/awokxdag](https://github.com/dagnazty/awokxdag).

This app sends only passive commands: Wi-Fi and client discovery, wardrive logging, and fleet join/start. It does not expose any other bridge opcodes.

AxD is by [dagnazty](https://github.com/dagnazty/awokxdag). This companion is MIT-licensed; see `LICENSE`.

## Firmware

Flash the orange USB-C port with the **`dual-c5-bridge`** profile. That profile compiles `AWOKxDAG/AWOKxDAG.ino` with `AWOK_DUAL_C5_BRIDGE`: the full firmware, headless, with the console command parser in `handleSerial()`.

The older `AxDBridge/AxDBridge.ino` sketch is a BLE-to-ESP-NOW relay. It does not accept these UART commands. Do not flash the Touch or Mini screen image onto the orange port.

Commands and responses below were checked against awokxdag commit `fe02853` (AxD **1.7.8**).

```bash
git clone https://github.com/dagnazty/awokxdag.git
cd awokxdag
python3 scripts/flash_firmware.py dual-c5-bridge --port PORT
```

`flash_firmware.py` builds the image if it is missing. Plug the cable into the **orange** port. If the chip does not enter download mode, hold BOOT while applying power. The white port is the screen chip and is not the UART target of this app.

Build dependencies for the firmware itself are in the AxD README (Arduino-ESP32 3.3.x, the libraries that repo pins, and `scripts/build_firmware.py`).

## Pinout

The Flipper USART is GPIO header pin **13** (PB6, TX) and pin **14** (PB7, RX), 115200 8N1. That matches `furi_hal` on current official firmware: `FuriHalSerialIdUsart` is `gpio_usart_tx` / `gpio_usart_rx`, numbered 13 and 14. Share ground (header pins 8, 11, or 18). The board can be powered from its orange USB port, or from header pin 1 (5V) if you use the Flipper to power it.

| Flipper header | Direction | Orange-port console |
| --- | --- | --- |
| 13 TX (PB6) | Flipper → C5 | `Serial` RX |
| 14 RX (PB7) | C5 → Flipper | `Serial` TX |
| 8, 11, or 18 | ground | GND |

AxD opens that console with `Serial.begin(115200)` in `AWOKxDAG.ino`. The on-board GPS receiver is a **different** UART: ESP GPIO 14 RX / 13 TX at 115200 NMEA (`board_pins.h`). Those ESP pins are not the Flipper header pins.

The app takes the USART for itself (`expansion_disable` before `furi_hal_serial_control_acquire`). Leave the Flipper log on USB, not on USART pins 13/14.

## Install the app

A prebuilt package is `dist/axd_recon.fap` (official Flipper firmware **1.5.1**, API **88.2**). Copy it to `SD:/apps/GPIO/axd_recon.fap` and open **GPIO → AxD Recon**.

Rebuild with [ufbt](https://github.com/flipperdevices/flipperzero-ufbt):

```bash
pip install ufbt
ufbt update
ufbt
```

`ufbt` writes `dist/axd_recon.fap`. With the Flipper connected over USB, `ufbt flash` installs it. The host parser check is:

```bash
gcc -Wall -Wextra -Werror -o /tmp/test_protocol tests/test_protocol.c src/axd_protocol.c -Isrc -lm
/tmp/test_protocol
```

## UART commands

Every command the app sends is one of these. `handleSerial()` accepts `$CMD,<opcode>,<arg>` terminated by a newline and dispatches it through `linkDispatchCommand()`. Baud is 115200 8N1.

| App action | What is written | Opcode |
| --- | --- | --- |
| Refresh the AP list, and each pass of the AP log | `$CMD,255,0` then `$CMD,1,0` then `$CMD,255,0` then `$CMD,3,0` | 255, 1, 255, 3 |
| Stop / Home, stop clients | `$CMD,255,0` | 255 |
| Start client sniffer | `$CMD,6,0` | 6 |
| Start bridge wardrive | `$CMD,255,0` then `$CMD,7,0` | 255, 7 |
| Stop bridge wardrive | `$CMD,8,0` | 8 |
| Toggle raw GPS on the console | `r` | single-character shortcut |
| Start a fleet (coordinator) | `$CMD,255,0` then `$CMD,60,0` | 255, 60 |
| Join a fleet | `$CMD,255,0` then `$CMD,61,0` | 255, 61 |
| Leave a fleet | `$CMD,62,0` | 62 |

Opcode 1 (`kAxdCmdWifiScan`) starts a continuous scan and clears `wifiCount`, but it does not print each AP. Opcode 3 (`kAxdCmdChannelMap`) calls `scanWifi()` only when that list is empty, and `scanWifi()` prints one line per AP:

```text
[wifi]  -42 dBm ch6   AA:BB:CC:DD:EE:FF Cafe WiFi
```

The refresh burst uses opcode 1 so the list is empty, stops that scan before it can merge results, then uses opcode 3 so the blocking scan prints the lines. A scan that is already inside `scanWifi()` does not read the console until it returns, so Stop waits until that pass finishes.

Lines the app understands:

- `[wifi] … dBm ch… BSSID SSID` — RSSI, channel, BSSID, SSID. Encryption is not in this line.
- `[wifi] passive access-point scan started`, `continuous scan started/stopped`, `scan failed (…)`
- `[clients] starting probe-request sniffer` and `[clients] stopped; N client(s)`
- `[wardrive] started …` and `[wardrive] stopped; N Wi-Fi, M BLE`
- `$WDSTAT,1,…` — wardrive dashboard from `broadcastWardriveDashboard()` (counts, elapsed time, distance, fix percent, satellites, HDOP, speed, firmware CSV name). It does not include latitude or longitude.
- `[fleet] armed…`, `coordinator up, session … code …`, `roster N=…`, `member N joined`, `N=… agg wifi=… ble=…`, `left the session`, and the other `[fleet]` status lines
- `[link] split wardrive started/stopped…` — fleet wardrive uses this path
- `[remote] command op=… arg=…` — the bridge accepted a `$CMD`
- Raw NMEA `GGA` / `RMC` after `r` turns echo on (`gpsRawEcho` defaults off)
- A WiGLE body row (`MAC,SSID,AuthMode,…,WIFI`) if one appears on the console

## How to use it

**Recon.** Refresh scan fills the access-point list (SSID, BSSID, channel, RSSI). Encryption is shown only when a row actually carries it; the scan line above does not. Up/Down moves through the list, OK scans again, Left sends Stop / Home. Clients starts opcode 6. The bridge prints a start line and, on stop, a client count. It does not print each client address on UART (those stay on the headless display path), so the Clients screen shows that summary.

**Wardrive.** Two start actions, because they use the radio differently:

- **Start AP log** repeats the passive scan and appends each AP to a CSV on the Flipper SD card, with the latest GPS fix when NMEA echo is on.
- **Start bridge drive** sends opcode 7. The orange chip runs AxD wardrive and prints `$WDSTAT` about once a second. Individual WiGLE rows are not written to this UART in 1.7.8 (they go to BLE if a phone is connected, or to the chip’s own SD if it has one). The Flipper log still records the dashboard and any NMEA.

Toggle GPS NMEA **before** starting a tool. `r` is ignored while a tool blocks serial shortcuts, which includes wardrive and the client sniffer. Stop either mode from its menu entry. **View log** shows the tail of the CSV. **Export log** copies it to `SD:/apps_data/axd_recon/export/wardrive.csv`. Session files are `SD:/apps_data/axd_recon/wd_YYYYMMDD_HHMMSS.csv`.

**Fleet.** Start fleet sends opcode 60 (become coordinator and start the fleet wardrive). Join sends opcode 61 (listen for an invite). Leave sends opcode 62. Status shows the code, session, roster, role, and aggregate Wi-Fi/BLE counts from the lines above.

**Serial log** is the recent console text, with the firmware’s startup command banner omitted.

## Assumptions

- The orange-port console UART is the one bonded to Flipper pins 13 and 14. AxD names the baud and the parser, not the Flipper header. This is the same USART the orange “F0” chip uses on these boards.
- Opcode 3 is used because it is the passive command that prints per-AP lines, and only when `wifiCount` is 0. The burst above is how a later refresh clears that list.
- Opcode 62 is included so a fleet you started or joined can be ended.
- Latitude and longitude are taken from NMEA echo. `$WDSTAT` carries fix quality, not coordinates.
- The app was built with ufbt against firmware 1.5.1 (API 88.2). It was not run on a Flipper or an ESP32-C5 in this environment.
