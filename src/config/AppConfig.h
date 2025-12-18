#pragma once

#include "../transport/TransportTypes.h"
#include "ConfigManager.h"

/**
 * @brief Application configuration
 * 
 * This file provides compatibility layer for AppConfig namespace.
 * Actual configuration is now managed by ConfigManager and stored in NVS.
 * 
 * @deprecated Use ConfigManager directly for runtime configuration.
 * This namespace is kept for backward compatibility.
 */

namespace AppConfig {

/// Get current transport type from ConfigManager
inline Transport::Type getTransportType() {
    return ConfigManager::getTransportType();
}

/// Get UART configuration from ConfigManager
inline esp_err_t getUartConfig(ConfigManager::UartConfig* config) {
    return ConfigManager::getUartConfig(config);
}

/// Get Parallel Port configuration from ConfigManager
inline esp_err_t getParallelPortConfig(ConfigManager::ParallelPortConfig* config) {
    return ConfigManager::getParallelPortConfig(config);
}

} // namespace AppConfig

