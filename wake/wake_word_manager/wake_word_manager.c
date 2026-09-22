#include "wake_word_manager.h"
#include "wake_word.h"
#include "animation_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "string.h"

static const char* TAG = "WAKE_WORD_MGR";

// Manager state
typedef struct {
    bool initialized;
    bool enabled;
    bool listening;
    wake_word_manager_callback_t user_callback;
    void* user_ctx;
    
    // Cooldown management
    int64_t last_detection_time;
    uint32_t cooldown_ms;
    
    // Animation integration
    bool trigger_animation;
    
} wake_word_manager_context_t;

static wake_word_manager_context_t g_manager_ctx = {0};

// Default cooldown between detections (prevent rapid triggers)
#define WAKE_WORD_COOLDOWN_MS 2000

// Forward declarations
static void wake_word_detection_callback(const char* wake_word, void* user_ctx);
static void trigger_wake_word_animation(void);

esp_err_t wake_word_manager_init(wake_word_manager_callback_t callback, void* user_ctx)
{
    if (g_manager_ctx.initialized) {
        ESP_LOGW(TAG, "Wake word manager already initialized");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Initializing wake word manager for 'Hey Tofu'");
    
    // Initialize manager context
    memset(&g_manager_ctx, 0, sizeof(wake_word_manager_context_t));
    g_manager_ctx.user_callback = callback;
    g_manager_ctx.user_ctx = user_ctx;
    g_manager_ctx.cooldown_ms = WAKE_WORD_COOLDOWN_MS;
    g_manager_ctx.trigger_animation = true;
    
    // Create wake word configuration
    wake_word_config_t config = wake_word_create_default_config(
        wake_word_detection_callback, 
        &g_manager_ctx
    );
    
    // Initialize wake word detection system
    esp_err_t ret = wake_word_init(&config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize wake word detection: %s", esp_err_to_name(ret));
        return ret;
    }
    
    g_manager_ctx.initialized = true;
    g_manager_ctx.enabled = true;
    
    ESP_LOGI(TAG, "Wake word manager initialized successfully");
    ESP_LOGI(TAG, "Keyword: '%s', Threshold: %.2f, Cooldown: %lu ms", 
             config.keyword, config.threshold, g_manager_ctx.cooldown_ms);
    
    // Notify user callback
    if (g_manager_ctx.user_callback) {
        g_manager_ctx.user_callback(WAKE_WORD_EVENT_ENABLED, NULL, g_manager_ctx.user_ctx);
    }
    
    return ESP_OK;
}

esp_err_t wake_word_manager_deinit(void)
{
    if (!g_manager_ctx.initialized) {
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Deinitializing wake word manager");
    
    // Stop detection first
    wake_word_manager_stop();
    
    // Deinitialize wake word system
    esp_err_t ret = wake_word_deinit();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Wake word deinit failed: %s", esp_err_to_name(ret));
    }
    
    g_manager_ctx.initialized = false;
    g_manager_ctx.enabled = false;
    g_manager_ctx.listening = false;
    
    ESP_LOGI(TAG, "Wake word manager deinitialized");
    return ESP_OK;
}

esp_err_t wake_word_manager_start(void)
{
    if (!g_manager_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Starting wake word detection");
    
    esp_err_t ret = wake_word_start();
    if (ret == ESP_OK) {
        g_manager_ctx.listening = true;
        ESP_LOGI(TAG, "🎤 Wake word detection started - listening for 'Hey Tofu'");
    } else {
        ESP_LOGE(TAG, "Failed to start wake word detection: %s", esp_err_to_name(ret));
    }
    
    return ret;
}

esp_err_t wake_word_manager_stop(void)
{
    if (!g_manager_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Stopping wake word detection");
    
    esp_err_t ret = wake_word_stop();
    if (ret == ESP_OK) {
        g_manager_ctx.listening = false;
        ESP_LOGI(TAG, "🔇 Wake word detection stopped");
    } else {
        ESP_LOGW(TAG, "Failed to stop wake word detection: %s", esp_err_to_name(ret));
    }
    
    return ret;
}

esp_err_t wake_word_manager_enable(bool enable)
{
    if (!g_manager_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    esp_err_t ret = wake_word_enable(enable);
    if (ret == ESP_OK) {
        g_manager_ctx.enabled = enable;
        
        // Notify user callback
        if (g_manager_ctx.user_callback) {
            wake_word_manager_event_t event = enable ? WAKE_WORD_EVENT_ENABLED : WAKE_WORD_EVENT_DISABLED;
            g_manager_ctx.user_callback(event, NULL, g_manager_ctx.user_ctx);
        }
        
        ESP_LOGI(TAG, "Wake word detection %s", enable ? "enabled" : "disabled");
    }
    
    return ret;
}

bool wake_word_manager_is_enabled(void)
{
    return g_manager_ctx.initialized && g_manager_ctx.enabled;
}

bool wake_word_manager_is_listening(void)
{
    return g_manager_ctx.initialized && g_manager_ctx.listening;
}

esp_err_t wake_word_manager_get_stats(wake_word_stats_t* stats)
{
    if (!g_manager_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    return wake_word_get_stats(stats);
}

esp_err_t wake_word_manager_reset_stats(void)
{
    if (!g_manager_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Resetting wake word statistics");
    return wake_word_reset_stats();
}

esp_err_t wake_word_manager_set_sensitivity(float sensitivity)
{
    if (!g_manager_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (sensitivity < 0.0f || sensitivity > 1.0f) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Convert sensitivity to threshold (inverse relationship)
    float threshold = 1.0f - sensitivity;
    
    esp_err_t ret = wake_word_set_threshold(threshold);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Wake word sensitivity set to %.2f (threshold: %.2f)", sensitivity, threshold);
    }
    
    return ret;
}

float wake_word_manager_get_sensitivity(void)
{
    if (!g_manager_ctx.initialized) {
        return 0.0f;
    }
    
    // Convert threshold back to sensitivity (inverse relationship)
    float threshold = wake_word_get_threshold();
    return 1.0f - threshold;
}

const char* wake_word_manager_get_last_detected(void)
{
    if (!g_manager_ctx.initialized) {
        return NULL;
    }
    
    return wake_word_get_last_detected();
}

// Private functions

static void wake_word_detection_callback(const char* wake_word, void* user_ctx)
{
    wake_word_manager_context_t* ctx = (wake_word_manager_context_t*)user_ctx;
    
    if (!ctx || !wake_word) {
        return;
    }
    
    // Check cooldown period
    int64_t current_time = esp_timer_get_time() / 1000; // Convert to ms
    if (ctx->last_detection_time > 0) {
        int64_t time_since_last = current_time - ctx->last_detection_time;
        if (time_since_last < ctx->cooldown_ms) {
            ESP_LOGD(TAG, "Wake word detection in cooldown (%" PRId64 "ms remaining)", 
                     ctx->cooldown_ms - time_since_last);
            return;
        }
    }
    
    ctx->last_detection_time = current_time;
    
    ESP_LOGI(TAG, "🎉 Wake word detected: '%s'", wake_word);
    
    // Trigger animation if enabled
    if (ctx->trigger_animation) {
        trigger_wake_word_animation();
    }
    
    // Notify user callback
    if (ctx->user_callback) {
        ctx->user_callback(WAKE_WORD_EVENT_DETECTED, wake_word, ctx->user_ctx);
    }
}

static void trigger_wake_word_animation(void)
{
    ESP_LOGI(TAG, "🎬 Triggering wake word animation");
    
    // Trigger excited animation to show wake word was detected
    // esp_err_t ret = animation_start(ANIMATION_TYPE_EXCITED, false);
    // if (ret != ESP_OK) {
    //     ESP_LOGW(TAG, "Failed to trigger wake word animation: %s", esp_err_to_name(ret));
    // }
    
    // Optional: Add audio feedback
    // This could play a brief sound to confirm detection
    
    // Optional: Briefly pause wake word detection to avoid re-triggering
    // during the response animation
    vTaskDelay(pdMS_TO_TICKS(1000)); // 1 second pause
}