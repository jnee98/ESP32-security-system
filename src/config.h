// ============================================================================
// ESP32 Home Security System - Hardware Configuration
// ============================================================================
// Edit this file to match YOUR hardware wiring
// ============================================================================

#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------------------
// Pin Definitions - CHANGE THESE TO MATCH YOUR PCB/WIRING
// ---------------------------------------------------------------------------
// Ultrasonic Sensor (HC-SR04 or compatible)
#define PIN_ULTRASONIC_TRIG       32
#define PIN_ULTRASONIC_ECHO       33

// Reed Switch (door/window) - NC = closed when magnet present
// Wire: GPIO -> Reed Switch -> GND (uses internal pullup)
#define PIN_REED_SWITCH           25

// IR Tripwire Receiver (TSOP38238, VS1838B, etc.)
// Output is ACTIVE LOW when 38kHz IR detected (beam intact)
// Beam broken = HIGH
#define PIN_IR_RECEIVER           26

// Status LEDs (active HIGH)
#define PIN_LED_ARMED             27   // Green - system armed
#define PIN_LED_ALARM             14   // Red - alarm active

// Buzzer/Siren (active HIGH)
#define PIN_BUZZER                12

// Optional: IR Emitter control (if you want to pulse it to save power)
// #define PIN_IR_EMITTER           4

// Optional: Tamper switch (enclosure open detection)
// #define PIN_TAMPER               13

// Optional: External siren relay
// #define PIN_SIREN_RELAY          13

// ---------------------------------------------------------------------------
// Timing Constants (ms)
// ---------------------------------------------------------------------------
#define ULTRASONIC_CHECK_MS       100    // How often to ping ultrasonic
#define TRIPWIRE_CHECK_MS         50     // How often to check IR receiver
#define STATUS_PRINT_MS           5000   // Periodic status print interval

#define EXIT_DELAY_MS             30000  // 30 seconds to exit after arming
#define ENTRY_DELAY_MS            10000  // 10 seconds to disarm after entry
#define ALARM_TIMEOUT_MS          60000  // 60 seconds siren timeout (legal limits)

// ---------------------------------------------------------------------------
// Sensor Thresholds
// ---------------------------------------------------------------------------
#define ULTRASONIC_TRIGGER_CM     50     // Trigger if object closer than this
#define ULTRASONIC_MAX_CM         400    // Max valid reading (HC-SR04 limit)
#define ULTRASONIC_SAMPLES        3      // Median filter samples

// Reed switch debouncing
#define REED_DEBOUNCE_MS          50

// IR receiver - number of consecutive reads to confirm state change
#define IR_CONFIRM_READS          3

// ---------------------------------------------------------------------------
// Buzzer Patterns (on_ms, off_ms)
// ---------------------------------------------------------------------------
#define BEEP_EXITING_ON_MS        100
#define BEEP_EXITING_OFF_MS       900
#define BEEP_ENTRY_ON_MS          100
#define BEEP_ENTRY_OFF_MS         400
#define BEEP_ALARM_ON_MS          500
#define BEEP_ALARM_OFF_MS         500

// ---------------------------------------------------------------------------
// System Features (enable/disable)
// ---------------------------------------------------------------------------
#define ENABLE_PERSISTENT_STATE   1      // Save alarm state to NVS
#define ENABLE_WATCHDOG           0      // Hardware watchdog timer (disabled for ESP32-C6 testing)
#define ENABLE_WIFI               0      // WiFi for remote monitoring (future)
#define ENABLE_OTA                0      // OTA updates (future)
#define ENABLE_DEEP_SLEEP         0      // Deep sleep between checks (battery)

// ---------------------------------------------------------------------------
// Debug
// ---------------------------------------------------------------------------
#define SERIAL_BAUD               115200
#define DEBUG_VERBOSE             1      // Set 0 for production

// ---------------------------------------------------------------------------
// Derived / Internal - DO NOT MODIFY
// ---------------------------------------------------------------------------
#if ENABLE_PERSISTENT_STATE
  #include <Preferences.h>
#endif

#if ENABLE_WATCHDOG
  #include <esp_task_wdt.h>
#endif