#include "WifiInterface.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/ip4_addr.h"
#include <cstring>

static const char *TAG = "WifiInterface";

esp_err_t WifiInterface::init(const void *config) {
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

  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &wifiEventHandler, this, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &wifiEventHandler, this, nullptr));

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

  m_initialized = true;
  return ESP_OK;
}

esp_err_t WifiInterface::start() {
  if (!m_initialized) {
    return ESP_ERR_INVALID_STATE;
  }

  wifi_config_t wifi_config = {};
  wifi_mode_t mode = WIFI_MODE_STA;

  if (m_config.apMode) {
    // Access Point mode
    mode = WIFI_MODE_AP;
    strncpy((char *)wifi_config.ap.ssid, m_config.apSsid,
            sizeof(wifi_config.ap.ssid) - 1);
    wifi_config.ap.ssid_len = strlen(m_config.apSsid);
    if (strlen(m_config.apPassword) >= 8) {
      strncpy((char *)wifi_config.ap.password, m_config.apPassword,
              sizeof(wifi_config.ap.password) - 1);
      wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
      wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }
    wifi_config.ap.channel = m_config.apChannel;
    wifi_config.ap.max_connection = m_config.apMaxConnections;
  } else {
    // Station mode
    mode = WIFI_MODE_STA;
    strncpy((char *)wifi_config.sta.ssid, m_config.ssid,
            sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, m_config.password,
            sizeof(wifi_config.sta.password) - 1);
    ESP_LOGI(TAG, "Connecting to SSID: '%s'", m_config.ssid);
  }

  // Create default netif if not exists (MUST be before esp_wifi_start)
  if (!m_netif) {
    if (mode == WIFI_MODE_STA)
      m_netif = esp_netif_create_default_wifi_sta();
    else
      m_netif = esp_netif_create_default_wifi_ap();
  }

  // Configure static IP if requested (STA mode only)
  if (mode == WIFI_MODE_STA && m_config.ipMode == Network::IpMode::STATIC &&
      m_netif) {
    esp_netif_dhcpc_stop(m_netif);
    esp_netif_ip_info_t ip_info = {};
    IP4_ADDR(&ip_info.ip, m_config.staticIp.addr[0], m_config.staticIp.addr[1],
             m_config.staticIp.addr[2], m_config.staticIp.addr[3]);
    IP4_ADDR(&ip_info.netmask, m_config.staticNetmask.addr[0],
             m_config.staticNetmask.addr[1], m_config.staticNetmask.addr[2],
             m_config.staticNetmask.addr[3]);
    IP4_ADDR(&ip_info.gw, m_config.staticGateway.addr[0],
             m_config.staticGateway.addr[1], m_config.staticGateway.addr[2],
             m_config.staticGateway.addr[3]);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(m_netif, &ip_info));
    ESP_LOGI(TAG, "WiFi static IP configured: %d.%d.%d.%d",
             m_config.staticIp.addr[0], m_config.staticIp.addr[1],
             m_config.staticIp.addr[2], m_config.staticIp.addr[3]);
  }

  ESP_ERROR_CHECK(esp_wifi_set_mode(mode));
  if (mode == WIFI_MODE_STA) {
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  } else {
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
  }

  ESP_ERROR_CHECK(esp_wifi_start());

  // Hostname setup
  if (m_netif) {
    esp_netif_set_hostname(m_netif, "datalogger-wifi");
  }

  ESP_LOGI(TAG, "WiFi started in %s mode",
           mode == WIFI_MODE_STA ? "STA" : "AP");
  return ESP_OK;
}

esp_err_t WifiInterface::stop() {
  if (!m_initialized)
    return ESP_ERR_INVALID_STATE;
  esp_wifi_stop();
  m_status = Network::Status::DISCONNECTED;
  return ESP_OK;
}

esp_err_t WifiInterface::deinit() {
  stop();
  esp_wifi_deinit();
  if (m_netif) {
    esp_netif_destroy(m_netif);
    m_netif = nullptr;
  }
  m_initialized = false;
  return ESP_OK;
}

Network::Status WifiInterface::getStatus() const { return m_status; }

esp_netif_t *WifiInterface::getNetif() { return m_netif; }

esp_err_t WifiInterface::getIpAddress(Network::IpAddress *ip) {
  if (!ip || !m_netif)
    return ESP_FAIL;
  esp_netif_ip_info_t ip_info;
  if (esp_netif_get_ip_info(m_netif, &ip_info) == ESP_OK) {
    ip->addr[0] = esp_ip4_addr1_16(&ip_info.ip);
    ip->addr[1] = esp_ip4_addr2_16(&ip_info.ip);
    ip->addr[2] = esp_ip4_addr3_16(&ip_info.ip);
    ip->addr[3] = esp_ip4_addr4_16(&ip_info.ip);
    return ESP_OK;
  }
  return ESP_FAIL;
}

esp_err_t WifiInterface::getStats(Network::Stats *stats) {
  // Basic stats placeholder
  if (stats)
    memset(stats, 0, sizeof(Network::Stats));
  return ESP_OK;
}

void WifiInterface::wifiEventHandler(void *arg, esp_event_base_t eventBase,
                                     int32_t eventId, void *eventData) {
  WifiInterface *self = static_cast<WifiInterface *>(arg);
  self->onWifiEvent(eventBase, eventId, eventData);
}

void WifiInterface::onWifiEvent(esp_event_base_t eventBase, int32_t eventId,
                                void *eventData) {
  if (eventBase == WIFI_EVENT && eventId == WIFI_EVENT_STA_START) {
    ESP_LOGI(TAG, "WiFi Started, auto-connecting...");
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK)
      ESP_LOGE(TAG, "Connect failed: %s", esp_err_to_name(err));
  } else if (eventBase == WIFI_EVENT && eventId == WIFI_EVENT_STA_CONNECTED) {
    ESP_LOGI(TAG, "WiFi Connected to AP. Waiting for IP...");
  } else if (eventBase == WIFI_EVENT &&
             eventId == WIFI_EVENT_STA_DISCONNECTED) {
    wifi_event_sta_disconnected_t *event =
        (wifi_event_sta_disconnected_t *)eventData;
    m_status = Network::Status::DISCONNECTED;
    ESP_LOGW(TAG, "WiFi disconnected (Reason: %d), retrying...", event->reason);
    esp_wifi_connect();
  } else if (eventBase == IP_EVENT && eventId == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)eventData;
    ESP_LOGI(TAG, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
    m_status = Network::Status::CONNECTED;
  }
}

// Unused helpers
esp_err_t WifiInterface::initWifi() { return ESP_OK; }
esp_err_t WifiInterface::configureIp() { return ESP_OK; }
