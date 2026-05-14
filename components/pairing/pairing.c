/**
 * @file pairing.c
 * @brief Pairing manager implementation
 */

#include "pairing.h"
#include "espnow_manager.h"
#include "led_rgb.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include <string.h>

static const char *TAG = "pairing";

#define PAIRING_TIMEOUT_MS 10000
#define PAIRING_BLINK_PERIOD_MS 300

typedef struct {
    bool initialized;
    bool is_master;
    int8_t rssi_threshold;
    rgb_color_t confirmation_color;
    uint32_t confirmation_duration;
    pairing_callback_t callback;
    
    pairing_state_t state;
    uint8_t pending_peer_mac[6];
    int8_t pending_peer_rssi;
    TimerHandle_t timeout_timer;
} pairing_context_t;

static pairing_context_t s_pairing_ctx = {0};

// Forward declarations
static void pairing_timeout_callback(TimerHandle_t xTimer);

static void pairing_set_state(pairing_state_t new_state)
{
    if (s_pairing_ctx.state != new_state) {
        ESP_LOGI(TAG, "State change: %d -> %d", s_pairing_ctx.state, new_state);
        s_pairing_ctx.state = new_state;
    }
}

static void pairing_start_timeout(void)
{
    if (s_pairing_ctx.timeout_timer) {
        xTimerStart(s_pairing_ctx.timeout_timer, 0);
    }
}

static void pairing_stop_timeout(void)
{
    if (s_pairing_ctx.timeout_timer) {
        xTimerStop(s_pairing_ctx.timeout_timer, 0);
    }
}

static void pairing_timeout_callback(TimerHandle_t xTimer)
{
    ESP_LOGW(TAG, "Pairing timeout");
    pairing_set_state(PAIRING_STATE_FAILED);
    
    if (s_pairing_ctx.callback) {
        s_pairing_ctx.callback(PAIRING_EVENT_FAILED, s_pairing_ctx.pending_peer_mac);
    }
    
    // Reset state
    memset(s_pairing_ctx.pending_peer_mac, 0, 6);
    s_pairing_ctx.pending_peer_rssi = 0;
    pairing_set_state(PAIRING_STATE_IDLE);
}

static void pairing_start_confirmation_led(void)
{
    ESP_LOGI(TAG, "Starting LED confirmation");
    led_rgb_blink(s_pairing_ctx.confirmation_color, PAIRING_BLINK_PERIOD_MS,
                  s_pairing_ctx.confirmation_duration);
}

static void pairing_complete_pairing(const uint8_t *peer_mac)
{
    pairing_stop_timeout();
    pairing_set_state(PAIRING_STATE_COMPLETE);
    
    ESP_LOGI(TAG, "Pairing complete with "MACSTR, MAC2STR(peer_mac));
    
    // Start LED confirmation
    pairing_start_confirmation_led();
    
    if (s_pairing_ctx.callback) {
        s_pairing_ctx.callback(PAIRING_EVENT_COMPLETE, peer_mac);
    }
    
    // Reset to idle after confirmation
    vTaskDelay(pdMS_TO_TICKS(s_pairing_ctx.confirmation_duration + 100));
    
    // Turn off LED after confirmation
    led_rgb_clear();
    led_rgb_refresh();
    
    memset(s_pairing_ctx.pending_peer_mac, 0, 6);
    s_pairing_ctx.pending_peer_rssi = 0;
    pairing_set_state(PAIRING_STATE_IDLE);
}

esp_err_t pairing_init(const pairing_config_t *config)
{
    if (config == NULL) {
        ESP_LOGE(TAG, "Config is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (s_pairing_ctx.initialized) {
        ESP_LOGW(TAG, "Pairing already initialized");
        return ESP_OK;
    }
    
    s_pairing_ctx.is_master = config->is_master;
    s_pairing_ctx.rssi_threshold = config->rssi_threshold;
    s_pairing_ctx.confirmation_color = config->confirmation_color;
    s_pairing_ctx.confirmation_duration = config->confirmation_duration;
    s_pairing_ctx.callback = config->callback;
    s_pairing_ctx.state = PAIRING_STATE_IDLE;
    
    // Create timeout timer
    s_pairing_ctx.timeout_timer = xTimerCreate("pairing_timeout",
                                               pdMS_TO_TICKS(PAIRING_TIMEOUT_MS),
                                               pdFALSE, NULL,
                                               pairing_timeout_callback);
    if (s_pairing_ctx.timeout_timer == NULL) {
        ESP_LOGE(TAG, "Failed to create timeout timer");
        return ESP_ERR_NO_MEM;
    }
    
    s_pairing_ctx.initialized = true;
    
    // Slave starts in listening state
    if (!s_pairing_ctx.is_master) {
        pairing_set_state(PAIRING_STATE_LISTENING);
    }
    
    ESP_LOGI(TAG, "Pairing initialized as %s (RSSI threshold: %d dBm)",
             config->is_master ? "MASTER" : "SLAVE", config->rssi_threshold);
    
    return ESP_OK;
}

esp_err_t pairing_deinit(void)
{
    if (!s_pairing_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    pairing_stop_timeout();
    
    if (s_pairing_ctx.timeout_timer) {
        xTimerDelete(s_pairing_ctx.timeout_timer, 0);
    }
    
    memset(&s_pairing_ctx, 0, sizeof(pairing_context_t));
    
    ESP_LOGI(TAG, "Pairing deinitialized");
    return ESP_OK;
}

esp_err_t pairing_master_start(void)
{
    if (!s_pairing_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!s_pairing_ctx.is_master) {
        ESP_LOGE(TAG, "Not a master device");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (s_pairing_ctx.state != PAIRING_STATE_IDLE) {
        ESP_LOGW(TAG, "Pairing already in progress");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Master starting pairing process");
    
    pairing_set_state(PAIRING_STATE_REQUESTING);
    
    // Send pairing request broadcast
    espnow_message_t msg = {
        .type = ESPNOW_MSG_PAIRING_REQUEST,
        .timestamp = xTaskGetTickCount() * portTICK_PERIOD_MS,
        .data_len = 0,
    };
    
    esp_err_t ret = espnow_manager_send(NULL, &msg); // Broadcast
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send pairing request");
        pairing_set_state(PAIRING_STATE_IDLE);
        return ret;
    }
    
    pairing_start_timeout();
    
    return ESP_OK;
}

esp_err_t pairing_handle_message(const uint8_t *src_mac, uint8_t msg_type,
                                  const uint8_t *data, uint16_t data_len, int8_t rssi)
{
    if (!s_pairing_ctx.initialized || src_mac == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGD(TAG, "Handle message type %d from "MACSTR" (RSSI: %d dBm)",
             msg_type, MAC2STR(src_mac), rssi);
    
    // SLAVE: Handle pairing request
    if (!s_pairing_ctx.is_master && msg_type == ESPNOW_MSG_PAIRING_REQUEST) {
        if (s_pairing_ctx.state != PAIRING_STATE_LISTENING) {
            ESP_LOGW(TAG, "Slave not in listening state");
            return ESP_OK;
        }
        
        ESP_LOGI(TAG, "Received pairing request from "MACSTR" (RSSI: %d dBm)",
                 MAC2STR(src_mac), rssi);
        
        // Check RSSI (distance)
        if (rssi < s_pairing_ctx.rssi_threshold) {
            ESP_LOGW(TAG, "RSSI too low (%d < %d), device too far",
                     rssi, s_pairing_ctx.rssi_threshold);
            return ESP_OK;
        }
        
        ESP_LOGI(TAG, "RSSI acceptable, sending pairing response");
        
        memcpy(s_pairing_ctx.pending_peer_mac, src_mac, 6);
        s_pairing_ctx.pending_peer_rssi = rssi;
        pairing_set_state(PAIRING_STATE_CONFIRMING);
        
        // Add master as peer
        espnow_manager_add_peer(src_mac);
        
        // Send pairing response
        espnow_message_t response = {
            .type = ESPNOW_MSG_PAIRING_RESPONSE,
            .timestamp = xTaskGetTickCount() * portTICK_PERIOD_MS,
            .data_len = 0,
        };
        espnow_manager_send(src_mac, &response);
        
        if (s_pairing_ctx.callback) {
            s_pairing_ctx.callback(PAIRING_EVENT_REQUEST_RECEIVED, src_mac);
        }
        
        // Start confirmation LED
        pairing_start_confirmation_led();
        
        // Send ready message after a short delay
        vTaskDelay(pdMS_TO_TICKS(500));
        espnow_message_t ready_msg = {
            .type = ESPNOW_MSG_READY,
            .timestamp = xTaskGetTickCount() * portTICK_PERIOD_MS,
            .data_len = 0,
        };
        espnow_manager_send(src_mac, &ready_msg);
        
        ESP_LOGI(TAG, "Sent READY message to master");
        
        // Complete pairing
        pairing_complete_pairing(src_mac);
    }
    
    // MASTER: Handle pairing response
    else if (s_pairing_ctx.is_master && msg_type == ESPNOW_MSG_PAIRING_RESPONSE) {
        if (s_pairing_ctx.state != PAIRING_STATE_REQUESTING) {
            ESP_LOGW(TAG, "Master not in requesting state");
            return ESP_OK;
        }
        
        ESP_LOGI(TAG, "Received pairing response from "MACSTR, MAC2STR(src_mac));
        
        memcpy(s_pairing_ctx.pending_peer_mac, src_mac, 6);
        s_pairing_ctx.pending_peer_rssi = rssi;
        pairing_set_state(PAIRING_STATE_CONFIRMING);
        
        // Add slave as peer
        espnow_manager_add_peer(src_mac);
        
        if (s_pairing_ctx.callback) {
            s_pairing_ctx.callback(PAIRING_EVENT_RESPONSE_RECEIVED, src_mac);
        }
    }
    
    // MASTER: Handle ready message
    else if (s_pairing_ctx.is_master && msg_type == ESPNOW_MSG_READY) {
        ESP_LOGI(TAG, "Received READY message from "MACSTR, MAC2STR(src_mac));
        
        if (s_pairing_ctx.callback) {
            s_pairing_ctx.callback(PAIRING_EVENT_READY_RECEIVED, src_mac);
        }
        
        // Complete pairing
        pairing_complete_pairing(src_mac);
    }
    
    return ESP_OK;
}

pairing_state_t pairing_get_state(void)
{
    return s_pairing_ctx.initialized ? s_pairing_ctx.state : PAIRING_STATE_IDLE;
}

bool pairing_is_active(void)
{
    if (!s_pairing_ctx.initialized) {
        return false;
    }
    
    return (s_pairing_ctx.state == PAIRING_STATE_REQUESTING ||
            s_pairing_ctx.state == PAIRING_STATE_CONFIRMING);
}

esp_err_t pairing_cancel(void)
{
    if (!s_pairing_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!pairing_is_active()) {
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Cancelling pairing");
    
    pairing_stop_timeout();
    led_rgb_stop_blink();
    
    memset(s_pairing_ctx.pending_peer_mac, 0, 6);
    s_pairing_ctx.pending_peer_rssi = 0;
    
    if (s_pairing_ctx.is_master) {
        pairing_set_state(PAIRING_STATE_IDLE);
    } else {
        pairing_set_state(PAIRING_STATE_LISTENING);
    }
    
    return ESP_OK;
}
