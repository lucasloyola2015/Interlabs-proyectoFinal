#pragma once

#include "../IDataSource.h"
#include "../TransportTypes.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include <cstddef>
#include <cstdint>

/**
 * @brief ParallelPortCapture - 8-bit parallel port transport implementation
 *
 * Captures data from an 8-bit parallel port with strobe signal.
 * Reads data byte when strobe signal transitions (edge-triggered).
 *
 * Features:
 * - Edge-triggered capture on strobe signal
 * - 8-bit data bus (GPIO pins)
 * - Burst detection via timeout
 * - Pinned to Core 0 for deterministic timing
 */
class ParallelPortCapture : public IDataSource {
public:
    /// Configuration structure
    struct Config {
        int dataPins[8];              ///< GPIO pins for data bits D0-D7
        int strobePin;                ///< GPIO pin for strobe signal (active edge)
        bool strobeActiveHigh = true; ///< true = rising edge, false = falling edge
        size_t ringBufSize = 32 * 1024; ///< Ring buffer size for processing
        uint32_t timeoutMs = 100;    ///< Burst end detection timeout
    };

    // IDataSource interface implementation
    esp_err_t init(const void* config) override;
    RingbufHandle_t getRingBuffer() override;
    void setBurstCallback(Transport::BurstCallback callback) override;
    esp_err_t getStats(Transport::Stats* stats) override;
    void resetStats() override;
    esp_err_t deinit() override;
    Transport::Type getType() const override { return Transport::Type::PARALLEL_PORT; }

private:
    Config m_config;
    RingbufHandle_t m_ringBuf = nullptr;
    TaskHandle_t m_taskHandle = nullptr;
    QueueHandle_t m_strobeQueue = nullptr;  // Queue for strobe events from ISR
    Transport::BurstCallback m_burstCallback = nullptr;
    bool m_initialized = false;
    Transport::Stats m_stats = {};

    // ISR handler for strobe signal
    static void IRAM_ATTR strobeISR(void* arg);
    
    // Task that processes strobe events
    static void captureTask(void *arg);
    
    // Read 8-bit data from GPIO pins
    uint8_t readDataByte() const;
};

