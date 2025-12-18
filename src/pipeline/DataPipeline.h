#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Forward declarations
class IDataSource;

/**
 * @brief DataPipeline - Coordinates data capture to Flash storage
 *
 * Runs a Flash Writer task on Core 1 that consumes data from any
 * IDataSource transport (UART, Parallel Port, etc.) and writes it to FlashRing.
 *
 * This achieves dual-core separation:
 * - Core 0: Transport ISR and capture task
 * - Core 1: Flash write operations
 */

namespace DataPipeline {

/// Configuration
struct Config {
  size_t writeChunkSize = 12288;  ///< Buffer size (12KB) to accumulate data while writing
  uint32_t flushTimeoutMs = 500; ///< Flush remaining data after this timeout
  bool autoStart = true;         ///< Start pipeline immediately
};

/**
 * @brief Initialize the data pipeline
 *
 * Must be called after FlashRing::init() and transport source init()
 *
 * @param config Configuration parameters
 * @param dataSource Pointer to initialized IDataSource transport
 * @return ESP_OK on success
 */
esp_err_t init(const Config &config, IDataSource* dataSource);

/**
 * @brief Start the pipeline (if autoStart was false)
 */
esp_err_t start();

/**
 * @brief Stop the pipeline
 */
esp_err_t stop();

/**
 * @brief Force flush any pending data to flash
 */
esp_err_t flush();

/**
 * @brief Get pipeline statistics
 */
struct Stats {
  size_t bytesWrittenToFlash;
  size_t bytesDropped;
  uint32_t writeOperations;
  uint32_t flushOperations;
  bool running;
};

esp_err_t getStats(Stats *stats);

/**
 * @brief Reset statistics to zero
 */
void resetStats();

/**
 * @brief Deinitialize
 */
void deinit();

} // namespace DataPipeline
