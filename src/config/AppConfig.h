#pragma once

#include "../transport/TransportTypes.h"
#include "ConfigManager.h"

/**
 * @brief Application configuration
 *
 * @deprecated This file is deprecated. Use ConfigManager::FullConfig directly.
 * Kept only for minimal backward compatibility.
 */

namespace AppConfig {

// Legacy compatibility - returns default values
// TODO: Remove this file and update all references to use
// ConfigManager::FullConfig

inline Transport::Type getTransportType() {
  // Return default since transport is not in FullConfig yet
  return Transport::Type::UART;
}

inline esp_err_t getUartConfig(ConfigManager::UartConfig *config) {
  // Return default UART config
  *config = ConfigManager::UartConfig();
  return ESP_OK;
}

struct ParallelPortConfig {
  int dataPins[8] = {2, 4, 5, 18, 19, 21, 22, 23};
  int strobePin = 0;
  bool strobeActiveHigh = true;
  size_t ringBufSize = 64 * 1024;
  uint32_t timeoutMs = 100;
};

inline esp_err_t getParallelPortConfig(ParallelPortConfig *config) {
  // Return default parallel port config
  *config = ParallelPortConfig();
  return ESP_OK;
}

} // namespace AppConfig
