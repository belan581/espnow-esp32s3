/**
 * @file espnow_example_main.c
 * @brief ESP-NOW Master-Slave LED Control
 * 
 * Master can pair with slaves and control their LEDs via ESP-NOW.
 * - Master: Has pairing button (GPIO1) and control button (GPIO7)
 * - Slave: Listens for pairing and control commands
 * - Both: Have RGB LED on GPIO48 for status indication
 */

#include <stdlib.h>
#include <time.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "nvs_flash.h"

// Custom components
#include "button.h"
#include "led_rgb.h"
#include "espnow_manager.h"
#include "pairing.h"

static const char *TAG = "main";

// LED state
static bool s_led_on = false;

// Forward declarations
static void on_espnow_recv(const uint8_t *src_mac, const espnow_message_t *msg);
static void on_espnow_send(const uint8_t *dest_mac, esp_now_send_status_t status);
static void on_pairing_event(pairing_event_t event, const uint8_t *mac_addr);

#ifdef CONFIG_DEVICE_ROLE_MASTER
static void master_init(void);
static void on_control_button(gpio_num_t gpio_num, button_event_t event, uint32_t press_duration);
#endif

#ifdef CONFIG_DEVICE_ROLE_SLAVE
static void slave_init(void);
static void on_pairing_button(gpio_num_t gpio_num, button_event_t event, uint32_t press_duration);
#endif

/**
 * @brief ESP-NOW receive callback
 */
static void on_espnow_recv(const uint8_t *src_mac, const espnow_message_t *msg)
{
    ESP_LOGI(TAG, "Received message type %d from "MACSTR" (RSSI: %d dBm)", 
             msg->type, MAC2STR(src_mac), msg->rssi);
    
    // Handle pairing messages
    if (msg->type == ESPNOW_MSG_PAIRING_REQUEST ||
        msg->type == ESPNOW_MSG_PAIRING_RESPONSE ||
        msg->type == ESPNOW_MSG_READY ||
        msg->type == ESPNOW_MSG_UNPAIR) {
        
        // Pass RSSI from message
        pairing_handle_message(src_mac, msg->type, msg->data, msg->data_len, msg->rssi);
    }
    
    // Handle LED control messages (Slave only)
#ifdef CONFIG_DEVICE_ROLE_SLAVE
    else if (msg->type == ESPNOW_MSG_LED_ON) {
        ESP_LOGI(TAG, "LED command received - RED");
        s_led_on = true;
        led_rgb_set_solid(RGB_COLOR_RED);
    }
    else if (msg->type == ESPNOW_MSG_LED_OFF) {
        ESP_LOGI(TAG, "LED command received - GREEN");
        s_led_on = false;
        led_rgb_set_solid(RGB_COLOR_GREEN);
    }
#endif
}

/**
 * @brief ESP-NOW send callback
 */
static void on_espnow_send(const uint8_t *dest_mac, esp_now_send_status_t status)
{
    if (status == ESP_NOW_SEND_SUCCESS) {
        ESP_LOGD(TAG, "Send success to "MACSTR, MAC2STR(dest_mac));
    } else {
        ESP_LOGW(TAG, "Send failed to "MACSTR, MAC2STR(dest_mac));
    }
}

/**
 * @brief Pairing event callback
 */
static void on_pairing_event(pairing_event_t event, const uint8_t *mac_addr)
{
    switch (event) {
        case PAIRING_EVENT_REQUEST_RECEIVED:
            ESP_LOGI(TAG, "Pairing request received from "MACSTR, MAC2STR(mac_addr));
            break;
            
        case PAIRING_EVENT_RESPONSE_RECEIVED:
            ESP_LOGI(TAG, "Pairing response received from "MACSTR, MAC2STR(mac_addr));
            break;
            
        case PAIRING_EVENT_READY_RECEIVED:
            ESP_LOGI(TAG, "Slave ready: "MACSTR, MAC2STR(mac_addr));
            break;
            
        case PAIRING_EVENT_COMPLETE:
            ESP_LOGI(TAG, "Pairing complete with "MACSTR, MAC2STR(mac_addr));
            // Reset LED state to off after pairing
            s_led_on = false;
            break;
            
        case PAIRING_EVENT_FAILED:
            ESP_LOGW(TAG, "Pairing failed with "MACSTR, MAC2STR(mac_addr));
            // Ensure LED is off after failed pairing
            s_led_on = false;
            led_rgb_clear();
            led_rgb_refresh();
            break;
            
        case PAIRING_EVENT_UNPAIRED:
            ESP_LOGI(TAG, "Device unpaired: "MACSTR, MAC2STR(mac_addr));
            break;
    }
}

#ifdef CONFIG_DEVICE_ROLE_MASTER

/**
 * @brief Control button callback (Master only)
 */
static void on_control_button(gpio_num_t gpio_num, button_event_t event, uint32_t press_duration)
{
    if (event == BUTTON_EVENT_PRESSED) {
        // Toggle LED state
        s_led_on = !s_led_on;
        
        ESP_LOGI(TAG, "Control button pressed - LED color: %s", s_led_on ? "RED" : "GREEN");
        
        // Update master's own LED
        if (s_led_on) {
            led_rgb_set_solid(RGB_COLOR_RED);
        } else {
            led_rgb_set_solid(RGB_COLOR_GREEN);
        }
        
        // Send command to all slaves
        espnow_message_t msg = {
            .type = s_led_on ? ESPNOW_MSG_LED_ON : ESPNOW_MSG_LED_OFF,
            .timestamp = xTaskGetTickCount() * portTICK_PERIOD_MS,
            .data_len = 0,
        };
        
        uint8_t peer_count = espnow_manager_get_peer_count();
        ESP_LOGI(TAG, "Broadcasting LED command to %d peers", peer_count);
        
        if (peer_count > 0) {
            espnow_manager_broadcast(&msg);
        } else {
            ESP_LOGW(TAG, "No paired slaves");
        }
    }
}

/**
 * @brief Initialize Master device
 */
static void master_init(void)
{
    ESP_LOGI(TAG, "Initializing as MASTER");
    
    // Initialize LED RGB
    led_rgb_config_t led_config = {
        .gpio_num = CONFIG_GPIO_LED_RGB,
        .led_count = 1,
    };
    ESP_ERROR_CHECK(led_rgb_init(&led_config));
    
    // Initialize ESP-NOW Manager
    espnow_manager_config_t espnow_config = {
        .channel = CONFIG_ESPNOW_CHANNEL,
        .recv_cb = on_espnow_recv,
        .send_cb = on_espnow_send,
        .is_master = true,
    };
    ESP_ERROR_CHECK(espnow_manager_init(&espnow_config));
    
    // Initialize Pairing
    pairing_config_t pairing_config = {
        .is_master = true,
        .rssi_threshold = CONFIG_PAIRING_RSSI_THRESHOLD,
        .confirmation_color = RGB_COLOR_BLUE,
        .confirmation_duration = CONFIG_PAIRING_CONFIRMATION_DURATION,
        .callback = on_pairing_event,
    };
    ESP_ERROR_CHECK(pairing_init(&pairing_config));
    
    // Initialize Control Button (GPIO7)
    button_config_t control_btn_config = {
        .gpio_num = CONFIG_GPIO_CONTROL_BUTTON,
        .active_level = false,  // Active low (pull-up)
        .long_press_time_ms = 3000,  // Not used for this button
        .callback = on_control_button,
    };
    ESP_ERROR_CHECK(button_init(&control_btn_config));
    
    // Brief LED indication
    led_rgb_set_solid(RGB_COLOR_MAGENTA);
    vTaskDelay(pdMS_TO_TICKS(1000));
    led_rgb_clear();
    led_rgb_refresh();
    
    ESP_LOGI(TAG, "Master initialized successfully");
    ESP_LOGI(TAG, "- Control button: GPIO%d", CONFIG_GPIO_CONTROL_BUTTON);
    ESP_LOGI(TAG, "- RGB LED: GPIO%d", CONFIG_GPIO_LED_RGB);
    ESP_LOGI(TAG, "- Pairing: Auto-respond to nearby slaves (RSSI > %d dBm)", CONFIG_PAIRING_RSSI_THRESHOLD);
}

#else // CONFIG_DEVICE_ROLE_SLAVE

static void slave_init(void);

/**
 * @brief Pairing button callback (Slave only)
 */
static void on_pairing_button(gpio_num_t gpio_num, button_event_t event, uint32_t press_duration)
{
    ESP_LOGI(TAG, "Pairing button event: %d, duration: %lu ms", event, press_duration);
    
    if (event == BUTTON_EVENT_LONG_PRESS) {
        ESP_LOGI(TAG, "Pairing button long press detected (%lu ms)", press_duration);
        
        if (pairing_is_paired()) {
            // Already paired - unpair from master
            ESP_LOGI(TAG, "Slave is paired, initiating unpair...");
            esp_err_t ret = pairing_slave_unpair();
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "Unpaired successfully");
            } else {
                ESP_LOGE(TAG, "Failed to unpair: %s", esp_err_to_name(ret));
            }
        } else {
            // Not paired - start pairing process
            if (pairing_is_active()) {
                ESP_LOGW(TAG, "Pairing already in progress");
                return;
            }
            
            ESP_LOGI(TAG, "Slave not paired, starting pairing process...");
            esp_err_t ret = pairing_slave_start();
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "Pairing initiated");
            } else {
                ESP_LOGE(TAG, "Failed to start pairing: %s", esp_err_to_name(ret));
            }
        }
    }
}

/**
 * @brief Initialize Slave device
 */
static void slave_init(void)
{
    ESP_LOGI(TAG, "Initializing as SLAVE");
    
    // Initialize LED RGB
    led_rgb_config_t led_config = {
        .gpio_num = CONFIG_GPIO_LED_RGB,
        .led_count = 1,
    };
    ESP_ERROR_CHECK(led_rgb_init(&led_config));
    
    // Initialize ESP-NOW Manager
    espnow_manager_config_t espnow_config = {
        .channel = CONFIG_ESPNOW_CHANNEL,
        .recv_cb = on_espnow_recv,
        .send_cb = on_espnow_send,
        .is_master = false,
    };
    ESP_ERROR_CHECK(espnow_manager_init(&espnow_config));
    
    // Initialize Pairing
    pairing_config_t pairing_config = {
        .is_master = false,
        .rssi_threshold = CONFIG_PAIRING_RSSI_THRESHOLD,
        .confirmation_color = RGB_COLOR_CYAN,
        .confirmation_duration = CONFIG_PAIRING_CONFIRMATION_DURATION,
        .callback = on_pairing_event,
    };
    ESP_ERROR_CHECK(pairing_init(&pairing_config));
    
    // Initialize Pairing Button (GPIO1)
    button_config_t pairing_btn_config = {
        .gpio_num = CONFIG_GPIO_PAIRING_BUTTON,
        .active_level = true,  // Active high (pull-down, button connects to 3.3V)
        .long_press_time_ms = CONFIG_PAIRING_LONG_PRESS_TIME,
        .callback = on_pairing_button,
    };
    ESP_ERROR_CHECK(button_init(&pairing_btn_config));
    
    // Brief LED indication
    led_rgb_set_solid(RGB_COLOR_BLUE);
    vTaskDelay(pdMS_TO_TICKS(1000));
    led_rgb_clear();
    led_rgb_refresh();
    
    ESP_LOGI(TAG, "Slave initialized successfully");
    ESP_LOGI(TAG, "- Pairing button: GPIO%d (long press %d ms)", 
             CONFIG_GPIO_PAIRING_BUTTON, CONFIG_PAIRING_LONG_PRESS_TIME);
    ESP_LOGI(TAG, "- RGB LED: GPIO%d", CONFIG_GPIO_LED_RGB);
    ESP_LOGI(TAG, "- Paired: %s", pairing_is_paired() ? "YES" : "NO");
}

#endif // CONFIG_DEVICE_ROLE_MASTER

void app_main(void)
{
    ESP_LOGI(TAG, "ESP-NOW Master-Slave LED Control");
    ESP_LOGI(TAG, "ESP-IDF Version: %s", esp_get_idf_version());
    
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
#ifdef CONFIG_DEVICE_ROLE_MASTER
    master_init();
#else
    slave_init();
#endif
    
    ESP_LOGI(TAG, "System ready");
}
