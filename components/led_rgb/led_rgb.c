/**
 * @file led_rgb.c
 * @brief WS2812B RGB LED controller implementation using led_strip
 */

#include "led_rgb.h"
#include "led_strip.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include <string.h>

static const char *TAG = "led_rgb";

typedef struct {
    led_strip_handle_t strip_handle;
    uint16_t led_count;
    rgb_color_t *led_buffer;
    bool initialized;
    
    // Blink control
    TimerHandle_t blink_timer;
    rgb_color_t blink_color;
    bool blink_state;
    bool is_blinking;
    uint32_t blink_duration_ms;
    uint32_t blink_start_time;
} led_rgb_context_t;

static led_rgb_context_t s_led_context = {0};

static void blink_timer_callback(TimerHandle_t xTimer)
{
    if (!s_led_context.is_blinking) {
        return;
    }
    
    // Check duration
    if (s_led_context.blink_duration_ms > 0) {
        uint32_t elapsed = (xTaskGetTickCount() * portTICK_PERIOD_MS) - s_led_context.blink_start_time;
        if (elapsed >= s_led_context.blink_duration_ms) {
            led_rgb_stop_blink();
            led_rgb_clear();
            return;
        }
    }
    
    // Toggle state
    s_led_context.blink_state = !s_led_context.blink_state;
    
    if (s_led_context.blink_state) {
        led_rgb_set_all(s_led_context.blink_color);
    } else {
        led_rgb_clear();
    }
    led_rgb_refresh();
}

esp_err_t led_rgb_init(const led_rgb_config_t *config)
{
    if (config == NULL) {
        ESP_LOGE(TAG, "Config is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (s_led_context.initialized) {
        ESP_LOGW(TAG, "LED RGB already initialized");
        return ESP_OK;
    }
    
    // Allocate LED buffer
    s_led_context.led_buffer = calloc(config->led_count, sizeof(rgb_color_t));
    if (s_led_context.led_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate LED buffer");
        return ESP_ERR_NO_MEM;
    }
    
    s_led_context.led_count = config->led_count;
    
    // Initialize LED strip using ESP-IDF led_strip component
    led_strip_config_t strip_config = {
        .strip_gpio_num = config->gpio_num,
        .max_leds = config->led_count,
    };
    
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000, // 10MHz
        .flags.with_dma = false,
    };
    
    esp_err_t ret = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_led_context.strip_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create LED strip: %s", esp_err_to_name(ret));
        free(s_led_context.led_buffer);
        return ret;
    }
    
    // Create blink timer
    s_led_context.blink_timer = xTimerCreate("blink_timer", pdMS_TO_TICKS(500), pdTRUE, NULL, blink_timer_callback);
    if (s_led_context.blink_timer == NULL) {
        ESP_LOGE(TAG, "Failed to create blink timer");
        led_strip_del(s_led_context.strip_handle);
        free(s_led_context.led_buffer);
        return ESP_ERR_NO_MEM;
    }
    
    s_led_context.initialized = true;
    s_led_context.is_blinking = false;
    
    // Clear LEDs on init
    led_strip_clear(s_led_context.strip_handle);
    
    ESP_LOGI(TAG, "LED RGB initialized on GPIO %d with %d LEDs", config->gpio_num, config->led_count);
    return ESP_OK;
}

esp_err_t led_rgb_deinit(void)
{
    if (!s_led_context.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    led_rgb_stop_blink();
    
    if (s_led_context.blink_timer) {
        xTimerDelete(s_led_context.blink_timer, 0);
    }
    
    led_strip_del(s_led_context.strip_handle);
    free(s_led_context.led_buffer);
    
    memset(&s_led_context, 0, sizeof(led_rgb_context_t));
    
    ESP_LOGI(TAG, "LED RGB deinitialized");
    return ESP_OK;
}

esp_err_t led_rgb_set_color(uint16_t led_index, rgb_color_t color)
{
    if (!s_led_context.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (led_index >= s_led_context.led_count) {
        return ESP_ERR_INVALID_ARG;
    }
    
    s_led_context.led_buffer[led_index] = color;
    
    // Set pixel in led_strip
    return led_strip_set_pixel(s_led_context.strip_handle, led_index, color.red, color.green, color.blue);
}

esp_err_t led_rgb_set_all(rgb_color_t color)
{
    if (!s_led_context.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    for (uint16_t i = 0; i < s_led_context.led_count; i++) {
        s_led_context.led_buffer[i] = color;
        led_strip_set_pixel(s_led_context.strip_handle, i, color.red, color.green, color.blue);
    }
    
    return ESP_OK;
}

esp_err_t led_rgb_clear(void)
{
    if (!s_led_context.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    return led_strip_clear(s_led_context.strip_handle);
}

esp_err_t led_rgb_refresh(void)
{
    if (!s_led_context.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    return led_strip_refresh(s_led_context.strip_handle);
}

esp_err_t led_rgb_blink(rgb_color_t color, uint32_t period_ms, uint32_t duration_ms)
{
    if (!s_led_context.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    led_rgb_stop_blink();
    
    s_led_context.blink_color = color;
    s_led_context.blink_state = false;
    s_led_context.is_blinking = true;
    s_led_context.blink_duration_ms = duration_ms;
    s_led_context.blink_start_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    xTimerChangePeriod(s_led_context.blink_timer, pdMS_TO_TICKS(period_ms / 2), 0);
    xTimerStart(s_led_context.blink_timer, 0);
    
    ESP_LOGI(TAG, "Blink started: period=%lu ms, duration=%lu ms", period_ms, duration_ms);
    return ESP_OK;
}

esp_err_t led_rgb_stop_blink(void)
{
    if (!s_led_context.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (s_led_context.is_blinking) {
        s_led_context.is_blinking = false;
        xTimerStop(s_led_context.blink_timer, 0);
        ESP_LOGI(TAG, "Blink stopped");
    }
    
    return ESP_OK;
}

esp_err_t led_rgb_set_solid(rgb_color_t color)
{
    if (!s_led_context.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    led_rgb_stop_blink();
    led_rgb_set_all(color);
    return led_rgb_refresh();
}
