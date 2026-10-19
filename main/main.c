/*
 * ESP32 Home Security System - ESP-IDF Native Port
 * Converted from Arduino version for ESP32-C6
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include "config.h"

#if ENABLE_WIFI && MQTT_ENABLED
#include "mqtt_client.h"
#endif

static const char *TAG = "SECURITY";

// State names for JSON serialization
static const char* state_names[] = {"DISARMED", "EXITING", "ARMED", "ENTRY", "ALARM"};

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
    uint64_t last_reed_ms;
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
static bool g_button_last_state = false;  // Track button press state

#if ENABLE_WIFI
// WiFi state
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static int s_wifi_retry_num = 0;
static httpd_handle_t s_web_server = NULL;
#if MQTT_ENABLED
static esp_mqtt_client_handle_t s_mqtt_client = NULL;
#endif
#endif

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
static void update_buzzer(void);
static void read_ultrasonic(void);
static void read_reed_switch(void);
static void read_tripwire(void);
static void read_arm_button(void);
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

#if ENABLE_WIFI
static void wifi_init(void);
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
static void wifi_start_webserver(void);
static void wifi_stop_webserver(void);
static esp_err_t web_get_status(httpd_req_t *req);
static esp_err_t web_post_command(httpd_req_t *req);
static esp_err_t web_get_config(httpd_req_t *req);
static esp_err_t web_post_config(httpd_req_t *req);
static void publish_mqtt_status(void);
#if MQTT_ENABLED
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);
#endif
#endif

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

    // Arm/Disarm Button (INPUT_PULLUP, active LOW)
    io_conf.pin_bit_mask = (1ULL << PIN_ARM_BUTTON);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io_conf);

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
    uint64_t now = get_time_ms();
    if (now - g_sensors.last_reed_ms < REED_CHECK_MS) return;
    g_sensors.last_reed_ms = now;

    bool raw = (gpio_get_level(PIN_REED_SWITCH) == 0); // LOW = open (magnet away)

    if (raw != g_sensors.last_reed_raw) {
        g_sensors.reed_stable_count = 0;
        g_sensors.last_reed_raw = raw;
        return;
    }

    if (g_sensors.reed_stable_count < (REED_DEBOUNCE_MS / REED_CHECK_MS)) {
        g_sensors.reed_stable_count++;
        return;
    }

    g_sensors.reed_open = raw;
}

// ============================================================================
// IR Tripwire
// ============================================================================
static void read_tripwire(void) {
#if IR_TRIPWIRE_ENABLED
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
#else
    // IR tripwire disabled - always report intact
    g_sensors.tripwire_broken = false;
#endif
}

// ============================================================================
// Arm/Disarm Button
// ============================================================================
static void read_arm_button(void) {
    static bool last_raw = true;  // HIGH = not pressed (pullup)
    static uint8_t stable_count = 0;

    bool raw = (gpio_get_level(PIN_ARM_BUTTON) == 0);  // LOW = pressed

    if (raw != last_raw) {
        stable_count = 0;
        last_raw = raw;
        return;
    }

    if (stable_count < 3) {
        stable_count++;
        return;
    }

    // Button pressed (rising edge: was not pressed, now pressed)
    if (raw && !g_button_last_state) {
        g_button_last_state = true;
        if (g_state == STATE_DISARMED) {
            enter_state(STATE_EXITING);
            ESP_LOGI(TAG, "Button: Arming - exit delay started");
        } else {
            enter_state(STATE_DISARMED);
            ESP_LOGI(TAG, "Button: Disarmed");
        }
    }
    // Button released
    else if (!raw && g_button_last_state) {
        g_button_last_state = false;
    }
}
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

static void update_buzzer(void) {
    uint64_t now = get_time_ms();
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

#if ENABLE_WIFI
    // Initialize WiFi
    wifi_init();
#endif

    // Main loop
    while (1) {
        uint64_t now = get_time_ms();

        // Read sensors
        read_ultrasonic();
        read_reed_switch();
        read_tripwire();
        read_arm_button();

        // Check for sensor triggers
        bool any_triggered = g_sensors.ultrasonic_triggered || g_sensors.reed_open || g_sensors.tripwire_broken;

        if (g_state == STATE_ARMED && any_triggered) {
            enter_state(STATE_ENTRY);
            ESP_LOGI(TAG, "Sensor triggered - ENTRY delay started");
        }

        // Update buzzer/LEDs based on state
        update_buzzer();

        // Periodic status print
        if (now - g_sensors.last_status_print_ms >= STATUS_PRINT_MS) {
            g_sensors.last_status_print_ms = now;
            print_status();
        }

#if ENABLE_WIFI
        // Publish MQTT status periodically
        static uint64_t last_mqtt_publish = 0;
        if (now - last_mqtt_publish >= 10000) { // Every 10 seconds
            last_mqtt_publish = now;
            publish_mqtt_status();
        }
#endif

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

#if ENABLE_WIFI
// ============================================================================
// WiFi Implementation
// ============================================================================

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        // Stop web server on disconnect
        wifi_stop_webserver();
        
        if (s_wifi_retry_num < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_wifi_retry_num++;
            ESP_LOGI(TAG, "WiFi retry %d/%d", s_wifi_retry_num, WIFI_MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            ESP_LOGE(TAG, "WiFi connection failed after %d retries", WIFI_MAX_RETRY);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "WiFi connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_wifi_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        
        // Start web server
        wifi_start_webserver();
        
#if MQTT_ENABLED
        // Initialize MQTT
        esp_mqtt_client_config_t mqtt_cfg = {
            .broker.address.uri = MQTT_BROKER_URI,
            .credentials.client_id = "esp32-security",
            .credentials.username = MQTT_USERNAME,
            .credentials.authentication.password = MQTT_PASSWORD,
            .session.keepalive = MQTT_KEEPALIVE_S,
        };
        s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
        esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
        esp_mqtt_client_start(s_mqtt_client);
#endif
    }
}

static void wifi_init(void) {
    s_wifi_event_group = xEventGroupCreate();
    
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip));
    
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    ESP_LOGI(TAG, "WiFi init complete, connecting to %s...", WIFI_SSID);
    
    // Wait for connection
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE, pdFALSE, portMAX_DELAY);
    
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to WiFi");
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Failed to connect to WiFi");
    }
}

static void wifi_start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = WEB_SERVER_PORT;
    config.max_uri_handlers = WEB_SERVER_MAX_URI_HANDLERS;
    
    if (httpd_start(&s_web_server, &config) == ESP_OK) {
        // GET /status - Get system status
        httpd_uri_t status_uri = {
            .uri = "/status",
            .method = HTTP_GET,
            .handler = web_get_status,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(s_web_server, &status_uri);
        
        // POST /command - Send command (arm, disarm, etc.)
        httpd_uri_t command_uri = {
            .uri = "/command",
            .method = HTTP_POST,
            .handler = web_post_command,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(s_web_server, &command_uri);
        
        // GET /config - Get configuration
        httpd_uri_t config_get_uri = {
            .uri = "/config",
            .method = HTTP_GET,
            .handler = web_get_config,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(s_web_server, &config_get_uri);
        
        // POST /config - Update configuration
        httpd_uri_t config_post_uri = {
            .uri = "/config",
            .method = HTTP_POST,
            .handler = web_post_config,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(s_web_server, &config_post_uri);
        
        ESP_LOGI(TAG, "Web server started on port %d", WEB_SERVER_PORT);
    } else {
        ESP_LOGE(TAG, "Failed to start web server");
    }
}

static void wifi_stop_webserver(void) {
    if (s_web_server) {
        httpd_stop(s_web_server);
        s_web_server = NULL;
        ESP_LOGI(TAG, "Web server stopped");
    }
}

static esp_err_t web_get_status(httpd_req_t *req) {
    
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "state", state_names[g_state]);
    cJSON_AddBoolToObject(root, "latched", g_alarm_latched);
    cJSON_AddNumberToObject(root, "uptime_s", get_time_ms() / 1000);
    
    cJSON *sensors = cJSON_CreateObject();
    cJSON_AddNumberToObject(sensors, "ultrasonic_cm", g_sensors.ultrasonic_distance_cm);
    cJSON_AddBoolToObject(sensors, "ultrasonic_triggered", g_sensors.ultrasonic_triggered);
    cJSON_AddBoolToObject(sensors, "reed_open", g_sensors.reed_open);
    cJSON_AddBoolToObject(sensors, "tripwire_broken", g_sensors.tripwire_broken);
    cJSON_AddItemToObject(root, "sensors", sensors);
    
    cJSON *delays = cJSON_CreateObject();
    cJSON_AddNumberToObject(delays, "exit_delay_ms", EXIT_DELAY_MS);
    cJSON_AddNumberToObject(delays, "entry_delay_ms", ENTRY_DELAY_MS);
    cJSON_AddNumberToObject(delays, "alarm_timeout_ms", ALARM_TIMEOUT_MS);
    cJSON_AddItemToObject(root, "delays", delays);
    
    cJSON *wifi = cJSON_CreateObject();
    cJSON_AddStringToObject(wifi, "ssid", WIFI_SSID);
    cJSON_AddNumberToObject(wifi, "rssi", 0);
    cJSON_AddItemToObject(root, "wifi", wifi);
    
    char *json_str = cJSON_Print(root);
    cJSON_Delete(root);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);
    
    return ESP_OK;
}

static esp_err_t web_post_command(httpd_req_t *req) {
    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        return ESP_FAIL;
    }
    buf[ret] = '\0';
    
    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    
    cJSON *cmd = cJSON_GetObjectItem(root, "command");
    if (!cJSON_IsString(cmd)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing command");
        return ESP_FAIL;
    }
    
    bool success = false;
    const char *cmd_str = cmd->valuestring;
    
    if (strcmp(cmd_str, "arm") == 0) {
        if (g_state == STATE_DISARMED || g_state == STATE_ALARM) {
            enter_state(STATE_EXITING);
            success = true;
        }
    } else if (strcmp(cmd_str, "disarm") == 0) {
        enter_state(STATE_DISARMED);
        success = true;
    } else if (strcmp(cmd_str, "reset_alarm") == 0) {
        g_alarm_latched = false;
        if (g_state == STATE_ALARM) {
            enter_state(STATE_DISARMED);
        }
        success = true;
    } else if (strcmp(cmd_str, "status") == 0) {
        success = true;
    }
    
    cJSON_Delete(root);
    
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", success);
    cJSON_AddStringToObject(response, "state", state_names[g_state]);
    char *json_str = cJSON_Print(response);
    cJSON_Delete(response);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);
    
    return ESP_OK;
}

static esp_err_t web_get_config(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "exit_delay_ms", EXIT_DELAY_MS);
    cJSON_AddNumberToObject(root, "entry_delay_ms", ENTRY_DELAY_MS);
    cJSON_AddNumberToObject(root, "alarm_timeout_ms", ALARM_TIMEOUT_MS);
    cJSON_AddNumberToObject(root, "ultrasonic_trigger_cm", ULTRASONIC_TRIGGER_CM);
    cJSON_AddBoolToObject(root, "ir_tripwire_enabled", IR_TRIPWIRE_ENABLED);
    
    char *json_str = cJSON_Print(root);
    cJSON_Delete(root);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);
    
    return ESP_OK;
}

static esp_err_t web_post_config(httpd_req_t *req) {
    // Config changes require recompile in this version
    // Could be extended to save to NVS
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", false);
    cJSON_AddStringToObject(response, "error", "Config changes require recompile");
    char *json_str = cJSON_Print(response);
    cJSON_Delete(response);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);
    
    return ESP_OK;
}

static void publish_mqtt_status(void) {
#if MQTT_ENABLED
    if (!s_mqtt_client) return;
    
    const char* state_names[] = {"DISARMED", "EXITING", "ARMED", "ENTRY", "ALARM"};
    
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "state", state_names[g_state]);
    cJSON_AddBoolToObject(root, "latched", g_alarm_latched);
    cJSON_AddNumberToObject(root, "uptime_s", get_time_ms() / 1000);
    cJSON_AddNumberToObject(root, "ultrasonic_cm", g_sensors.ultrasonic_distance_cm);
    cJSON_AddBoolToObject(root, "ultrasonic_triggered", g_sensors.ultrasonic_triggered);
    cJSON_AddBoolToObject(root, "reed_open", g_sensors.reed_open);
    cJSON_AddBoolToObject(root, "tripwire_broken", g_sensors.tripwire_broken);
    
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    
    esp_mqtt_client_publish(s_mqtt_client, MQTT_TOPIC_STATUS, json_str, 0, MQTT_QOS, 0);
    free(json_str);
#endif
}

#if MQTT_ENABLED
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t) event_data;
    
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT connected");
            esp_mqtt_client_subscribe(s_mqtt_client, MQTT_TOPIC_COMMAND, MQTT_QOS);
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT disconnected");
            break;
        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "MQTT subscribed, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_DATA:
            // Handle incoming commands
            if (strncmp(event->topic, MQTT_TOPIC_COMMAND, event->topic_len) == 0) {
                char *data = strndup(event->data, event->data_len);
                cJSON *root = cJSON_Parse(data);
                if (root) {
                    cJSON *cmd = cJSON_GetObjectItem(root, "command");
                    if (cJSON_IsString(cmd)) {
                        // Reuse command logic
                        if (strcmp(cmd->valuestring, "arm") == 0 && (g_state == STATE_DISARMED || g_state == STATE_ALARM)) {
                            enter_state(STATE_EXITING);
                        } else if (strcmp(cmd->valuestring, "disarm") == 0) {
                            enter_state(STATE_DISARMED);
                        }
                    }
                    cJSON_Delete(root);
                }
                free(data);
            }
            break;
        default:
            break;
    }
}
#endif
#endif
