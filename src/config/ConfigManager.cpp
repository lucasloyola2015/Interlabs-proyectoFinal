#include "ConfigManager.h"
#include "esp_crc.h"
#include "esp_efuse.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

static const char *TAG = "ConfigManager";

// NVS namespace and key
static const char *NVS_NAMESPACE = "appconfig";
static const char *NVS_KEY_FULLCONFIG = "fullconfig";

// Configuration version
static const uint32_t CONFIG_VERSION = 3;

// Current configuration (cached in RAM)
static ConfigManager::FullConfig s_config;
static bool s_initialized = false;

namespace ConfigManager {

// ============== Default Configuration ==============

FullConfig getDefaultConfig() {
  FullConfig config;
  config.version = CONFIG_VERSION;
  config.crc32 = 0;

  // Device defaults
  config.device.type = DeviceType::COORDINADOR;
  strncpy(config.device.name, "DataLogger", sizeof(config.device.name) - 1);
  config.device.id[0] = '\0'; // Will be generated

  // Network - LAN defaults
  config.network.lan.enabled = true;
  config.network.lan.useDhcp = false;
  config.network.lan.staticIp = {192, 168, 29, 10};
  config.network.lan.netmask = {255, 255, 255, 0};
  config.network.lan.gateway = {192, 168, 29, 1};

  // Network - WLAN-OP defaults
  config.network.wlanOp.enabled = false;
  config.network.wlanOp.ssid[0] = '\0';
  config.network.wlanOp.password[0] = '\0';
  config.network.wlanOp.useDhcp = true;
  config.network.wlanOp.staticIp = {192, 168, 1, 50};
  config.network.wlanOp.netmask = {255, 255, 255, 0};
  config.network.wlanOp.gateway = {192, 168, 1, 1};

  // Network - WLAN-SAFE defaults
  strncpy(config.network.wlanSafe.ssid, "DataLogger-AP",
          sizeof(config.network.wlanSafe.ssid) - 1);
  strncpy(config.network.wlanSafe.password, "12345678",
          sizeof(config.network.wlanSafe.password) - 1);
  config.network.wlanSafe.channel = 6;
  config.network.wlanSafe.hidden = false;
  config.network.wlanSafe.apIp = {192, 168, 4, 1};

  config.network.webServerPort = 80;

  // Endpoint defaults
  strncpy(config.endpoint.hostName, "Device01",
          sizeof(config.endpoint.hostName) - 1);
  config.endpoint.source = DataSource::DESHABILITADO;
  config.endpoint.serial.interface = PhysicalInterface::RS232;
  config.endpoint.serial.baudRate = 115200;
  config.endpoint.serial.dataBits = 8;
  config.endpoint.serial.parity = UART_PARITY_DISABLE;
  config.endpoint.serial.stopBits = UART_STOP_BITS_1;

  // MQTT defaults
  strncpy(config.mqtt.host, "mqtt.example.com", sizeof(config.mqtt.host) - 1);
  config.mqtt.port = 1883;
  config.mqtt.useAuth = false;
  config.mqtt.username[0] = '\0';
  config.mqtt.password[0] = '\0';
  strncpy(config.mqtt.topicPub, "datalogger/telemetry",
          sizeof(config.mqtt.topicPub) - 1);
  strncpy(config.mqtt.topicSub, "datalogger/commands",
          sizeof(config.mqtt.topicSub) - 1);

  // Web user defaults
  strncpy(config.webUser.username, "admin",
          sizeof(config.webUser.username) - 1);
  strncpy(config.webUser.password, "admin",
          sizeof(config.webUser.password) - 1);

  return config;
}

// ============== Validation Functions ==============

bool validateIpAddress(const Network::IpAddress &ip) {
  // Check for 0.0.0.0
  if (ip.addr[0] == 0 && ip.addr[1] == 0 && ip.addr[2] == 0 &&
      ip.addr[3] == 0) {
    return false;
  }
  // Check for broadcast 255.255.255.255
  if (ip.addr[0] == 255 && ip.addr[1] == 255 && ip.addr[2] == 255 &&
      ip.addr[3] == 255) {
    return false;
  }
  return true;
}

bool validateNetmask(const Network::IpAddress &mask) {
  // Convert to 32-bit value
  uint32_t m = ((uint32_t)mask.addr[0] << 24) | ((uint32_t)mask.addr[1] << 16) |
               ((uint32_t)mask.addr[2] << 8) | (uint32_t)mask.addr[3];

  // Check if bits are contiguous (valid netmask)
  // A valid netmask has all 1s followed by all 0s
  // Invert and add 1, should be a power of 2
  uint32_t inverted = ~m;
  return ((inverted & (inverted + 1)) == 0);
}

bool validatePort(uint16_t port) { return (port >= 1 && port <= 65535); }

bool validateChannel(uint8_t channel) {
  return (channel >= 1 && channel <= 11);
}

bool validateConfig(FullConfig *config, bool applyDefaults) {
  bool isValid = true;
  FullConfig defaults = getDefaultConfig();

  // Validate device name (always required)
  if (strlen(config->device.name) == 0) {
    ESP_LOGW(TAG, "Empty device name, using default: %s", defaults.device.name);
    strncpy(config->device.name, defaults.device.name,
            sizeof(config->device.name) - 1);
    isValid = false;
  }

  // Validate LAN configuration (if enabled)
  if (config->network.lan.enabled && !config->network.lan.useDhcp) {
    if (!validateIpAddress(config->network.lan.staticIp)) {
      ESP_LOGW(TAG, "Invalid LAN IP, using default: %d.%d.%d.%d",
               defaults.network.lan.staticIp.addr[0],
               defaults.network.lan.staticIp.addr[1],
               defaults.network.lan.staticIp.addr[2],
               defaults.network.lan.staticIp.addr[3]);
      if (applyDefaults)
        config->network.lan.staticIp = defaults.network.lan.staticIp;
      isValid = false;
    }
    if (!validateNetmask(config->network.lan.netmask)) {
      ESP_LOGW(TAG, "Invalid LAN netmask, using default: %d.%d.%d.%d",
               defaults.network.lan.netmask.addr[0],
               defaults.network.lan.netmask.addr[1],
               defaults.network.lan.netmask.addr[2],
               defaults.network.lan.netmask.addr[3]);
      if (applyDefaults)
        config->network.lan.netmask = defaults.network.lan.netmask;
      isValid = false;
    }
    if (!validateIpAddress(config->network.lan.gateway)) {
      ESP_LOGW(TAG, "Invalid LAN gateway, using default: %d.%d.%d.%d",
               defaults.network.lan.gateway.addr[0],
               defaults.network.lan.gateway.addr[1],
               defaults.network.lan.gateway.addr[2],
               defaults.network.lan.gateway.addr[3]);
      if (applyDefaults)
        config->network.lan.gateway = defaults.network.lan.gateway;
      isValid = false;
    }
  }

  // Validate WLAN-OP configuration (if enabled)
  if (config->network.wlanOp.enabled) {
    if (strlen(config->network.wlanOp.ssid) == 0) {
      ESP_LOGE(TAG, "Empty WLAN-OP SSID (required when enabled)");
      if (applyDefaults) {
        config->network.wlanOp.enabled = false; // Disable if no SSID
      }
      isValid = false;
    }
    if (strlen(config->network.wlanOp.password) == 0) {
      ESP_LOGE(TAG, "Empty WLAN-OP password (required when enabled)");
      if (applyDefaults) {
        config->network.wlanOp.enabled = false; // Disable if no password
      }
      isValid = false;
    }
    if (!config->network.wlanOp.useDhcp) {
      if (!validateIpAddress(config->network.wlanOp.staticIp)) {
        ESP_LOGW(TAG, "Invalid WLAN-OP IP, using default");
        if (applyDefaults)
          config->network.wlanOp.staticIp = defaults.network.wlanOp.staticIp;
        isValid = false;
      }
      if (!validateNetmask(config->network.wlanOp.netmask)) {
        ESP_LOGW(TAG, "Invalid WLAN-OP netmask, using default");
        if (applyDefaults)
          config->network.wlanOp.netmask = defaults.network.wlanOp.netmask;
        isValid = false;
      }
      if (!validateIpAddress(config->network.wlanOp.gateway)) {
        ESP_LOGW(TAG, "Invalid WLAN-OP gateway, using default");
        if (applyDefaults)
          config->network.wlanOp.gateway = defaults.network.wlanOp.gateway;
        isValid = false;
      }
    }
  }

  // Validate WLAN-SAFE (always active)
  if (strlen(config->network.wlanSafe.ssid) == 0) {
    ESP_LOGW(TAG, "Empty WLAN-SAFE SSID, using default: %s",
             defaults.network.wlanSafe.ssid);
    if (applyDefaults)
      strncpy(config->network.wlanSafe.ssid, defaults.network.wlanSafe.ssid,
              sizeof(config->network.wlanSafe.ssid) - 1);
    isValid = false;
  }
  if (strlen(config->network.wlanSafe.password) == 0) {
    ESP_LOGW(TAG, "Empty WLAN-SAFE password, using default");
    if (applyDefaults)
      strncpy(config->network.wlanSafe.password,
              defaults.network.wlanSafe.password,
              sizeof(config->network.wlanSafe.password) - 1);
    isValid = false;
  }
  if (!validateChannel(config->network.wlanSafe.channel)) {
    ESP_LOGW(TAG, "Invalid WLAN-SAFE channel (%d), using default: %d",
             config->network.wlanSafe.channel,
             defaults.network.wlanSafe.channel);
    if (applyDefaults)
      config->network.wlanSafe.channel = defaults.network.wlanSafe.channel;
    isValid = false;
  }
  if (!validateIpAddress(config->network.wlanSafe.apIp)) {
    ESP_LOGW(TAG, "Invalid WLAN-SAFE AP IP, using default");
    if (applyDefaults)
      config->network.wlanSafe.apIp = defaults.network.wlanSafe.apIp;
    isValid = false;
  }

  // Validate web server port
  if (!validatePort(config->network.webServerPort)) {
    ESP_LOGW(TAG, "Invalid web server port (%d), using default: %d",
             config->network.webServerPort, defaults.network.webServerPort);
    if (applyDefaults)
      config->network.webServerPort = defaults.network.webServerPort;
    isValid = false;
  }

  // Validate endpoint configuration (if device is ENDPOINT)
  if (config->device.type == DeviceType::ENDPOINT) {
    if (strlen(config->endpoint.hostName) == 0) {
      ESP_LOGW(TAG, "Empty endpoint host name, using default: %s",
               defaults.endpoint.hostName);
      if (applyDefaults)
        strncpy(config->endpoint.hostName, defaults.endpoint.hostName,
                sizeof(config->endpoint.hostName) - 1);
      isValid = false;
    }

    // Validate serial configuration (if source is SERIE)
    if (config->endpoint.source == DataSource::SERIE) {
      if (config->endpoint.serial.baudRate < 9600 ||
          config->endpoint.serial.baudRate > 921600) {
        ESP_LOGW(TAG, "Invalid baud rate (%lu), using default: %lu",
                 config->endpoint.serial.baudRate,
                 defaults.endpoint.serial.baudRate);
        if (applyDefaults)
          config->endpoint.serial.baudRate = defaults.endpoint.serial.baudRate;
        isValid = false;
      }
      if (config->endpoint.serial.dataBits < 5 ||
          config->endpoint.serial.dataBits > 8) {
        ESP_LOGW(TAG, "Invalid data bits (%d), using default: %d",
                 config->endpoint.serial.dataBits,
                 defaults.endpoint.serial.dataBits);
        if (applyDefaults)
          config->endpoint.serial.dataBits = defaults.endpoint.serial.dataBits;
        isValid = false;
      }
    }

    // Validate MQTT configuration (required for ENDPOINT)
    if (strlen(config->mqtt.host) == 0) {
      ESP_LOGE(TAG,
               "Empty MQTT host (required for ENDPOINT), using default: %s",
               defaults.mqtt.host);
      if (applyDefaults)
        strncpy(config->mqtt.host, defaults.mqtt.host,
                sizeof(config->mqtt.host) - 1);
      isValid = false;
    }
    if (!validatePort(config->mqtt.port)) {
      ESP_LOGW(TAG, "Invalid MQTT port (%d), using default: %d",
               config->mqtt.port, defaults.mqtt.port);
      if (applyDefaults)
        config->mqtt.port = defaults.mqtt.port;
      isValid = false;
    }
    if (strlen(config->mqtt.topicPub) == 0) {
      ESP_LOGW(TAG, "Empty MQTT pub topic, using default: %s",
               defaults.mqtt.topicPub);
      if (applyDefaults)
        strncpy(config->mqtt.topicPub, defaults.mqtt.topicPub,
                sizeof(config->mqtt.topicPub) - 1);
      isValid = false;
    }
    if (strlen(config->mqtt.topicSub) == 0) {
      ESP_LOGW(TAG, "Empty MQTT sub topic, using default: %s",
               defaults.mqtt.topicSub);
      if (applyDefaults)
        strncpy(config->mqtt.topicSub, defaults.mqtt.topicSub,
                sizeof(config->mqtt.topicSub) - 1);
      isValid = false;
    }

    // Validate MQTT auth credentials (if auth is enabled)
    if (config->mqtt.useAuth) {
      if (strlen(config->mqtt.username) == 0) {
        ESP_LOGE(TAG, "Empty MQTT username (required when auth enabled)");
        if (applyDefaults)
          config->mqtt.useAuth = false; // Disable auth if no username
        isValid = false;
      }
      if (strlen(config->mqtt.password) == 0) {
        ESP_LOGE(TAG, "Empty MQTT password (required when auth enabled)");
        if (applyDefaults)
          config->mqtt.useAuth = false; // Disable auth if no password
        isValid = false;
      }
    }
  }

  // Validate web user credentials (always required)
  if (strlen(config->webUser.username) == 0) {
    ESP_LOGW(TAG, "Empty web username, using default: %s",
             defaults.webUser.username);
    if (applyDefaults)
      strncpy(config->webUser.username, defaults.webUser.username,
              sizeof(config->webUser.username) - 1);
    isValid = false;
  }
  if (strlen(config->webUser.password) == 0) {
    ESP_LOGW(TAG, "Empty web password, using default");
    if (applyDefaults)
      strncpy(config->webUser.password, defaults.webUser.password,
              sizeof(config->webUser.password) - 1);
    isValid = false;
  }

  return isValid;
}

uint32_t calculateCrc32(const FullConfig *config) {
  // Calculate CRC32 of entire structure except the crc32 field itself
  const uint8_t *data = (const uint8_t *)config;
  size_t offset = offsetof(FullConfig, crc32);
  size_t size1 = offset;
  size_t size2 = sizeof(FullConfig) - offset - sizeof(uint32_t);

  uint32_t crc = 0;
  crc = esp_crc32_le(crc, data, size1);
  crc = esp_crc32_le(crc, data + offset + sizeof(uint32_t), size2);
  return crc;
}

// ============== NVS Operations ==============

esp_err_t init() {
  if (s_initialized) {
    ESP_LOGW(TAG, "Already initialized");
    return ESP_OK;
  }

  // Initialize NVS
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_LOGW(TAG, "NVS partition truncated, erasing...");
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize NVS: %s", esp_err_to_name(ret));
    return ret;
  }

  // Load configuration
  ret = load(&s_config);
  if (ret != ESP_OK) {
    ESP_LOGI(TAG, "No valid configuration found, using defaults");
    s_config = getDefaultConfig();

    // Generate device ID if empty
    if (strlen(s_config.device.id) == 0) {
      generateDeviceId(s_config.device.id, sizeof(s_config.device.id));
      ESP_LOGI(TAG, "Generated Device ID: %s", s_config.device.id);
    }

    // Save defaults
    save(&s_config);
  }

  s_initialized = true;
  ESP_LOGI(TAG, "Configuration Manager initialized");
  return ESP_OK;
}

esp_err_t load(FullConfig *config) {
  nvs_handle_t handle;
  esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
  if (ret != ESP_OK) {
    return ret;
  }

  size_t size = sizeof(FullConfig);
  ret = nvs_get_blob(handle, NVS_KEY_FULLCONFIG, config, &size);
  nvs_close(handle);

  if (ret != ESP_OK) {
    return ret;
  }

  // Validate version
  if (config->version != CONFIG_VERSION) {
    ESP_LOGW(TAG,
             "Config version mismatch (Stored: %lu, Current: %lu). Resetting "
             "to defaults.",
             config->version, CONFIG_VERSION);
    return ESP_ERR_INVALID_VERSION;
  }

  // Validate CRC
  uint32_t calculatedCrc = calculateCrc32(config);
  if (config->crc32 != calculatedCrc) {
    ESP_LOGE(
        TAG,
        "CRITICAL: Config CRC mismatch! (stored: 0x%08lX, calculated: 0x%08lX)",
        config->crc32, calculatedCrc);
    return ESP_ERR_INVALID_CRC;
  }

  // Log loaded configuration for debugging
  ESP_LOGI(TAG, "Loaded config: LAN enabled=%d, IP=%d.%d.%d.%d",
           config->network.lan.enabled,
           config->network.lan.staticIp.addr[0],
           config->network.lan.staticIp.addr[1],
           config->network.lan.staticIp.addr[2],
           config->network.lan.staticIp.addr[3]);

  // Validate and fix any invalid fields
  if (!validateConfig(config, true)) {
    ESP_LOGW(TAG, "Configuration had invalid fields, defaults applied");
    // Log after validation
    ESP_LOGI(TAG, "After validation: LAN enabled=%d",
             config->network.lan.enabled);
    // Re-save with corrected values
    save(config);
  }

  ESP_LOGI(TAG, "Configuration loaded successfully");
  return ESP_OK;
}

esp_err_t save(const FullConfig *config) {
  // Create a copy to modify
  FullConfig configCopy = *config;

  // Validate before saving
  if (!validateConfig(&configCopy, true)) {
    ESP_LOGW(TAG, "Configuration corrected before saving");
  }

  // Calculate and set CRC
  configCopy.crc32 = calculateCrc32(&configCopy);

  // Save to NVS
  nvs_handle_t handle;
  esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
    return ret;
  }

  ret =
      nvs_set_blob(handle, NVS_KEY_FULLCONFIG, &configCopy, sizeof(FullConfig));
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to save config: %s", esp_err_to_name(ret));
    nvs_close(handle);
    return ret;
  }

  ret = nvs_commit(handle);
  nvs_close(handle);

  if (ret == ESP_OK) {
    // Update cached config
    s_config = configCopy;
    ESP_LOGI(TAG, "Configuration saved successfully (CRC: 0x%08lX)",
             configCopy.crc32);
  }

  return ret;
}

esp_err_t restore() {
  ESP_LOGW(TAG, "Restoring factory defaults and clearing Safe Mode flag");

  // 1. Clear Safe Mode flag in NVS
  esp_err_t ret = setSafeMode(false);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to clear safe mode flag: %s", esp_err_to_name(ret));
    return ret;
  }

  // 2. Get default values directly into cached config to save stack
  s_config = getDefaultConfig();

  // 3. Generate Device ID
  generateDeviceId(s_config.device.id, sizeof(s_config.device.id));

  // 4. Log the configuration before saving (for debugging)
  ESP_LOGI(TAG, "Factory defaults: LAN enabled=%d, IP=%d.%d.%d.%d",
           s_config.network.lan.enabled,
           s_config.network.lan.staticIp.addr[0],
           s_config.network.lan.staticIp.addr[1],
           s_config.network.lan.staticIp.addr[2],
           s_config.network.lan.staticIp.addr[3]);

  // 5. Save to NVS (save() will create its own copy for validation)
  ret = save(&s_config);
  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "Factory defaults restored successfully");
  } else {
    ESP_LOGE(TAG, "Failed to save factory defaults: %s", esp_err_to_name(ret));
  }

  return ret;
}

void generateDeviceId(char *idOut, size_t maxLen) {
  // Read ESP32 WiFi MAC address
  uint8_t mac[6];
  esp_err_t ret = esp_read_mac(mac, ESP_MAC_WIFI_STA);

  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to read MAC: %s", esp_err_to_name(ret));
    // Fallback to random ID
    snprintf(idOut, maxLen, "ERR%08lX",
             (unsigned long)(esp_random() & 0xFFFFFFFF));
    return;
  }

  // Format as 12-character hex string: AABBCCDDEEFF
  snprintf(idOut, maxLen, "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2],
           mac[3], mac[4], mac[5]);

  ESP_LOGI(TAG, "Device ID generated from WiFi MAC: %s", idOut);
}

// ============== Legacy API (Aliases) ==============

esp_err_t getConfig(FullConfig *config) {
  if (!s_initialized) {
    return ESP_ERR_INVALID_STATE;
  }
  *config = s_config;
  return ESP_OK;
}

esp_err_t saveConfig(const FullConfig *config) { return save(config); }

esp_err_t getNetworkConfig(NetworkConfig *config) {
  if (!s_initialized) {
    return ESP_ERR_INVALID_STATE;
  }
  config->type = Network::Type::ETHERNET; // Legacy
  config->webServerPort = s_config.network.webServerPort;
  return ESP_OK;
}

esp_err_t saveNetworkConfig(const NetworkConfig *config) {
  s_config.network.webServerPort = config->webServerPort;
  return save(&s_config);
}

// ============== Safe Mode Management ==============

static const char *NVS_NAMESPACE_SAFE = "safemode";
static const char *NVS_KEY_SAFE_FLAG = "enabled";

bool getSafeMode() {
  nvs_handle_t handle;
  esp_err_t ret = nvs_open(NVS_NAMESPACE_SAFE, NVS_READONLY, &handle);
  if (ret != ESP_OK) {
    // If namespace doesn't exist, safe mode is not enabled
    return false;
  }

  uint8_t safeMode = 0;
  ret = nvs_get_u8(handle, NVS_KEY_SAFE_FLAG, &safeMode);
  nvs_close(handle);

  if (ret != ESP_OK) {
    return false;
  }

  return (safeMode != 0);
}

esp_err_t setSafeMode(bool enabled) {
  nvs_handle_t handle;
  esp_err_t ret = nvs_open(NVS_NAMESPACE_SAFE, NVS_READWRITE, &handle);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open NVS for safe mode: %s", esp_err_to_name(ret));
    return ret;
  }

  uint8_t value = enabled ? 1 : 0;
  ret = nvs_set_u8(handle, NVS_KEY_SAFE_FLAG, value);
  if (ret == ESP_OK) {
    ret = nvs_commit(handle);
    if (ret == ESP_OK) {
      ESP_LOGI(TAG, "Safe mode flag set to: %s", enabled ? "ON" : "OFF");
    }
  }

  nvs_close(handle);
  return ret;
}

} // namespace ConfigManager
