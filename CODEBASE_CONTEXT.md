# Backseat Minder — Codebase Context

## Purpose

This repository contains ESP-IDF firmware for an **ESP32-S3 camera-based back-seat occupancy reminder prototype**. On first boot it opens a Wi-Fi setup portal. Afterwards it scans for the driver's BLE beacon, and only when the driver is away does it power up the back-seat sensors (camera + face detection, thermal camera, mmWave radar, temperature/humidity). The indicator LED reflects the combined face, mmWave and thermal presence signals.

The project also provides an on-device captive-portal setup flow for collecting and persisting contact-related settings. A primary BOOT press/release can now request a test SMS through the secondary ESP32 in `cell/`. Real sending defaults off for bring-up. There are no automatic sensor-triggered SMS alerts, calls or emergency-services integrations.

## Platform and Build

- Target: `esp32s3`
- Framework: ESP-IDF `5.5.4`
- Project name: `backseat-minder`
- Build system: CMake / `idf.py`
- Main source directory: `main/`
- Application partition: 6 MB factory app (`partitions.csv`; the board has 8 MB flash).
  Bluetooth + Wi-Fi + camera + face models are about 1.9 MB, which nearly filled the old 2 MB partition.

ESP-WHO **release/v1.1.0** is a git submodule in `esp-who/` (the newer ESP-WHO master
branch is a rewrite without `who_camera.h`/the `modules` component and does not work here).
The root `CMakeLists.txt` adds `esp-who/components` to `EXTRA_COMPONENT_DIRS`, or the
checkout named by the `ESP_WHO_PATH` environment variable, and stops with a clear message
if it is missing. The IDF component manager downloads the rest (`dependencies.lock`).

`sdkconfig.defaults` holds every setting the project needs (target, flash/PSRAM, partition
table, camera pins, BLE 4.2 scanning), so a deleted `sdkconfig` is rebuilt correctly.
BLE 4.2 features must stay enabled: on the ESP32-S3 ESP-IDF defaults to BLE 5.0 only,
which removes the `esp_ble_gap_set_scan_params`/`esp_ble_gap_start_scanning` scan API.

Typical device workflow:

```text
git submodule update --init --recursive esp-who
idf.py build
idf.py -p COM4 flash monitor
```

If the flash still holds settings from an older build you want to discard, run
`idf.py -p COM4 erase-flash` once before flashing; the device then boots into setup.

`COM4` is an example local serial port, not a portable project setting.

## Source Map

| File | Role |
| --- | --- |
| `main/app_main.cpp` | Firmware entry point; setup-first boot, beacon gate that starts/stops the sensors, LED and telemetry loop. |
| `main/beacon_tracker.cpp` | BLE scan for the driver's beacon (fixed MAC); median RSSI with threshold + hysteresis and a 5 s lost-beacon timeout. |
| `main/presence_sensors.cpp` | Starts/stops all back-seat sensors together; owns the thermal-camera task. |
| `main/camera_sensor.cpp` | Camera init/deinit and one capture → face detection → USB preview task. |
| `main/tuned_face_detection.cpp` | Two-stage ESP-WHO face detector on one frame. |
| `main/usb_camera_stream.cpp` | Rate-limited JPEG preview frames over native USB. |
| `main/thermal_camera.cpp`, `main/mmwave_sensor.cpp`, `main/temp_humidity_sensor.cpp` | MLX90640, C4001 and AM2302/DHT22 drivers, each with init/start and deinit/stop. |
| `main/captive_dns.h`, `main/phone_number.h` | Host-tested helpers for the setup portal's DNS replies and phone validation. |
| `main/boot_button.cpp` | Debounced BOOT press/release event; no sleep or driver-presence toggle. |
| `main/cellular_link.cpp` | UART2 SMS worker, acknowledgement/result logging, busy guard and timeouts. |
| `main/cellular_diagnostics.cpp` | Thread-safe last-eight-event history, sent over native USB as BSMC JSON snapshots. |
| `shared/cellular_protocol.h` | Shared bounded framing and phone/message validation. |
| `cell/src/main.cpp` | Secondary UART command service and TEL0161 modem transactions. |
| `main/setup_mode.h` | Public interface for setup mode. |
| `main/setup_mode.cpp` | Captive portal, Wi-Fi access point, DNS redirection, form parsing, and NVS configuration storage. |
| `main/CMakeLists.txt` | Registers application source files and component dependencies. |
| `dependencies.lock` | Pinned ESP-IDF and Espressif component versions. |
| `partitions.csv` | Flash partition layout. |
| `sdkconfig` | Generated ESP-IDF configuration; treat as platform/build configuration rather than primary application logic. |

## Runtime Behavior

### Boot: setup first

1. `app_main()` calls `setup_mode_init()` (NVS + GPIO 38 button), holds GPIO 48 (onboard
   WS2812/NeoPixel) low and configures GPIO 2 as the indicator LED.
2. `setup_mode_requested_at_boot()` opens setup **before Bluetooth, the camera or any sensor
   is created** when any of these is true:
   - no usable name and primary phone number are saved (first boot, or after `erase-flash`);
   - the GPIO 38 setup button is held during boot;
   - setup was requested at runtime (see below).
   Setup saves the form and restarts, so normal mode always starts from a clean boot.

### Normal mode

1. BOOT button, cellular UART link and native USB telemetry are initialized.
2. Bluetooth starts a continuous passive BLE scan for the driver's beacon
   (`beacon_tracker.cpp`, MAC `78:9f:38:08:0b:e5`).
3. **All presence sensors stay off while the driver is near.** Nothing is initialized:
   no camera driver, no I2C bus for the thermal camera, no mmWave UART, no DHT22 RMT channel.
4. The driver counts as **away** when the beacon has not been seen for 5 s, or its median
   RSSI (last 6 readings) drops more than 5 dB below `RSSI_THRESHOLD` (currently -50 dBm for
   bench testing; -70 to -80 is a more realistic "left the car" radius). It counts as **near**
   again when the median is at or above the threshold. After boot the driver is assumed near
   for 5 s while the scanner looks for the beacon. The decision must then hold for 3 s
   before the main loop acts on it.
5. Driver away → `presence_sensors_start()`: camera + face detection, MLX90640 thermal task,
   C4001 mmWave (full configuration on the first start after boot, `sensorStart` afterwards)
   and the DHT22.
   Driver near again → `presence_sensors_stop()`: camera driver deinitialized, thermal I2C bus
   released, mmWave `sensorStop` + UART released, DHT22 RMT channel released.
6. The main loop runs every 20 ms:
   - BOOT press/release sends the test SMS to the saved primary number;
   - holding GPIO 38 restarts the device straight into setup (`setup_mode_restart_into_setup()`,
     using a flag kept in RTC memory across the restart);
   - while sensors are on, GPIO 2 is high when face, mmWave or thermal indicates presence;
     it is low otherwise and always low while the driver is near;
   - every three seconds it reads the DHT22 (only while sensors are on) and sends USB telemetry.
     Telemetry also includes `driver_near`, `sensors_active` and `beacon_rssi`.

These presence signals drive the LED and USB telemetry; they do not automatically trigger SMS.

The main task has an 8192-byte stack (`sdkconfig` and `sdkconfig.defaults`). Its
minimum free stack is logged every 60 seconds in normal mode. Telemetry formatting
buffers use static storage protected by the USB write mutex, avoiding 1408 bytes
of stack use on each sensor/diagnostic send. This addresses the main-task stack
overflow observed with the former 3584-byte allocation.

### Optional USB development telemetry

The device remains fully standalone. When its native **USB Serial/JTAG** USB-C port (left) is
connected to a host, `usb_telemetry.cpp` sends a telemetry packet every three seconds
after the temperature/humidity sample. Each packet includes uptime, face-detection
status, sensor validity, temperature in Fahrenheit, and relative humidity. Camera-frame
and telemetry packets are multiplexed over the same USB-OTG serial connection. No
external host is needed for normal operation, and a disconnected or slow host does not
block the main loop.

Use `tools/usb_telemetry_viewer.py` on a development laptop to display serial output.
It mirrors `idf.py monitor` output by default; use `--json-only` to display only the
formatted telemetry records. It requires `pyserial` (`python -m pip install pyserial`)
and a port name, for example:

```text
python tools/usb_telemetry_viewer.py --port COM7
```

On the Freenove ESP32-S3 WROOM board, use the connector labeled **USB-OTG**. It is
wired directly to the ESP32-S3's USB D- (GPIO 19) and D+ (GPIO 20) lines. Do not use
the **USB-UART** connector for this dedicated telemetry stream; that connector is
instead attached to UART0 through the board's USB-to-UART chip. Windows should expose
the USB-OTG connection as a COM port after the firmware is flashed (COM3).

For a development camera preview, install Pillow in addition to `pyserial`, then run:

```text
python -m pip install pillow
python tools/usb_telemetry_viewer.py --port COM5
```

The preview is JPEG-compressed and limited to one frame per second to avoid making USB
debugging alter normal face-detection behavior. It uses the detected-frame output, so
face boxes may be visible in the preview.

### Manual cellular SMS test

The firmware remains in normal active mode. BOOT (GPIO0) produces one event after
both press and release have been stable for 50 ms; a startup-held button is ignored
until released. Driver-presence toggling and deep sleep have been removed.

The primary reads and normalizes only `bsm_cfg/phone`, then queues
`SEND_SMS|number|message` through UART2 TX39/RX40 at 115200 8N1. A worker logs the
secondary acknowledgement and final result without blocking monitoring. Another
press while active logs `BUSY`. There are no automatic retries; after a 3-second
ACK timeout the link stays busy until a result or the 110-second overall deadline.

The secondary is an ESP32-WROOM-32 development board (`esp32dev` in PlatformIO).
Its UART2 RX26/TX27 connects to primary TX39/RX40 respectively. It owns modem UART1
RX18/TX19, validates commands, checks modem/SIM/network
readiness, and reports a bounded result. It sends the exact requested message and
stores no recipient. Real sending defaults off (`BSM_SEND_REAL_SMS=0`); a successful
dry-run check returns `RESULT|ERROR|DRY_RUN`. `RESULT|OK` means modem acceptance,
not proof of handset delivery.

See [cell/README.md](cell/README.md) for wiring, enablement, build commands, timeout
policy and bench acceptance checks. Keep the primary's microSD slot empty: GPIO39/40
share its bus. GPIO38 setup, UART1 mmWave, camera, other sensors and USB retain their
existing behavior. During the blocking setup session BOOT is not polled.

The Python USB viewer displays a Cellular SMS panel with the latest summary and
recent timestamped events. `usb_telemetry_send()` sends a `BSMC` JSON snapshot with
each sensor sample (about every three seconds), preserving short-lived ACK/results
without making the SMS worker wait on USB. No recipient or SMS content is included.
The panel marks telemetry stale after ten seconds without a valid cellular packet.

### Setup mode

Setup opens automatically on boot when nothing usable is saved, when GPIO 38 is held at
boot, or after GPIO 38 is held at runtime (the device restarts into setup). It always runs
with Bluetooth and every sensor off:

1. The device starts an **open** Wi-Fi access point:
   - SSID: `Backseat Minder`
   - Password: none
   - IP address/gateway: `192.168.4.1`
2. DHCP hands out `192.168.4.1` as the DNS server and as the captive-portal URI (DHCP option 114).
   A lightweight UDP DNS server answers every A query with `192.168.4.1`, creating
   captive-portal behavior. Replies drop any EDNS record from the query (the old in-place
   reply was malformed for EDNS queries, which phones send).
3. An HTTP server serves the setup page and redirects common Android, iOS/macOS, Windows, and Firefox connectivity-check URLs to the portal.
4. The portal accepts settings, validates them (required name; required mobile number and
   optional contacts must have 7-15 digits, optionally with a leading `+`; formatting is
   stripped before saving), saves them to NVS, displays a confirmation page, then ends setup.
5. The firmware tears down the Wi-Fi/AP resources and reboots.

While waiting for portal confirmation, GPIO 2 blinks every 200 ms.

## Persistent Configuration

Settings are stored in ESP32 nonvolatile storage (NVS):

- Namespace: `bsm_cfg`
- User name: `name` (portal limit 63 characters; storage reserves 252 UTF-8 bytes plus a terminator)
- Primary phone number: `phone`
- Emergency contact 1: `ec1`
- Emergency contact 2: `ec2`
- Emergency contact 3: `ec3`
- Emergency-alert toggle: `emerg_alerts`

Each phone/contact field is capped at 31 characters plus a null terminator in memory. The setup page reloads saved values when it is reopened.

The required Name field is saved and reloaded with the contact configuration. Setup
rejects blank names and names that cannot fit the secondary's ASCII SMS protocol.
Older devices without a saved `name` must complete setup before sending an alert.
Saved form values are HTML-escaped
when displayed, including names containing apostrophes, quotes or ampersands.

The manual BOOT test reads the saved name and sends:
`Backseat Minder test for {name}. This checks delivery of passenger alerts. No action is needed.`
Setup also checks that the name fits the future alert text
`ALERT: {name}'s Backseat Minder device has detected an unattended passenger in their vehicle.`
(`alert_message.h`), which is not sent automatically yet.

The primary `phone` field is read for each manual SMS request through `setup_mode_get_phone_number()`. The helper strips common phone formatting and rejects missing or malformed values. Emergency contacts and the emergency-alert toggle remain stored only.

## Hardware Assumptions

- Freenove ESP32-S3 WROOM with the custom camera pin map in `sdkconfig`.
- GPIO 0: active-low onboard BOOT button for a manual test SMS press/release.
- GPIO 39/40: cellular UART2 TX/RX; microSD slot must remain empty.
- GPIO 38: active-low external setup button, using the internal pull-up.
- GPIO 2: external/status LED output used for face-detection status and setup-mode blinking.
- GPIO 1: AM2302/DHT22 single-wire temperature and relative-humidity sensor data pin.
- GPIO 48: onboard WS2812/NeoPixel data pin, explicitly held low.

GPIO 4 cannot be used for the AM2302/DHT22 because the camera uses it as its SIOD control
line. The AM2302/DHT22 needs its data line pulled high (normally with an external 4.7–10 kOhm
resistor from data to the sensor supply). The firmware reads it every three seconds,
converts temperature to Fahrenheit, and logs successful readings. The values are not yet
used to change alert or face-detection behavior.

Pin assignments are hard-coded in the source. Confirm the board wiring before changing hardware or reusing the firmware on another ESP32-S3 board.

## Dependencies

Key dependencies locked in `dependencies.lock` include:

- `espressif/esp32-camera`
- `espressif/esp32_s3_eye_noglib`
- `espressif/mdns`
- ESP-IDF

The face-detection models (`HumanFaceDetectMSR01`/`HumanFaceDetectMNP01`), `draw_detection_result` and the `CAMERA_PIN_*` configuration come from ESP-WHO release/v1.1.0; the camera driver itself is `espressif/esp32-camera`. ESP-WHO's `register_camera()` is not used because its capture task cannot be stopped.

## Security and Product Notes

- Setup Wi-Fi is intentionally open. Anyone within range while setup mode is active can view and submit the configuration form.
- Setup logs no longer print the submitted form body or saved phone/contact values. Cellular logs redact the recipient.
- Saved name and phone/contact values are HTML-escaped before insertion into the portal form.
- The emergency-alert wording in the UI is aspirational. There is no emergency-services integration in the current code.
- Face detection alone is not a reliable determination of a child, passenger, vehicle state, or emergency. Any real safety product needs additional sensors, failure handling, user testing, privacy design, and appropriate regulatory/legal review.

## Current Gaps / Likely Next Work

- Define the actual alert policy (when to alert, how long to wait, how to cancel, and failure behavior).
- Complete cellular bench verification and define any future automatic alert policy.
- Define runtime handling for emergency contacts and alert settings; only the primary phone is currently used.
- Add secure provisioning/access control and avoid logging personal data.
- Set `RSSI_THRESHOLD` in `beacon_tracker.cpp` for the real car and beacon (currently -50 dBm for bench testing), and make the beacon MAC configurable in setup.
- Validate camera/face-detection accuracy and recovery behavior under real vehicle lighting, motion, heat, and network conditions.
- Extend the cellular host tests in `tests/` with hardware-in-the-loop verification.

## Guidance for Future LLM Work

- Keep setup ahead of Bluetooth/sensor start-up in `app_main()`; runtime setup requests restart instead of opening Wi-Fi alongside BLE and the camera.
- Prefer changing `main/app_main.cpp` for normal detection-loop behavior and `main/setup_mode.cpp` for provisioning/configuration behavior.
- Keep ESP-IDF lifecycle constraints in mind: the setup code intentionally keeps `esp_netif` and the default event loop initialized across setup sessions, while tearing down only the Wi-Fi driver and AP netif.
- `enter_setup_mode()` blocks until the form is submitted and then restarts the device. Treat it as a mode transition, not a non-blocking helper.
- Avoid assuming that stored settings result in notifications; verify runtime consumers before describing a feature as implemented.
- Avoid editing generated `build/` output and generally avoid manual edits to `sdkconfig` unless an ESP-IDF configuration change is intended.
