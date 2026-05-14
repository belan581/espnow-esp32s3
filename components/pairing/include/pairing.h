/**
 * @file pairing.h
 * @brief Pairing manager for Master-Slave ESP-NOW communication
 */

#ifndef PAIRING_H
#define PAIRING_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "led_rgb.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Pairing state enum
 */
typedef enum {
    PAIRING_STATE_IDLE,           /*!< Idle, no pairing in progress */
    PAIRING_STATE_LISTENING,      /*!< Slave listening for pairing request */
    PAIRING_STATE_REQUESTING,     /*!< Master requesting pairing */
    PAIRING_STATE_CONFIRMING,     /*!< Pairing confirmation in progress */
    PAIRING_STATE_COMPLETE,       /*!< Pairing complete */
    PAIRING_STATE_FAILED,         /*!< Pairing failed */
} pairing_state_t;

/**
 * @brief Pairing event types
 */
typedef enum {
    PAIRING_EVENT_REQUEST_RECEIVED,  /*!< Pairing request received (slave) */
    PAIRING_EVENT_RESPONSE_RECEIVED, /*!< Pairing response received (master) */
    PAIRING_EVENT_READY_RECEIVED,    /*!< Ready message received (master) */
    PAIRING_EVENT_COMPLETE,          /*!< Pairing complete */
    PAIRING_EVENT_FAILED,            /*!< Pairing failed */
} pairing_event_t;

/**
 * @brief Pairing callback function type
 * 
 * @param event Pairing event
 * @param mac_addr MAC address of the peer involved
 */
typedef void (*pairing_callback_t)(pairing_event_t event, const uint8_t *mac_addr);

/**
 * @brief Pairing configuration
 */
typedef struct {
    bool is_master;                  /*!< True for master, false for slave */
    int8_t rssi_threshold;           /*!< RSSI threshold for distance check (e.g., -30 for ~4cm) */
    rgb_color_t confirmation_color;  /*!< LED color for pairing confirmation */
    uint32_t confirmation_duration;  /*!< LED blink duration in milliseconds */
    pairing_callback_t callback;     /*!< Pairing event callback */
} pairing_config_t;

/**
 * @brief Initialize pairing manager
 * 
 * @param config Pairing configuration
 * @return esp_err_t ESP_OK on success
 */
esp_err_t pairing_init(const pairing_config_t *config);

/**
 * @brief Deinitialize pairing manager
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t pairing_deinit(void);

/**
 * @brief Start pairing process (Master only)
 * 
 * This should be called when the pairing button is pressed on the master
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t pairing_master_start(void);

/**
 * @brief Handle received ESP-NOW message for pairing
 * 
 * This should be called from the ESP-NOW receive callback
 * 
 * @param src_mac Source MAC address
 * @param msg_type Message type
 * @param data Message data
 * @param data_len Data length
 * @param rssi Signal strength
 * @return esp_err_t ESP_OK on success
 */
esp_err_t pairing_handle_message(const uint8_t *src_mac, uint8_t msg_type,
                                  const uint8_t *data, uint16_t data_len, int8_t rssi);

/**
 * @brief Get current pairing state
 * 
 * @return pairing_state_t Current state
 */
pairing_state_t pairing_get_state(void);

/**
 * @brief Check if pairing is in progress
 * 
 * @return true if pairing is in progress, false otherwise
 */
bool pairing_is_active(void);

/**
 * @brief Cancel ongoing pairing
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t pairing_cancel(void);

#ifdef __cplusplus
}
#endif

#endif // PAIRING_H
