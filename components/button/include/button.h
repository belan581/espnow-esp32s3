/**
 * @file button.h
 * @brief Button handler component for ESP32-S3
 */

#ifndef BUTTON_H
#define BUTTON_H

#include <stdint.h>
#include <stdbool.h>
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Button event types
 */
typedef enum {
    BUTTON_EVENT_PRESSED,       /*!< Button pressed */
    BUTTON_EVENT_RELEASED,      /*!< Button released */
    BUTTON_EVENT_LONG_PRESS,    /*!< Button long press detected */
} button_event_t;

/**
 * @brief Button callback function type
 * 
 * @param gpio_num GPIO number of the button
 * @param event Button event type
 * @param press_duration Duration of button press in milliseconds (for long press)
 */
typedef void (*button_callback_t)(gpio_num_t gpio_num, button_event_t event, uint32_t press_duration);

/**
 * @brief Button configuration structure
 */
typedef struct {
    gpio_num_t gpio_num;              /*!< GPIO number for the button */
    bool active_level;                 /*!< Active level (true for active high, false for active low) */
    uint32_t long_press_time_ms;      /*!< Long press time in milliseconds */
    button_callback_t callback;        /*!< Callback function for button events */
} button_config_t;

/**
 * @brief Initialize button component
 * 
 * @param config Button configuration
 * @return esp_err_t ESP_OK on success
 */
esp_err_t button_init(const button_config_t *config);

/**
 * @brief Deinitialize button component
 * 
 * @param gpio_num GPIO number of the button to deinitialize
 * @return esp_err_t ESP_OK on success
 */
esp_err_t button_deinit(gpio_num_t gpio_num);

/**
 * @brief Get current button state
 * 
 * @param gpio_num GPIO number of the button
 * @return true if button is pressed, false otherwise
 */
bool button_is_pressed(gpio_num_t gpio_num);

#ifdef __cplusplus
}
#endif

#endif // BUTTON_H
