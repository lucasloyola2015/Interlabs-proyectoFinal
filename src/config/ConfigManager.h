#pragma once

#include "../transport/TransportTypes.h"
#include "driver/uart.h"
#include <cstdint>
#include <cstddef>

/**
 * @brief Configuration Manager
 * 
 * Manages application configuration stored in NVS.
 * Handles transport type selection and all transport-specific settings.
 */

namespace ConfigManager {

/// Complete UART configuration
struct UartConfig {
    uart_port_t uartPort = UART_NUM_2;
    int rxPin = 16;
    int txPin = 17;
    uint32_t baudRate = 115200;
    uart_word_length_t dataBits = UART_DATA_8_BITS;
    uart_parity_t parity = UART_PARITY_DISABLE;
    uart_stop_bits_t stopBits = UART_STOP_BITS_1;
    size_t rxBufSize = 32 * 1024;
    size_t ringBufSize = 64 * 1024;
    uint32_t timeoutMs = 100;
};

/// Complete Parallel Port configuration
struct ParallelPortConfig {
    int dataPins[8] = {2, 4, 5, 18, 19, 21, 22, 23};
    int strobePin = 0;
    bool strobeActiveHigh = true;
    size_t ringBufSize = 64 * 1024;
    uint32_t timeoutMs = 100;
};

/// Complete application configuration
struct AppConfig {
    Transport::Type transportType = Transport::Type::UART;
    UartConfig uart;
    ParallelPortConfig parallelPort;
    
    // Version for migration purposes
    uint32_t version = 1;
};

/**
 * @brief Initialize configuration manager
 * Loads configuration from NVS or creates default if not found
 * @return ESP_OK on success
 */
esp_err_t init();

/**
 * @brief Get current configuration
 * @param config Output structure to fill
 * @return ESP_OK on success
 */
esp_err_t getConfig(AppConfig* config);

/**
 * @brief Save configuration to NVS
 * @param config Configuration to save
 * @return ESP_OK on success
 */
esp_err_t saveConfig(const AppConfig* config);

/**
 * @brief Get UART configuration
 * @param config Output structure to fill
 * @return ESP_OK on success
 */
esp_err_t getUartConfig(UartConfig* config);

/**
 * @brief Save UART configuration
 * @param config Configuration to save
 * @return ESP_OK on success
 */
esp_err_t saveUartConfig(const UartConfig* config);

/**
 * @brief Get Parallel Port configuration
 * @param config Output structure to fill
 * @return ESP_OK on success
 */
esp_err_t getParallelPortConfig(ParallelPortConfig* config);

/**
 * @brief Save Parallel Port configuration
 * @param config Configuration to save
 * @return ESP_OK on success
 */
esp_err_t saveParallelPortConfig(const ParallelPortConfig* config);

/**
 * @brief Set transport type
 * @param type Transport type to use
 * @return ESP_OK on success
 */
esp_err_t setTransportType(Transport::Type type);

/**
 * @brief Get transport type
 * @return Current transport type
 */
Transport::Type getTransportType();

/**
 * @brief Reset configuration to defaults
 * @return ESP_OK on success
 */
esp_err_t resetToDefaults();

} // namespace ConfigManager

