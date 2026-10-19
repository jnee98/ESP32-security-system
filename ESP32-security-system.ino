// ============================================================================
// ESP32 Home Security System - Production Firmware
// ============================================================================
// Hardware-ready version with NVS persistence, watchdog, filtering
// Configure pins and behavior in config.h
// ============================================================================

#include "src/config.h"
#include <Arduino.h>

#if CONFIG_IDF_TARGET_ESP32C6
#include <soc/timer_group_reg.h>
#include <soc/timer_group_struct.h>
#include <hal/timer_ll.h>

// Disable TIMERG1 hardware watchdog at startup (before setup())
// This runs as a constructor before setup() is called
__attribute__((constructor(101))) void disable_timg1_wdt_early() {
  // Disable TIMERG1 watchdog immediately at startup
  TIMERG1.wdtwprotect.val = TIMG_WDT_WKEY_V;
  TIMERG1.wdtfeed.val = 1;
  TIMERG1.wdtconfig0.val = 0;
  TIMERG1.wdtwprotect.val = 0;
}
#endif

// ============================================================================
// State Machine
// ============================================================================
enum class SystemState : uint8_t {
  DISARMED = 0,
  EXITING = 1,
  ARMED = 2,
  ENTRY = 3,
  ALARM = 4
};

SystemState g_state = SystemState::DISARMED;
uint32_t g_state_start_ms = 0;

// ============================================================================
// Sensor State
// ============================================================================
struct SensorState {
  bool ultrasonic_triggered = false;
  float ultrasonic_distance_cm = 0.0f;
  bool reed_open = false;
  bool tripwire_broken = false;
  uint32_t last_ultrasonic_ms = 0;
  uint32_t last_tripwire_ms = 0;
  uint32_t last_status_print_ms = 0;
  uint8_t reed_stable_count = 0;
  uint8_t ir_stable_count = 0;
  bool last_reed_raw = false;
  bool last_ir_raw = false;
} g_sensors;

bool g_alarm_latched = false;
bool g_alarm_auto_timed_out = false;

// ============================================================================
// NVS Preferences
// ============================================================================
#if ENABLE_PERSISTENT_STATE
Preferences g_prefs;
const char* PREFS_NAMESPACE = "security";
const char* PREFS_KEY_STATE = "state";
const char* PREFS_KEY_LATCHED = "latched";
#endif

// Forward declarations
void enterState(SystemState ns);
void updateBuzzer(uint32_t now);
void readUltrasonic();
void readReedSwitch();
void readTripwire();
void handleSerialCommand(char c);
void printStatus();
void saveStateToNVS();
void loadStateFromNVS();
void feedWatchdog();

// ============================================================================
// Setup
// ============================================================================
void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(500);

  Serial.println(F("\n========================================"));
  Serial.println(F("ESP32 Home Security System - Production"));
  Serial.println(F("========================================"));

  pinMode(PIN_ULTRASONIC_TRIG, OUTPUT);
  pinMode(PIN_ULTRASONIC_ECHO, INPUT);
  pinMode(PIN_REED_SWITCH, INPUT_PULLUP);
  pinMode(PIN_IR_RECEIVER, INPUT);
  pinMode(PIN_LED_ARMED, OUTPUT);
  pinMode(PIN_LED_ALARM, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);

  digitalWrite(PIN_ULTRASONIC_TRIG, LOW);
  digitalWrite(PIN_LED_ARMED, LOW);
  digitalWrite(PIN_LED_ALARM, LOW);
  digitalWrite(PIN_BUZZER, LOW);

#if CONFIG_IDF_TARGET_ESP32C6
  Serial.println(F("[INIT] TIMERG1 HW watchdog disabled at startup"));
#endif

#if ENABLE_WATCHDOG
#if CONFIG_IDF_TARGET_ESP32C6
  // ESP32-C6 uses different watchdog API - use esp_task_wdt_init with config struct
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = 30000,
    .idle_core_mask = 1,  // Single core on C6
    .trigger_panic = true
  };
  esp_err_t ret = esp_task_wdt_init(&wdt_config);
  if (ret == ESP_OK) {
    esp_task_wdt_add(NULL);
    Serial.println(F("[INIT] Watchdog enabled (30s)"));
  } else {
    Serial.printf(F("[INIT] Watchdog init failed: %d\n"), ret);
  }
#else
  // ESP32/ESP32-S2/S3
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = 30000,
    .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
    .trigger_panic = true
  };
  esp_task_wdt_init(&wdt_config);
  esp_task_wdt_add(NULL);
  Serial.println(F("[INIT] Watchdog enabled (30s)"));
#endif
#endif

#if ENABLE_PERSISTENT_STATE
  loadStateFromNVS();
#endif

  if (g_alarm_latched) {
    enterState(SystemState::ALARM);
    g_alarm_auto_timed_out = false;
    Serial.println(F("[INIT] Restored ALARM state from NVS"));
  } else {
    enterState(SystemState::DISARMED);
  }

  Serial.println(F("[INIT] System ready"));
  Serial.println(F("Commands: A=Arm, D=Disarm, S=Status, R=Reset NVS"));
}

// ============================================================================
// Main Loop
// ============================================================================
void loop() {
  uint32_t now = millis();

#if ENABLE_WATCHDOG
  feedWatchdog();
#endif

  readUltrasonic();
  readReedSwitch();
  readTripwire();

  if (g_state == SystemState::ARMED) {
    if (g_sensors.ultrasonic_triggered || g_sensors.reed_open || g_sensors.tripwire_broken) {
      if (g_sensors.ultrasonic_triggered) Serial.println(F("[TRIGGER] Ultrasonic: object detected"));
      if (g_sensors.reed_open) Serial.println(F("[TRIGGER] Reed switch: door/window opened"));
      if (g_sensors.tripwire_broken) Serial.println(F("[TRIGGER] Tripwire: IR beam broken"));
      g_alarm_latched = true;
      saveStateToNVS();
      enterState(SystemState::ENTRY);
    }
  }

  if (Serial.available() > 0) {
    handleSerialCommand(Serial.read());
  }

  switch (g_state) {
    case SystemState::EXITING:
      if (now - g_state_start_ms >= EXIT_DELAY_MS) enterState(SystemState::ARMED);
      break;
    case SystemState::ENTRY:
      if (now - g_state_start_ms >= ENTRY_DELAY_MS) enterState(SystemState::ALARM);
      break;
    case SystemState::ALARM:
      if (!g_alarm_auto_timed_out && (now - g_state_start_ms >= ALARM_TIMEOUT_MS)) {
        g_alarm_auto_timed_out = true;
      }
      break;
  }

  updateBuzzer(now);

  if (now - g_sensors.last_status_print_ms >= STATUS_PRINT_MS) {
    g_sensors.last_status_print_ms = now;
#if DEBUG_VERBOSE
    printStatus();
#endif
  }
}

// ============================================================================
// State Transition
// ============================================================================
void enterState(SystemState ns) {
  g_state = ns;
  g_state_start_ms = millis();
  g_alarm_auto_timed_out = false;

  digitalWrite(PIN_LED_ARMED, ns == SystemState::ARMED ? HIGH : LOW);
  digitalWrite(PIN_LED_ALARM, ns == SystemState::ALARM ? HIGH : LOW);

  switch (ns) {
    case SystemState::DISARMED:
      g_alarm_latched = false;
      g_sensors.ultrasonic_triggered = g_sensors.reed_open = g_sensors.tripwire_broken = false;
      Serial.println(F("[STATE] DISARMED"));
      break;
    case SystemState::EXITING:
      Serial.printf("[STATE] EXITING - %lu sec exit delay\n", EXIT_DELAY_MS / 1000);
      break;
    case SystemState::ARMED:
      Serial.println(F("[STATE] ARMED - monitoring active"));
      break;
    case SystemState::ENTRY:
      Serial.printf("[STATE] ENTRY - %lu sec to disarm\n", ENTRY_DELAY_MS / 1000);
      break;
    case SystemState::ALARM:
      Serial.println(F("[STATE] ALARM - SIREN ACTIVE"));
      break;
  }
  saveStateToNVS();
}

// ============================================================================
// Buzzer Control
// ============================================================================
void updateBuzzer(uint32_t now) {
  uint32_t elapsed = now - g_state_start_ms;
  bool beep_on = false;

  switch (g_state) {
    case SystemState::EXITING:
      beep_on = (elapsed % (BEEP_EXITING_ON_MS + BEEP_EXITING_OFF_MS)) < BEEP_EXITING_ON_MS;
      break;
    case SystemState::ENTRY:
      beep_on = (elapsed % (BEEP_ENTRY_ON_MS + BEEP_ENTRY_OFF_MS)) < BEEP_ENTRY_ON_MS;
      break;
    case SystemState::ALARM:
      if (!g_alarm_auto_timed_out) {
        beep_on = (elapsed % (BEEP_ALARM_ON_MS + BEEP_ALARM_OFF_MS)) < BEEP_ALARM_ON_MS;
      }
      break;
    default:
      beep_on = false;
      break;
  }
  digitalWrite(PIN_BUZZER, beep_on ? HIGH : LOW);
}

// ============================================================================
// Ultrasonic Sensor with Median Filter
// ============================================================================
void readUltrasonic() {
  uint32_t now = millis();
  if (now - g_sensors.last_ultrasonic_ms < ULTRASONIC_CHECK_MS) return;
  g_sensors.last_ultrasonic_ms = now;

  float samples[ULTRASONIC_SAMPLES];
  int valid = 0;

  for (int i = 0; i < ULTRASONIC_SAMPLES; i++) {
    digitalWrite(PIN_ULTRASONIC_TRIG, LOW);
    delayMicroseconds(2);
    digitalWrite(PIN_ULTRASONIC_TRIG, HIGH);
    delayMicroseconds(10);
    digitalWrite(PIN_ULTRASONIC_TRIG, LOW);

    uint32_t dur = pulseIn(PIN_ULTRASONIC_ECHO, HIGH, 30000);
    if (dur > 0) {
      float d = dur * 0.01715f;
      if (d > 2.0f && d < ULTRASONIC_MAX_CM) samples[valid++] = d;
    }
    delay(2);
  }

  if (valid > 0) {
    for (int i = 0; i < valid - 1; i++)
      for (int j = i + 1; j < valid; j++)
        if (samples[i] > samples[j]) {
          float t = samples[i];
          samples[i] = samples[j];
          samples[j] = t;
        }
    g_sensors.ultrasonic_distance_cm = samples[valid / 2];
    g_sensors.ultrasonic_triggered = (g_sensors.ultrasonic_distance_cm < ULTRASONIC_TRIGGER_CM);
  } else {
    g_sensors.ultrasonic_triggered = false;
  }

#if DEBUG_VERBOSE
  if (g_sensors.ultrasonic_triggered) Serial.printf("[ULTRA] %.1f cm TRIGGERED\n", g_sensors.ultrasonic_distance_cm);
#endif
}

// ============================================================================
// Reed Switch (Door/Window)
// ============================================================================
void readReedSwitch() {
  uint32_t now = millis();
  bool raw = (digitalRead(PIN_REED_SWITCH) == HIGH);

  if (raw != g_sensors.last_reed_raw) {
    g_sensors.reed_stable_count = 0;
    g_sensors.last_reed_raw = raw;
    return;
  }

  if (g_sensors.reed_stable_count < REED_DEBOUNCE_MS / 10) {
    g_sensors.reed_stable_count++;
    return;
  }

  g_sensors.reed_open = raw;
}

// ============================================================================
// IR Tripwire Receiver
// ============================================================================
void readTripwire() {
  uint32_t now = millis();
  if (now - g_sensors.last_tripwire_ms < TRIPWIRE_CHECK_MS) return;
  g_sensors.last_tripwire_ms = now;

  bool raw = (digitalRead(PIN_IR_RECEIVER) == HIGH);

  if (raw != g_sensors.last_ir_raw) {
    g_sensors.ir_stable_count = 0;
    g_sensors.last_ir_raw = raw;
    return;
  }

  if (g_sensors.ir_stable_count < IR_CONFIRM_READS) {
    g_sensors.ir_stable_count++;
    return;
  }

  g_sensors.tripwire_broken = raw;
}

// ============================================================================
// Serial Commands
// ============================================================================
void handleSerialCommand(char c) {
  switch (c) {
    case 'A':
    case 'a':
      if (g_state == SystemState::DISARMED || g_state == SystemState::ALARM) {
        enterState(SystemState::EXITING);
        Serial.println(F("[CMD] Arming - exit delay started"));
      } else Serial.println(F("[CMD] Can only arm from DISARMED or ALARM"));
      break;
    case 'D':
    case 'd':
      enterState(SystemState::DISARMED);
      Serial.println(F("[CMD] Disarmed"));
      break;
    case 'S':
    case 's':
      printStatus();
      break;
    case 'R':
    case 'r':
#if ENABLE_PERSISTENT_STATE
      g_prefs.begin(PREFS_NAMESPACE, false);
      g_prefs.clear();
      g_prefs.end();
      g_alarm_latched = false;
      Serial.println(F("[CMD] NVS cleared - restart to apply"));
#else
      Serial.println(F("[CMD] NVS not enabled"));
#endif
      break;
    default:
      Serial.println(F("[CMD] Use A/D/S/R"));
  }
}

// ============================================================================
// Status Print
// ============================================================================
void printStatus() {
  const char* s[] = { "DISARMED", "EXITING", "ARMED", "ENTRY", "ALARM" };
  Serial.printf("[STATUS] State: %s | Latched: %s | Uptime: %lus\n",
                s[(int)g_state], g_alarm_latched ? "YES" : "NO", millis() / 1000);
  Serial.printf("[STATUS] Ultra: %.1f cm (%s) | Reed: %s | IR: %s\n",
                g_sensors.ultrasonic_distance_cm,
                g_sensors.ultrasonic_triggered ? "TRIGGER" : "OK",
                g_sensors.reed_open ? "OPEN" : "CLOSED",
                g_sensors.tripwire_broken ? "BROKEN" : "INTACT");
  Serial.printf("[STATUS] Free heap: %lu bytes\n", ESP.getFreeHeap());
}

// ============================================================================
// NVS Persistence
// ============================================================================
void saveStateToNVS() {
#if ENABLE_PERSISTENT_STATE
  g_prefs.begin(PREFS_NAMESPACE, false);
  g_prefs.putUChar(PREFS_KEY_STATE, (uint8_t)g_state);
  g_prefs.putBool(PREFS_KEY_LATCHED, g_alarm_latched);
  g_prefs.end();
#endif
}

void loadStateFromNVS() {
#if ENABLE_PERSISTENT_STATE
  g_prefs.begin(PREFS_NAMESPACE, true);
  g_alarm_latched = g_prefs.getBool(PREFS_KEY_LATCHED, false);
  g_prefs.end();
#endif
}

// ============================================================================
// Watchdog
// ============================================================================
void feedWatchdog() {
#if ENABLE_WATCHDOG
  esp_task_wdt_reset();
#endif
}