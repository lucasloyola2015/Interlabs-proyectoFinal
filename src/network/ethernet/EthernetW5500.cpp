#include "EthernetW5500.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "lwip/inet.h"
#include <cstring>

// W5500 driver headers (available when CONFIG_ETH_SPI_ETHERNET_W5500 is
// enabled)
#ifdef CONFIG_ETH_SPI_ETHERNET_W5500
#include "esp_eth_mac_w5500.h"
#include "esp_eth_phy_w5500.h"

#endif

static const char *TAG = "EthernetW5500";

esp_err_t EthernetW5500::init(const void *config) {
  if (m_initialized) {
    ESP_LOGW(TAG, "Already initialized");
    return ESP_OK;
  }

  if (!config) {
    ESP_LOGE(TAG, "Config is null");
    return ESP_ERR_INVALID_ARG;
  }

  m_config = *static_cast<const Config *>(config);
  m_status = Network::Status::DISCONNECTED;

  // Register event handlers
  ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                             &ethEventHandler, this));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                             &ethEventHandler, this));

  // Create default netif for Ethernet
  esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
  m_netif = esp_netif_new(&cfg);
  if (!m_netif) {
    ESP_LOGE(TAG, "Failed to create netif");
    return ESP_ERR_NO_MEM;
  }

  // Initialize SPI
  esp_err_t ret = initSpi();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize SPI");
    esp_netif_destroy(m_netif);
    m_netif = nullptr;
    return ret;
  }

  // Initialize W5500
  ret = initW5500();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize W5500");
    esp_netif_destroy(m_netif);
    m_netif = nullptr;
    return ret;
  }

  m_initialized = true;
  ESP_LOGI(TAG, "Ethernet W5500 initialized");
  return ESP_OK;
}

esp_err_t EthernetW5500::start() {
  if (!m_initialized) {
    ESP_LOGE(TAG, "Not initialized");
    return ESP_ERR_INVALID_STATE;
  }

  if (m_status == Network::Status::CONNECTED) {
    ESP_LOGW(TAG, "Already started");
    return ESP_OK;
  }

  esp_err_t ret = esp_eth_start(m_ethHandle);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start Ethernet: %s", esp_err_to_name(ret));
    return ret;
  }

  m_status = Network::Status::CONNECTING;
  ESP_LOGI(TAG, "Ethernet started, waiting for connection...");
  return ESP_OK;
}

esp_err_t EthernetW5500::stop() {
  if (!m_initialized) {
    return ESP_ERR_INVALID_STATE;
  }

  if (m_status == Network::Status::DISCONNECTED) {
    return ESP_OK;
  }

  esp_err_t ret = esp_eth_stop(m_ethHandle);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to stop Ethernet: %s", esp_err_to_name(ret));
    return ret;
  }

  m_status = Network::Status::DISCONNECTED;
  ESP_LOGI(TAG, "Ethernet stopped");
  return ESP_OK;
}

esp_err_t EthernetW5500::deinit() {
  if (!m_initialized) {
    return ESP_OK;
  }

  stop();

  if (m_ethHandle) {
    esp_eth_driver_uninstall(m_ethHandle);
    m_ethHandle = nullptr;
  }

  // Unregister event handlers
  esp_event_handler_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP, &ethEventHandler);
  esp_event_handler_unregister(ETH_EVENT, ESP_EVENT_ANY_ID, &ethEventHandler);

  if (m_netif) {
    esp_netif_destroy(m_netif);
    m_netif = nullptr;
  }

  m_initialized = false;
  m_status = Network::Status::DISCONNECTED;
  ESP_LOGI(TAG, "Ethernet W5500 deinitialized");
  return ESP_OK;
}

Network::Status EthernetW5500::getStatus() const { return m_status; }

esp_netif_t *EthernetW5500::getNetif() { return m_netif; }

esp_err_t EthernetW5500::getIpAddress(Network::IpAddress *ip) {
  if (!ip) {
    return ESP_ERR_INVALID_ARG;
  }

  if (!m_netif || m_status != Network::Status::CONNECTED) {
    *ip = Network::IpAddress();
    return ESP_ERR_INVALID_STATE;
  }

  esp_netif_ip_info_t ipInfo;
  esp_err_t ret = esp_netif_get_ip_info(m_netif, &ipInfo);
  if (ret != ESP_OK) {
    return ret;
  }

  ip->addr[0] = (ipInfo.ip.addr >> 0) & 0xFF;
  ip->addr[1] = (ipInfo.ip.addr >> 8) & 0xFF;
  ip->addr[2] = (ipInfo.ip.addr >> 16) & 0xFF;
  ip->addr[3] = (ipInfo.ip.addr >> 24) & 0xFF;

  return ESP_OK;
}

esp_err_t EthernetW5500::getStats(Network::Stats *stats) {
  if (!stats) {
    return ESP_ERR_INVALID_ARG;
  }

  if (!m_ethHandle) {
    memset(stats, 0, sizeof(Network::Stats));
    return ESP_ERR_INVALID_STATE;
  }

  // TODO: Get statistics from netif when available
  // For now, return zero stats
  memset(stats, 0, sizeof(Network::Stats));
  return ESP_OK;
}

void EthernetW5500::ethEventHandler(void *arg, esp_event_base_t eventBase,
                                    int32_t eventId, void *eventData) {
  EthernetW5500 *self = static_cast<EthernetW5500 *>(arg);
  self->onEthEvent(eventBase, eventId, eventData);
}

void EthernetW5500::onEthEvent(esp_event_base_t eventBase, int32_t eventId,
                               void *eventData) {
  if (eventBase == ETH_EVENT) {
    switch (eventId) {
    case ETHERNET_EVENT_CONNECTED:
      ESP_LOGI(TAG, "Ethernet link up");
      m_status = Network::Status::CONNECTING;
      break;
    case ETHERNET_EVENT_DISCONNECTED:
      ESP_LOGI(TAG, "Ethernet link down");
      m_status = Network::Status::DISCONNECTED;
      break;
    case ETHERNET_EVENT_START:
      ESP_LOGI(TAG, "Ethernet started");
      break;
    case ETHERNET_EVENT_STOP:
      ESP_LOGI(TAG, "Ethernet stopped");
      m_status = Network::Status::DISCONNECTED;
      break;
    default:
      break;
    }
  } else if (eventBase == IP_EVENT && eventId == IP_EVENT_ETH_GOT_IP) {
    ip_event_got_ip_t *event = static_cast<ip_event_got_ip_t *>(eventData);
    ESP_LOGI(TAG, "Got IP address: " IPSTR, IP2STR(&event->ip_info.ip));
    m_status = Network::Status::CONNECTED;
  }
}

esp_err_t EthernetW5500::initSpi() {
  spi_bus_config_t buscfg = {};
  buscfg.mosi_io_num = m_config.mosiPin;
  buscfg.miso_io_num = m_config.misoPin;
  buscfg.sclk_io_num = m_config.sclkPin;
  buscfg.quadwp_io_num = -1;
  buscfg.quadhd_io_num = -1;

  esp_err_t ret =
      spi_bus_initialize(static_cast<spi_host_device_t>(m_config.spiHost),
                         &buscfg, SPI_DMA_CH_AUTO);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(ret));
    return ret;
  }

  ESP_LOGI(TAG, "SPI bus initialized");
  return ESP_OK;
}

esp_err_t EthernetW5500::initW5500() {
#ifdef CONFIG_ETH_SPI_ETHERNET_W5500
  // Reset W5500 if reset pin is configured
  if (m_config.resetPin >= 0) {
    gpio_config_t io_conf = {.pin_bit_mask = (1ULL << m_config.resetPin),
                             .mode = GPIO_MODE_OUTPUT,
                             .pull_up_en = GPIO_PULLUP_DISABLE,
                             .pull_down_en = GPIO_PULLDOWN_DISABLE,
                             .intr_type = GPIO_INTR_DISABLE};
    gpio_config(&io_conf);

    // Reset sequence: low -> delay -> high
    gpio_set_level(static_cast<gpio_num_t>(m_config.resetPin), 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(static_cast<gpio_num_t>(m_config.resetPin), 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    ESP_LOGI(TAG, "W5500 reset pin (GPIO%d) configured", m_config.resetPin);
  }

  // Install GPIO ISR service (required for Ethernet interrupt)
  // This might have been installed by other components, so we ignore
  // ESP_ERR_INVALID_STATE
  esp_err_t isr_ret = gpio_install_isr_service(0);
  if (isr_ret != ESP_OK && isr_ret != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "Failed to install GPIO ISR service: %s",
             esp_err_to_name(isr_ret));
    // Continue anyway, as it might work in polling mode or if it was already
    // installed
  }

  // MAC configuration
  eth_mac_config_t macConfig = ETH_MAC_DEFAULT_CONFIG();
  macConfig.rx_task_stack_size = 4096; // Recommended for W5500

  // SPI device configuration for W5500
  spi_device_interface_config_t spiDevCfg = {};
  spiDevCfg.mode = 0;
  spiDevCfg.clock_speed_hz = m_config.clockSpeedHz;
  spiDevCfg.spics_io_num = m_config.csPin;
  spiDevCfg.queue_size = 20;

  // W5500-specific configuration (Modern API for IDF 5.x)
  eth_w5500_config_t w5500Config = ETH_W5500_DEFAULT_CONFIG(
      static_cast<spi_host_device_t>(m_config.spiHost), &spiDevCfg);

  w5500Config.int_gpio_num = m_config.interruptPin;
  if (w5500Config.int_gpio_num < 0) {
    w5500Config.poll_period_ms = 10; // Poll every 10ms if no interrupt
  } else {
    w5500Config.poll_period_ms = 0; // Use interrupt mode
  }

  // Create MAC instance (2 parameters in IDF 5.5+)
  esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500Config, &macConfig);
  if (!mac) {
    ESP_LOGE(TAG, "Failed to create W5500 MAC");
    return ESP_FAIL;
  }

  // PHY configuration (W5500 has integrated PHY)
  eth_phy_config_t phyConfig = ETH_PHY_DEFAULT_CONFIG();
  phyConfig.phy_addr = 0;                       // W5500 doesn't use PHY address
  phyConfig.reset_gpio_num = m_config.resetPin; // Use configured reset pin

  esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phyConfig);
  if (!phy) {
    ESP_LOGE(TAG, "Failed to create W5500 PHY");
    mac->del(mac);
    return ESP_FAIL;
  }

  // Ethernet driver configuration
  esp_eth_config_t ethConfig = ETH_DEFAULT_CONFIG(mac, phy);
  esp_err_t ret = esp_eth_driver_install(&ethConfig, &m_ethHandle);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to install Ethernet driver: %s",
             esp_err_to_name(ret));
    phy->del(phy);
    mac->del(mac);
    return ret;
  }

  // Set MAC address (W5500 doesn't have one by default)
  // Must be done BEFORE attaching to netif for consistency
  uint8_t mac_addr[6] = {0};
  esp_efuse_mac_get_default(mac_addr);
  mac_addr[5] += 1; // Use an offset from base MAC
  esp_eth_ioctl(m_ethHandle, ETH_CMD_S_MAC_ADDR, mac_addr);
  ESP_LOGI(TAG, "Ethernet MAC: %02X:%02X:%02X:%02X:%02X:%02X", mac_addr[0],
           mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);

  // Attach Ethernet driver to netif
  esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(m_ethHandle);
  if (!glue) {
    ESP_LOGE(TAG, "Failed to create Ethernet netif glue");
    esp_eth_driver_uninstall(m_ethHandle);
    m_ethHandle = nullptr;
    phy->del(phy);
    mac->del(mac);
    return ESP_ERR_NO_MEM;
  }
  esp_netif_attach(m_netif, glue);

  // Configure IP
  ret = configureIp();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to configure IP");
    esp_eth_driver_uninstall(m_ethHandle);
    m_ethHandle = nullptr;
    phy->del(phy);
    mac->del(mac);
    return ret;
  }

  ESP_LOGI(TAG, "W5500 initialized successfully");
  return ESP_OK;
#else
  ESP_LOGE(TAG, "W5500 driver not enabled in menuconfig!");
  ESP_LOGE(TAG, "Please enable: CONFIG_ETH_SPI_ETHERNET_W5500");
  ESP_LOGE(TAG, "Run: pio run -t menuconfig -> Component config -> Ethernet -> "
                "SPI Ethernet -> W5500");
  return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t EthernetW5500::configureIp() {
  if (m_config.ipMode == Network::IpMode::DHCP) {
    // DHCP is default, just ensure it's enabled
    ESP_LOGI(TAG, "Using DHCP for IP configuration");
    return ESP_OK;
  } else {
    // Static IP configuration
    esp_netif_ip_info_t ipInfo;
    IP4_ADDR(&ipInfo.ip, m_config.staticIp.addr[0], m_config.staticIp.addr[1],
             m_config.staticIp.addr[2], m_config.staticIp.addr[3]);
    IP4_ADDR(&ipInfo.netmask, m_config.staticNetmask.addr[0],
             m_config.staticNetmask.addr[1], m_config.staticNetmask.addr[2],
             m_config.staticNetmask.addr[3]);
    IP4_ADDR(&ipInfo.gw, m_config.staticGateway.addr[0],
             m_config.staticGateway.addr[1], m_config.staticGateway.addr[2],
             m_config.staticGateway.addr[3]);

    esp_err_t ret = esp_netif_dhcpc_stop(m_netif);
    if (ret != ESP_OK && ret != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
      ESP_LOGE(TAG, "Failed to stop DHCP client: %s", esp_err_to_name(ret));
      return ret;
    }

    ret = esp_netif_set_ip_info(m_netif, &ipInfo);
    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "Failed to set static IP: %s", esp_err_to_name(ret));
      return ret;
    }

    // Set DNS if provided
    if (!m_config.staticDns.isZero()) {
      esp_netif_dns_info_t dnsInfo;
      IP4_ADDR(&dnsInfo.ip.u_addr.ip4, m_config.staticDns.addr[0],
               m_config.staticDns.addr[1], m_config.staticDns.addr[2],
               m_config.staticDns.addr[3]);
      dnsInfo.ip.type = ESP_IPADDR_TYPE_V4;
      ret = esp_netif_set_dns_info(m_netif, ESP_NETIF_DNS_MAIN, &dnsInfo);
      if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set DNS: %s", esp_err_to_name(ret));
      }
    }

    ESP_LOGI(TAG, "Static IP configured: %d.%d.%d.%d",
             m_config.staticIp.addr[0], m_config.staticIp.addr[1],
             m_config.staticIp.addr[2], m_config.staticIp.addr[3]);
    return ESP_OK;
  }
}
