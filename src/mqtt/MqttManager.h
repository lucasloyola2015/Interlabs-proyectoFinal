#pragma once

#include "esp_err.h"
#include "MqttClient.h"
#include "config/ConfigManager.h"
#include <cstdint>
#include <cstddef>

/**
 * @brief MqttManager - High-level MQTT communication manager
 * 
 * Provides a simplified API for MQTT communication with automatic JSON formatting.
 * Handles connection management, message formatting, and telemetry publishing.
 * 
 * Usage:
 *   MqttManager manager;
 *   manager.init();
 *   manager.connect();
 *   manager.sendTelemetry("temperature", 25.5f);
 *   manager.sendStatus("online");
 */
class MqttManager {
public:
  /**
   * @brief Telemetry data structure
   */
  struct TelemetryData {
    const char* key;
    union {
      float fValue;
      int32_t iValue;
      bool bValue;
      const char* sValue;
    } value;
    enum Type { FLOAT, INT, BOOL, STRING } type;
  };

  /**
   * @brief Constructor
   */
  MqttManager();

  /**
   * @brief Destructor
   */
  ~MqttManager();

  /**
   * @brief Initialize the MQTT manager
   * Loads configuration and initializes the underlying MqttClient
   * @return ESP_OK on success
   */
  esp_err_t init();

  /**
   * @brief Connect to MQTT broker
   * @return ESP_OK on success
   */
  esp_err_t connect();

  /**
   * @brief Disconnect from MQTT broker
   * @return ESP_OK on success
   */
  esp_err_t disconnect();

  /**
   * @brief Check if connected to broker
   * @return true if connected
   */
  bool isConnected() const;

  /**
   * @brief Send telemetry data (single key-value pair)
   * @param key Key name
   * @param value Float value
   * @return ESP_OK on success
   */
  esp_err_t sendTelemetry(const char* key, float value);

  /**
   * @brief Send telemetry data (single key-value pair)
   * @param key Key name
   * @param value Integer value
   * @return ESP_OK on success
   */
  esp_err_t sendTelemetry(const char* key, int32_t value);

  /**
   * @brief Send telemetry data (single key-value pair)
   * @param key Key name
   * @param value Boolean value
   * @return ESP_OK on success
   */
  esp_err_t sendTelemetry(const char* key, bool value);

  /**
   * @brief Send telemetry data (single key-value pair)
   * @param key Key name
   * @param value String value
   * @return ESP_OK on success
   */
  esp_err_t sendTelemetry(const char* key, const char* value);

  /**
   * @brief Send telemetry data with timestamp
   * @param key Key name
   * @param value Float value
   * @param timestamp Unix timestamp (0 = use current time)
   * @return ESP_OK on success
   */
  esp_err_t sendTelemetry(const char* key, float value, int64_t timestamp);

  /**
   * @brief Send telemetry data with timestamp
   * @param key Key name
   * @param value Integer value
   * @param timestamp Unix timestamp (0 = use current time)
   * @return ESP_OK on success
   */
  esp_err_t sendTelemetry(const char* key, int32_t value, int64_t timestamp);

  /**
   * @brief Send multiple telemetry data points in a single message
   * @param data Array of telemetry data
   * @param count Number of data points
   * @param timestamp Unix timestamp (0 = use current time)
   * @return ESP_OK on success
   */
  esp_err_t sendTelemetry(const TelemetryData* data, size_t count, int64_t timestamp = 0);

  /**
   * @brief Send device status message
   * @param status Status string (e.g., "online", "offline", "error")
   * @return ESP_OK on success
   */
  esp_err_t sendStatus(const char* status);

  /**
   * @brief Send custom JSON message
   * @param json JSON string (must be valid JSON)
   * @return ESP_OK on success
   */
  esp_err_t sendJson(const char* json);

  /**
   * @brief Send custom JSON message with topic override
   * @param topic Topic to publish to
   * @param json JSON string (must be valid JSON)
   * @return ESP_OK on success
   */
  esp_err_t sendJson(const char* topic, const char* json);

  /**
   * @brief Set callback for received messages
   * @param callback Function to call when a message is received
   */
  void setMessageCallback(MqttClient::MessageCallback callback);

  /**
   * @brief Set callback for connection events
   * @param callback Function to call when connection state changes
   */
  void setConnectionCallback(MqttClient::ConnectionCallback callback);

  /**
   * @brief Reload configuration from ConfigManager
   * @return ESP_OK on success
   */
  esp_err_t reloadConfig();

private:
  /**
   * @brief Format telemetry data to JSON
   * @param data Array of telemetry data
   * @param count Number of data points
   * @param timestamp Unix timestamp (0 = use current time)
   * @param buffer Output buffer
   * @param bufferSize Size of output buffer
   * @return Number of bytes written (excluding null terminator)
   */
  size_t formatJson(const TelemetryData* data, size_t count, int64_t timestamp, char* buffer, size_t bufferSize);

  /**
   * @brief Get current Unix timestamp
   * @return Unix timestamp in seconds
   */
  int64_t getCurrentTimestamp() const;

  MqttClient m_client;           ///< Underlying MQTT client
  bool m_initialized;            ///< Initialization flag
  char m_jsonBuffer[1024];       ///< Buffer for JSON formatting
  char m_deviceId[16];           ///< Device ID from NVS
  char m_deviceName[32];         ///< Device name from NVS
};
