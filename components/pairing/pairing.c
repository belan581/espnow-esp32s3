/**
 * @file pairing.c
 * @brief Pairing manager implementation with NVS persistence
 */

#include "pairing.h"
#include "espnow_manager.h"
#include "led_rgb.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include <string.h>

static const char *TAG = "pairing";

#define PAIRING_TIMEOUT_MS 10000
#define PAIRING_BLINK_PERIOD_MS 300
#define NVS_NAMESPACE "pairing"
#define NVS_KEY_MASTER "master_mac"
#define NVS_KEY_SLAVE_COUNT "slave_cnt"
#define NVS_KEY_SLAVE_PREFIX "slave_"

#define MAX_SLAVES 10

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
    
    // Persistence data
    uint8_t master_mac[6];  // For slave: stores paired master MAC
    bool is_paired;          // True if slave is paired with a master
} pairing_context_t;

static pairing_context_t s_pairing_ctx = {0};

// Forward declarations
static void pairing_timeout_callback(TimerHandle_t xTimer);

// ============================================================================
// NVS Persistence Functions
// ============================================================================

esp_err_t pairing_save_to_nvs(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return ret;
    }
    
    if (s_pairing_ctx.is_master) {
        // Master: Save list of slaves
        uint8_t peer_count = espnow_manager_get_peer_count();
        ret = nvs_set_u8(nvs_handle, NVS_KEY_SLAVE_COUNT, peer_count);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save slave count: %s", esp_err_to_name(ret));
            nvs_close(nvs_handle);
            return ret;
        }
        
        // Save each slave MAC
        for (uint8_t i = 0; i < peer_count && i < MAX_SLAVES; i++) {
            espnow_peer_t peer;
            if (espnow_manager_get_peer(i, &peer) == ESP_OK) {
                char key[16];
                snprintf(key, sizeof(key), "%s%d", NVS_KEY_SLAVE_PREFIX, i);
                ret = nvs_set_blob(nvs_handle, key, peer.mac_addr, 6);
                if (ret != ESP_OK) {
                    ESP_LOGW(TAG, "Failed to save slave %d: %s", i, esp_err_to_name(ret));
                }
            }
        }
        
        ESP_LOGI(TAG, "Saved %d slaves to NVS", peer_count);
    } else {
        // Slave: Save master MAC
        if (s_pairing_ctx.is_paired) {
            ret = nvs_set_blob(nvs_handle, NVS_KEY_MASTER, s_pairing_ctx.master_mac, 6);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to save master MAC: %s", esp_err_to_name(ret));
            } else {
                ESP_LOGI(TAG, "Saved master MAC to NVS: "MACSTR, MAC2STR(s_pairing_ctx.master_mac));
            }
        } else {
            // Erase master MAC if not paired
            nvs_erase_key(nvs_handle, NVS_KEY_MASTER);
        }
    }
    
    ret = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    return ret;
}

esp_err_t pairing_load_from_nvs(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No pairing data in NVS");
        return ESP_OK;
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return ret;
    }
    
    if (s_pairing_ctx.is_master) {
        // Master: Load list of slaves
        uint8_t peer_count = 0;
        ret = nvs_get_u8(nvs_handle, NVS_KEY_SLAVE_COUNT, &peer_count);
        if (ret == ESP_OK && peer_count > 0) {
            ESP_LOGI(TAG, "Loading %d slaves from NVS", peer_count);
            
            for (uint8_t i = 0; i < peer_count && i < MAX_SLAVES; i++) {
                char key[16];
                snprintf(key, sizeof(key), "%s%d", NVS_KEY_SLAVE_PREFIX, i);
                
                uint8_t mac[6];
                size_t mac_len = 6;
                ret = nvs_get_blob(nvs_handle, key, mac, &mac_len);
                if (ret == ESP_OK && mac_len == 6) {
                    espnow_manager_add_peer(mac);
                    ESP_LOGI(TAG, "Loaded slave %d: "MACSTR, i, MAC2STR(mac));
                }
            }
        } else {
            ESP_LOGI(TAG, "No slaves in NVS");
        }
    } else {
        // Slave: Load master MAC
        size_t mac_len = 6;
        ret = nvs_get_blob(nvs_handle, NVS_KEY_MASTER, s_pairing_ctx.master_mac, &mac_len);
        if (ret == ESP_OK && mac_len == 6) {
            s_pairing_ctx.is_paired = true;
            espnow_manager_add_peer(s_pairing_ctx.master_mac);
            ESP_LOGI(TAG, "Loaded master MAC from NVS: "MACSTR, MAC2STR(s_pairing_ctx.master_mac));
        } else {
            s_pairing_ctx.is_paired = false;
            ESP_LOGI(TAG, "No master in NVS");
        }
    }
    
    nvs_close(nvs_handle);
    return ESP_OK;
}

esp_err_t pairing_clear_nvs(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = nvs_erase_all(nvs_handle);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs_handle);
        ESP_LOGI(TAG, "Cleared all pairing data from NVS");
    }
    
    nvs_close(nvs_handle);
    return ret;
}

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
    
    // Save pairing data to NVS
    if (!s_pairing_ctx.is_master) {
        // Slave: Save master MAC
        memcpy(s_pairing_ctx.master_mac, peer_mac, 6);
        s_pairing_ctx.is_paired = true;
    }
    // Master's peers are already added to espnow_manager
    pairing_save_to_nvs();
    
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
    
    if (s_pairing_ctx.is_master) {
        pairing_set_state(PAIRING_STATE_IDLE);
    } else {
        pairing_set_state(PAIRING_STATE_LISTENING);
    }
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
    s_pairing_ctx.is_paired = false;
    memset(s_pairing_ctx.master_mac, 0, 6);
    
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
    
    // Load paired devices from NVS
    pairing_load_from_nvs();
    
    // Slave starts in listening state
    if (!s_pairing_ctx.is_master) {
        pairing_set_state(PAIRING_STATE_LISTENING);
    }
    
    ESP_LOGI(TAG, "Pairing initialized as %s (RSSI threshold: %d dBm, Paired: %s)",
             config->is_master ? "MASTER" : "SLAVE", config->rssi_threshold,
             s_pairing_ctx.is_paired ? "YES" : "NO");
    
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

esp_err_t pairing_slave_start(void)
{
    if (!s_pairing_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (s_pairing_ctx.is_master) {
        ESP_LOGE(TAG, "Not a slave device");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (s_pairing_ctx.state != PAIRING_STATE_LISTENING) {
        ESP_LOGW(TAG, "Pairing already in progress");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Slave starting pairing process");
    
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
        pairing_set_state(PAIRING_STATE_LISTENING);
        return ret;
    }
    
    pairing_start_timeout();
    
    return ESP_OK;
}

esp_err_t pairing_slave_unpair(void)
{
    if (!s_pairing_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (s_pairing_ctx.is_master) {
        ESP_LOGE(TAG, "Not a slave device");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!s_pairing_ctx.is_paired) {
        ESP_LOGW(TAG, "Slave not paired");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Slave unpairing from master "MACSTR, MAC2STR(s_pairing_ctx.master_mac));
    
    // Send unpair message to master
    espnow_message_t msg = {
        .type = ESPNOW_MSG_UNPAIR,
        .timestamp = xTaskGetTickCount() * portTICK_PERIOD_MS,
        .data_len = 0,
    };
    
    espnow_manager_send(s_pairing_ctx.master_mac, &msg);
    
    // Remove master from peer list
    espnow_manager_remove_peer(s_pairing_ctx.master_mac);
    
    // Clear local pairing data
    memset(s_pairing_ctx.master_mac, 0, 6);
    s_pairing_ctx.is_paired = false;
    
    // Save to NVS
    pairing_save_to_nvs();
    
    // LED indication
    led_rgb_set_solid(RGB_COLOR_YELLOW);
    vTaskDelay(pdMS_TO_TICKS(1000));
    led_rgb_clear();
    led_rgb_refresh();
    
    if (s_pairing_ctx.callback) {
        s_pairing_ctx.callback(PAIRING_EVENT_UNPAIRED, s_pairing_ctx.master_mac);
    }
    
    ESP_LOGI(TAG, "Unpaired successfully");
    
    return ESP_OK;
}

bool pairing_is_paired(void)
{
    if (!s_pairing_ctx.initialized) {
        return false;
    }
    
    return s_pairing_ctx.is_paired;
}

esp_err_t pairing_get_master_mac(uint8_t *mac_addr)
{
    if (!s_pairing_ctx.initialized || mac_addr == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (s_pairing_ctx.is_master) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!s_pairing_ctx.is_paired) {
        return ESP_ERR_NOT_FOUND;
    }
    
    memcpy(mac_addr, s_pairing_ctx.master_mac, 6);
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
    
    // MASTER: Handle pairing request (auto-respond to nearby slaves)
    if (s_pairing_ctx.is_master && msg_type == ESPNOW_MSG_PAIRING_REQUEST) {
        ESP_LOGI(TAG, "Received pairing request from slave "MACSTR" (RSSI: %d dBm)",
                 MAC2STR(src_mac), rssi);
        
        // Check RSSI (distance) - only pair if slave is close enough
        if (rssi < s_pairing_ctx.rssi_threshold) {
            ESP_LOGW(TAG, "RSSI too low (%d < %d), slave too far",
                     rssi, s_pairing_ctx.rssi_threshold);
            return ESP_OK;
        }
        
        // Check if already paired
        if (espnow_manager_peer_exists(src_mac)) {
            ESP_LOGI(TAG, "Slave already paired, ignoring request");
            return ESP_OK;
        }
        
        ESP_LOGI(TAG, "RSSI acceptable, auto-responding to slave");
        
        // Add slave as peer
        espnow_manager_add_peer(src_mac);
        
        // Send pairing response
        espnow_message_t response = {
            .type = ESPNOW_MSG_PAIRING_RESPONSE,
            .timestamp = xTaskGetTickCount() * portTICK_PERIOD_MS,
            .data_len = 0,
        };
        espnow_manager_send(src_mac, &response);
        
        // Save to NVS
        pairing_save_to_nvs();
        
        // LED indication
        led_rgb_set_solid(s_pairing_ctx.confirmation_color);
        vTaskDelay(pdMS_TO_TICKS(500));
        led_rgb_clear();
        led_rgb_refresh();
        
        if (s_pairing_ctx.callback) {
            s_pairing_ctx.callback(PAIRING_EVENT_COMPLETE, src_mac);
        }
        
        ESP_LOGI(TAG, "Slave paired successfully: "MACSTR, MAC2STR(src_mac));
    }
    
    // SLAVE: Handle pairing response from master
    else if (!s_pairing_ctx.is_master && msg_type == ESPNOW_MSG_PAIRING_RESPONSE) {
        if (s_pairing_ctx.state != PAIRING_STATE_REQUESTING) {
            ESP_LOGW(TAG, "Slave not in requesting state");
            return ESP_OK;
        }
        
        ESP_LOGI(TAG, "Received pairing response from master "MACSTR, MAC2STR(src_mac));
        
        memcpy(s_pairing_ctx.pending_peer_mac, src_mac, 6);
        s_pairing_ctx.pending_peer_rssi = rssi;
        
        // Add master as peer
        espnow_manager_add_peer(src_mac);
        
        if (s_pairing_ctx.callback) {
            s_pairing_ctx.callback(PAIRING_EVENT_RESPONSE_RECEIVED, src_mac);
        }
        
        // Complete pairing
        pairing_complete_pairing(src_mac);
    }
    
    // MASTER: Handle unpair request from slave
    else if (s_pairing_ctx.is_master && msg_type == ESPNOW_MSG_UNPAIR) {
        ESP_LOGI(TAG, "Received unpair request from slave "MACSTR, MAC2STR(src_mac));
        
        // Remove slave from peer list
        espnow_manager_remove_peer(src_mac);
        
        // Save to NVS
        pairing_save_to_nvs();
        
        // LED indication
        led_rgb_set_solid(RGB_COLOR_YELLOW);
        vTaskDelay(pdMS_TO_TICKS(500));
        led_rgb_clear();
        led_rgb_refresh();
        
        if (s_pairing_ctx.callback) {
            s_pairing_ctx.callback(PAIRING_EVENT_UNPAIRED, src_mac);
        }
        
        ESP_LOGI(TAG, "Slave unpaired: "MACSTR, MAC2STR(src_mac));
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
