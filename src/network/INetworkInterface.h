#pragma once

#include "NetworkTypes.h"
#include "esp_err.h"
#include "esp_netif.h"

/**
 * @brief Network Interface Abstraction
 * 
 * Abstract interface for network connectivity (Ethernet, WiFi, etc.)
 * Allows switching between different network implementations seamlessly.
 */

class INetworkInterface {
public:
    virtual ~INetworkInterface() = default;

    /**
     * @brief Initialize the network interface
     * @param config Configuration structure (implementation-specific)
     * @return ESP_OK on success
     */
    virtual esp_err_t init(const void* config) = 0;

    /**
     * @brief Start the network interface
     * @return ESP_OK on success
     */
    virtual esp_err_t start() = 0;

    /**
     * @brief Stop the network interface
     * @return ESP_OK on success
     */
    virtual esp_err_t stop() = 0;

    /**
     * @brief Deinitialize the network interface
     * @return ESP_OK on success
     */
    virtual esp_err_t deinit() = 0;

    /**
     * @brief Get current connection status
     * @return Current status
     */
    virtual Network::Status getStatus() const = 0;

    /**
     * @brief Get network interface type
     * @return Network type
     */
    virtual Network::Type getType() const = 0;

    /**
     * @brief Get ESP netif handle (for use with ESP-IDF APIs)
     * @return esp_netif_t* handle or nullptr if not initialized
     */
    virtual esp_netif_t* getNetif() = 0;

    /**
     * @brief Get current IP address
     * @param ip Output IP address
     * @return ESP_OK on success
     */
    virtual esp_err_t getIpAddress(Network::IpAddress* ip) = 0;

    /**
     * @brief Get network statistics
     * @param stats Output statistics structure
     * @return ESP_OK on success
     */
    virtual esp_err_t getStats(Network::Stats* stats) = 0;

    /**
     * @brief Check if interface is connected
     * @return true if connected
     */
    virtual bool isConnected() const {
        return getStatus() == Network::Status::CONNECTED;
    }
};

