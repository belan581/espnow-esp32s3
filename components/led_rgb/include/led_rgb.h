/**
 * @file led_rgb.h
 * @brief WS2812B RGB LED controller for ESP32-S3
 */

#ifndef LED_RGB_H
#define LED_RGB_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief RGB color structure
 */
typedef struct {
    uint8_t red;    /*!< Red component (0-255) */
    uint8_t green;  /*!< Green component (0-255) */
    uint8_t blue;   /*!< Blue component (0-255) */
} rgb_color_t;

/**
 * @brief Predefined colors
 */
#define RGB_COLOR_RED      ((rgb_color_t){.red = 255, .green = 0,   .blue = 0})
#define RGB_COLOR_GREEN    ((rgb_color_t){.red = 0,   .green = 255, .blue = 0})
#define RGB_COLOR_BLUE     ((rgb_color_t){.red = 0,   .green = 0,   .blue = 255})
#define RGB_COLOR_YELLOW   ((rgb_color_t){.red = 255, .green = 255, .blue = 0})
#define RGB_COLOR_CYAN     ((rgb_color_t){.red = 0,   .green = 255, .blue = 255})
#define RGB_COLOR_MAGENTA  ((rgb_color_t){.red = 255, .green = 0,   .blue = 255})
#define RGB_COLOR_WHITE    ((rgb_color_t){.red = 255, .green = 255, .blue = 255})
#define RGB_COLOR_ON       ((rgb_color_t){.red = 255, .green = 255, .blue = 255})
#define RGB_COLOR_OFF      ((rgb_color_t){.red = 0,   .green = 0,   .blue = 0})

/**
 * @brief LED RGB configuration structure
 */
typedef struct {
    uint8_t gpio_num;        /*!< GPIO number for LED data line */
    uint16_t led_count;      /*!< Number of LEDs in the strip */
} led_rgb_config_t;

/**
 * @brief Initialize LED RGB controller
 * 
 * @param config LED RGB configuration
 * @return esp_err_t ESP_OK on success
 */
esp_err_t led_rgb_init(const led_rgb_config_t *config);

/**
 * @brief Deinitialize LED RGB controller
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t led_rgb_deinit(void);

/**
 * @brief Set color of a specific LED
 * 
 * @param led_index Index of the LED (0-based)
 * @param color RGB color to set
 * @return esp_err_t ESP_OK on success
 */
esp_err_t led_rgb_set_color(uint16_t led_index, rgb_color_t color);

/**
 * @brief Set color for all LEDs
 * 
 * @param color RGB color to set
 * @return esp_err_t ESP_OK on success
 */
esp_err_t led_rgb_set_all(rgb_color_t color);

/**
 * @brief Turn off all LEDs
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t led_rgb_clear(void);

/**
 * @brief Refresh/update the LED strip with current buffer
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t led_rgb_refresh(void);

/**
 * @brief Start blinking effect with specified color
 * 
 * @param color RGB color to blink
 * @param period_ms Blink period in milliseconds (on + off time)
 * @param duration_ms Total duration of blinking in milliseconds (0 for infinite)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t led_rgb_blink(rgb_color_t color, uint32_t period_ms, uint32_t duration_ms);

/**
 * @brief Stop blinking effect
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t led_rgb_stop_blink(void);

/**
 * @brief Set LED to solid color (stops blinking if active)
 * 
 * @param color RGB color to set
 * @return esp_err_t ESP_OK on success
 */
esp_err_t led_rgb_set_solid(rgb_color_t color);

#ifdef __cplusplus
}
#endif

#endif // LED_RGB_H
