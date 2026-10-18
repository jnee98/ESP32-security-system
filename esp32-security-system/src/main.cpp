/*
 * ESP32 Home Security System
 * ==========================
 * A complete, non-blocking security alarm system with:
 *   - PIR motion sensor (motion detection)
 *   - Magnetic reed switch (door/window open detection)
 *   - 4x4 matrix keypad (PIN arm / disarm)
 *   - Buzzer / siren output
 *   - 3 status LEDs (Armed / Disarmed / Alarm)
 *   - I2C LCD 16x2 (status display)
 *   - WiFi web server (remote arm/disarm + JSON status API)
 *
 * State machine:
 *   DISARMED  -> (enter valid PIN) -> EXITING -> (exit delay expires) -> ARMED
 *   ARMED     -> (sensor tripped)  -> ENTRY   -> (entry delay expires) -> ALARM
 *   ENTRY     -> (enter valid PIN) -> DISARMED
 *   ALARM     -> (enter valid PIN) -> DISARMED  (or auto-timeout)
 *
 * Copy src/secrets.h.example to src/secrets.h and set your WiFi credentials.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Keypad.h>
#include <LiquidCrystal_I2C.h>

#ifdef SECRETS_H
#include "secrets.h"
#else
#warning "secrets.h not found - using placeholder credentials"
#define WIFI_SSID     "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
#endif

// ---------------------------------------------------------------------------
// Pin assignments (ESP32 GPIO numbers)
// ---------------------------------------------------------------------------
const int PIN_PIR        = 32;   // PIR motion sensor output
const int PIN_REED       = 33;   // Magnetic reed switch (door/window)
const int PIN_LED_ARMED  = 25;   // Green LED - system armed
const int PIN_LED_DISARM = 26;   // Yellow LED - system disarmed
const int PIN_LED_ALARM  = 27;   // Red LED - alarm active
const int PIN_BUZZER     = 14;   // Buzzer / siren
const int I2C_ADDR       = 0x27; // I2C LCD address (common: 0x27 or 0x3F)

// ---------------------------------------------------------------------------
// Timing constants (milliseconds)
// ---------------------------------------------------------------------------
const unsigned long EXIT_DELAY_MS    = 30000;   // 30 s to leave after arming
const unsigned long ENTRY_DELAY_MS   = 15000;   // 15 s to disarm after entry
const unsigned long ALARM_TIMEOUT_MS = 180000;  // 3 min siren auto-shutoff
const unsigned long PIR_COOLDOWN_MS  = 5000;    // PIR re-trigger cooldown
const unsigned long WIFI_RECONNECT_MS = 10000;  // WiFi reconnect interval
const unsigned long LCD_UPDATE_MS    = 1000;    // LCD refresh interval

// ---------------------------------------------------------------------------
// Security PIN (4 digits)
// ---------------------------------------------------------------------------
const char CORRECT_PIN[] = "1234";

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------
enum class SystemState {
  DISARMED,   // System off, no monitoring
  EXITING,    // Counting down exit delay after arming
  ARMED,      // Fully armed, monitoring sensors
  ENTRY,      // Entry delay active after sensor trip
  ALARM       // Siren sounding
};

SystemState g_state = SystemState::DISARMED;
unsigned long g_state_start_ms = 0;
bool g_sensor_tripped = false;     // latched trip while ARMED/ENTRY
unsigned long g_last_pir_ms = 0;
unsigned long g_last_lcd_ms = 0;
unsigned long g_last_wifi_check_ms = 0;
char g_pin_buffer[8] = {0};
uint8_t g_pin_len = 0;
bool g_alarm_auto_armed = false;   // alarm reached timeout, waiting for disarm

// ---------------------------------------------------------------------------
// Peripherals
// ---------------------------------------------------------------------------
WebServer g_server(80);
LiquidCrystal_I2C g_lcd(I2C_ADDR, 16, 2);

// Keypad setup
const byte ROWS = 4;
const byte COLS = 4;
char keys[ROWS][COLS] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};
byte rowPins[ROWS] = {19, 18, 5, 17};   // ESP32 GPIOs for keypad rows
byte colPins[COLS] = {16, 4, 0, 2};     // ESP32 GPIOs for keypad cols
Keypad g_keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// ---------------------------------------------------------------------------
// Helper: LED control
// ---------------------------------------------------------------------------
void setLeds(SystemState state) {
  digitalWrite(PIN_LED_ARMED,  state == SystemState::ARMED   ? HIGH : LOW);
  digitalWrite(PIN_LED_DISARM, state == SystemState::DISARMED ? HIGH : LOW);
  digitalWrite(PIN_LED_ALARM,  state == SystemState::ALARM    ? HIGH : LOW);
}

// ---------------------------------------------------------------------------
// Helper: buzzer control
// ---------------------------------------------------------------------------
void updateBuzzer(SystemState state, unsigned long now) {
  unsigned long elapsed = now - g_state_start_ms;

  switch (state) {
    case SystemState::EXITING:
      digitalWrite(PIN_BUZZER, (elapsed % 1000) < 100 ? HIGH : LOW);
      break;
    case SystemState::ENTRY:
      digitalWrite(PIN_BUZZER, (elapsed % 500) < 100 ? HIGH : LOW);
      break;
    case SystemState::ALARM:
      digitalWrite(PIN_BUZZER, (elapsed % 500) < 250 ? HIGH : LOW);
      break;
    default:
      digitalWrite(PIN_BUZZER, LOW);
      break;
  }
}

// ---------------------------------------------------------------------------
// Helper: LCD update
// ---------------------------------------------------------------------------
void updateLcd(SystemState state, unsigned long now) {
  if (now - g_last_lcd_ms < LCD_UPDATE_MS) return;
  g_last_lcd_ms = now;

  g_lcd.clear();
  g_lcd.setCursor(0, 0);

  switch (state) {
    case SystemState::DISARMED:
      g_lcd.print("DISARMED");
      g_lcd.setCursor(0, 1);
      g_lcd.print("PIN to ARM");
      break;
    case SystemState::EXITING: {
      unsigned long remaining = (EXIT_DELAY_MS - (now - g_state_start_ms)) / 1000;
      g_lcd.print("EXITING ");
      g_lcd.print(remaining);
      g_lcd.print("s   ");
      g_lcd.setCursor(0, 1);
      g_lcd.print("Leave now");
      break;
    }
    case SystemState::ARMED:
      g_lcd.print("ARMED");
      g_lcd.setCursor(0, 1);
      g_lcd.print("PIR+Door active");
      break;
    case SystemState::ENTRY: {
      unsigned long remaining = (ENTRY_DELAY_MS - (now - g_state_start_ms)) / 1000;
      g_lcd.print("ENTRY  ");
      g_lcd.print(remaining);
      g_lcd.print("s   ");
      g_lcd.setCursor(0, 1);
      g_lcd.print("PIN to stop");
      break;
    }
    case SystemState::ALARM:
      g_lcd.print("!! ALARM !! ");
      g_lcd.setCursor(0, 1);
      g_lcd.print("PIN to stop");
      break;
  }
}

// ---------------------------------------------------------------------------
// State transitions
// ---------------------------------------------------------------------------
void enterState(SystemState newState, unsigned long now) {
  g_state = newState;
  g_state_start_ms = now;
  setLeds(newState);

  switch (newState) {
    case SystemState::DISARMED:
      g_sensor_tripped = false;
      g_pin_len = 0;
      memset(g_pin_buffer, 0, sizeof(g_pin_buffer));
      Serial.println("[STATE] DISARMED");
      break;
    case SystemState::EXITING:
      Serial.println("[STATE] EXITING - exit delay started");
      break;
    case SystemState::ARMED:
      Serial.println("[STATE] ARMED - monitoring active");
      break;
    case SystemState::ENTRY:
      Serial.println("[STATE] ENTRY - entry delay started");
      break;
    case SystemState::ALARM:
      Serial.println("[STATE] ALARM - siren active");
      break;
  }
}

// ---------------------------------------------------------------------------
// PIN handling
// ---------------------------------------------------------------------------
bool checkPin(const char* pin) {
  return strncmp(pin, CORRECT_PIN, 4) == 0;
}

// ---------------------------------------------------------------------------
// WiFi + Web server
// ---------------------------------------------------------------------------
void handleRoot() {
  String json = "{";
  json += "\"state\":\"";
  switch (g_state) {
    case SystemState::DISARMED: json += "DISARMED"; break;
    case SystemState::EXITING:  json += "EXITING";  break;
    case SystemState::ARMED:    json += "ARMED";    break;
    case SystemState::ENTRY:    json += "ENTRY";    break;
    case SystemState::ALARM:    json += "ALARM";    break;
  }
  json += "\",";
  json += "\"sensor_tripped\":" + String(g_sensor_tripped ? "true" : "false") + ",";
  json += "\"wifi_rssi\":" + String(WiFi.RSSI()) + "}";
  g_server.send(200, "application/json", json);
}

void handleArm() {
  if (g_state == SystemState::DISARMED || g_state == SystemState::ALARM) {
    enterState(SystemState::EXITING, millis());
    g_server.send(200, "text/plain", "ARMING - exit delay started");
  } else {
    g_server.send(400, "text/plain", "Cannot arm in current state");
  }
}

void handleDisarm() {
  enterState(SystemState::DISARMED, millis());
  g_server.send(200, "text/plain", "DISARMED");
}

void handleStatus() {
  handleRoot();
}

void startWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(300);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("WiFi connected - IP: ");
    Serial.println(WiFi.localIP());

    g_server.on("/", HTTP_GET, handleRoot);
    g_server.on("/status", HTTP_GET, handleStatus);
    g_server.on("/arm", HTTP_POST, handleArm);
    g_server.on("/disarm", HTTP_POST, handleDisarm);
    g_server.begin();
    Serial.println("HTTP server started on port 80");
  } else {
    Serial.println();
    Serial.println("WiFi failed - continuing in standalone mode");
  }
}

void checkWiFi(unsigned long now) {
  if (now - g_last_wifi_check_ms < WIFI_RECONNECT_MS) return;
  g_last_wifi_check_ms = now;
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi disconnected - reconnecting...");
    WiFi.reconnect();
  }
  g_server.handleClient();
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(500);

  // Pin modes
  pinMode(PIN_PIR, INPUT_PULLDOWN);
  pinMode(PIN_REED, INPUT_PULLUP);   // reed switch: closed = LOW when magnet present
  pinMode(PIN_LED_ARMED, OUTPUT);
  pinMode(PIN_LED_DISARM, OUTPUT);
  pinMode(PIN_LED_ALARM, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);

  digitalWrite(PIN_LED_ARMED, LOW);
  digitalWrite(PIN_LED_DISARM, LOW);
  digitalWrite(PIN_LED_ALARM, LOW);
  digitalWrite(PIN_BUZZER, LOW);

  // LCD
  g_lcd.init();
  g_lcd.backlight();
  g_lcd.clear();
  g_lcd.setCursor(0, 0);
  g_lcd.print("Security System");
  g_lcd.setCursor(0, 1);
  g_lcd.print("Initializing...");
  delay(1500);

  // Serial welcome
  Serial.println("=== ESP32 Home Security System ===");
  Serial.println("Default PIN: 1234 (change in code)");
  Serial.println("Keypad: A=Arm, D=Disarm, #=Enter, *=Clear");

  // WiFi
  startWiFi();

  // Initial state
  enterState(SystemState::DISARMED, millis());
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------
void loop() {
  unsigned long now = millis();

  // WiFi maintenance
  checkWiFi(now);

  // ---- Read sensors ----
  bool pirMotion = digitalRead(PIN_PIR) == HIGH;
  // Reed: magnet present = door closed = HIGH (INPUT_PULLUP). Door open = LOW.
  bool doorOpen = digitalRead(PIN_REED) == LOW;

  // PIR edge detection with cooldown
  if (pirMotion && (now - g_last_pir_ms > PIR_COOLDOWN_MS)) {
    g_last_pir_ms = now;
    if (g_state == SystemState::ARMED) {
      g_sensor_tripped = true;
      enterState(SystemState::ENTRY, now);
    }
  }

  // Reed switch: immediate trip when armed
  if (doorOpen && g_state == SystemState::ARMED) {
    g_sensor_tripped = true;
    enterState(SystemState::ENTRY, now);
  }

  // ---- Keypad ----
  char key = g_keypad.getKey();
  if (key) {
    Serial.print("Key: ");
    Serial.println(key);

    switch (key) {
      case 'A': // Arm
        if (g_state == SystemState::DISARMED || g_state == SystemState::ALARM) {
          enterState(SystemState::EXITING, now);
        }
        break;
      case 'D': // Disarm
        enterState(SystemState::DISARMED, now);
        break;
      case '#': // Enter PIN
        if (g_pin_len > 0) {
          g_pin_buffer[g_pin_len] = '\0';
          if (checkPin(g_pin_buffer)) {
            switch (g_state) {
              case SystemState::DISARMED:
                enterState(SystemState::EXITING, now);
                break;
              case SystemState::EXITING:
                enterState(SystemState::DISARMED, now);
                break;
              case SystemState::ARMED:
                enterState(SystemState::DISARMED, now);
                break;
              case SystemState::ENTRY:
              case SystemState::ALARM:
                enterState(SystemState::DISARMED, now);
                break;
            }
          } else {
            Serial.println("[PIN] Incorrect");
            g_lcd.clear();
            g_lcd.setCursor(0, 0);
            g_lcd.print("Wrong PIN");
            delay(800);
          }
          g_pin_len = 0;
          memset(g_pin_buffer, 0, sizeof(g_pin_buffer));
        }
        break;
      case '*': // Clear
        g_pin_len = 0;
        memset(g_pin_buffer, 0, sizeof(g_pin_buffer));
        break;
      default: // 0-9
        if (g_pin_len < 6) {
          g_pin_buffer[g_pin_len++] = key;
        }
        break;
    }
  }

  // ---- State machine timing ----
  switch (g_state) {
    case SystemState::EXITING:
      if (now - g_state_start_ms >= EXIT_DELAY_MS) {
        enterState(SystemState::ARMED, now);
      }
      break;
    case SystemState::ENTRY:
      if (now - g_state_start_ms >= ENTRY_DELAY_MS) {
        enterState(SystemState::ALARM, now);
      }
      break;
    case SystemState::ALARM:
      if (now - g_state_start_ms >= ALARM_TIMEOUT_MS) {
        // Siren auto-shuts off but system remains "tripped" until PIN disarm
        g_alarm_auto_armed = true;
        digitalWrite(PIN_BUZZER, LOW);
        digitalWrite(PIN_LED_ALARM, LOW);
        // Stay in ALARM state visually; user must enter PIN to fully clear
        Serial.println("[ALARM] Timeout - siren off, awaiting PIN disarm");
      }
      break;
    default:
      break;
  }

  // ---- Outputs ----
  updateBuzzer(g_state, now);
  updateLcd(g_state, now);

  // Small yield for WiFi/keypad stability
  delay(10);
}