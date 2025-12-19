#include "MqttManager.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "time.h"
#include <cstring>
#include <cmath>

static const char *TAG = "MqttManager";

MqttManager::MqttManager() : m_initialized(false) {
  m_jsonBuffer[0] = '\0';
  m_deviceId[0] = '\0';
  m_deviceName[0] = '\0';
}

MqttManager::~MqttManager() {
  if (m_initialized) {
    disconnect();
  }
}

esp_err_t MqttManager::init() {
  if (m_initialized) {
    ESP_LOGW(TAG, "MqttManager ya está inicializado");
    return ESP_OK;
  }

  // Load device info from ConfigManager
  ConfigManager::FullConfig config;
  esp_err_t ret = ConfigManager::getConfig(&config);
  if (ret == ESP_OK) {
    strncpy(m_deviceId, config.device.id, sizeof(m_deviceId) - 1);
    m_deviceId[sizeof(m_deviceId) - 1] = '\0';
    strncpy(m_deviceName, config.device.name, sizeof(m_deviceName) - 1);
    m_deviceName[sizeof(m_deviceName) - 1] = '\0';
    ESP_LOGI(TAG, "Device info cargado: ID=%s, Name=%s", m_deviceId, m_deviceName);
  } else {
    ESP_LOGW(TAG, "No se pudo cargar configuración del dispositivo, usando valores por defecto");
    m_deviceId[0] = '\0';
    strncpy(m_deviceName, "DataLogger", sizeof(m_deviceName) - 1);
    m_deviceName[sizeof(m_deviceName) - 1] = '\0';
  }

  // Initialize underlying MqttClient
  ret = m_client.init();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Error al inicializar MqttClient: %s", esp_err_to_name(ret));
    return ret;
  }

  m_initialized = true;
  ESP_LOGI(TAG, "MqttManager inicializado correctamente");
  return ESP_OK;
}

esp_err_t MqttManager::connect() {
  if (!m_initialized) {
    ESP_LOGE(TAG, "MqttManager no inicializado");
    return ESP_ERR_INVALID_STATE;
  }
  return m_client.connect();
}

esp_err_t MqttManager::disconnect() {
  if (!m_initialized) {
    return ESP_ERR_INVALID_STATE;
  }
  return m_client.disconnect();
}

bool MqttManager::isConnected() const {
  return m_initialized && m_client.isConnected();
}

esp_err_t MqttManager::sendTelemetry(const char* key, float value) {
  return sendTelemetry(key, value, 0);
}

esp_err_t MqttManager::sendTelemetry(const char* key, int32_t value) {
  return sendTelemetry(key, value, 0);
}

esp_err_t MqttManager::sendTelemetry(const char* key, bool value) {
  TelemetryData data;
  data.key = key;
  data.value.bValue = value;
  data.type = TelemetryData::BOOL;
  return sendTelemetry(&data, 1, 0);
}

esp_err_t MqttManager::sendTelemetry(const char* key, const char* value) {
  TelemetryData data;
  data.key = key;
  data.value.sValue = value;
  data.type = TelemetryData::STRING;
  return sendTelemetry(&data, 1, 0);
}

esp_err_t MqttManager::sendTelemetry(const char* key, float value, int64_t timestamp) {
  TelemetryData data;
  data.key = key;
  data.value.fValue = value;
  data.type = TelemetryData::FLOAT;
  return sendTelemetry(&data, 1, timestamp);
}

esp_err_t MqttManager::sendTelemetry(const char* key, int32_t value, int64_t timestamp) {
  TelemetryData data;
  data.key = key;
  data.value.iValue = value;
  data.type = TelemetryData::INT;
  return sendTelemetry(&data, 1, timestamp);
}

esp_err_t MqttManager::sendTelemetry(const TelemetryData* data, size_t count, int64_t timestamp) {
  if (!m_initialized || !isConnected()) {
    ESP_LOGW(TAG, "MqttManager no conectado, no se puede enviar telemetría");
    return ESP_ERR_INVALID_STATE;
  }

  if (!data || count == 0) {
    ESP_LOGE(TAG, "Datos de telemetría inválidos");
    return ESP_ERR_INVALID_ARG;
  }

  // Use provided timestamp or current time
  int64_t ts = (timestamp > 0) ? timestamp : getCurrentTimestamp();

  // Format JSON
  size_t jsonLen = formatJson(data, count, ts, m_jsonBuffer, sizeof(m_jsonBuffer));
  if (jsonLen == 0) {
    ESP_LOGE(TAG, "Error al formatear JSON");
    return ESP_FAIL;
  }

  // Publish via MqttClient
  return m_client.publish((const uint8_t*)m_jsonBuffer, jsonLen);
}

esp_err_t MqttManager::sendStatus(const char* status) {
  if (!status) {
    return ESP_ERR_INVALID_ARG;
  }

  // Format status message as JSON with device info
  int64_t ts = getCurrentTimestamp();
  size_t len;
  
  if (strlen(m_deviceId) > 0) {
    len = snprintf(m_jsonBuffer, sizeof(m_jsonBuffer),
                   "{\"deviceId\":\"%s\",\"deviceName\":\"%s\",\"status\":\"%s\",\"timestamp\":%lld}",
                   m_deviceId, m_deviceName, status, ts);
  } else {
    len = snprintf(m_jsonBuffer, sizeof(m_jsonBuffer),
                   "{\"deviceName\":\"%s\",\"status\":\"%s\",\"timestamp\":%lld}",
                   m_deviceName, status, ts);
  }
  
  if (len >= sizeof(m_jsonBuffer)) {
    ESP_LOGW(TAG, "Buffer JSON truncado para status");
    len = sizeof(m_jsonBuffer) - 1;
  }

  return m_client.publish((const uint8_t*)m_jsonBuffer, len);
}

esp_err_t MqttManager::sendJson(const char* json) {
  if (!json) {
    return ESP_ERR_INVALID_ARG;
  }
  size_t len = strlen(json);
  return m_client.publish((const uint8_t*)json, len);
}

esp_err_t MqttManager::sendJson(const char* topic, const char* json) {
  if (!topic || !json) {
    return ESP_ERR_INVALID_ARG;
  }
  size_t len = strlen(json);
  return m_client.publish(topic, (const uint8_t*)json, len);
}

void MqttManager::setMessageCallback(MqttClient::MessageCallback callback) {
  m_client.setMessageCallback(callback);
}

void MqttManager::setConnectionCallback(MqttClient::ConnectionCallback callback) {
  m_client.setConnectionCallback(callback);
}

esp_err_t MqttManager::reloadConfig() {
  if (!m_initialized) {
    return ESP_ERR_INVALID_STATE;
  }

  // Reload device info from ConfigManager
  ConfigManager::FullConfig config;
  esp_err_t ret = ConfigManager::getConfig(&config);
  if (ret == ESP_OK) {
    strncpy(m_deviceId, config.device.id, sizeof(m_deviceId) - 1);
    m_deviceId[sizeof(m_deviceId) - 1] = '\0';
    strncpy(m_deviceName, config.device.name, sizeof(m_deviceName) - 1);
    m_deviceName[sizeof(m_deviceName) - 1] = '\0';
    ESP_LOGI(TAG, "Device info recargado: ID=%s, Name=%s", m_deviceId, m_deviceName);
  }

  // Reload MQTT client configuration
  return m_client.reloadConfig();
}

size_t MqttManager::formatJson(const TelemetryData* data, size_t count, int64_t timestamp, char* buffer, size_t bufferSize) {
  if (!data || count == 0 || !buffer || bufferSize == 0) {
    return 0;
  }

  // Start JSON object
  size_t pos = 0;
  pos += snprintf(buffer + pos, bufferSize - pos, "{");

  // Add device ID and name (always included)
  bool needsComma = false;
  if (strlen(m_deviceId) > 0) {
    pos += snprintf(buffer + pos, bufferSize - pos, "\"deviceId\":\"%s\"", m_deviceId);
    needsComma = true;
  }
  if (strlen(m_deviceName) > 0) {
    if (needsComma) {
      pos += snprintf(buffer + pos, bufferSize - pos, ",");
    }
    pos += snprintf(buffer + pos, bufferSize - pos, "\"deviceName\":\"%s\"", m_deviceName);
    needsComma = true;
  }

  // Add timestamp if provided
  if (timestamp > 0) {
    if (needsComma) {
      pos += snprintf(buffer + pos, bufferSize - pos, ",");
    }
    pos += snprintf(buffer + pos, bufferSize - pos, "\"timestamp\":%lld", timestamp);
    needsComma = true;
  }

  // Add telemetry data
  for (size_t i = 0; i < count; i++) {
    if (pos >= bufferSize - 1) {
      ESP_LOGW(TAG, "Buffer JSON lleno");
      break;
    }

    // Add comma between fields
    if (needsComma) {
      pos += snprintf(buffer + pos, bufferSize - pos, ",");
    }
    needsComma = true;

    // Add key
    pos += snprintf(buffer + pos, bufferSize - pos, "\"%s\":", data[i].key);

    // Add value based on type
    switch (data[i].type) {
      case TelemetryData::FLOAT: {
        float val = data[i].value.fValue;
        // Check for NaN or Infinity
        if (std::isnan(val) || std::isinf(val)) {
          pos += snprintf(buffer + pos, bufferSize - pos, "null");
        } else {
          pos += snprintf(buffer + pos, bufferSize - pos, "%.6f", val);
        }
        break;
      }
      case TelemetryData::INT:
        pos += snprintf(buffer + pos, bufferSize - pos, "%ld", (long)data[i].value.iValue);
        break;
      case TelemetryData::BOOL:
        pos += snprintf(buffer + pos, bufferSize - pos, "%s", data[i].value.bValue ? "true" : "false");
        break;
      case TelemetryData::STRING: {
        // Escape JSON string (basic escaping)
        const char* str = data[i].value.sValue;
        if (!str) {
          pos += snprintf(buffer + pos, bufferSize - pos, "null");
          break;
        }
        pos += snprintf(buffer + pos, bufferSize - pos, "\"");
        // Simple escaping: escape quotes and backslashes
        for (const char* p = str; *p && pos < bufferSize - 2; p++) {
          if (*p == '"' || *p == '\\') {
            pos += snprintf(buffer + pos, bufferSize - pos, "\\%c", *p);
          } else if (*p == '\n') {
            pos += snprintf(buffer + pos, bufferSize - pos, "\\n");
          } else if (*p == '\r') {
            pos += snprintf(buffer + pos, bufferSize - pos, "\\r");
          } else if (*p == '\t') {
            pos += snprintf(buffer + pos, bufferSize - pos, "\\t");
          } else {
            buffer[pos++] = *p;
          }
        }
        pos += snprintf(buffer + pos, bufferSize - pos, "\"");
        break;
      }
    }
  }

  // Close JSON object
  pos += snprintf(buffer + pos, bufferSize - pos, "}");

  // Ensure null termination
  if (pos >= bufferSize) {
    pos = bufferSize - 1;
  }
  buffer[pos] = '\0';

  return pos;
}

int64_t MqttManager::getCurrentTimestamp() const {
  time_t now;
  time(&now);
  return (int64_t)now;
}

