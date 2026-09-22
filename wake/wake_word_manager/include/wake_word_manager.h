#ifndef WAKE_WORD_MANAGER_H
#define WAKE_WORD_MANAGER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include "esp_err.h"
#include "wake_word.h"

/**
 * @brief Wake word manager events
 */
typedef enum {
    WAKE_WORD_EVENT_DETECTED,       ///< Wake word was detected
    WAKE_WORD_EVENT_ENABLED,        ///< Wake word detection enabled
    WAKE_WORD_EVENT_DISABLED,       ///< Wake word detection disabled
    WAKE_WORD_EVENT_ERROR,          ///< Error occurred
} wake_word_manager_event_t;

/**
 * @brief Wake word manager callback function type
 * @param event Event type
 * @param data Event data (wake word string for DETECTED event)
 * @param user_ctx User context
 */
typedef void (*wake_word_manager_callback_t)(wake_word_manager_event_t event, const char* data, void* user_ctx);

/**
 * @brief Initialize wake word manager
 * @param callback Event callback function
 * @param user_ctx User context for callback
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t wake_word_manager_init(wake_word_manager_callback_t callback, void* user_ctx);

/**
 * @brief Deinitialize wake word manager
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t wake_word_manager_deinit(void);

/**
 * @brief Start wake word detection
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t wake_word_manager_start(void);

/**
 * @brief Stop wake word detection
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t wake_word_manager_stop(void);

/**
 * @brief Enable or disable wake word detection
 * @param enable true to enable, false to disable
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t wake_word_manager_enable(bool enable);

/**
 * @brief Check if wake word detection is enabled
 * @return true if enabled, false otherwise
 */
bool wake_word_manager_is_enabled(void);

/**
 * @brief Check if wake word detection is listening
 * @return true if listening, false otherwise
 */
bool wake_word_manager_is_listening(void);

/**
 * @brief Get wake word detection statistics
 * @param stats Pointer to statistics structure
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t wake_word_manager_get_stats(wake_word_stats_t* stats);

/**
 * @brief Reset wake word detection statistics
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t wake_word_manager_reset_stats(void);

/**
 * @brief Set wake word detection sensitivity
 * @param sensitivity Sensitivity level (0.0-1.0, higher = more sensitive)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t wake_word_manager_set_sensitivity(float sensitivity);

/**
 * @brief Get current wake word detection sensitivity
 * @return Current sensitivity level
 */
float wake_word_manager_get_sensitivity(void);

/**
 * @brief Get the last detected wake word
 * @return Pointer to last detected wake word string, NULL if none
 */
const char* wake_word_manager_get_last_detected(void);

#ifdef __cplusplus
}
#endif

#endif // WAKE_WORD_MANAGER_H