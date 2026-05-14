/**
 * @file espnow_manager.h
 * @brief ESP-NOW communication manager
 */

#ifndef ESPNOW_MANAGER_H
#define ESPNOW_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_now.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief ESP-NOW message types
 */
typedef enum {
    ESPNOW_MSG_PAIRING_REQUEST,    /*!< Pairing request from slave */
    ESPNOW_MSG_PAIRING_RESPONSE,   /*!< Pairing response from master */
    ESPNOW_MSG_READY,              /*!< Slave ready notification */
    ESPNOW_MSG_LED_ON,             /*!< Turn LED on command */
    ESPNOW_MSG_LED_OFF,            /*!< Turn LED off command */
    ESPNOW_MSG_HEARTBEAT,          /*!< Heartbeat message */
} espnow_msg_type_t;

/**
 * @brief ESP-NOW message structure
 */
typedef struct {
    espnow_msg_type_t type;     /*!< Message type */
    uint32_t timestamp;          /*!< Timestamp */
    uint8_t data[200];           /*!< Message payload */
    uint16_t data_len;           /*!< Payload length */
    int8_t rssi;                 /*!< Signal strength (RSSI) */
} espnow_message_t;

/**
 * @brief Receive callback function type
 * 
 * @param src_mac Source MAC address
 * @param msg Received message
 */
typedef void (*espnow_recv_cb_t)(const uint8_t *src_mac, const espnow_message_t *msg);

/**
 * @brief Send callback function type
 * 
 * @param dest_mac Destination MAC address
 * @param status Send status
 */
typedef void (*espnow_send_cb_t)(const uint8_t *dest_mac, esp_now_send_status_t status);

/**
 * @brief ESP-NOW manager configuration
 */
typedef struct {
    uint8_t channel;              /*!< WiFi channel (1-13) */
    espnow_recv_cb_t recv_cb;     /*!< Receive callback */
    espnow_send_cb_t send_cb;     /*!< Send callback */
    bool is_master;               /*!< True for master, false for slave */
} espnow_manager_config_t;

/**
 * @brief Peer information structure
 */
typedef struct {
    uint8_t mac_addr[ESP_NOW_ETH_ALEN];  /*!< MAC address */
    int8_t rssi;                          /*!< Signal strength */
    uint32_t last_seen;                   /*!< Last seen timestamp */
    bool active;                           /*!< Active status */
} espnow_peer_t;

/**
 * @brief Initialize ESP-NOW manager
 * 
 * @param config Configuration
 * @return esp_err_t ESP_OK on success
 */
esp_err_t espnow_manager_init(const espnow_manager_config_t *config);

/**
 * @brief Deinitialize ESP-NOW manager
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t espnow_manager_deinit(void);

/**
 * @brief Send message to specific peer
 * 
 * @param dest_mac Destination MAC address (NULL for broadcast)
 * @param msg Message to send
 * @return esp_err_t ESP_OK on success
 */
esp_err_t espnow_manager_send(const uint8_t *dest_mac, const espnow_message_t *msg);

/**
 * @brief Send message to all paired peers
 * 
 * @param msg Message to send
 * @return esp_err_t ESP_OK on success
 */
esp_err_t espnow_manager_broadcast(const espnow_message_t *msg);

/**
 * @brief Add peer to the list
 * 
 * @param mac_addr Peer MAC address
 * @return esp_err_t ESP_OK on success
 */
esp_err_t espnow_manager_add_peer(const uint8_t *mac_addr);

/**
 * @brief Remove peer from the list
 * 
 * @param mac_addr Peer MAC address
 * @return esp_err_t ESP_OK on success
 */
esp_err_t espnow_manager_remove_peer(const uint8_t *mac_addr);

/**
 * @brief Check if peer exists
 * 
 * @param mac_addr Peer MAC address
 * @return true if peer exists, false otherwise
 */
bool espnow_manager_peer_exists(const uint8_t *mac_addr);

/**
 * @brief Get number of paired peers
 * 
 * @return uint8_t Number of peers
 */
uint8_t espnow_manager_get_peer_count(void);

/**
 * @brief Get peer information by index
 * 
 * @param index Peer index
 * @param peer Output peer information
 * @return esp_err_t ESP_OK on success
 */
esp_err_t espnow_manager_get_peer(uint8_t index, espnow_peer_t *peer);

/**
 * @brief Get own MAC address
 * 
 * @param mac_addr Output MAC address buffer (6 bytes)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t espnow_manager_get_mac(uint8_t *mac_addr);

/**
 * @brief Clear all peers
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t espnow_manager_clear_peers(void);

#ifdef __cplusplus
}
#endif

#endif // ESPNOW_MANAGER_H
