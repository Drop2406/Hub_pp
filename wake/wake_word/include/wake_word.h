#ifndef WAKE_WORD_H
#define WAKE_WORD_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

// Wake word configuration
#define WAKE_WORD_KEYWORD           "hey tofu"
#define WAKE_WORD_DISPLAY_TEXT      "Hey Tofu"
#define WAKE_WORD_THRESHOLD         0.10f      // Detection threshold (0.0-1.0)
#define WAKE_WORD_DURATION_MS       3000       // Maximum phrase duration in ms
#define WAKE_WORD_SAMPLE_RATE       16000      // Expected audio sample rate
#define WAKE_WORD_CHANNELS          1          // Mono audio
#define WAKE_WORD_BITS_PER_SAMPLE   16         // 16-bit audio

// Event bits for wake word system
#define WAKE_WORD_BIT_ENABLED       BIT0
#define WAKE_WORD_BIT_DETECTED      BIT1
#define WAKE_WORD_BIT_LISTENING     BIT2

// Wake word callback function type
typedef void (*wake_word_callback_t)(const char* wake_word, void* user_ctx);

// Wake word statistics
typedef struct {
    uint32_t total_detections;
    uint32_t false_positives;
    uint32_t detection_attempts;
    float average_confidence;
    uint32_t last_detection_time;
} wake_word_stats_t;

// Wake word configuration structure
typedef struct {
    const char* keyword;
    const char* display_text;
    float threshold;
    uint32_t duration_ms;
    wake_word_callback_t callback;
    void* user_ctx;
    bool enabled;
} wake_word_config_t;

/**
 * @brief Initialize wake word detection system
 * @param config Wake word configuration
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t wake_word_init(const wake_word_config_t* config);

/**
 * @brief Deinitialize wake word detection system
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t wake_word_deinit(void);

/**
 * @brief Start wake word detection
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t wake_word_start(void);

/**
 * @brief Stop wake word detection
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t wake_word_stop(void);

/**
 * @brief Feed audio data to wake word detection
 * @param audio_data Pointer to audio data (16-bit mono PCM)
 * @param data_len Length of audio data in bytes
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t wake_word_feed_audio(const int16_t* audio_data, size_t data_len);

/**
 * @brief Check if wake word system is enabled
 * @return true if enabled, false otherwise
 */
bool wake_word_is_enabled(void);

/**
 * @brief Check if wake word is currently listening
 * @return true if listening, false otherwise
 */
bool wake_word_is_listening(void);

/**
 * @brief Get wake word detection statistics
 * @param stats Pointer to statistics structure
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t wake_word_get_stats(wake_word_stats_t* stats);

/**
 * @brief Reset wake word detection statistics
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t wake_word_reset_stats(void);

/**
 * @brief Set wake word detection threshold
 * @param threshold New threshold value (0.0-1.0)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t wake_word_set_threshold(float threshold);

/**
 * @brief Get current wake word detection threshold
 * @return Current threshold value
 */
float wake_word_get_threshold(void);

/**
 * @brief Enable or disable wake word detection
 * @param enable true to enable, false to disable
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t wake_word_enable(bool enable);

/**
 * @brief Get the last detected wake word
 * @return Pointer to last detected wake word string, NULL if none
 */
const char* wake_word_get_last_detected(void);

/**
 * @brief Register a callback for wake word detection events
 * @param callback Callback function
 * @param user_ctx User context to pass to callback
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t wake_word_register_callback(wake_word_callback_t callback, void* user_ctx);

/**
 * @brief Create default wake word configuration for "hey Tofu"
 * @param callback Callback function for detection events
 * @param user_ctx User context for callback
 * @return Default configuration structure
 */
wake_word_config_t wake_word_create_default_config(wake_word_callback_t callback, void* user_ctx);

#ifdef __cplusplus
}
#endif

#endif // WAKE_WORD_H