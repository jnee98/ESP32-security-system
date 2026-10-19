/*
 * ESP32 Home Security System - ESP-IDF Native Port
 * Converted from Arduino version for ESP32-C6
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "config.h"

static const char *TAG = "SECURITY";

// ============================================================================
// State Machine
// ============================================================================
typedef enum {
    STATE_DISARMED = 0,
    STATE_EXITING  = 1,
    STATE_ARMED    = 2,
    STATE_ENTRY    = 3,
    STATE_ALARM    = 4
} system_state_t;

system_state_t g_state = STATE_DISARMED;
uint64_t g_state_start_ms = 0;

// ============================================================================
// Sensor State
// ============================================================================
typedef struct {
    bool ultrasonic_triggered;
    float ultrasonic_distance_cm;
    bool reed_open;
    bool tripwire_broken;
    uint64_t last_ultrasonic_ms;
    uint64_t last_tripwire_ms;
    uint64_t last_status_print_ms;
    uint8_t reed_stable_count;
    uint8_t ir_stable_count;
    bool last_reed_raw;
    bool last_ir_raw;
} sensor_state_t;

sensor_state_t g_sensors = {0};

bool g_alarm_latched = false;
bool g_alarm_auto_timed_out = false;

// ============================================================================
// NVS
// ============================================================================
#define NVS_NAMESPACE "security"
#define NVS_KEY_STATE "state"
#define NVS_KEY_LATCHED "latched"

// ============================================================================
// Forward Declarations
// ============================================================================
static void enter_state(system_state_t new_state);
static void update_buzzer(uint64_t now_ms);
static void read_ultrasonic(void);
static void read_reed_switch(void);
static void read_tripwire(void);
static void handle_serial_command(char c);
static void print_status(void);
static void save_state_to_nvs(void);
static void load_state_from_nvs(void);
static uint64_t get_time_ms(void);
static void gpio_init(void);
static void ultrasonic_trigger_pulse(void);
static int64_t ultrasonic_read_echo(void);
static void buzzer_pwm_init(void);
static void buzzer_set_duty(uint32_t duty);
static void buzzer_beep(uint32_t on_ms, uint32_t off_ms);

// ============================================================================
// Buzzer PWM
// ============================================================================
#define BUZZER_PWM_FREQ_HZ 2000
#define BUZZER_PWM_RESOLUTION LEDC_TIMER_13_BIT
#define BUZZER_PWM_MAX_DUTY ((1 << 13) - 1)
#define BUZZER_PWM_HALF_DUTY (BUZZER_PWM_MAX_DUTY / 2)

static void buzzer_pwm_init(void) {
    ledc_timer_config_t timer_conf = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = BUZZER_PWM_RESOLUTION,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = BUZZER_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ledc_timer_config(&timer_conf);

    ledc_channel_config_t channel_conf = {
        .gpio_num = PIN_BUZZER,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0
    };
    ledc_channel_config(&channel_conf);
}

static void buzzer_set_duty(uint32_t duty) {
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void buzzer_beep(uint32_t on_ms, uint32_t off_ms) {
    static uint64_t last_toggle = 0;
    static bool beep_on = false;
    uint64_t now = get_time_ms();

    if (beep_on) {
        if (now - last_toggle >= on_ms) {
            buzzer_set_duty(0);
            beep_on = false;
            last_toggle = now;
        }
    } else {
        if (now - last_toggle >= off_ms) {
            buzzer_set_duty(BUZZER_PWM_HALF_DUTY);
            beep_on = true;
            last_toggle = now;
        }
    }
}

// ============================================================================
// Time
// ============================================================================
static uint64_t get_time_ms(void) {
    return esp_timer_get_time() / 1000;
}

// ============================================================================
// GPIO Init
// ============================================================================
static void gpio_init(void) {
    // Ultrasonic
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PIN_ULTRASONIC_TRIG),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
    gpio_set_level(PIN_ULTRASONIC_TRIG, 0);

    io_conf.pin_bit_mask = (1ULL << PIN_ULTRASONIC_ECHO);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&io_conf);

    // Reed switch (INPUT_PULLUP)
    io_conf.pin_bit_mask = (1ULL << PIN_REED_SWITCH);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&io_conf);

    // IR receiver
    io_conf.pin_bit_mask = (1ULL << PIN_IR_RECEIVER);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&io_conf);

    // LEDs
    io_conf.pin_bit_mask = (1ULL << PIN_LED_ARMED) | (1ULL << PIN_LED_ALARM);
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&io_conf);
    gpio_set_level(PIN_LED_ARMED, 0);
    gpio_set_level(PIN_LED_ALARM, 0);

    // Buzzer PWM
    buzzer_pwm_init();
    buzzer_set_duty(0);
}

// ============================================================================
// Ultrasonic
// ============================================================================
static void ultrasonic_trigger_pulse(void) {
    gpio_set_level(PIN_ULTRASONIC_TRIG, 1);
    esp_rom_delay_us(10);
    gpio_set_level(PIN_ULTRASONIC_TRIG, 0);
}

static int64_t ultrasonic_read_echo(void) {
    int64_t start = esp_timer_get_time();
    int64_t timeout = start + 30000; // 30ms timeout (~5m)

    // Wait for HIGH
    while (gpio_get_level(PIN_ULTRASONIC_ECHO) == 0) {
        if (esp_timer_get_time() > timeout) return -1;
    }
    int64_t high_start = esp_timer_get_time();

    // Wait for LOW
    while (gpio_get_level(PIN_ULTRASONIC_ECHO) == 1) {
        if (esp_timer_get_time() > timeout) return -1;
    }
    int64_t high_end = esp_timer_get_time();

    return high_end - high_start; // microseconds
}

static void read_ultrasonic(void) {
    uint64_t now = get_time_ms();
    if (now - g_sensors.last_ultrasonic_ms < ULTRASONIC_CHECK_MS) return;
    g_sensors.last_ultrasonic_ms = now;

    float samples[ULTRASONIC_SAMPLES];
    int valid = 0;

    for (int i = 0; i < ULTRASONIC_SAMPLES; i++) {
        ultrasonic_trigger_pulse();
        int64_t pulse_us = ultrasonic_read_echo();
        if (pulse_us > 0) {
            float dist = (pulse_us * 0.0343f) / 2.0f; // cm
            if (dist <= ULTRASONIC_MAX_CM) {
                samples[valid++] = dist;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (valid > 0) {
        // Median filter
        for (int i = 0; i < valid - 1; i++) {
            for (int j = i + 1; j < valid; j++) {
                if (samples[i] > samples[j]) {
                    float tmp = samples[i];
                    samples[i] = samples[j];
                    samples[j] = tmp;
                }
            }
        }
        g_sensors.ultrasonic_distance_cm = samples[valid / 2];
        g_sensors.ultrasonic_triggered = (g_sensors.ultrasonic_distance_cm < ULTRASONIC_TRIGGER_CM);
    }
}

// ============================================================================
// Reed Switch
// ============================================================================
static void read_reed_switch(void) {
    bool raw = (gpio_get_level(PIN_REED_SWITCH) == 0); // LOW = open (magnet away)

    if (raw != g_sensors.last_reed_raw) {
        g_sensors.reed_stable_count = 0;
        g_sensors.last_reed_raw = raw;
        return;
    }

    if (g_sensors.reed_stable_count < (REED_DEBOUNCE_MS / ULTRASONIC_CHECK_MS)) {
        g_sensors.reed_stable_count++;
        return;
    }

    g_sensors.reed_open = raw;
}

// ============================================================================
// IR Tripwire
// ============================================================================
static void read_tripwire(void) {
    uint64_t now = get_time_ms();
    if (now - g_sensors.last_tripwire_ms < TRIPWIRE_CHECK_MS) return;
    g_sensors.last_tripwire_ms = now;

    bool raw = (gpio_get_level(PIN_IR_RECEIVER) == 1); // HIGH = broken

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
// State Machine
// ============================================================================
static void enter_state(system_state_t new_state) {
    g_state = new_state;
    g_state_start_ms = get_time_ms();
    g_alarm_auto_timed_out = false;

    ESP_LOGI(TAG, "State: %d", new_state);

    switch (new_state) {
        case STATE_DISARMED:
            gpio_set_level(PIN_LED_ARMED, 0);
            gpio_set_level(PIN_LED_ALARM, 0);
            buzzer_set_duty(0);
            g_alarm_latched = false;
            save_state_to_nvs();
            break;

        case STATE_EXITING:
            gpio_set_level(PIN_LED_ARMED, 1);
            gpio_set_level(PIN_LED_ALARM, 0);
            break;

        case STATE_ARMED:
            gpio_set_level(PIN_LED_ARMED, 1);
            gpio_set_level(PIN_LED_ALARM, 0);
            buzzer_set_duty(0);
            break;

        case STATE_ENTRY:
            gpio_set_level(PIN_LED_ALARM, 1);
            break;

        case STATE_ALARM:
            gpio_set_level(PIN_LED_ALARM, 1);
            g_alarm_latched = true;
            save_state_to_nvs();
            break;
    }
}

static void update_buzzer(uint64_t now) {
    uint64_t elapsed = now - g_state_start_ms;

    switch (g_state) {
        case STATE_EXITING:
            buzzer_beep(BEEP_EXITING_ON_MS, BEEP_EXITING_OFF_MS);
            if (elapsed >= EXIT_DELAY_MS) {
                enter_state(STATE_ARMED);
                ESP_LOGI(TAG, "Exit delay complete - ARMED");
            }
            break;

        case STATE_ENTRY:
            buzzer_beep(BEEP_ENTRY_ON_MS, BEEP_ENTRY_OFF_MS);
            if (elapsed >= ENTRY_DELAY_MS) {
                enter_state(STATE_ALARM);
                ESP_LOGI(TAG, "Entry delay expired - ALARM!");
            }
            break;

        case STATE_ALARM:
            if (!g_alarm_auto_timed_out && elapsed >= ALARM_TIMEOUT_MS) {
                buzzer_set_duty(0);
                g_alarm_auto_timed_out = true;
                ESP_LOGI(TAG, "Alarm timeout - siren off, latched");
            } else if (!g_alarm_auto_timed_out) {
                buzzer_beep(BEEP_ALARM_ON_MS, BEEP_ALARM_OFF_MS);
            }
            break;

        default:
            buzzer_set_duty(0);
            break;
    }
}

// ============================================================================
// Serial Commands
// ============================================================================
static void handle_serial_command(char c) {
    switch (c) {
        case 'A':
        case 'a':
            if (g_state == STATE_DISARMED || g_state == STATE_ALARM) {
                enter_state(STATE_EXITING);
                ESP_LOGI(TAG, "[CMD] Arming - exit delay started");
            } else {
                ESP_LOGI(TAG, "[CMD] Can only arm from DISARMED or ALARM");
            }
            break;
        case 'D':
        case 'd':
            enter_state(STATE_DISARMED);
            ESP_LOGI(TAG, "[CMD] Disarmed");
            break;
        case 'S':
        case 's':
            print_status();
            break;
        case 'R':
        case 'r': {
            nvs_handle_t handle;
            esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
            if (err == ESP_OK) {
                nvs_erase_all(handle);
                nvs_commit(handle);
                nvs_close(handle);
                g_alarm_latched = false;
                ESP_LOGI(TAG, "[CMD] NVS cleared - restart to apply");
            }
            break;
        }
        default:
            ESP_LOGI(TAG, "[CMD] Use A/D/S/R");
    }
}

static void print_status(void) {
    const char* state_names[] = {"DISARMED", "EXITING", "ARMED", "ENTRY", "ALARM"};
    ESP_LOGI(TAG, "[STATUS] State: %s | Latched: %s | Uptime: %llus",
             state_names[g_state], g_alarm_latched ? "YES" : "NO", get_time_ms() / 1000);
    ESP_LOGI(TAG, "[STATUS] Ultra: %.1f cm (%s) | Reed: %s | IR: %s",
             g_sensors.ultrasonic_distance_cm,
             g_sensors.ultrasonic_triggered ? "TRIGGER" : "OK",
             g_sensors.reed_open ? "OPEN" : "CLOSED",
             g_sensors.tripwire_broken ? "BROKEN" : "INTACT");
    ESP_LOGI(TAG, "[STATUS] Free heap: %lu bytes", esp_get_free_heap_size());
}

// ============================================================================
// NVS
// ============================================================================
static void save_state_to_nvs(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        nvs_set_u8(handle, NVS_KEY_STATE, (uint8_t)g_state);
        nvs_set_u8(handle, NVS_KEY_LATCHED, g_alarm_latched ? 1 : 0);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

static void load_state_from_nvs(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        uint8_t state = STATE_DISARMED;
        uint8_t latched = 0;
        nvs_get_u8(handle, NVS_KEY_STATE, &state);
        nvs_get_u8(handle, NVS_KEY_LATCHED, &latched);
        g_alarm_latched = (latched != 0);
        nvs_close(handle);

        if (g_alarm_latched && state == STATE_ALARM) {
            enter_state(STATE_ALARM);
            ESP_LOGI(TAG, "Restored ALARM state from NVS");
        } else {
            enter_state(STATE_DISARMED);
        }
    } else {
        enter_state(STATE_DISARMED);
    }
}

// ============================================================================
// UART Task for Serial Commands
// ============================================================================
static void uart_task(void *arg) {
    uint8_t data[128];
    while (1) {
        int len = uart_read_bytes(UART_NUM_0, data, sizeof(data) - 1, pdMS_TO_TICKS(100));
        if (len > 0) {
            data[len] = '\0';
            for (int i = 0; i < len; i++) {
                handle_serial_command(data[i]);
            }
        }
    }
}

// ============================================================================
// Main App
// ============================================================================
void app_main(void) {
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "ESP32 Home Security System - ESP-IDF");
    ESP_LOGI(TAG, "========================================");

    // Init NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Init GPIO
    gpio_init();

    // Load state from NVS
    load_state_from_nvs();

    // Init UART for serial commands
    uart_config_t uart_config = {
        .baud_rate = SERIAL_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };
    uart_param_config(UART_NUM_0, &uart_config);
    uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0);

    // Create UART task
    xTaskCreate(uart_task, "uart_task", 4096, NULL, 5, NULL);

    // Main loop
    while (1) {
        uint64_t now = get_time_ms();

        // Read sensors
        read_ultrasonic();
        read_reed_switch();
        read_tripwire();

        // Check for sensor triggers
        bool any_triggered = g_sensors.ultrasonic_triggered || g_sensors.reed_open || g_sensors.tripwire_broken;

        if (g_state == STATE_ARMED && any_triggered) {
            enter_state(STATE_ENTRY);
            ESP_LOGI(TAG, "Sensor triggered - ENTRY delay started");
        }

        // Update buzzer/LEDs based on state
        update_buzzer(now);

        // Periodic status print
        if (now - g_sensors.last_status_print_ms >= STATUS_PRINT_MS) {
            g_sensors.last_status_print_ms = now;
            print_status();
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
