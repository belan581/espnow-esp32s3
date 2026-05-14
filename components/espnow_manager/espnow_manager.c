/**
 * @file espnow_manager.c
 * @brief ESP-NOW communication manager implementation
 */

#include "espnow_manager.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_crc.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>

static const char *TAG = "espnow_mgr";

#define MAX_PEERS 20
#define ESPNOW_QUEUE_SIZE 10
#define ESPNOW_TASK_STACK_SIZE 4096
#define ESPNOW_TASK_PRIORITY 5

// Broadcast MAC address
static const uint8_t s_broadcast_mac[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

typedef struct {
    bool initialized;
    bool is_master;
    uint8_t channel;
    espnow_recv_cb_t recv_cb;
    espnow_send_cb_t send_cb;
    
    espnow_peer_t peers[MAX_PEERS];
    uint8_t peer_count;
    
    QueueHandle_t event_queue;
    TaskHandle_t task_handle;
} espnow_manager_context_t;

typedef enum {
    ESPNOW_EVENT_SEND_CB,
    ESPNOW_EVENT_RECV_CB,
} espnow_event_id_t;

typedef struct {
    espnow_event_id_t id;
    union {
        struct {
            uint8_t mac_addr[ESP_NOW_ETH_ALEN];
            esp_now_send_status_t status;
        } send_cb;
        struct {
            uint8_t mac_addr[ESP_NOW_ETH_ALEN];
            uint8_t *data;
            int data_len;
            int rssi;
        } recv_cb;
    } info;
} espnow_event_t;

static espnow_manager_context_t s_context = {0};

// WiFi initialization
static esp_err_t espnow_manager_wifi_init(uint8_t channel)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE));
    
    ESP_LOGI(TAG, "WiFi initialized on channel %d", channel);
    return ESP_OK;
}

// ESP-NOW send callback
static void espnow_send_cb(const wifi_tx_info_t *tx_info, esp_now_send_status_t status)
{
    if (tx_info == NULL) {
        ESP_LOGE(TAG, "Send callback error: NULL tx_info");
        return;
    }
    
    const uint8_t *mac_addr = tx_info->des_addr;
    if (mac_addr == NULL) {
        ESP_LOGE(TAG, "Send callback error: NULL MAC");
        return;
    }
    
    espnow_event_t evt = {
        .id = ESPNOW_EVENT_SEND_CB,
    };
    memcpy(evt.info.send_cb.mac_addr, mac_addr, ESP_NOW_ETH_ALEN);
    evt.info.send_cb.status = status;
    
    if (xQueueSend(s_context.event_queue, &evt, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Send callback queue full");
    }
}

// ESP-NOW receive callback
static void espnow_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len)
{
    if (recv_info == NULL || data == NULL || len <= 0) {
        ESP_LOGE(TAG, "Receive callback error");
        return;
    }
    
    espnow_event_t evt = {
        .id = ESPNOW_EVENT_RECV_CB,
    };
    memcpy(evt.info.recv_cb.mac_addr, recv_info->src_addr, ESP_NOW_ETH_ALEN);
    evt.info.recv_cb.data = malloc(len);
    if (evt.info.recv_cb.data == NULL) {
        ESP_LOGE(TAG, "Failed to allocate receive buffer");
        return;
    }
    memcpy(evt.info.recv_cb.data, data, len);
    evt.info.recv_cb.data_len = len;
    evt.info.recv_cb.rssi = recv_info->rx_ctrl ? recv_info->rx_ctrl->rssi : 0;
    
    if (xQueueSend(s_context.event_queue, &evt, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Receive callback queue full");
        free(evt.info.recv_cb.data);
    }
}

// ESP-NOW task
static void espnow_manager_task(void *pvParameter)
{
    espnow_event_t evt;
    
    while (1) {
        if (xQueueReceive(s_context.event_queue, &evt, portMAX_DELAY) == pdTRUE) {
            switch (evt.id) {
                case ESPNOW_EVENT_SEND_CB:
                    ESP_LOGD(TAG, "Send to "MACSTR" status: %d",
                            MAC2STR(evt.info.send_cb.mac_addr),
                            evt.info.send_cb.status);
                    
                    if (s_context.send_cb) {
                        s_context.send_cb(evt.info.send_cb.mac_addr, evt.info.send_cb.status);
                    }
                    break;
                    
                case ESPNOW_EVENT_RECV_CB:
                    ESP_LOGD(TAG, "Received from "MACSTR" len: %d, RSSI: %d",
                            MAC2STR(evt.info.recv_cb.mac_addr),
                            evt.info.recv_cb.data_len,
                            evt.info.recv_cb.rssi);
                    
                    // Parse message
                    if (evt.info.recv_cb.data_len >= sizeof(espnow_message_t)) {
                        espnow_message_t *msg = (espnow_message_t *)evt.info.recv_cb.data;
                        
                        // Set RSSI in message
                        msg->rssi = evt.info.recv_cb.rssi;
                        
                        // Update peer RSSI if exists
                        for (int i = 0; i < s_context.peer_count; i++) {
                            if (memcmp(s_context.peers[i].mac_addr, evt.info.recv_cb.mac_addr, ESP_NOW_ETH_ALEN) == 0) {
                                s_context.peers[i].rssi = evt.info.recv_cb.rssi;
                                s_context.peers[i].last_seen = xTaskGetTickCount() * portTICK_PERIOD_MS;
                                break;
                            }
                        }
                        
                        if (s_context.recv_cb) {
                            s_context.recv_cb(evt.info.recv_cb.mac_addr, msg);
                        }
                    }
                    
                    free(evt.info.recv_cb.data);
                    break;
                    
                default:
                    ESP_LOGE(TAG, "Unknown event type: %d", evt.id);
                    break;
            }
        }
    }
}

esp_err_t espnow_manager_init(const espnow_manager_config_t *config)
{
    if (config == NULL) {
        ESP_LOGE(TAG, "Config is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (s_context.initialized) {
        ESP_LOGW(TAG, "ESP-NOW manager already initialized");
        return ESP_OK;
    }
    
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    // Initialize WiFi
    ret = espnow_manager_wifi_init(config->channel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize WiFi");
        return ret;
    }
    
    // Initialize ESP-NOW
    ret = esp_now_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize ESP-NOW: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Register callbacks
    ret = esp_now_register_send_cb(espnow_send_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register send callback");
        esp_now_deinit();
        return ret;
    }
    
    ret = esp_now_register_recv_cb(espnow_recv_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register receive callback");
        esp_now_deinit();
        return ret;
    }
    
    // Add broadcast peer
    esp_now_peer_info_t broadcast_peer = {
        .channel = config->channel,
        .ifidx = WIFI_IF_STA,
        .encrypt = false,
    };
    memcpy(broadcast_peer.peer_addr, s_broadcast_mac, ESP_NOW_ETH_ALEN);
    
    ret = esp_now_add_peer(&broadcast_peer);
    if (ret != ESP_OK && ret != ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGE(TAG, "Failed to add broadcast peer");
        esp_now_deinit();
        return ret;
    }
    
    // Create event queue
    s_context.event_queue = xQueueCreate(ESPNOW_QUEUE_SIZE, sizeof(espnow_event_t));
    if (s_context.event_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create event queue");
        esp_now_deinit();
        return ESP_ERR_NO_MEM;
    }
    
    // Create task
    BaseType_t task_ret = xTaskCreate(espnow_manager_task, "espnow_task",
                                      ESPNOW_TASK_STACK_SIZE, NULL,
                                      ESPNOW_TASK_PRIORITY, &s_context.task_handle);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create ESP-NOW task");
        vQueueDelete(s_context.event_queue);
        esp_now_deinit();
        return ESP_FAIL;
    }
    
    s_context.initialized = true;
    s_context.is_master = config->is_master;
    s_context.channel = config->channel;
    s_context.recv_cb = config->recv_cb;
    s_context.send_cb = config->send_cb;
    s_context.peer_count = 0;
    
    // Get and log MAC address
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    ESP_LOGI(TAG, "ESP-NOW manager initialized as %s, MAC: "MACSTR,
             config->is_master ? "MASTER" : "SLAVE", MAC2STR(mac));
    
    return ESP_OK;
}

esp_err_t espnow_manager_deinit(void)
{
    if (!s_context.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (s_context.task_handle) {
        vTaskDelete(s_context.task_handle);
    }
    
    if (s_context.event_queue) {
        vQueueDelete(s_context.event_queue);
    }
    
    esp_now_deinit();
    esp_wifi_stop();
    esp_wifi_deinit();
    
    memset(&s_context, 0, sizeof(espnow_manager_context_t));
    
    ESP_LOGI(TAG, "ESP-NOW manager deinitialized");
    return ESP_OK;
}

esp_err_t espnow_manager_send(const uint8_t *dest_mac, const espnow_message_t *msg)
{
    if (!s_context.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (msg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    const uint8_t *target_mac = dest_mac ? dest_mac : s_broadcast_mac;
    
    esp_err_t ret = esp_now_send(target_mac, (const uint8_t *)msg, sizeof(espnow_message_t));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send message: %s", esp_err_to_name(ret));
    }
    
    return ret;
}

esp_err_t espnow_manager_broadcast(const espnow_message_t *msg)
{
    if (!s_context.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (s_context.peer_count == 0) {
        ESP_LOGW(TAG, "No peers to broadcast to");
        return ESP_OK;
    }
    
    esp_err_t ret = ESP_OK;
    for (int i = 0; i < s_context.peer_count; i++) {
        if (s_context.peers[i].active) {
            esp_err_t send_ret = espnow_manager_send(s_context.peers[i].mac_addr, msg);
            if (send_ret != ESP_OK) {
                ret = send_ret;
            }
        }
    }
    
    return ret;
}

esp_err_t espnow_manager_add_peer(const uint8_t *mac_addr)
{
    if (!s_context.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (mac_addr == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Check if peer already exists
    if (espnow_manager_peer_exists(mac_addr)) {
        ESP_LOGW(TAG, "Peer "MACSTR" already exists", MAC2STR(mac_addr));
        return ESP_OK;
    }
    
    if (s_context.peer_count >= MAX_PEERS) {
        ESP_LOGE(TAG, "Maximum peers reached");
        return ESP_ERR_NO_MEM;
    }
    
    // Add to ESP-NOW
    esp_now_peer_info_t peer_info = {
        .channel = s_context.channel,
        .ifidx = WIFI_IF_STA,
        .encrypt = false,
    };
    memcpy(peer_info.peer_addr, mac_addr, ESP_NOW_ETH_ALEN);
    
    esp_err_t ret = esp_now_add_peer(&peer_info);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add peer to ESP-NOW: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Add to local list
    memcpy(s_context.peers[s_context.peer_count].mac_addr, mac_addr, ESP_NOW_ETH_ALEN);
    s_context.peers[s_context.peer_count].rssi = 0;
    s_context.peers[s_context.peer_count].last_seen = xTaskGetTickCount() * portTICK_PERIOD_MS;
    s_context.peers[s_context.peer_count].active = true;
    s_context.peer_count++;
    
    ESP_LOGI(TAG, "Peer added: "MACSTR" (total: %d)", MAC2STR(mac_addr), s_context.peer_count);
    return ESP_OK;
}

esp_err_t espnow_manager_remove_peer(const uint8_t *mac_addr)
{
    if (!s_context.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (mac_addr == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Find and remove from local list
    int found_idx = -1;
    for (int i = 0; i < s_context.peer_count; i++) {
        if (memcmp(s_context.peers[i].mac_addr, mac_addr, ESP_NOW_ETH_ALEN) == 0) {
            found_idx = i;
            break;
        }
    }
    
    if (found_idx == -1) {
        ESP_LOGW(TAG, "Peer not found");
        return ESP_ERR_NOT_FOUND;
    }
    
    // Remove from ESP-NOW
    esp_now_del_peer(mac_addr);
    
    // Shift array
    for (int i = found_idx; i < s_context.peer_count - 1; i++) {
        s_context.peers[i] = s_context.peers[i + 1];
    }
    s_context.peer_count--;
    
    ESP_LOGI(TAG, "Peer removed: "MACSTR" (remaining: %d)", MAC2STR(mac_addr), s_context.peer_count);
    return ESP_OK;
}

bool espnow_manager_peer_exists(const uint8_t *mac_addr)
{
    if (!s_context.initialized || mac_addr == NULL) {
        return false;
    }
    
    for (int i = 0; i < s_context.peer_count; i++) {
        if (memcmp(s_context.peers[i].mac_addr, mac_addr, ESP_NOW_ETH_ALEN) == 0) {
            return true;
        }
    }
    
    return false;
}

uint8_t espnow_manager_get_peer_count(void)
{
    return s_context.initialized ? s_context.peer_count : 0;
}

esp_err_t espnow_manager_get_peer(uint8_t index, espnow_peer_t *peer)
{
    if (!s_context.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (peer == NULL || index >= s_context.peer_count) {
        return ESP_ERR_INVALID_ARG;
    }
    
    *peer = s_context.peers[index];
    return ESP_OK;
}

esp_err_t espnow_manager_get_mac(uint8_t *mac_addr)
{
    if (mac_addr == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    return esp_wifi_get_mac(WIFI_IF_STA, mac_addr);
}

esp_err_t espnow_manager_clear_peers(void)
{
    if (!s_context.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    for (int i = 0; i < s_context.peer_count; i++) {
        esp_now_del_peer(s_context.peers[i].mac_addr);
    }
    
    s_context.peer_count = 0;
    ESP_LOGI(TAG, "All peers cleared");
    return ESP_OK;
}
