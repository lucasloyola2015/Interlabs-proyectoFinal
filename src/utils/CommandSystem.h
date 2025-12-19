#pragma once

#include "esp_system.h"
#include "transport/IDataSource.h"

/**
 * @brief Command System for Debug UART
 *
 * Handles reading commands from the debug UART (UART0) and executing them.
 */
namespace CommandSystem {

/**
 * @brief Initialize the command system
 *
 * Starts the CLI task to process UART commands.
 *
 * @param dataSource Global transport instance for stats and control
 * @return ESP_OK on success
 */
esp_err_t initialize(IDataSource *dataSource);

/**
 * @brief Stop the command system
 */
void deinit();

} // namespace CommandSystem
