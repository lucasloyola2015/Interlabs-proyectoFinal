#include "ButtonMonitor.h"
#include "../config/ConfigManager.h"
#include "LedManager.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace ButtonMonitor {

static const char *TAG = "ButtonMonitor";
static const gpio_num_t BOOT_BUTTON_GPIO = GPIO_NUM_0;
static const uint32_t HOLD_SAFE_MS = 3000;
static const uint32_t HOLD_FACTORY_MS = 8000;
static const uint32_t POLL_INTERVAL_MS = 100;

static TaskHandle_t s_taskHandle = nullptr;
static bool s_running = false;

/**
 * @brief Button monitor task
 *
 * Monitors GPIO 0 (BOOT button) and triggers safe mode or factory reset
 */
static void buttonMonitorTask(void *pvParameters) {
  uint32_t pressedTime = 0;
  bool wasPressed = false;
  bool safeThresholdReached = false;
  bool factoryThresholdReached = false;

  ESP_LOGI(TAG, "Button monitor task started");

  while (s_running) {
    // Read button state (active LOW - pressed when 0)
    int buttonState = gpio_get_level(BOOT_BUTTON_GPIO);
    bool isPressed = (buttonState == 0);

    if (isPressed) {
      if (!wasPressed) {
        wasPressed = true;
        pressedTime = 0;
        safeThresholdReached = false;
        factoryThresholdReached = false;
        ESP_LOGI(TAG, "BOOT button pressed");
        LedManager::setState(LedManager::State::HOLD_3S);
      } else {
        pressedTime += POLL_INTERVAL_MS;

        // Stage 3: Factory Reset threshold (>8s)
        if (pressedTime >= HOLD_FACTORY_MS && !factoryThresholdReached) {
          ESP_LOGW(TAG,
                   "FACTORY RESET threshold reached! Release now to reset.");
          LedManager::setState(LedManager::State::FACTORY_READY);
          factoryThresholdReached = true;
        }
        // Stage 2: Safe Mode threshold (>3s)
        else if (pressedTime >= HOLD_SAFE_MS && !safeThresholdReached) {
          ESP_LOGW(
              TAG,
              "SAFE MODE threshold reached. Keep holding for factory reset.");
          LedManager::setState(LedManager::State::HOLD_8S);
          safeThresholdReached = true;
        }
      }
    } else {
      // Button released
      if (wasPressed) {
        ESP_LOGI(TAG, "BOOT button released after %lu ms", pressedTime);

        if (factoryThresholdReached) {
          ESP_LOGE(TAG, "PERFORMING FACTORY RESET...");

          // Clear safe mode flag first
          ConfigManager::setSafeMode(false);

          // Restore everything to defaults
          if (ConfigManager::restore() == ESP_OK) {
            ESP_LOGI(TAG, "Factory reset complete. Rebooting in 2s...");
            vTaskDelay(pdMS_TO_TICKS(
                2000)); // Give time for NVS and for user to release button
            esp_restart();
          } else {
            ESP_LOGE(TAG, "Factory reset FAILED!");
            LedManager::setState(LedManager::State::IDLE);
          }
        } else if (safeThresholdReached) {
          ESP_LOGW(TAG, "Triggering SAFE MODE...");
          if (ConfigManager::setSafeMode(true) == ESP_OK) {
            ESP_LOGI(TAG, "Safe mode flag set. Rebooting in 1s...");
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
          } else {
            ESP_LOGE(TAG, "Failed to set safe mode flag");
            LedManager::setState(LedManager::State::IDLE);
          }
        } else {
          // Normal short release
          LedManager::setState(LedManager::State::IDLE);
        }

        wasPressed = false;
        pressedTime = 0;
        safeThresholdReached = false;
        factoryThresholdReached = false;
      }
    }

    vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
  }

  vTaskDelete(nullptr);
}

esp_err_t init() {
  if (s_running) {
    ESP_LOGW(TAG, "Button monitor already running");
    return ESP_OK;
  }

  // Configure GPIO 0 (BOOT button) as input with pull-up
  gpio_config_t io_conf = {};
  io_conf.intr_type = GPIO_INTR_DISABLE;
  io_conf.mode = GPIO_MODE_INPUT;
  io_conf.pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO);
  io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io_conf.pull_up_en = GPIO_PULLUP_ENABLE;

  esp_err_t ret = gpio_config(&io_conf);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to configure BOOT button GPIO: %s",
             esp_err_to_name(ret));
    return ret;
  }

  // Create button monitor task with very low priority
  // Increased stack size to prevent overflow when calling restore()
  s_running = true;
  BaseType_t taskRet = xTaskCreate(buttonMonitorTask, "button_monitor",
                                   4096,                 // Stack size (increased from 2048)
                                   nullptr,              // Parameters
                                   tskIDLE_PRIORITY + 1, // Very low priority
                                   &s_taskHandle);

  if (taskRet != pdPASS) {
    ESP_LOGE(TAG, "Failed to create button monitor task");
    s_running = false;
    return ESP_FAIL;
  }

  ESP_LOGI(
      TAG,
      "Button monitor initialized (GPIO %d, Safe: %lu ms, Factory: %lu ms)",
      BOOT_BUTTON_GPIO, HOLD_SAFE_MS, HOLD_FACTORY_MS);

  // Ensure we are not starting with a "dirty" state
  ConfigManager::setSafeMode(false);

  return ESP_OK;
}

void deinit() {
  if (!s_running) {
    return;
  }

  s_running = false;

  if (s_taskHandle != nullptr) {
    // Task will delete itself when s_running becomes false
    vTaskDelay(pdMS_TO_TICKS(200)); // Give task time to exit
    s_taskHandle = nullptr;
  }

  ESP_LOGI(TAG, "Button monitor deinitialized");
}

} // namespace ButtonMonitor
