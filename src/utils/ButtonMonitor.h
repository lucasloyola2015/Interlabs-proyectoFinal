#pragma once

#include "esp_err.h"

namespace ButtonMonitor {

/**
 * @brief Initialize the button monitor task
 *
 * Starts a low-priority FreeRTOS task that monitors the BOOT button (GPIO 0).
 * If the button is held for more than 3 seconds, it triggers safe mode on
 * release.
 *
 * @return ESP_OK on success
 */
esp_err_t init();

/**
 * @brief Stop the button monitor task
 */
void deinit();

} // namespace ButtonMonitor
