# 3D Scanner – ESP32-C6 firmware

C++ (ESP-IDF) port of `Rpi-3d-scanner`. The ESP32-C6 drives the turntable (TMC2209)
and serves the same web UI / REST API. The phone (the camera) connects over
WebSocket, as it did with the Raspberry Pi.

## Feature map (Python → C++)

| Raspberry Pi (Python)                  | ESP32-C6 (C++)                                   |
|----------------------------------------|--------------------------------------------------|
| `config.py`                            | `idf.py menuconfig` → *3D Scanner configuration* (`main/Kconfig.projbuild`, `main/config.h`) |
| `controlers/motor.py` (gpiozero)       | `main/motor.cpp` – STEP pulses from a hardware timer interrupt |
| `controlers/camera.py`                 | `main/camera.cpp`                                |
| `controlers/scanner.py` (thread)       | `main/scanner.cpp` (FreeRTOS task)               |
| `app.py` Flask, port 5000              | `main/web_server.cpp` (esp_http_server)          |
| `app.py` websockets, port 8765         | `main/web_server.cpp` (second esp_http_server, WebSocket) |
| `templates/index.html`, `static/style.css` | `web/` – embedded in the firmware            |
| `Tests/motor_test.py`, `Tests/move_test.py` | menuconfig → *Firmware mode* → Motor test / Move test |
| Raspberry Pi network                   | `main/wifi.cpp` – Wi-Fi STA or own access point, mDNS `scanner.local` |

### HTTP API (port 5000, unchanged)

| Method | Path            | Response |
|--------|-----------------|----------|
| GET    | `/`             | Web UI |
| GET    | `/static/style.css` | stylesheet |
| POST   | `/scan/start`   | `{"status":"started"}` / `{"status":"already running"}` |
| POST   | `/scan/stop`    | `{"status":"stopping"}` |
| GET    | `/scan/status`  | `{"running":bool,"photo":n,"total":n,"error":null\|"..."}` |
| POST   | `/camera/photo` | `{"status":"photo_taken"}` / 400 `{"status":"error","message":"..."}` |
| POST   | `/motor/cw`     | `{"status":"ok"}` / 409 `{"status":"busy"}` |
| POST   | `/motor/ccw`    | `{"status":"ok"}` / 409 `{"status":"busy"}` |
| GET    | `/events`       | Server-Sent Events, scan status JSON on every change |

### Phone WebSocket (port 8765, unchanged)

`ws://<scanner-ip>:8765/` (any path is accepted).
Scanner → phone: `take_photo`. Phone → scanner: `photo_taken`.
The newest connection is the active camera.

## Wiring (ESP32-C6-DevKitC-1 defaults, change in menuconfig)

| TMC2209 | ESP32-C6 |
|---------|----------|
| STEP    | GPIO19   |
| DIR     | GPIO20   |
| EN      | GPIO21   |
| VIO     | 3V3      |
| GND     | GND      |

Motor power (VM) comes from the motor supply as before; tie its GND to the ESP GND.
Microstepping (MS1/MS2) stays set in hardware, as on the Pi (default 1/8 → 1600 microsteps/rev).

Recommended: a 10 kΩ pull-up from EN to 3V3, so the driver stays disabled while
the ESP32 is resetting/booting (GPIOs float during reset).

Avoid GPIO8, GPIO9, GPIO15 (strapping), GPIO12/13 (USB) and GPIO16/17 (UART console)
when choosing other pins.

## Build & flash

Requires **ESP-IDF v5.3 or newer** (the mDNS component is fetched automatically).

```bash
idf.py set-target esp32c6
```

```bash
idf.py menuconfig
```

Set at least *3D Scanner configuration → Wi-Fi* (SSID/password, or AP mode).

```bash
idf.py build flash monitor
```

The serial log shows the IP address. Open `http://<ip>:5000/` or
`http://scanner.local:5000/`, and point the phone app at `ws://<ip>:8765`.

## Defaults (same as config.py)

| Setting | Value |
|---------|-------|
| Jog | 10 microsteps |
| Steps per revolution | 200 |
| Microstepping | 8 |
| Step delay | 1000 µs (0.001 s) |
| Photos per revolution | 50 |
| Pre-capture delay | 3000 ms |
| Post-capture delay | 2000 ms |
| Phone send timeout / photo confirm timeout | 5 s / 10 s |

## Behaviour differences vs. the Python version

- No local photo directory: the Pi version only created an empty `photos/` folder;
  photos were always stored on the phone. Same here.
- Fixed: `camera.disconnect()` was called without its `websocket` argument in
  `app.py` (TypeError on phone disconnect). Here a disconnect is handled correctly
  and only clears the camera if it is the active connection.
- Fixed: `JOG_STEPS = config.MOTOR_JOG | 10` was a bitwise OR (only correct for 10);
  the jog distance is now exactly `MOTOR_JOG`.
- *Stop scan* interrupts the pre/post-capture waits immediately instead of
  waiting for them to run out.
- Jog and scan cannot drive the motor at the same time (motor lock), and only one
  photo request goes to the phone at a time.
- `/events` sends a `: ping` comment every 15 s to detect closed browser tabs;
  at most 4 SSE clients, the oldest is dropped when a 5th connects.
- Motor position is not stored across reboots (same as the Pi: it starts at 0).
