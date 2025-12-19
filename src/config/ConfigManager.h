#pragma once

#include "../network/NetworkTypes.h"
#include "driver/uart.h"
#include <cstddef>
#include <cstdint>

/**
 * @brief Unified Configuration Manager
 *
 * Single source of truth for all system configuration.
 * Manages NVS persistence, validation, defaults, and JSON import/export.
 */

namespace ConfigManager {

/// Device type enumeration
enum class DeviceType : uint8_t { COORDINADOR = 0, ENDPOINT = 1 };

/// Data source for endpoint devices
enum class DataSource : uint8_t { DESHABILITADO = 0, SERIE = 1, PARALELO = 2 };

/// Physical interface for serial communication
enum class PhysicalInterface : uint8_t { RS232 = 0, RS485 = 1 };

/// Complete unified configuration structure
struct FullConfig {
  uint32_t version = 2; // Configuration version for migration
  uint32_t crc32 = 0;   // CRC32 for integrity verification

  // Device Configuration
  struct {
    DeviceType type = DeviceType::COORDINADOR;
    char name[32] = "DataLogger"; // REQUIRED
    char id[16] = "";             // Auto-generated from W5500 MAC
  } device;

  // Network Configuration
  struct {
    // LAN (Ethernet W5500)
    struct {
      bool enabled = true;
      bool useDhcp = false;
      Network::IpAddress staticIp = {192, 168, 29, 10};
      Network::IpAddress netmask = {255, 255, 255, 0};
      Network::IpAddress gateway = {192, 168, 29, 1};
    } lan;

    // WLAN-OP (Station Mode)
    struct {
      bool enabled = false;
      char ssid[33] = "";     // REQUIRED if enabled
      char password[65] = ""; // REQUIRED if enabled
      bool useDhcp = true;
      Network::IpAddress staticIp = {192, 168, 1, 50};
      Network::IpAddress netmask = {255, 255, 255, 0};
      Network::IpAddress gateway = {192, 168, 1, 1};
    } wlanOp;

    // WLAN-SAFE (Access Point Mode - Always Active)
    struct {
      char ssid[33] = "DataLogger-AP"; // REQUIRED
      char password[65] = "12345678";  // REQUIRED
      uint8_t channel = 6;             // 1-11, REQUIRED
      bool hidden = false;
      Network::IpAddress apIp = {192, 168, 4, 1}; // REQUIRED
    } wlanSafe;

    uint16_t webServerPort = 80;
  } network;

  // Endpoint Configuration (only if device.type == ENDPOINT)
  struct {
    char hostName[32] = "Device01"; // REQUIRED if ENDPOINT
    DataSource source = DataSource::DESHABILITADO;
    struct {
      PhysicalInterface interface = PhysicalInterface::RS232;
      uint32_t baudRate = 115200; // REQUIRED if source == SERIE
      uint8_t dataBits = 8;       // 5-8
      uart_parity_t parity = UART_PARITY_DISABLE;
      uart_stop_bits_t stopBits = UART_STOP_BITS_1;
    } serial;
  } endpoint;

  // MQTT Configuration (only if device.type == ENDPOINT)
  struct {
    char host[64] = "mqtt.example.com"; // REQUIRED if ENDPOINT
    uint16_t port = 1883;               // REQUIRED if ENDPOINT (1-65535)
    uint8_t qos = 1;                    // Quality of Service (0, 1 or 2)
    bool useAuth = false;
    char username[32] = "";                     // REQUIRED if useAuth
    char password[64] = "";                     // REQUIRED if useAuth
    char topicPub[64] = "datalogger/telemetry"; // REQUIRED if ENDPOINT
    char topicSub[64] = "datalogger/commands";  // REQUIRED if ENDPOINT
  } mqtt;

  // Web User Credentials
  struct {
    char username[32] = "admin"; // REQUIRED
    char password[32] = "admin"; // REQUIRED
  } webUser;
};

/**
 * @brief Initialize configuration manager
 * Loads configuration from NVS or creates default if not found
 * @return ESP_OK on success
 */
esp_err_t init();

/**
 * @brief Load configuration from NVS
 * @param config Output structure to fill
 * @return ESP_OK on success
 */
esp_err_t load(FullConfig *config);

/**
 * @brief Save configuration to NVS
 * Validates and calculates CRC before saving
 * @param config Configuration to save
 * @return ESP_OK on success
 */
esp_err_t save(const FullConfig *config);

/**
 * @brief Restore configuration to factory defaults
 * @return ESP_OK on success
 */
esp_err_t restore();

/**
 * @brief Get safe mode flag
 * @return true if safe mode is enabled
 */
bool getSafeMode();

/**
 * @brief Set safe mode flag
 * @param enabled true to enable safe mode on next boot
 * @return ESP_OK on success
 */
esp_err_t setSafeMode(bool enabled);

/**
 * @brief Get default configuration
 * @return Default configuration structure
 */
FullConfig getDefaultConfig();

/**
 * @brief Validate configuration
 * Checks all required fields and replaces invalid values with defaults
 * @param config Configuration to validate
 * @param applyDefaults If true, replace invalid fields with defaults
 * @return true if configuration is valid (or was corrected)
 */
bool validateConfig(FullConfig *config, bool applyDefaults = true);

/**
 * @brief Validate IP address
 * @param ip IP address to validate
 * @return true if valid (not 0.0.0.0, not broadcast)
 */
bool validateIpAddress(const Network::IpAddress &ip);

/**
 * @brief Validate netmask
 * @param mask Netmask to validate
 * @return true if valid (contiguous bits)
 */
bool validateNetmask(const Network::IpAddress &mask);

/**
 * @brief Validate port number
 * @param port Port to validate
 * @return true if valid (1-65535)
 */
bool validatePort(uint16_t port);

/**
 * @brief Validate WiFi channel
 * @param channel Channel to validate
 * @return true if valid (1-11)
 */
bool validateChannel(uint8_t channel);

/**
 * @brief Generate unique device ID from W5500 MAC address
 * @param idOut Output buffer for ID string
 * @param maxLen Maximum length of output buffer
 */
void generateDeviceId(char *idOut, size_t maxLen);

/**
 * @brief Calculate CRC32 of configuration
 * @param config Configuration to calculate CRC for
 * @return CRC32 value
 */
uint32_t calculateCrc32(const FullConfig *config);

/**
 * @brief Export configuration to JSON string
 * @param config Configuration to export
 * @return Allocated JSON string (caller must free)
 */
char *toJson(const FullConfig *config);

/**
 * @brief Import configuration from JSON string
 * @param json JSON string to parse
 * @param config Output configuration structure
 * @return ESP_OK on success
 */
esp_err_t fromJson(const char *json, FullConfig *config);

// ============== Legacy API (Deprecated) ==============
// Kept for backward compatibility, will be removed in future version

struct UartConfig {
  uart_port_t uartPort = UART_NUM_2;
  int rxPin = 16;
  int txPin = 17;
  uint32_t baudRate = 115200;
  uart_word_length_t dataBits = UART_DATA_8_BITS;
  uart_parity_t parity = UART_PARITY_DISABLE;
  uart_stop_bits_t stopBits = UART_STOP_BITS_1;
  size_t rxBufSize = 32 * 1024;
  size_t ringBufSize = 64 * 1024;
  uint32_t timeoutMs = 100;
};

struct EthernetConfig {
  int spiHost = 2;
  int mosiPin = 23;
  int misoPin = 19;
  int sclkPin = 18;
  int csPin = 21;
  int resetPin = 22;
  int interruptPin = 25;
  int clockSpeedHz = 20 * 1000 * 1000;
  Network::IpMode ipMode = Network::IpMode::DHCP;
  Network::IpAddress staticIp = {192, 168, 1, 100};
  Network::IpAddress staticNetmask = {255, 255, 255, 0};
  Network::IpAddress staticGateway = {192, 168, 1, 1};
  Network::IpAddress staticDns = {8, 8, 8, 8};
};

struct WifiConfig {
  bool enabled = false;
  char ssid[32] = "DataLogger";
  char password[64] = "password";
  bool apMode = false;
  Network::IpMode ipMode = Network::IpMode::DHCP;
  Network::IpAddress staticIp = {192, 168, 1, 50};
  Network::IpAddress staticNetmask = {255, 255, 255, 0};
  Network::IpAddress staticGateway = {192, 168, 1, 1};
  char apSsid[32] = "DataLoggerAP";
  char apPassword[64] = {0};
  uint8_t apChannel = 1;
  uint8_t apMaxConnections = 4;
};

struct NetworkConfig {
  Network::Type type = Network::Type::ETHERNET;
  uint16_t webServerPort = 80;
};

struct UserCredentials {
  char username[32] = "admin";
  char password[32] = "admin";
};

// Legacy functions - use new FullConfig API instead
esp_err_t getConfig(FullConfig *config);        // Alias for load()
esp_err_t saveConfig(const FullConfig *config); // Alias for save()
esp_err_t getNetworkConfig(NetworkConfig *config);
esp_err_t saveNetworkConfig(const NetworkConfig *config);

} // namespace ConfigManager
