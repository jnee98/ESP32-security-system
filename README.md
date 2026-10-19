# ESP32 Home Security System (ESP-IDF Port)

A complete, non-blocking ESP32 security alarm system ported to **native ESP-IDF** for the **ESP32-C6 DevKitC-1**. Features ultrasonic distance sensing, reed switch door/window detection, IR tripwire, status LEDs, buzzer siren, state machine with entry/exit delays, NVS persistent state, and UART serial command interface.

## Wokwi Simulation

Try it in [Wokwi](https://wokwi.com/) without any hardware:

<https://wokwi.com/projects/new> → import `wokwi/diagram.json`

Or open the diagram directly in this repo and click "Run simulation".

## Features

| Feature | Details |
|---|---|
| **Ultrasonic sensor (HC-SR04)** | Distance-based intrusion detection (GPIO 18 Trig, GPIO 19 Echo) |
| **Reed switch** | Door/window open detection (GPIO 20, internal pullup) |
| **IR tripwire receiver** | Beam-break detection (GPIO 21, TSOP38238/VS1838B) |
| **Arm/Disarm button** | Physical button to arm/disarm (GPIO 22, internal pullup) |
| **Buzzer / siren** | PWM-driven beeper with different patterns per state (GPIO 10) |
| **Status LEDs** | Armed (green, GPIO 2), Alarm (red, GPIO 3) |
| **State machine** | Disarmed → Exiting → Armed → Entry → Alarm, with entry/exit delays and alarm auto-timeout |
| **NVS persistence** | Alarm state survives reboot |
| **UART CLI** | Serial commands: `A`=Arm, `D`=Disarm, `S`=Status, `R`=Reset NVS |
| **Watchdog** | Optional (disabled by default for ESP32-C6 testing) |

## Hardware wiring (ESP32-C6 DevKitC-1)

| ESP32-C6 GPIO | Component | Notes |
|---|---|---|
| 2 | Armed LED (green) | +220 Ω resistor |
| 3 | Alarm LED (red) | +220 Ω resistor |
| 10 | Buzzer / siren | Active buzzer, PWM driven |
| 18 | Ultrasonic TRIG | Output |
| 19 | Ultrasonic ECHO | Input |
| 20 | Reed switch | INPUT_PULLUP, other side to GND |
| 21 | IR receiver | Active LOW when beam intact, HIGH when broken |
| 22 | Arm/Disarm button | INPUT_PULLUP, other side to GND |

> **Note:** GPIO 8 is the onboard RGB LED on the ESP32-C6 DevKitC-1 — avoid using it for security LEDs.

### Arm/Disarm button wiring
Connect one side of the button to GPIO 22 and the other to GND. Press and release to toggle arm/disarm (short press). The button is debounced in software.

### Reed switch wiring
Connect one side of the reed switch to GPIO 20 and the other to GND. Place the magnet on the moving part (door/window) so it sits next to the reed switch when closed. With `INPUT_PULLUP`, the pin reads HIGH when the magnet is near (door closed) and LOW when the magnet moves away (door open).

### IR tripwire wiring
Connect the IR receiver output to GPIO 21. When the 38 kHz IR beam is intact, the receiver output is LOW (active). When the beam is broken, the output goes HIGH.

### Ultrasonic wiring
Connect HC-SR04 VCC→3.3V/5V, GND→GND, TRIG→GPIO 18, ECHO→GPIO 19.

## Software setup (ESP-IDF)

### Prerequisites
- [ESP-IDF v5.x](https://docs.espressif.com/projects/esp-idf/en/latest/esp-get-started/index.html) installed and sourced
- ESP32-C6 DevKitC-1 connected via USB

### Build, flash, monitor

```bash
# Source ESP-IDF environment
source $HOME/esp/esp-idf/export.sh   # or your IDF path

# Set target (one-time)
idf.py set-target esp32c6

# Build
idf.py build

# Flash
idf.py -p /dev/ttyACM0 flash

# Monitor (Ctrl-] to exit)
idf.py -p /dev/ttyACM0 monitor
```

Or combine flash and monitor:
```bash
idf.py -p /dev/ttyACM0 flash monitor
```

### UART serial commands

Send commands over the USB serial port at 115200 baud:

| Command | Action |
|---|---|
| `A` / `a` | Arm system (starts exit delay, DISARMED or ALARM only) |
| `D` / `d` | Disarm system |
| `S` / `s` | Print full status |
| `R` / `r` | Clear NVS (restart to apply) |

Example:
```bash
# Using screen, picocom, or your serial terminal of choice
picocom -b 115200 /dev/ttyACM0
# Type: A (arm), D (disarm), S (status), R (reset NVS)
```

## State machine

```
DISARMED  --(A/Arm)-->  EXITING  --(exit delay)-->  ARMED
   rm/home/jnee/Downloads/IOT-PROJECTS/ESP32-security-system/wokwi && mkdir -p /home/jnee/Downloads/IOT-PROJECTS/ESP32-security-system/wokwi                           |                    |
   |                            |                    | (sensor tripped)
   |                            |                    v
   |                          (D/Disarm)          ENTRY --(entry delay)--> ALARM
   |                            ^                    |  |
   +-----------(D/Disarm)-------+                    +--+-- (D/Disarm)
                                                                   |
                                                             (auto-timeout: siren off, latched)
```

- **Exit delay** (default 30 s): time to leave the premises after arming. Buzzer beeps slowly.
- **Entry delay** (default 15 s): time to disarm after a sensor trips before the siren sounds. Buzzer beeps faster.
- **Alarm timeout** (default 60 s): siren stops automatically, but system stays latched in ALARM until you Disarm.

## Configuration

Edit `main/config.h` to change pins, timing, and thresholds:

```c
#define PIN_ULTRASONIC_TRIG       18
#define PIN_ULTRASONIC_ECHO       19
#define PIN_REED_SWITCH           20
#define PIN_IR_RECEIVER           21
#define PIN_ARM_BUTTON            22
#define PIN_LED_ARMED             2
#define PIN_LED_ALARM             3
#define PIN_BUZZER                10

#define ULTRASONIC_TRIGGER_CM     50    // Trigger if object closer than this
#define EXIT_DELAY_MS             30000 // 30 seconds
#define ENTRY_DELAY_MS            10000 // 10 seconds
#define ALARM_TIMEOUT_MS          60000 // 60 seconds
```

## Project structure

```
ESP32-security-system/
├── CMakeLists.txt              # ESP-IDF project root
├── main/CMakeLists.txt         # Component build config
├── main/main.c                 # Full ESP-IDF port (state machine, sensors, UART, NVS)
├── main/config.h               # Pin definitions & timing constants
├── wokwi/diagram.json          # Wokwi simulation diagram
├── platformio.ini              # Legacy PlatformIO config (kept for reference)
├── src/config.h                # Legacy Arduino config.h (kept for reference)
├── ESP32-security-system.ino   # Legacy Arduino sketch (kept for reference)
└── README.md                   # This file
```

## License

MIT — build, modify, and deploy as you like.
