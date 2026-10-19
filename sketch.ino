/*
 * ESP32 Home Security System - Wokwi Simulation Prototype
 *
 * Sensors: HC-SR04 ultrasonic, reed switch, IR break-beam (simulated by button)
 * Outputs: armed LED, alarm LED, buzzer
 * Input:   arm/disarm button + serial commands
 *
 * States:
 *   DISARMED -> (arm) -> EXITING -> (delay) -> ARMED
 *   ARMED    -> (sensor trip) -> ENTRY -> (delay) -> ALARM
 *   ENTRY/ALARM/EXITING/ARMED -> (disarm) -> DISARMED
 *
 * Serial: 'A' arm, 'D' disarm, 'S' status
 * Button (GPIO4): toggles arm/disarm
 */

#include <Arduino.h>

// ---------------- Pins (ESP32-C6 DevKit: GPIO 0-21) ----------------
const int PIN_TRIG      = 2;   // TRIG
const int PIN_ECHO      = 3;   // ECHO
const int PIN_REED      = 4;   // Reed switch (INPUT_PULLUP, LOW = open)
const int PIN_IR        = 5;   // IR tripwire (INPUT_PULLUP, LOW = broken)
const int PIN_BTN       = 6;   // Arm/disarm button (INPUT_PULLUP)
const int PIN_LED_ARMED = 7;   // Armed LED (via 220Ω)
const int PIN_LED_ALARM = 8;   // Alarm LED (via 220Ω)
const int PIN_BUZZER    = 9;   // Buzzer (PWM)

// ---------------- Timing (ms) ----------------
const unsigned long EXIT_DELAY_MS       = 5000;
const unsigned long ENTRY_DELAY_MS      = 3000;
const unsigned long ALARM_TIMEOUT_MS    = 10000;
const unsigned long ULTRASONIC_CHECK_MS = 100;
const unsigned long SERIAL_UPDATE_MS    = 1000;
const unsigned long DEBOUNCE_MS         = 50;

const float TRIGGER_DISTANCE_CM = 50.0;
const int   BUZZER_FREQ         = 2000;

// ---------------- State ----------------
enum class SystemState { DISARMED, EXITING, ARMED, ENTRY, ALARM };

SystemState   g_state = SystemState::DISARMED;
unsigned long g_state_start_ms = 0;
bool          g_siren_silenced = false;

unsigned long g_last_ultrasonic_ms = 0;
unsigned long g_last_serial_ms = 0;
float         g_distance_cm = 999.0;

bool          g_objectClose = false;
bool          g_doorOpen = false;
bool          g_tripBroken = false;

bool          g_btn_last = HIGH;
unsigned long g_btn_change_ms = 0;

// ---------------- Helpers ----------------
const char* stateName(SystemState s) {
  switch (s) {
    case SystemState::DISARMED: return "DISARMED";
    case SystemState::EXITING:  return "EXITING";
    case SystemState::ARMED:    return "ARMED";
    case SystemState::ENTRY:    return "ENTRY";
    case SystemState::ALARM:    return "ALARM";
  }
  return "?";
}

void setLeds(SystemState s) {
  digitalWrite(PIN_LED_ARMED, s == SystemState::ARMED ? HIGH : LOW);
  digitalWrite(PIN_LED_ALARM, s == SystemState::ALARM ? HIGH : LOW);
}

void buzzerOn(bool on) {
  if (on) tone(PIN_BUZZER, BUZZER_FREQ);
  else    noTone(PIN_BUZZER);
}

void updateBuzzer(SystemState s, unsigned long now) {
  unsigned long elapsed = now - g_state_start_ms;
  switch (s) {
    case SystemState::EXITING: buzzerOn((elapsed % 1000) < 100); break;
    case SystemState::ENTRY:   buzzerOn((elapsed % 500) < 100);  break;
    case SystemState::ALARM:   buzzerOn(!g_siren_silenced && (elapsed % 500) < 250); break;
    default:                   buzzerOn(false); break;
  }
}

void enterState(SystemState s, unsigned long now) {
  g_state = s;
  g_state_start_ms = now;
  g_siren_silenced = false;
  setLeds(s);
  Serial.print("[STATE] ");
  Serial.println(stateName(s));
}

void armSystem(unsigned long now) {
  if (g_state == SystemState::DISARMED) {
    enterState(SystemState::EXITING, now);
    Serial.println("[COMMAND] Arming - exit delay started");
  }
}

void disarmSystem(unsigned long now) {
  enterState(SystemState::DISARMED, now);
  Serial.println("[COMMAND] Disarmed");
}

void printStatus() {
  Serial.print("[STATUS] State: ");
  Serial.println(stateName(g_state));
  Serial.print("[STATUS] Sensors: ");
  Serial.print(g_objectClose ? "ULTRA_CLOSE " : "ULTRA_OK ");
  Serial.print(g_doorOpen    ? "DOOR_OPEN "   : "DOOR_CLOSED ");
  Serial.println(g_tripBroken ? "TRIP_BROKEN" : "TRIP_OK");
}

float readDistanceCm() {
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);
  long duration = pulseIn(PIN_ECHO, HIGH, 30000);
  if (duration <= 0) return 999.0;          // no echo = nothing in range
  return duration * 0.0343f / 2.0f;
}

// ---------------- Setup ----------------
void setup() {
  Serial.begin(115200);
  delay(100);

  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  pinMode(PIN_REED, INPUT_PULLUP);
  pinMode(PIN_IR,   INPUT_PULLUP);
  pinMode(PIN_BTN,  INPUT_PULLUP);
  pinMode(PIN_LED_ARMED, OUTPUT);
  pinMode(PIN_LED_ALARM, OUTPUT);

  digitalWrite(PIN_TRIG, LOW);
  digitalWrite(PIN_LED_ARMED, LOW);
  digitalWrite(PIN_LED_ALARM, LOW);
  noTone(PIN_BUZZER);

  Serial.println("ESP32 Security System - Wokwi Simulation");
  Serial.println("Sensors: Ultrasonic, Reed Switch, IR Tripwire");
  Serial.println("Commands: 'A' arm, 'D' disarm, 'S' status, or press the Arm button");

  enterState(SystemState::DISARMED, millis());
}

// ---------------- Loop ----------------
void loop() {
  unsigned long now = millis();

  // ---- Sensors ----
  if (now - g_last_ultrasonic_ms >= ULTRASONIC_CHECK_MS) {
    g_last_ultrasonic_ms = now;
    g_distance_cm = readDistanceCm();
    g_objectClose = (g_distance_cm < TRIGGER_DISTANCE_CM);

    if (now - g_last_serial_ms >= SERIAL_UPDATE_MS) {
      g_last_serial_ms = now;
      Serial.print("Distance: ");
      Serial.print(g_distance_cm);
      Serial.println(" cm");
    }
  }
  g_doorOpen   = (digitalRead(PIN_REED) == LOW);
  g_tripBroken = (digitalRead(PIN_IR) == LOW);

  // ---- Button (debounced, toggles arm/disarm) ----
  bool btn = digitalRead(PIN_BTN);
  if (btn != g_btn_last && (now - g_btn_change_ms) > DEBOUNCE_MS) {
    g_btn_change_ms = now;
    g_btn_last = btn;
    if (btn == LOW) {
      if (g_state == SystemState::DISARMED) armSystem(now);
      else                                  disarmSystem(now);
    }
  }

  // ---- Serial commands ----
  while (Serial.available() > 0) {
    char c = Serial.read();
    switch (c) {
      case 'A': case 'a': armSystem(now); break;
      case 'D': case 'd': disarmSystem(now); break;
      case 'S': case 's': printStatus(); break;
      default: break;
    }
  }

  // ---- Trigger detection ----
  if (g_state == SystemState::ARMED &&
      (g_objectClose || g_doorOpen || g_tripBroken)) {
    if (g_objectClose) Serial.println("[TRIGGER] Object detected by ultrasonic");
    if (g_doorOpen)    Serial.println("[TRIGGER] Door/window opened (reed switch)");
    if (g_tripBroken)  Serial.println("[TRIGGER] Tripwire broken (IR)");
    enterState(SystemState::ENTRY, now);
  }

  // ---- State timing ----
  switch (g_state) {
    case SystemState::EXITING:
      if (now - g_state_start_ms >= EXIT_DELAY_MS) enterState(SystemState::ARMED, now);
      break;
    case SystemState::ENTRY:
      if (now - g_state_start_ms >= ENTRY_DELAY_MS) enterState(SystemState::ALARM, now);
      break;
    case SystemState::ALARM:
      if (!g_siren_silenced && now - g_state_start_ms >= ALARM_TIMEOUT_MS) {
        g_siren_silenced = true;
        digitalWrite(PIN_LED_ALARM, LOW);
        Serial.println("[ALARM] Timeout - siren off, send 'D' or press button to disarm");
      }
      break;
    default:
      break;
  }

  updateBuzzer(g_state, now);
  delay(10);
}
