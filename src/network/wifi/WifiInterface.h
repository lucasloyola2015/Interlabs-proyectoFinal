#pragma once

#include "../INetworkInterface.h"
#include <cstdint>

/**
 * @brief WiFi Network Interface
 *
 * Implementation of INetworkInterface for WiFi connectivity.
 *
 * @note This is a placeholder structure prepared for future implementation.
 * Currently returns error on all operations.
 */

class WifiInterface : public INetworkInterface {
public:
  /// WiFi mode
  enum class Mode {
    STA,  ///< Station mode (connect to AP)
    AP,   ///< Access Point mode
    APSTA ///< Both modes simultaneously
  };

  /// Configuration structure - Compatible with ConfigManager::WifiConfig
  struct Config {
    bool enabled = false;    ///< WiFi enabled flag
    char ssid[32] = {0};     ///< Station SSID (array, not pointer)
    char password[64] = {0}; ///< Station password (array, not pointer)
    bool apMode = false;     ///< true = AP mode, false = STA mode

    // IP configuration
    Network::IpMode ipMode = Network::IpMode::DHCP;
    Network::IpAddress staticIp = {192, 168, 1, 50};
    Network::IpAddress staticNetmask = {255, 255, 255, 0};
    Network::IpAddress staticGateway = {192, 168, 1, 1};

    // Access Point mode configuration (optional)
    char apSsid[32] = "DataLoggerAP";
    char apPassword[64] = {0};
    uint8_t apChannel = 1;
    uint8_t apMaxConnections = 4;
  };

  // INetworkInterface implementation
  esp_err_t init(const void *config) override;
  esp_err_t start() override;
  esp_err_t stop() override;
  esp_err_t deinit() override;
  Network::Status getStatus() const override;
  Network::Type getType() const override { return Network::Type::WIFI; }
  esp_netif_t *getNetif() override;
  esp_err_t getIpAddress(Network::IpAddress *ip) override;
  esp_err_t getStats(Network::Stats *stats) override;

private:
  Config m_config;
  esp_netif_t *m_netif = nullptr;
  Network::Status m_status = Network::Status::DISCONNECTED;
  bool m_initialized = false;

  // Event handlers
  static void wifiEventHandler(void *arg, esp_event_base_t eventBase,
                               int32_t eventId, void *eventData);
  void onWifiEvent(esp_event_base_t eventBase, int32_t eventId,
                   void *eventData);

  // Helper functions
  esp_err_t initWifi();
  esp_err_t configureIp();
};
