#pragma once

#include "TransportTypes.h"
#include "esp_err.h"
#include "freertos/ringbuf.h"

/**
 * @brief Abstract interface for data source transports
 * 
 * This interface allows DataPipeline to work with any transport
 * implementation (UART, Parallel Port, etc.) without knowing the
 * specific details of each transport.
 */
class IDataSource {
public:
    virtual ~IDataSource() = default;

    /**
     * @brief Initialize the data source
     * @param config Pointer to transport-specific configuration
     * @return ESP_OK on success
     */
    virtual esp_err_t init(const void* config) = 0;

    /**
     * @brief Get the ring buffer handle for reading captured data
     * @return RingbufHandle_t or nullptr if not initialized
     */
    virtual RingbufHandle_t getRingBuffer() = 0;

    /**
     * @brief Set callback for burst events
     * @param callback Function to call on burst start/end
     */
    virtual void setBurstCallback(Transport::BurstCallback callback) = 0;

    /**
     * @brief Get transport statistics
     * @param stats Pointer to stats structure (output)
     * @return ESP_OK on success
     */
    virtual esp_err_t getStats(Transport::Stats* stats) = 0;

    /**
     * @brief Reset statistics to zero
     */
    virtual void resetStats() = 0;

    /**
     * @brief Deinitialize and release resources
     */
    virtual esp_err_t deinit() = 0;

    /**
     * @brief Get transport type
     * @return Transport type identifier
     */
    virtual Transport::Type getType() const = 0;
};

