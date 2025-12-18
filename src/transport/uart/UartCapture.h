#pragma once

#include "../IDataSource.h"
#include "../TransportTypes.h"
#include "driver/uart.h"
#include "esp_err.h"
#include <cstddef>
#include <cstdint>

/**
 * @brief UartCapture - UART transport implementation
 *
 * Captures data from UART and places it into a FreeRTOS ring buffer.
 *
 * Features:
 * - Event-driven reception (no polling)
 * - Large hardware buffer to absorb bursts
 * - Pinned to Core 0 for deterministic timing
 * - Timeout detection for end-of-burst
 */
class UartCapture : public IDataSource {
public:
    /// Configuration structure
    struct Config {
        uart_port_t uartPort = UART_NUM_2;
        int rxPin = 16;                 ///< GPIO for UART RX
        int txPin = 17;                 ///< GPIO for UART TX (not used for RX-only)
        uint32_t baudRate = 1000000;    ///< Baud rate in bps
        uart_word_length_t dataBits = UART_DATA_8_BITS;  ///< Data bits (5-8)
        uart_parity_t parity = UART_PARITY_DISABLE;      ///< Parity (DISABLE, EVEN, ODD)
        uart_stop_bits_t stopBits = UART_STOP_BITS_1;   ///< Stop bits (1, 1.5, 2)
        size_t rxBufSize = 16 * 1024;   ///< Hardware RX buffer size
        size_t ringBufSize = 32 * 1024; ///< Ring buffer size for processing
        uint32_t timeoutMs = 100;       ///< Burst end detection timeout
    };

    // IDataSource interface implementation
    esp_err_t init(const void* config) override;
    RingbufHandle_t getRingBuffer() override;
    void setBurstCallback(Transport::BurstCallback callback) override;
    esp_err_t getStats(Transport::Stats* stats) override;
    void resetStats() override;
    esp_err_t deinit() override;
    Transport::Type getType() const override { return Transport::Type::UART; }

    // UART-specific methods
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
    uint32_t getBaudRate() const;

private:
    Config m_config;
    RingbufHandle_t m_ringBuf = nullptr;
    TaskHandle_t m_taskHandle = nullptr;
    QueueHandle_t m_uartQueue = nullptr;
    Transport::BurstCallback m_burstCallback = nullptr;
    bool m_initialized = false;
    Transport::Stats m_stats = {};

    static void uartTask(void *arg);
};

