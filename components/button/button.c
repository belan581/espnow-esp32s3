/**
 * @file button.c
 * @brief Button handler implementation for ESP32-S3
 */

#include "button.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include <string.h>
#include <sys/time.h>

static const char *TAG = "button";

#define MAX_BUTTONS 4
#define DEBOUNCE_TIME_MS 50
#define BUTTON_TASK_STACK_SIZE 4096
#define BUTTON_TASK_PRIORITY 5

typedef struct {
    gpio_num_t gpio_num;
    bool active_level;
    uint32_t long_press_time_ms;
    button_callback_t callback;
    bool is_pressed;
    uint64_t press_start_time;
    bool long_press_triggered;
    bool initialized;
} button_context_t;

static button_context_t button_contexts[MAX_BUTTONS];
static QueueHandle_t button_event_queue = NULL;
static TaskHandle_t button_task_handle = NULL;
static bool isr_service_installed = false;

typedef struct {
    gpio_num_t gpio_num;
    uint32_t level;
    uint64_t timestamp;
} button_queue_event_t;

static void IRAM_ATTR button_isr_handler(void *arg)
{
    gpio_num_t gpio_num = (gpio_num_t)(int)arg;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint64_t timestamp_ms = (tv.tv_sec * 1000ULL) + (tv.tv_usec / 1000ULL);
    
    button_queue_event_t evt = {
        .gpio_num = gpio_num,
        .level = gpio_get_level(gpio_num),
        .timestamp = timestamp_ms
    };
    
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xQueueSendFromISR(button_event_queue, &evt, &xHigherPriorityTaskWoken);
    
    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

static button_context_t* get_button_context(gpio_num_t gpio_num)
{
    for (int i = 0; i < MAX_BUTTONS; i++) {
        if (button_contexts[i].initialized && button_contexts[i].gpio_num == gpio_num) {
            return &button_contexts[i];
        }
    }
    return NULL;
}

static void button_task(void *arg)
{
    button_queue_event_t evt;
    
    while (1) {
        // Use timeout instead of portMAX_DELAY to periodically check for long press
        if (xQueueReceive(button_event_queue, &evt, pdMS_TO_TICKS(100)) == pdTRUE) {
            button_context_t *ctx = get_button_context(evt.gpio_num);
            
            if (ctx == NULL) {
                continue;
            }
            
            // Debounce check
            vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_TIME_MS));
            uint32_t current_level = gpio_get_level(evt.gpio_num);
            
            if (current_level != evt.level) {
                // Bounce detected, ignore
                continue;
            }
            
            // Check if button is pressed or released
            bool is_pressed = (current_level == ctx->active_level);
            
            if (is_pressed && !ctx->is_pressed) {
                // Button pressed
                ctx->is_pressed = true;
                struct timeval tv;
                gettimeofday(&tv, NULL);
                ctx->press_start_time = (tv.tv_sec * 1000ULL) + (tv.tv_usec / 1000ULL);
                ctx->long_press_triggered = false;
                
                if (ctx->callback) {
                    ctx->callback(evt.gpio_num, BUTTON_EVENT_PRESSED, 0);
                }
                
                ESP_LOGD(TAG, "Button GPIO %d pressed", evt.gpio_num);
                
            } else if (!is_pressed && ctx->is_pressed) {
                // Button released
                struct timeval tv;
                gettimeofday(&tv, NULL);
                uint64_t current_time = (tv.tv_sec * 1000ULL) + (tv.tv_usec / 1000ULL);
                uint32_t press_duration = (uint32_t)(current_time - ctx->press_start_time);
                ctx->is_pressed = false;
                
                if (ctx->callback) {
                    ctx->callback(evt.gpio_num, BUTTON_EVENT_RELEASED, press_duration);
                }
                
                ESP_LOGD(TAG, "Button GPIO %d released (duration: %lu ms)", evt.gpio_num, press_duration);
            }
        }
        
        // Check for long press on all buttons
        for (int i = 0; i < MAX_BUTTONS; i++) {
            button_context_t *ctx = &button_contexts[i];
            if (ctx->initialized && ctx->is_pressed && !ctx->long_press_triggered) {
                struct timeval tv;
                gettimeofday(&tv, NULL);
                uint64_t current_time = (tv.tv_sec * 1000ULL) + (tv.tv_usec / 1000ULL);
                uint32_t press_duration = (uint32_t)(current_time - ctx->press_start_time);
                
                if (press_duration >= ctx->long_press_time_ms) {
                    ctx->long_press_triggered = true;
                    
                    if (ctx->callback) {
                        ctx->callback(ctx->gpio_num, BUTTON_EVENT_LONG_PRESS, press_duration);
                    }
                    
                    ESP_LOGI(TAG, "Button GPIO %d long press detected (%lu ms)", ctx->gpio_num, press_duration);
                }
            }
        }
    }
}

esp_err_t button_init(const button_config_t *config)
{
    if (config == NULL) {
        ESP_LOGE(TAG, "Button config is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    // Find free slot
    int slot = -1;
    for (int i = 0; i < MAX_BUTTONS; i++) {
        if (!button_contexts[i].initialized) {
            slot = i;
            break;
        }
    }
    
    if (slot == -1) {
        ESP_LOGE(TAG, "No free button slots available");
        return ESP_ERR_NO_MEM;
    }
    
    // Create queue if not exists
    if (button_event_queue == NULL) {
        button_event_queue = xQueueCreate(10, sizeof(button_queue_event_t));
        if (button_event_queue == NULL) {
            ESP_LOGE(TAG, "Failed to create button event queue");
            return ESP_ERR_NO_MEM;
        }
    }
    
    // Configure GPIO
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << config->gpio_num),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = config->active_level ? GPIO_PULLUP_DISABLE : GPIO_PULLUP_ENABLE,
        .pull_down_en = config->active_level ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE
    };
    
    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure GPIO %d", config->gpio_num);
        return ret;
    }
    
    // Initialize context
    button_contexts[slot].gpio_num = config->gpio_num;
    button_contexts[slot].active_level = config->active_level;
    button_contexts[slot].long_press_time_ms = config->long_press_time_ms;
    button_contexts[slot].callback = config->callback;
    button_contexts[slot].is_pressed = false;
    button_contexts[slot].press_start_time = 0;
    button_contexts[slot].long_press_triggered = false;
    button_contexts[slot].initialized = true;
    
    // Install ISR service (only once)
    if (!isr_service_installed) {
        ret = gpio_install_isr_service(0);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "Failed to install ISR service: %s", esp_err_to_name(ret));
            button_contexts[slot].initialized = false;
            return ret;
        }
        isr_service_installed = true;
    }
    
    // Add ISR handler for this button
    ret = gpio_isr_handler_add(config->gpio_num, button_isr_handler, (void *)(int)config->gpio_num);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add ISR handler for GPIO %d: %s", config->gpio_num, esp_err_to_name(ret));
        button_contexts[slot].initialized = false;
        return ret;
    }
    
    // Create button task if not exists
    if (button_task_handle == NULL) {
        BaseType_t task_ret = xTaskCreate(button_task, "button_task", BUTTON_TASK_STACK_SIZE, 
                                          NULL, BUTTON_TASK_PRIORITY, &button_task_handle);
        if (task_ret != pdPASS) {
            ESP_LOGE(TAG, "Failed to create button task");
            gpio_isr_handler_remove(config->gpio_num);
            button_contexts[slot].initialized = false;
            return ESP_FAIL;
        }
    }
    
    ESP_LOGI(TAG, "Button initialized on GPIO %d", config->gpio_num);
    return ESP_OK;
}

esp_err_t button_deinit(gpio_num_t gpio_num)
{
    button_context_t *ctx = get_button_context(gpio_num);
    
    if (ctx == NULL) {
        ESP_LOGE(TAG, "Button GPIO %d not found", gpio_num);
        return ESP_ERR_NOT_FOUND;
    }
    
    gpio_isr_handler_remove(gpio_num);
    ctx->initialized = false;
    
    // Check if all buttons are deinitialized
    bool all_deinit = true;
    for (int i = 0; i < MAX_BUTTONS; i++) {
        if (button_contexts[i].initialized) {
            all_deinit = false;
            break;
        }
    }
    
    // Delete task and queue if all buttons are deinitialized
    if (all_deinit) {
        if (button_task_handle != NULL) {
            vTaskDelete(button_task_handle);
            button_task_handle = NULL;
        }
        if (button_event_queue != NULL) {
            vQueueDelete(button_event_queue);
            button_event_queue = NULL;
        }
    }
    
    ESP_LOGI(TAG, "Button GPIO %d deinitialized", gpio_num);
    return ESP_OK;
}

bool button_is_pressed(gpio_num_t gpio_num)
{
    button_context_t *ctx = get_button_context(gpio_num);
    
    if (ctx == NULL) {
        return false;
    }
    
    return ctx->is_pressed;
}
