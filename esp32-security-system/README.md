# ESP32 Home Security System

A complete, non-blocking ESP32 security alarm system with PIR motion detection,
door/window reed switch, 4×4 matrix keypad, siren, status LEDs, I2C LCD, and a
WiFi web interface for remote arm/disarm.

## Features

| Feature | Details |
|---|---|
| **PIR motion sensor** | Detects movement while armed (GPIO 32) |
| **Reed switch** | Door/window open detection (GPIO 33) |
| **4×4 keypad** | PIN arm/disarm: `A`=Arm, `D`=Disarm, `#`=Enter, `*`=Clear |
| **Siren / buzzer** | Beeps during delays, full siren during alarm (GPIO 14) |
| **Status LEDs** | Armed (green), Disarmed (yellow), Alarm (red) |
| **I2C LCD 16×2** | Real-time state + countdown display |
| **WiFi web server** | `GET /status` (JSON), `POST /arm`, `POST /disarm` |
| **State machine** | Disarmed → Exiting → Armed → Entry → Alarm, with entry/exit delays and alarm auto-timeout |

## Hardware wiring

| ESP32 GPIO | Component | Notes |
|---|---|---|
| 32 | PIR motion sensor OUT | INPUT_PULLDOWN |
| 33 | Reed switch | INPUT_PULLUP, magnet present = door closed |
| 25 | Armed LED (green) | +220 Ω resistor |
| 26 | Disarmed LED (yellow) | +220 Ω resistor |
| 27 | Alarm LED (red) | +220 Ω resistor |
| 14 | Buzzer / siren | Active buzzer, + transistor if needed |
| 19 | Keypad row 1 | |
| 18 | Keypad row 2 | |
| 5 | Keypad row 3 | |
| 17 | Keypad row 4 | |
| 16 | Keypad col 1 | |
| 4 | Keypad col 2 | |
| 0 | Keypad col 3 | |
| 2 | Keypad col 4 | |
| 21 (SDA) | I2C LCD SDA | |
| 22 (SCL) | I2C LCD SCL | |

> **I2C LCD address:** The code defaults to `0x27`. If your module uses `0x3F`, change `I2C_ADDR` in `src/main.cpp`. Run an I2C scanner sketch to confirm.

### Reed switch wiring
Connect one side of the reed switch to GPIO 33 and the other to GND. Place the magnet on the moving part (door/window) so it sits next to the reed switch when closed. With `INPUT_PULLUP`, the pin reads HIGH when the magnet is near (door closed) and LOW when the magnet moves away (door open).

### PIR wiring
Most HC-SR501 PIR modules output 3.3 V logic when motion is detected. Connect VCC→5V, GND→GND, OUT→GPIO 32.

## Software setup

1. **Install PlatformIO** (VS Code extension) or the PlatformIO CLI.
2. **Copy credentials:**
   ```bash
   cp src/secrets.h.example src/secrets.h
   ```
   Edit `src/secrets.h` and set your WiFi SSID/password.
3. **Build & upload:**
   ```bash
   pio run -t upload
   ```
4. **Monitor serial:**
   ```bash
   pio device monitor
   ```

## Libraries (auto-installed by PlatformIO)

- [`Keypad`](https://github.com/Chris--A/Keypad.git) — 4×4 matrix keypad
- [`Arduino-LiquidCrystal-I2C-library`](https://github.com/fdebrabander/Arduino-LiquidCrystal-I2C-library.git) — I2C LCD 16×2

## Web API

Once the ESP32 connects to WiFi, note the IP from the serial monitor.

| Endpoint | Method | Description |
|---|---|---|
| `http://<IP>/` | GET | Current state as JSON |
| `http://<IP>/status` | GET | Same as `/` |
| `http://<IP>/arm` | POST | Start arming (exit delay) |
| `http://<IP>/disarm` | POST | Disarm immediately |

Example:
```bash
curl http://192.168.1.50/status
# {"state":"ARMED","sensor_tripped":false,"wifi_rssi":-45}

curl -X POST http://192.168.1.50/disarm
```

## Default PIN

`1234` — change `CORRECT_PIN` in `src/main.cpp` before deploying.

## State machine

```
DISARMED  --(PIN or /arm)-->  EXITING  --(exit delay)-->  ARMED
   ^                               |                      |
   |                               |                      | (sensor tripped)
   |                               |                      v
   |                         (PIN)              ENTRY --(entry delay)--> ALARM
   |                               ^                      |  |
   +-----------(PIN or /disarm)----+                      +--+-- (PIN)
                                                              |
                                                        (auto-timeout)
                                                        siren off, awaiting PIN
```

- **Exit delay** (default 30 s): time to leave the premises after arming.
- **Entry delay** (default 15 s): time to disarm after a sensor trips before the siren sounds.
- **Alarm timeout** (default 3 min): siren stops automatically, but the system stays in alarm state until you enter the PIN.

## Customising

Edit these constants at the top of `src/main.cpp`:

```cpp
const unsigned long EXIT_DELAY_MS    = 30000;   // 30 s
const unsigned long ENTRY_DELAY_MS   = 15000;   // 15 s
const unsigned long ALARM_TIMEOUT_MS = 180000;  // 3 min
const char CORRECT_PIN[] = "1234";
```

Change GPIOs by updating the `PIN_*` constants and `rowPins`/`colPins` arrays.

## License

MIT — build, modify, and deploy as you like.