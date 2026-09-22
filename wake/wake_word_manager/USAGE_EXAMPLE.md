# Wake Word Detection Usage Example

This document shows how to integrate the "Hey Tofu" wake word detection system into your application.

## Basic Integration

### 1. Include Headers
```c
#include "wake_word_manager.h"
#include "realtime.h"
```

### 2. Define Event Handler
```c
static void my_wake_word_event_handler(wake_word_manager_event_t event, const char* data, void* user_ctx)
{
    switch (event) {
        case WAKE_WORD_EVENT_DETECTED:
            ESP_LOGI("MY_APP", "🎉 Hey Tofu detected: '%s'", data);
            
            // Example: Trigger a response
            // - Start TTS response: "Yes, I'm listening!"
            // - Change LED color to blue
            // - Start recording for command processing
            // - Trigger excited animation
            
            break;
            
        case WAKE_WORD_EVENT_ENABLED:
            ESP_LOGI("MY_APP", "✅ Wake word detection enabled");
            break;
            
        case WAKE_WORD_EVENT_DISABLED:
            ESP_LOGI("MY_APP", "❌ Wake word detection disabled");
            break;
            
        case WAKE_WORD_EVENT_ERROR:
            ESP_LOGW("MY_APP", "⚠️ Wake word detection error");
            break;
    }
}
```

### 3. Initialize in Main Application
```c
void app_main(void)
{
    // ... other initialization code ...
    
    // Initialize realtime audio system first
    esp_err_t ret = realtime_streaming_init();
    if (ret != ESP_OK) {
        ESP_LOGE("MY_APP", "Failed to init realtime system");
        return;
    }
    
    // Start realtime system (this will automatically initialize wake word detection)
    ret = realtime_streaming_run();
    if (ret != ESP_OK) {
        ESP_LOGE("MY_APP", "Failed to start realtime system");
        return;
    }
    
    // The wake word system is now active and listening for "Hey Tofu"
    ESP_LOGI("MY_APP", "System ready - say 'Hey Tofu' to activate");
    
    // ... rest of your application ...
}
```

## Manual Wake Word Management

If you want more control over the wake word system:

```c
void manual_wake_word_setup(void)
{
    // Initialize wake word manager separately
    esp_err_t ret = wake_word_manager_init(my_wake_word_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE("MY_APP", "Failed to init wake word manager");
        return;
    }
    
    // Start detection
    ret = wake_word_manager_start();
    if (ret != ESP_OK) {
        ESP_LOGE("MY_APP", "Failed to start wake word detection");
        return;
    }
    
    // Adjust sensitivity (0.0 = least sensitive, 1.0 = most sensitive)
    wake_word_manager_set_sensitivity(0.8f);
    
    ESP_LOGI("MY_APP", "Manual wake word setup complete");
}
```

## Runtime Control

```c
void runtime_control_example(void)
{
    // Check if wake word is enabled
    if (wake_word_manager_is_enabled()) {
        ESP_LOGI("MY_APP", "Wake word detection is active");
    }
    
    // Check if currently listening
    if (wake_word_manager_is_listening()) {
        ESP_LOGI("MY_APP", "Currently listening for 'Hey Tofu'");
    }
    
    // Temporarily disable wake word detection
    wake_word_manager_enable(false);
    ESP_LOGI("MY_APP", "Wake word detection paused");
    
    // Wait 5 seconds
    vTaskDelay(pdMS_TO_TICKS(5000));
    
    // Re-enable wake word detection
    wake_word_manager_enable(true);
    ESP_LOGI("MY_APP", "Wake word detection resumed");
    
    // Get statistics
    wake_word_stats_t stats;
    if (wake_word_manager_get_stats(&stats) == ESP_OK) {
        ESP_LOGI("MY_APP", "Wake word stats: %lu detections, %.2f avg confidence", 
                 stats.total_detections, stats.average_confidence);
    }
    
    // Get last detected wake word
    const char* last_detected = wake_word_manager_get_last_detected();
    if (last_detected) {
        ESP_LOGI("MY_APP", "Last detected: %s", last_detected);
    }
}
```

## Configuration Options

### Sensitivity Adjustment
```c
// More sensitive (detects more easily, may have false positives)
wake_word_manager_set_sensitivity(0.9f);

// Less sensitive (fewer false positives, may miss some detections)
wake_word_manager_set_sensitivity(0.3f);

// Default balanced setting
wake_word_manager_set_sensitivity(0.8f);
```

### Get Current Sensitivity
```c
float current_sensitivity = wake_word_manager_get_sensitivity();
ESP_LOGI("MY_APP", "Current sensitivity: %.2f", current_sensitivity);
```

## Integration with Realtime System

The wake word detection is automatically integrated with the realtime audio system:

1. **Automatic Initialization**: When you call `realtime_streaming_init()`, the wake word system is initialized
2. **Audio Feed**: The recording task automatically feeds audio data to the wake word detector
3. **Event Handling**: Wake word events are handled in the realtime system
4. **Resource Sharing**: The wake word system shares audio resources efficiently

## System Requirements

- **ESP32-S3** or **ESP32-P4** with PSRAM (for ESP-SR support)
- **16kHz mono audio** input from microphone
- **Realtime audio system** must be running
- Sufficient **heap memory** for audio buffers

## Troubleshooting

### Wake Word Not Detected
1. Check microphone connection and gain
2. Verify audio is reaching the system (check realtime logs)
3. Adjust sensitivity higher
4. Ensure minimal background noise
5. Speak clearly and at normal volume

### False Positives
1. Adjust sensitivity lower
2. Check for audio feedback or echo
3. Verify microphone placement
4. Consider acoustic isolation

### Performance Issues
1. Monitor CPU usage and memory
2. Check audio buffer health
3. Ensure PSRAM is available
4. Consider reducing other system loads during detection

## Audio Quality Requirements

For best wake word detection performance:
- **Clear audio**: Minimal background noise
- **Good microphone**: INMP441 or similar I2S microphone
- **Proper placement**: Microphone facing the user
- **Stable power**: Avoid power fluctuations that affect audio
- **Acoustic design**: Consider enclosure effects on audio quality