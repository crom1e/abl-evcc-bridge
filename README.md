# Salto Bridge

Local Home Assistant control of an **ABL Sursum EVCC** wallbox (tested with charger firmware V5.0) through a small ESP8266 RS485 bridge.

The project has two parts:

| Part | Folder | What it does |
|---|---|---|
| **Firmware** | `salto_bridge/` | Wemos D1 mini (ESP8266) + TTL-to-RS485 module. Talks to the charger, exposes a small REST API, a read-only web page, telnet logging and OTA. |
| **HA integration** | `custom_components/salto_bridge/` | Home Assistant custom integration (installable via HACS). Adds a *Salto* device with a charging-current slider, status sensors and binary sensors. |

**The charger does not speak Modbus.** This EVCC firmware only answers ABL's ASCII command protocol at **38400 baud, 8N1**: request `!0 02` + CRLF, reply `>0 02 0005`. See [Protocol](#protocol).

## Design principles

- **The bridge makes no control decisions.** All logic, including load balancing against the grid meter, lives in Home Assistant. The bridge polls the charger every 5 s and sets the charging current when asked.
- **One safeguard:** the charging current must be an integer between 6 A and `MAX_A` (config, default 16 A). Everything else is rejected.
- **One write command only:** `!0 12` (present current limit). There is deliberately no way to send arbitrary commands to the charger.
- **No watchdog.** The last setpoint stays in effect until HA sends a new one. At startup the bridge only reads and never writes.

---

## Hardware and wiring

| TTL485 module (MAX3485) | Wemos D1 mini | Note |
|---|---|---|
| TXD | D7 (GPIO13) | ESP RX (SoftwareSerial) |
| RXD | D8 (GPIO15) | ESP TX (SoftwareSerial) |
| VCC | 3V3 | MAX3485 is a 3.3 V part |
| GND | G | |
| A / B | A / B on the EVCC's `RS485 M B A` terminals | A → A, B → B |

```
  Wemos D1 mini               TTL485 module                ABL EVCC
 ┌───────────────┐           ┌───────────────┐            ┌──────────────────┐
 │           3V3 ├───────────┤ VCC           │            │  RS485           │
 │             G ├─────┬─────┤ GND        A  ├────────────┤ A                │
 │  D7 (GPIO13)  ├─────│─────┤ TXD        B  ├────────────┤ B                │
 │  D8 (GPIO15)  ├─────│─────┤ RXD           │            │                  │
 │               │     │     └───────────────┘            │                  │
 │      USB      │     └────────────────────────────────────┤ M (ground)       │
 └───────────────┘                                        └──────────────────┘

  ESP RX = D7  ←  module TXD   (data from the charger)
  ESP TX = D8  →  module RXD   (data to the charger)
```

- **EVCC terminals:** along the bottom edge are four groups, `EV FE CP CS`, `LED X+ X1- X2-`, `CTL M E1 E2` and `RS485 M B A`. RS485 is the **rightmost** group. Do not confuse it with `CTL`, which also has an `M` terminal.
- The module switches direction automatically (no DE/RE). The lines are crossed, so module TXD goes to ESP RX.
- **D8/GPIO15 is a boot strap pin and must be low at power-up.** Never connect module TXD to D8, or the ESP will not boot. If the D1 mini does not boot with the module attached, the module's RXD probably has a pull-up. Add 4.7 kΩ from D8 to GND.
- With a 5 V transceiver (MAX485, SN75176, ST485), power the module from 5 V. Divide TXD down to ~3.3 V before D7, e.g. 1 kΩ in series and 2 kΩ to GND.

**Stop/start:** the charger cannot be paused over RS485 (see [Protocol](#protocol)). Use the EVCC's enable input instead, for example with a relay such as a Shelly. Pulling it low makes the charger report status `0064` (disabled). The bridge stays online and keeps the current limit.

---

## Firmware

### One-time setup

```bash
winget install ArduinoSA.CLI            # or see arduino.github.io/arduino-cli
arduino-cli config init
arduino-cli config add board_manager.additional_urls https://arduino.esp8266.com/stable/package_esp8266com_index.json
arduino-cli core update-index
arduino-cli core install esp8266:esp8266
arduino-cli lib install ArduinoJson
cp salto_bridge/config.h.example salto_bridge/config.h   # fill in WiFi, token and passwords
```

`config.h` is listed in `.gitignore` and must never be committed.

### Build

```bash
arduino-cli compile --fqbn esp8266:esp8266:d1_mini --output-dir build \
  --build-property "compiler.cpp.extra_flags=-DFW_VERSION_RAW=$(git rev-parse --short HEAD 2>/dev/null || date +%Y%m%d-%H%M)" \
  salto_bridge
# -> build/salto_bridge.ino.bin
```
The version is stringified in the code, so no quotes are needed. Without `FW_VERSION_RAW` the version is `dev`. Shortcut: `tools/salto.sh build`.

### First flash over USB

```bash
arduino-cli board list                                    # find the COM port, e.g. COM8
arduino-cli upload -p COM8 --fqbn esp8266:esp8266:d1_mini --input-dir build salto_bridge
arduino-cli monitor -p COM8 -c baudrate=115200            # watch the boot log and IP address
```

### OTA updates

**HTTP OTA (`/update`), recommended:**
```bash
curl -u "$SALTO_UPDATE_USER:$SALTO_UPDATE_PASS" -F "firmware=@build/salto_bridge.ino.bin" http://<ip>/update
```
Shortcut: `tools/salto.sh ota-http`, which builds, uploads and resolves the IP. `/update` also works in a browser.

**ArduinoOTA / espota:** arduino-cli needs the **IP address** (from `arduino-cli board list`), not `salto.local`:
```bash
arduino-cli upload -p 192.168.1.50 -l network -F password="$SALTO_OTA_PASS" \
  --fqbn esp8266:esp8266:d1_mini --input-dir build salto_bridge
```
> **Windows:** with espota the ESP connects *back* to the PC. If you see "Authenticating...OK" followed by "No response from device", the firewall is blocking that connection. Use HTTP OTA, or allow inbound traffic for the `python3.exe` that ships with the esp8266 core.

During OTA, RS485 polling stops and the charger is not touched. After the reboot, `/api/status` reports the new `fw_version` and `build_time`. Check with `tools/salto.sh verify`.

> curl and Python on Windows cannot resolve `salto.local`, but `ping` can. `tools/salto.sh` resolves the IP via ping. Override with `SALTO_HOST=<ip>`.

### Telnet

```bash
telnet <ip> 23        # first line: the API token
```
Commands: `help`, `status`, `log`, `anomalies`, `level debug|info|warn`, `reboot`, `quit`.
Telnet shows the same log lines as USB serial (`[millis] LEVEL message`). With `level debug` every TX/RX frame is shown. One client at a time. A wrong token disconnects.

Non-interactive: `tools/salto.sh telnet status log` or `tools/salto.sh tail 60`.

### REST API

All POST requests and `/api/auth` require `Authorization: Bearer <API_TOKEN>`. A wrong token returns 401 after a short delay. With `READONLY_STATUS_OPEN=1`, `GET /api/status`, `GET /api/log` and the web page are open. With `0` they need the token too, and the web page reads it from the URL: `http://<ip>/#<token>`.

```bash
H="Authorization: Bearer $SALTO_TOKEN"; J="Content-Type: application/json"

curl http://<ip>/api/status
curl http://<ip>/api/log
curl -H "$H" http://<ip>/api/auth
curl -H "$H" http://<ip>/api/anomalies
curl -X POST -H "$H" -H "$J" -d '{"amps": 10}'      http://<ip>/api/current        # 6..MAX_A
curl -X POST -H "$H" -H "$J" -d '{"level": "debug"}' http://<ip>/api/debug/loglevel
curl -X POST -H "$H" http://<ip>/api/debug/reboot
```

| Endpoint | Response |
|---|---|
| `GET /api/status` | see the fields below |
| `GET /api/log` | `commands[]`: last 20 (`ms, time, amps, raw, source, ok, error`). `frames[]`: last 50 RS485 frames (`ms, time, dir tx/rx, ascii, hex`). Both newest first. `time` is local wall-clock time from NTP (frames with milliseconds), `null` until the clock is set. `now_ms` = current millis. |
| `GET /api/auth` | `{"ok": true}` if the token is valid. Never touches the charger. |
| `GET /api/anomalies` | Persistent log (token required): the last 100 rejected or odd frames, newest first, kept across reboots. Each entry: `time` (local, `null` before NTP sync), `uptime_s`, `boot`, `kind` (`invalid`, `out_of_range`, `unconfirmed`, `stray`, `suppressed`), `cmd`, `held` (value the bridge held), `ascii`, `hex`. See [reading integrity](#reading-integrity). |
| `POST /api/anomalies/clear` | clears the anomaly log (token required) |
| `POST /api/current {"amps": N}` | Sends `!0 12 <N/0.06>` twice and reads back with `!0 11`. Returns the status plus `command_ok` and `command_raw`. **400** if N is not an integer 6..MAX_A, or is 0 (pause is not supported). **502** if the charger did not acknowledge or replied `ERR`. |
| `POST /api/resume` | **501**. The charger has no known pause/resume command. |
| `POST /api/debug/loglevel` | `{"level": "debug\|info\|warn\|err"}` |
| `POST /api/debug/reboot` | reboots the ESP |

**Fields in `/api/status`:**

| Field | Meaning |
|---|---|
| `online` | valid reply from the charger within the last 15 s |
| `status`, `status_code` | from `!0 02`, e.g. `"charging"` / `"0005"` (see [status codes](#status-codes)) |
| `applied_a`, `applied_raw` | **present** current limit from the charger (`!0 11`), in A and raw |
| `default_a`, `default_raw` | default current / ceiling (`!0 26`). Never changed by the bridge. |
| `charger_fw` | charger firmware (`!0 01`). Only read while no car is connected, otherwise `null`. |
| `setpoint_a` | last setpoint from HA (`null` before the first command) |
| `min_a`, `max_a` | allowed range for `/api/current` |
| `seconds_since_command`, `last_command_source` | when and from where the last command came |
| `counters` | `sent`, `recv`, `invalid` (malformed replies), `timeouts`, `rejected` (well-formed values discarded: out of range or not confirmed), `stray` (bytes that belonged to no request), `anomalies` (anomaly log entries since boot) |
| `anomaly_log_entries`, `boot_no`, `fs_ok` | entries in the persistent anomaly log, boot counter, filesystem OK |
| `time` | local time from NTP (`null` until synced) |
| `mac` | WiFi MAC address (stable device ID for HA) |
| `uptime_s`, `rssi`, `heap_free`, `reset_reason`, `fw_version`, `build_time`, `log_level`, `ota_active` | system |

---

## Home Assistant integration

### Installation via HACS

1. HACS → ⋮ → **Custom repositories** → add `https://github.com/crom1e/abl-evcc-bridge`, category **Integration**.
2. Install **Salto Bridge (ABL EVCC)** and restart Home Assistant.
3. **Settings → Devices & services → Add integration → Salto Bridge.** Enter the bridge's IP address and the `API_TOKEN`. The bridge is also discovered automatically via mDNS (`_http._tcp`, name `salto`).

Give the bridge a fixed DHCP reservation.

### Entities

| Entity | Type | Notes |
|---|---|---|
| Charging current limit | `number` (slider, A) | 6..MAX_A. Calls `/api/current` and reads the value back from the charger. |
| Status | `sensor` (enum) | no car, connected/waiting, charging, disabled, transition, unknown |
| Set charging current | `sensor` (A) | present current limit reported by the charger |
| Set charging power | `sensor` (W) | current limit × 230 V (single-phase). **Not** measured power. |
| Default charging current | `sensor` (A, diagnostic) | the charger's ceiling (`!0 26`) |
| Charger firmware | `sensor` (diagnostic) | kept while charging (the charger only reports it when idle) |
| Status code / WiFi signal | `sensor` (diagnostic, disabled by default) | raw `!0 02` code, RSSI |
| Rejected readings / Anomaly log entries | `sensor` (diagnostic, disabled by default) | `counters.rejected`, `anomaly_log_entries` |
| Charging | `binary_sensor` (battery_charging) | status `0005` |
| Car connected | `binary_sensor` (plug) | status `0004`/`0005`/`0064`. Turns off only after 30 s of `0000`, which briefly appears during transitions. |
| Charger link | `binary_sensor` (connectivity, diagnostic) | RS485 link between bridge and charger. If the bridge itself is unreachable, all entities become unavailable. |

The integration never exposes a current limit outside 0 / min..max: such a value becomes *unknown* and a warning is logged.

The charger does not report measured current or power. Use the vehicle's integration or your grid meter for actual consumption.

### Using it in automations

```yaml
action: number.set_value
target:
  entity_id: number.salto_charging_current_limit
data:
  value: 10
```

### Load balancing against a grid meter (e.g. AMS/HAN)

The bridge does not regulate on its own. Build this in HA:
- **Minimum is 6 A.** Below that, stop charging with the enable input relay, and start again once there is headroom.
- Single-phase 230 V: 1 A ≈ 230 W. Measured: 6 A ≈ 1.3 kW, 10 A ≈ 2.2 kW, 16 A ≈ 3.7 kW.
- **Good practice:**
  - adjust at most every ~30 s
  - **decrease fast** (5–10 s) when the limit is exceeded, **increase slowly** (only after it has been stable for 60 s or more)
  - only send changes of at least 1 A
  - the car needs 2–10 s to follow a new limit
- The bridge has no watchdog. Send a sensible initial value when HA starts and a car is charging.
- **Base decisions on your own last setpoint** (e.g. an `input_number`) and the grid meter, not on the read-back of the number entity. Do not clamp-and-write the read-back value: a single bad reading should never trigger a write.

---

## Protocol

ABL ASCII command protocol, **38400 8N1**, address 0. Request `!0 <cmd> [value]` + CRLF, reply `>0 <cmd> [value]` + CRLF within ~5 ms. A rejected command returns `>0 <cmd> ERR`.

| Command | Reply (example) | Meaning | Used by the bridge |
|---|---|---|---|
| `!0 01` | `>0 01 V5.0` | charger firmware (returns `ERR` while charging) | every minute, only while idle |
| `!0 02` | `>0 02 0005` | status | every 5 s |
| `!0 11` | `>0 11 0267` | present current limit | every 5 s and after each write |
| `!0 26` | `>0 26 0267` | default current / ceiling | every minute |
| `!0 12 XXXX` | `>0 12` | **set present current limit** | `/api/current` |

- **Values** are the PWM duty cycle × 10 (IEC 61851): **A = value × 0.06**, value = A / 0.06. For example 16 A = `0267`, 10 A = `0167`, 6 A = `0100`.
- `!0 12` is limited internally by the charger to the default current (`!0 26`). Tested while charging: 6, 10 and 16 A work. `!0 12 1000` (0 A) returns `ERR`.
- **Never send `!0 15`** (changes the default/ceiling permanently, reportedly up to 32 A) **or `!0 00 1111`** (reset). The bridge deliberately has no path for such commands.
- The charger did not respond to Modbus ASCII (`:010300040001F7`) at any baud rate or parity. Newer EVCC2/3 firmware supports Modbus, this one does not.

### Reading integrity

The protocol has **no checksum**, and SoftwareSerial occasionally flips a bit at 38400 baud (roughly 0.3 % of replies are visibly corrupted). A flip can also turn one digit into another and still look valid: `>0 11 0183` (11 A) became `>0 11 0383` (23 A) once, and an automation acted on it. The bridge therefore:

- accepts only replies of exactly the form `>0 XX NNNN` for the command that was sent, and flushes the receive buffer before every request (leftover bytes are counted as `stray`);
- **never publishes a current limit outside 0 / 6..MAX_A** (`0000` and `1000` mean no current);
- **accepts a changed value only when a second read returns the same value** (up to 3 reads per poll), or when it is the value just written. Otherwise the previous value is kept;
- counts discarded values in `counters.rejected` and writes them, with raw hex, to the persistent anomaly log (`GET /api/anomalies`, telnet `anomalies`). The log lives in LittleFS, is written at most every 10 s and at most 30 entries per hour to spare the flash, and survives reboots and OTA updates.

### Status codes

Mapped by observation, toggling the enable input while a car was charging:

| Code | Meaning |
|---|---|
| `0000` | no car. Also appears briefly during transitions, so require it to persist (e.g. `for: 30 s`) before acting on it. |
| `0004` | car connected, waiting |
| `0005` | charging |
| `0064` | disabled (enable input pulled low) |
| `0012` | brief transition while disabling |

Sequence when toggling the enable input off and on: `0005` → `0064` → (`0000` → `0012`) → `0064` … on → `0004` → `0005` after ~7 s. The current limit (`!0 11`) is retained.

### Open questions

1. Status codes for fault states (F states), and the meaning of `0012`.
2. Pause/stop over RS485 does not appear to exist: `!0 12 1000` and `!0 03` return `ERR`, and `!0 25` only caused a ~4 s stop. Use the enable input.
3. Whether `!0 12` survives a charger power cycle or falls back to the default current.
4. Only single-phase operation has been tested. Power values assume 230 V single-phase.

---

## Repository layout

```
salto_bridge/salto_bridge.ino         firmware sketch
salto_bridge/types.h                  types (separate header because of Arduino's auto-generated prototypes)
salto_bridge/config.h.example         configuration template - copy to config.h
custom_components/salto_bridge/       Home Assistant integration
hacs.json                             HACS metadata
tools/salto.sh                        build / OTA / API / telnet helper (Git Bash, Linux, macOS)
tools/telnet.py                       non-interactive telnet client (used by salto.sh)
```
