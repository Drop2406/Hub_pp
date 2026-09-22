#include "wake_word.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "string.h"
#include "math.h"
#include "esp_heap_caps.h"

// ESP-SR includes for wake word detection 
#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4
#if __has_include("esp_mn_iface.h")
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "model_path.h"
#define ESP_SR_AVAILABLE 1
#else
#define ESP_SR_AVAILABLE 0
#endif
#else
#define ESP_SR_AVAILABLE 0
#endif

static const char* TAG = "WAKE_WORD";

// Wake word system state
typedef struct {
    bool initialized;
    bool enabled;
    bool listening;
    wake_word_config_t config;
    EventGroupHandle_t event_group;
    TaskHandle_t detection_task;
    
    // ESP-SR components 
#if ESP_SR_AVAILABLE
    esp_mn_iface_t* multinet;
    model_iface_data_t* multinet_model_data;
    srmodel_list_t* models;
    char* mn_name;
#endif
    
    // Statistics
    wake_word_stats_t stats;
    char last_detected_word[64];
    
    // Audio processing
    int16_t* audio_buffer;
    size_t audio_buffer_size;
    size_t audio_buffer_pos;
    
} wake_word_context_t;

// Global wake word context
static wake_word_context_t g_wake_word_ctx = {0};

// Forward declarations
static void wake_word_detection_task(void* arg);
static esp_err_t wake_word_init_multinet(void);
static void wake_word_deinit_multinet(void);
static bool wake_word_process_audio_chunk(const int16_t* audio_data, size_t samples);

esp_err_t wake_word_init(const wake_word_config_t* config)
{
    if (g_wake_word_ctx.initialized) {
        ESP_LOGW(TAG, "Wake word system already initialized");
        return ESP_OK;
    }
    
    if (!config) {
        ESP_LOGE(TAG, "Invalid configuration");
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Initializing wake word detection system...");
    ESP_LOGI(TAG, "Keyword: '%s', Display: '%s', Threshold: %.2f", 
             config->keyword, config->display_text, config->threshold);
    
    // Initialize context
    memset(&g_wake_word_ctx, 0, sizeof(wake_word_context_t));
    memcpy(&g_wake_word_ctx.config, config, sizeof(wake_word_config_t));
    
    // Allocate strings
    if (config->keyword) {
        g_wake_word_ctx.config.keyword = strdup(config->keyword);
    }
    if (config->display_text) {
        g_wake_word_ctx.config.display_text = strdup(config->display_text);
    }
    
    // Create event group
    g_wake_word_ctx.event_group = xEventGroupCreate();
    if (!g_wake_word_ctx.event_group) {
        ESP_LOGE(TAG, "Failed to create event group");
        return ESP_ERR_NO_MEM;
    }
    
    // Allocate audio buffer for processing using PSRAM
    g_wake_word_ctx.audio_buffer_size = WAKE_WORD_SAMPLE_RATE / 10; // 100ms buffer
    g_wake_word_ctx.audio_buffer = heap_caps_malloc(
        g_wake_word_ctx.audio_buffer_size * sizeof(int16_t), 
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (!g_wake_word_ctx.audio_buffer) {
        ESP_LOGE(TAG, "Failed to allocate audio buffer");
        vEventGroupDelete(g_wake_word_ctx.event_group);
        return ESP_ERR_NO_MEM;
    }
    
    // ESP-SR will be initialized in detection task to avoid memory conflicts
    ESP_LOGI(TAG, "ESP-SR initialization deferred to detection task");
    
    // Create detection task
    BaseType_t task_ret = xTaskCreatePinnedToCore(
        wake_word_detection_task, 
        "wake_word_detect", 
        8192,  // Increase stack size for ESP-SR 
        NULL, 
        3, 
        &g_wake_word_ctx.detection_task, 
        1
    );
    
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create detection task");
        wake_word_deinit();
        return ESP_ERR_NO_MEM;
    }
    
    g_wake_word_ctx.initialized = true;
    g_wake_word_ctx.enabled = config->enabled;
    
    if (g_wake_word_ctx.enabled) {
        xEventGroupSetBits(g_wake_word_ctx.event_group, WAKE_WORD_BIT_ENABLED);
    }
    
    ESP_LOGI(TAG, "✅ Wake word detection system initialized successfully");
    ESP_LOGI(TAG, "📋 Config: keyword='%s', threshold=%.2f, enabled=%d", 
             config->keyword, config->threshold, config->enabled);
    return ESP_OK;
}

esp_err_t wake_word_deinit(void)
{
    if (!g_wake_word_ctx.initialized) {
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Deinitializing wake word detection system");
    
    g_wake_word_ctx.enabled = false;
    g_wake_word_ctx.listening = false;
    
    // Stop detection task
    if (g_wake_word_ctx.detection_task) {
        vTaskDelete(g_wake_word_ctx.detection_task);
        g_wake_word_ctx.detection_task = NULL;
    }
    
    // Cleanup ESP-SR
    wake_word_deinit_multinet();
    
    // Free audio buffer (PSRAM)
    if (g_wake_word_ctx.audio_buffer) {
        heap_caps_free(g_wake_word_ctx.audio_buffer);
        g_wake_word_ctx.audio_buffer = NULL;
    }
    
    // Free strings
    if (g_wake_word_ctx.config.keyword) {
        free((void*)g_wake_word_ctx.config.keyword);
    }
    if (g_wake_word_ctx.config.display_text) {
        free((void*)g_wake_word_ctx.config.display_text);
    }
    
    // Delete event group
    if (g_wake_word_ctx.event_group) {
        vEventGroupDelete(g_wake_word_ctx.event_group);
        g_wake_word_ctx.event_group = NULL;
    }
    
    g_wake_word_ctx.initialized = false;
    ESP_LOGI(TAG, "Wake word detection system deinitialized");
    return ESP_OK;
}

esp_err_t wake_word_start(void)
{
    if (!g_wake_word_ctx.initialized) {
        ESP_LOGE(TAG, "❌ Cannot start - wake word system not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "🎤 Starting wake word detection for '%s'", 
             g_wake_word_ctx.config.keyword ? g_wake_word_ctx.config.keyword : "unknown");
    g_wake_word_ctx.listening = true;
    xEventGroupSetBits(g_wake_word_ctx.event_group, WAKE_WORD_BIT_LISTENING);
    
    ESP_LOGI(TAG, "✅ Wake word detection started - say '%s' to activate", 
             g_wake_word_ctx.config.display_text ? g_wake_word_ctx.config.display_text : "Hey Tofu");
    
    return ESP_OK;
}

esp_err_t wake_word_stop(void)
{
    if (!g_wake_word_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Stopping wake word detection");
    g_wake_word_ctx.listening = false;
    xEventGroupClearBits(g_wake_word_ctx.event_group, WAKE_WORD_BIT_LISTENING);
    
    return ESP_OK;
}

esp_err_t wake_word_feed_audio(const int16_t* audio_data, size_t data_len)
{
    if (!g_wake_word_ctx.initialized || !g_wake_word_ctx.enabled || !g_wake_word_ctx.listening) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!audio_data || data_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    size_t samples = data_len / sizeof(int16_t);
    
    // Debug: Log audio feed occasionally
    static int feed_count = 0;
    feed_count++;
    if (feed_count % 1000 == 0) {
        ESP_LOGI(TAG, "📡 Audio feed active: %d samples, feed count: %d", (int)samples, feed_count);
    }
    
    // Process audio in chunks
    for (size_t i = 0; i < samples; i++) {
        g_wake_word_ctx.audio_buffer[g_wake_word_ctx.audio_buffer_pos++] = audio_data[i];
        
        // Process when buffer is full
        if (g_wake_word_ctx.audio_buffer_pos >= g_wake_word_ctx.audio_buffer_size) {
            bool detected = wake_word_process_audio_chunk(
                g_wake_word_ctx.audio_buffer, 
                g_wake_word_ctx.audio_buffer_size
            );
            
            if (detected) {
                ESP_LOGI(TAG, "Wake word detected: %s", g_wake_word_ctx.config.display_text);
                
                // Update statistics
                g_wake_word_ctx.stats.total_detections++;
                g_wake_word_ctx.stats.last_detection_time = esp_timer_get_time() / 1000; // Convert to ms
                
                // Store last detected word
                strncpy(g_wake_word_ctx.last_detected_word, 
                       g_wake_word_ctx.config.display_text, 
                       sizeof(g_wake_word_ctx.last_detected_word) - 1);
                
                // Set detection event
                xEventGroupSetBits(g_wake_word_ctx.event_group, WAKE_WORD_BIT_DETECTED);
                
                // Call callback if registered
                if (g_wake_word_ctx.config.callback) {
                    g_wake_word_ctx.config.callback(
                        g_wake_word_ctx.config.display_text, 
                        g_wake_word_ctx.config.user_ctx
                    );
                }
            }
            
            // Reset buffer position
            g_wake_word_ctx.audio_buffer_pos = 0;
        }
    }
    
    return ESP_OK;
}

bool wake_word_is_enabled(void)
{
    return g_wake_word_ctx.initialized && g_wake_word_ctx.enabled;
}

bool wake_word_is_listening(void)
{
    return g_wake_word_ctx.initialized && g_wake_word_ctx.listening;
}

esp_err_t wake_word_get_stats(wake_word_stats_t* stats)
{
    if (!stats) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!g_wake_word_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    memcpy(stats, &g_wake_word_ctx.stats, sizeof(wake_word_stats_t));
    return ESP_OK;
}

esp_err_t wake_word_reset_stats(void)
{
    if (!g_wake_word_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    memset(&g_wake_word_ctx.stats, 0, sizeof(wake_word_stats_t));
    ESP_LOGI(TAG, "Wake word statistics reset");
    return ESP_OK;
}

esp_err_t wake_word_set_threshold(float threshold)
{
    if (threshold < 0.0f || threshold > 1.0f) {
        return ESP_ERR_INVALID_ARG;
    }
    
    g_wake_word_ctx.config.threshold = threshold;
    
#ifdef CONFIG_ESP32_S3_ENABLE
    if (g_wake_word_ctx.multinet && g_wake_word_ctx.multinet_model_data) {
        g_wake_word_ctx.multinet->set_det_threshold(g_wake_word_ctx.multinet_model_data, threshold);
    }
#endif
    
    ESP_LOGI(TAG, "Wake word threshold set to %.2f", threshold);
    return ESP_OK;
}

float wake_word_get_threshold(void)
{
    return g_wake_word_ctx.config.threshold;
}

esp_err_t wake_word_enable(bool enable)
{
    if (!g_wake_word_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    g_wake_word_ctx.enabled = enable;
    
    if (enable) {
        xEventGroupSetBits(g_wake_word_ctx.event_group, WAKE_WORD_BIT_ENABLED);
        ESP_LOGI(TAG, "Wake word detection enabled");
    } else {
        xEventGroupClearBits(g_wake_word_ctx.event_group, WAKE_WORD_BIT_ENABLED);
        ESP_LOGI(TAG, "Wake word detection disabled");
    }
    
    return ESP_OK;
}

const char* wake_word_get_last_detected(void)
{
    if (!g_wake_word_ctx.initialized || strlen(g_wake_word_ctx.last_detected_word) == 0) {
        return NULL;
    }
    
    return g_wake_word_ctx.last_detected_word;
}

esp_err_t wake_word_register_callback(wake_word_callback_t callback, void* user_ctx)
{
    if (!g_wake_word_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    g_wake_word_ctx.config.callback = callback;
    g_wake_word_ctx.config.user_ctx = user_ctx;
    
    ESP_LOGI(TAG, "Wake word callback registered");
    return ESP_OK;
}

wake_word_config_t wake_word_create_default_config(wake_word_callback_t callback, void* user_ctx)
{
    wake_word_config_t config = {
        .keyword = WAKE_WORD_KEYWORD,
        .display_text = WAKE_WORD_DISPLAY_TEXT,
        .threshold = WAKE_WORD_THRESHOLD,
        .duration_ms = WAKE_WORD_DURATION_MS,
        .callback = callback,
        .user_ctx = user_ctx,
        .enabled = true
    };
    
    return config;
}

// Private functions

static void wake_word_detection_task(void* arg)
{
    ESP_LOGI(TAG, "Wake word detection task started");
    
    // Initialize ESP-SR in task context for better memory management
#if ESP_SR_AVAILABLE
    vTaskDelay(pdMS_TO_TICKS(1000)); // Give system time to settle
    ESP_LOGI(TAG, "Initializing ESP-SR from detection task...");
    esp_err_t ret = wake_word_init_multinet();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ESP-SR MultiNet initialization failed, using fallback detection");
    } else {
        ESP_LOGI(TAG, "✅ ESP-SR MultiNet initialized successfully in task context");
    }
#endif
    
    while (g_wake_word_ctx.initialized) {
        // Wait for events or just monitor the system
        EventBits_t bits = xEventGroupWaitBits(
            g_wake_word_ctx.event_group,
            WAKE_WORD_BIT_ENABLED | WAKE_WORD_BIT_DETECTED,
            pdFALSE, // Don't clear bits
            pdFALSE, // Wait for any bit
            pdMS_TO_TICKS(1000) // 1 second timeout
        );
        
        if (bits & WAKE_WORD_BIT_DETECTED) {
            // Clear the detection bit
            xEventGroupClearBits(g_wake_word_ctx.event_group, WAKE_WORD_BIT_DETECTED);
        }
        
        // Periodic health check or maintenance tasks
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    ESP_LOGI(TAG, "Wake word detection task ended");
    vTaskDelete(NULL);
}

#if ESP_SR_AVAILABLE
static esp_err_t wake_word_init_multinet(void)
{
    ESP_LOGI(TAG, "Initializing ESP-SR ...");
    
    // Log memory before ESP-SR initialization
    ESP_LOGI(TAG, "Memory before ESP-SR: Internal=%d, PSRAM=%d", 
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    
    // Step 1: Initialize models using esp_srmodel_init 
    g_wake_word_ctx.models = esp_srmodel_init("model");
    if (g_wake_word_ctx.models == NULL || g_wake_word_ctx.models->num == -1) {
        ESP_LOGW(TAG, "❌ Failed to initialize ESP-SR model");
        ESP_LOGW(TAG, "💡 Check: managed_components/espressif__esp-sr/model/ directory");
        return ESP_FAIL;
    }
    
    // Step 2: Filter models for MultiNet 
    g_wake_word_ctx.mn_name = esp_srmodel_filter(g_wake_word_ctx.models, ESP_MN_PREFIX, NULL);
    if (g_wake_word_ctx.mn_name == NULL) {
        ESP_LOGW(TAG, "❌ Failed to get MultiNet model name for English");
        ESP_LOGW(TAG, "💡 Please ensure English models are selected in menuconfig");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "📋 Using MultiNet model: %s", g_wake_word_ctx.mn_name);
    
    // Step 3: Get MultiNet handle 
    ESP_LOGI(TAG, "🔧 Getting MultiNet handle for: %s", g_wake_word_ctx.mn_name);
    g_wake_word_ctx.multinet = esp_mn_handle_from_name(g_wake_word_ctx.mn_name);
    if (g_wake_word_ctx.multinet == NULL) {
        ESP_LOGW(TAG, "❌ Failed to get MultiNet handle for: %s", g_wake_word_ctx.mn_name);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "✅ MultiNet handle obtained successfully");
    
    // Step 4: Create model data 
    ESP_LOGI(TAG, "🔧 Creating MultiNet model data with duration: %d ms", (int)g_wake_word_ctx.config.duration_ms);
    g_wake_word_ctx.multinet_model_data = g_wake_word_ctx.multinet->create(
        g_wake_word_ctx.mn_name, 
        g_wake_word_ctx.config.duration_ms
    );
    
    if (g_wake_word_ctx.multinet_model_data == NULL) {
        ESP_LOGW(TAG, "❌ Failed to create MultiNet model data");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "✅ MultiNet model data created successfully");
    
    // Step 5: Set detection threshold 
    g_wake_word_ctx.multinet->set_det_threshold(
        g_wake_word_ctx.multinet_model_data, 
        g_wake_word_ctx.config.threshold
    );
    
    // Step 6: Setup commands 
    esp_mn_commands_clear();
    
    // Add wake word command (
    const char* wake_command = g_wake_word_ctx.config.keyword ? g_wake_word_ctx.config.keyword : "hey tofu";
    ESP_LOGI(TAG, "📝 Adding wake command: '%s'", wake_command);
    esp_mn_commands_add(1, wake_command);  // Command ID
    esp_mn_commands_update();
    
    ESP_LOGI(TAG, "✅ ESP-SR MultiNet initialized successfully with command: '%s'", 
             wake_command);
    
    // Log memory after ESP-SR initialization
    ESP_LOGI(TAG, "Memory after ESP-SR: Internal=%d, PSRAM=%d", 
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    
    // Print active commands for debugging
    g_wake_word_ctx.multinet->print_active_speech_commands(g_wake_word_ctx.multinet_model_data);
    
    return ESP_OK;
}
#else
static esp_err_t wake_word_init_multinet(void)
{
    ESP_LOGW(TAG, "ESP-SR not compiled in, using enhanced fallback detection");
    return ESP_ERR_NOT_SUPPORTED;
}
#endif

static void wake_word_deinit_multinet(void)
{
#if ESP_SR_AVAILABLE
    if (g_wake_word_ctx.multinet && g_wake_word_ctx.multinet_model_data) {
        g_wake_word_ctx.multinet->destroy(g_wake_word_ctx.multinet_model_data);
        g_wake_word_ctx.multinet_model_data = NULL;
    }
    
    if (g_wake_word_ctx.models) {
        esp_srmodel_deinit(g_wake_word_ctx.models);
        g_wake_word_ctx.models = NULL;
    }
    
    g_wake_word_ctx.multinet = NULL;
    g_wake_word_ctx.mn_name = NULL;
    
    ESP_LOGI(TAG, "ESP-SR MultiNet deinitialized");
#endif
}

static bool wake_word_process_audio_chunk(const int16_t* audio_data, size_t samples)
{
    if (!audio_data || samples == 0) {
        return false;
    }
    
    g_wake_word_ctx.stats.detection_attempts++;
    
#if ESP_SR_AVAILABLE
    if (g_wake_word_ctx.multinet && g_wake_word_ctx.multinet_model_data) {
        // Use ESP-SR MultiNet for detection 
        esp_mn_state_t mn_state = g_wake_word_ctx.multinet->detect(
            g_wake_word_ctx.multinet_model_data, 
            (int16_t*)audio_data
        );
        
        if (mn_state == ESP_MN_STATE_DETECTING) {
            // Still detecting, continue
            return false;
        } else if (mn_state == ESP_MN_STATE_DETECTED) {
            // Command detected! Get results
            esp_mn_results_t *mn_result = g_wake_word_ctx.multinet->get_results(g_wake_word_ctx.multinet_model_data);
            if (mn_result && mn_result->num > 0) {
                ESP_LOGI(TAG, "🎉 ESP-SR detected: command_id=%d, string=%s, prob=%f", 
                        mn_result->command_id[0], mn_result->string, mn_result->prob[0]);
                
                // Verify it's our wake word command (ID 1)
                if (mn_result->command_id[0] == 1) {
                    ESP_LOGI(TAG, "✅ Wake word 'Hey Tofu' confirmed!");
                    // Clean the state 
                    g_wake_word_ctx.multinet->clean(g_wake_word_ctx.multinet_model_data);
                    return true;
                }
            }
        } else if (mn_state == ESP_MN_STATE_TIMEOUT) {
            ESP_LOGD(TAG, "ESP-SR detection timeout, cleaning state");
            g_wake_word_ctx.multinet->clean(g_wake_word_ctx.multinet_model_data);
        }
    } else {
#endif
        // Fallback: Enhanced audio pattern detection for "Hey Tofu"
        // This is more sophisticated than simple energy detection
        
        // Calculate RMS energy
        int64_t energy_sum = 0;
        for (size_t i = 0; i < samples; i++) {
            energy_sum += (int64_t)audio_data[i] * audio_data[i];
        }
        float rms_energy = sqrt((double)energy_sum / samples);
        
        // Detect speech patterns - "Hey Tofu" has two syllables with pause
        static float energy_history[10] = {0}; // Last 10 chunks
        static int history_index = 0;
        static int speech_pattern_score = 0;
        
        // Update energy history
        energy_history[history_index] = rms_energy;
        history_index = (history_index + 1) % 10;
        
        // Look for speech pattern: quiet -> loud -> quiet -> loud (Hey ... Tofu)
        const float speech_threshold = 1200.0f;  // Balanced - between 1200 and 2000
        const float quiet_threshold = 600.0f;    // Balanced - between 600 and 1000
        
        bool is_speech = rms_energy > speech_threshold;
        bool is_quiet = rms_energy < quiet_threshold;
        
        // Simple state machine for "Hey Tofu" pattern
        static enum { QUIET, HEY, PAUSE, TOFU, DETECTED } pattern_state = QUIET;
        static int state_counter = 0;
        static int debug_counter = 0;
        
        // Debug logging every 100 chunks - reduce log spam
        debug_counter++;
        if (debug_counter % 100 == 0) {
            ESP_LOGD(TAG, "🔊 Audio: RMS=%.1f, speech_thresh=%.1f, state=%d, counter=%d", 
                     rms_energy, speech_threshold, pattern_state, state_counter);
        }
        
        switch (pattern_state) {
            case QUIET:
                if (is_speech) {
                    pattern_state = HEY;
                    state_counter = 0;
                    ESP_LOGD(TAG, "🎤 Speech detected - entering HEY state (RMS: %.1f)", rms_energy);
                }
                break;
                
            case HEY:
                state_counter++;
                if (!is_speech && state_counter > 2) { // End of "Hey" - balanced sensitivity
                    pattern_state = PAUSE;
                    state_counter = 0;
                    ESP_LOGD(TAG, "🔇 End of HEY detected, entering PAUSE state");
                } else if (state_counter > 15) { // More tolerant for "Hey" 
                    pattern_state = QUIET;
                    ESP_LOGD(TAG, "🔄 HEY too long, resetting to QUIET");
                }
                break;
                
            case PAUSE:
                state_counter++;
                if (is_speech) { // Start of "Tofu"
                    pattern_state = TOFU;
                    state_counter = 0;
                    ESP_LOGD(TAG, "🎤 Start of TOFU detected");
                } else if (state_counter > 8) { // More tolerant pause - increased from 5
                    pattern_state = QUIET;
                    ESP_LOGD(TAG, "🔄 Pause too long, resetting to QUIET");
                }
                break;
                
            case TOFU:
                state_counter++;
                if (!is_speech && state_counter > 2) { // End of "Tofu" - balanced sensitivity
                    pattern_state = DETECTED;
                    ESP_LOGD(TAG, "🎉 End of TOFU detected - WAKE WORD TRIGGERED!");
                } else if (state_counter > 15) { // More tolerant for "Tofu"
                    pattern_state = QUIET;
                    ESP_LOGD(TAG, "🔄 TOFU too long, resetting to QUIET");
                }
                break;
                
            case DETECTED:
                pattern_state = QUIET;
                ESP_LOGI(TAG, "Fallback detection: Hey Tofu pattern detected (RMS: %.1f)", rms_energy);
                return true;
        }
        
        // Reset if no activity for too long
        static int no_activity_counter = 0;
        if (rms_energy < quiet_threshold) {
            no_activity_counter++;
            if (no_activity_counter > 20) { // Reset after ~2 seconds of quiet
                pattern_state = QUIET;
                no_activity_counter = 0;
            }
        } else {
            no_activity_counter = 0;
        }
        
#if ESP_SR_AVAILABLE
    }
#endif
    
    return false;
}

