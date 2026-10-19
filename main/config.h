// ============================================================================
// ESP32 Home Security System - Hardware Configuration
// ============================================================================
// Edit this file to match YOUR hardware wiring
// ============================================================================

#pragma once

// ---------------------------------------------------------------------------
// Pin Definitions - CHANGE THESE TO MATCH YOUR PCB/WIRING
// ESP32-C6 has GPIOs 0-22 only (GPIO 8 is onboard RGB LED)
// ---------------------------------------------------------------------------
// Ultrasonic Sensor (HC-SR04 or compatible)
#define PIN_ULTRASONIC_TRIG       18
#define PIN_ULTRASONIC_ECHO       19

// Reed Switch (door/window) - NC = closed when magnet present
// Wire: GPIO -> Reed Switch -> GND (uses internal pullup)
#define PIN_REED_SWITCH           20

// IR Tripwire Receiver (TSOP38238, VS1838B, etc.)
// Output is ACTIVE LOW when 38kHz IR detected (beam intact)
// Beam broken = HIGH
#define PIN_IR_RECEIVER           21

// Arm/Disarm Button (active LOW with internal pullup)
#define PIN_ARM_BUTTON            22

// Status LEDs (active HIGH)
#define PIN_LED_ARMED             2   // Green - system armed
#define PIN_LED_ALARM             3   // Red - alarm active

// Buzzer/Siren (active HIGH)
#define PIN_BUZZER                10

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

#define EXIT_DELAY_MS             3000  // 30 seconds to exit after arming
#define ENTRY_DELAY_MS            10000  // 10 seconds to disarm after entry
#define ALARM_TIMEOUT_MS          60000  // 60 seconds siren timeout (legal limits)

// ---------------------------------------------------------------------------
// Sensor Thresholds
// ---------------------------------------------------------------------------
#define ULTRASONIC_TRIGGER_CM     50     // Trigger if object closer than this
#define ULTRASONIC_MAX_CM         400    // Max valid reading (HC-SR04 limit)
#define ULTRASONIC_SAMPLES        2     // Median filter samples

// Reed switch debouncing
#define REED_DEBOUNCE_MS          50
#define REED_CHECK_MS             10     // How often to check reed switch

// IR receiver - number of consecutive reads to confirm state change
#define IR_CONFIRM_READS          3

// Enable/disable IR tripwire (set to 0 if no IR transmitter installed)
#define IR_TRIPWIRE_ENABLED       0

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
#define ENABLE_WIFI               1      // WiFi for remote monitoring
#define ENABLE_OTA                0      // OTA updates (future)
#define ENABLE_DEEP_SLEEP         0      // Deep sleep between checks (battery)

// ---------------------------------------------------------------------------
// WiFi Configuration (set when ENABLE_WIFI = 1)
// ---------------------------------------------------------------------------
#define WIFI_SSID                 "!..!"
#define WIFI_PASSWORD             "john12@#"
#define WIFI_MAX_RETRY            5      // Connection retries
#define WIFI_RETRY_DELAY_MS       2000   // Delay between retries

// ---------------------------------------------------------------------------
// Web Server Configuration (requires ENABLE_WIFI = 1)
// ---------------------------------------------------------------------------
#define WEB_SERVER_PORT           80     // HTTP port
#define WEB_SERVER_MAX_URI_HANDLERS 16   // Max URI handlers

// ---------------------------------------------------------------------------
// MQTT Configuration (optional, requires ENABLE_WIFI = 1)
// ---------------------------------------------------------------------------
#define MQTT_ENABLED              1     // Set to 1 to enable MQTT
#define MQTT_BROKER_URI           "mqtt://broker.hivemq.com:1883"
#define MQTT_CLIENT_ID            "esp32-security-%06x"
#define MQTT_USERNAME             ""
#define MQTT_PASSWORD             ""
#define MQTT_TOPIC_STATUS         "home/security/status"
#define MQTT_TOPIC_COMMAND        "home/security/command"
#define MQTT_TOPIC_CONFIG         "home/security/config"
#define MQTT_KEEPALIVE_S          60
#define MQTT_QOS                  1

// ---------------------------------------------------------------------------
// Debug
// ---------------------------------------------------------------------------
#define SERIAL_BAUD               115200
#define DEBUG_VERBOSE             1      // Set 0 for production

// ---------------------------------------------------------------------------
// Derived / Internal - DO NOT MODIFY
// ---------------------------------------------------------------------------