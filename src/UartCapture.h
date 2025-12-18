#pragma once

#include "driver/uart.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include <cstddef>
#include <cstdint>


/**
 * @brief UartCapture - High-speed UART reception for ESP32
 *
 * Captures data from UART2 at 1Mbps and places it into a FreeRTOS
 * ring buffer for processing by another task.
 *
 * Features:
 * - Event-driven reception (no polling)
 * - Large hardware buffer to absorb bursts
 * - Pinned to Core 0 for deterministic timing
 * - Timeout detection for end-of-burst
 */

namespace UartCapture {

/// Configuration structure
struct Config {
  uart_port_t uartPort = UART_NUM_2;
  int rxPin = 16;                 // GPIO16 for UART2 RX
  int txPin = 17;                 // GPIO17 for UART2 TX (not used for RX-only)
  uint32_t baudRate = 1000000;    // 1 Mbps
  size_t rxBufSize = 16 * 1024;   // 16KB hardware buffer
  size_t ringBufSize = 32 * 1024; // 32KB ring buffer for processing
  uint32_t timeoutMs = 100;       // Burst end detection timeout
};

/// Callback for burst events
using BurstCallback = void (*)(bool burstEnded, size_t bytesInBurst);

/**
 * @brief Initialize UART capture
 *
 * Sets up UART with large buffers and creates the capture task.
 *
 * @param config Configuration parameters
 * @return ESP_OK on success
 */
esp_err_t init(const Config &config);

/**
 * @brief Get the ring buffer handle for reading captured data
 *
 * The consumer task should use this to retrieve data.
 *
 * @return RingbufHandle_t or nullptr if not initialized
 */
RingbufHandle_t getRingBuffer();

/**
 * @brief Set callback for burst events
 *
 * Called when burst starts or ends (based on timeout).
 *
 * @param callback Function to call
 */
void setBurstCallback(BurstCallback callback);

/**
 * @brief Get statistics
 */
struct Stats {
  size_t totalBytesReceived;
  size_t bytesInCurrentBurst;
  uint32_t burstCount;
  uint32_t overflowCount;
  bool burstActive;
};

esp_err_t getStats(Stats *stats);

/**
 * @brief Reset statistics to zero
 */
void resetStats();

/**
 * @brief Change baudrate at runtime
 * @param baudRate New baudrate
 * @return ESP_OK on success
 */
esp_err_t setBaudRate(uint32_t baudRate);

/**
 * @brief Get current baudrate
 * @return Current baudrate
 */
uint32_t getBaudRate();

/**
 * @brief Stop capture and release resources
 */
void deinit();

} // namespace UartCapture
