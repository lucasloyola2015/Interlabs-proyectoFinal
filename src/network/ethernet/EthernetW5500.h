#pragma once

#include "../INetworkInterface.h"
#include "driver/spi_master.h"
#include "esp_eth.h"
#include "esp_netif.h"
#include <cstdint>

/**
 * @brief Ethernet W5500 Network Interface
 * 
 * Implementation of INetworkInterface for W5500 Ethernet controller.
 * Uses SPI communication to interface with the W5500 chip.
 */

class EthernetW5500 : public INetworkInterface {
public:
    /// Configuration structure
    struct Config {
        // SPI configuration
        int spiHost = SPI2_HOST;          ///< SPI host (SPI2_HOST or SPI3_HOST)
        int mosiPin = 23;                  ///< MOSI GPIO pin
        int misoPin = 19;                  ///< MISO GPIO pin
        int sclkPin = 18;                  ///< SCLK GPIO pin
        int csPin = 21;                    ///< Chip Select GPIO pin
        int resetPin = 22;                 ///< Reset GPIO pin (-1 to disable)
        int interruptPin = 25;             ///< Interrupt GPIO pin
        int clockSpeedHz = 20 * 1000 * 1000; ///< SPI clock speed (20MHz default)
        
        // IP configuration
        Network::IpMode ipMode = Network::IpMode::DHCP;
        Network::IpAddress staticIp = {192, 168, 1, 100};
        Network::IpAddress staticNetmask = {255, 255, 255, 0};
        Network::IpAddress staticGateway = {192, 168, 1, 1};
        Network::IpAddress staticDns = {8, 8, 8, 8};
    };

    // INetworkInterface implementation
    esp_err_t init(const void* config) override;
    esp_err_t start() override;
    esp_err_t stop() override;
    esp_err_t deinit() override;
    Network::Status getStatus() const override;
    Network::Type getType() const override { return Network::Type::ETHERNET; }
    esp_netif_t* getNetif() override;
    esp_err_t getIpAddress(Network::IpAddress* ip) override;
    esp_err_t getStats(Network::Stats* stats) override;

private:
    Config m_config;
    esp_netif_t* m_netif = nullptr;
    esp_eth_handle_t m_ethHandle = nullptr;
    Network::Status m_status = Network::Status::DISCONNECTED;
    bool m_initialized = false;

    // Event handlers
    static void ethEventHandler(void* arg, esp_event_base_t eventBase,
                               int32_t eventId, void* eventData);
    void onEthEvent(esp_event_base_t eventBase, int32_t eventId, void* eventData);
    
    // Helper functions
    esp_err_t initSpi();
    esp_err_t initW5500();
    esp_err_t configureIp();
};

