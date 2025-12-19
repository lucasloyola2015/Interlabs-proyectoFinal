#include "MqttCommandHandler.h"
#include "CommandSystem.h"
#include "../config/ConfigManager.h"
#include "esp_log.h"
#include "cJSON.h"
#include <cstring>

static const char *TAG = "MqttCmdHandler";

namespace MqttCommandHandler {

static MqttManager *s_mqttManager = nullptr;
static char s_responseTopic[128] = {0};
static char s_commandTopic[128] = {0};
static char s_deviceId[16] = {0}; // Device ID for command filtering
static char s_deviceName[32] = {0}; // Device name for responses
static bool s_initialized = false;
static bool s_handlerActive = false;

// Helper function to extract JSON string value
static const char *getJsonString(cJSON *obj, const char *key, const char *defaultValue = "") {
  cJSON *item = cJSON_GetObjectItem(obj, key);
  if (item && cJSON_IsString(item)) {
    return item->valuestring;
  }
  return defaultValue;
}

// Helper function to publish command response using MqttManager
static void publishResponse(const char *requestId, const char *command,
                            const CommandSystem::CommandResult *result) {
  if (!s_mqttManager || !s_mqttManager->isConnected()) {
    ESP_LOGW(TAG, "MQTT not connected, cannot publish response");
    return;
  }

  // Prepare status, message, data, and error
  const char *status = result->status == ESP_OK ? "ok" : "error";
  const char *message = result->message ? result->message : "";
  const char *data = nullptr;
  const char *error = nullptr;

  if (result->data && result->dataLen > 0) {
    data = result->data;
  }

  if (result->status != ESP_OK) {
    error = (result->data && result->dataLen > 0) 
        ? result->data 
        : esp_err_to_name(result->status);
  }

  // Use response topic if configured
  const char *topic = strlen(s_responseTopic) > 0 
      ? s_responseTopic 
      : "datalogger/telemetry/response"; // Fallback to default

  // Use MqttManager to send response (automatically includes deviceId and deviceName)
  esp_err_t ret = s_mqttManager->sendCommandResponse(topic, requestId, command, 
                                                      status, message, data, error);
  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "Published command response for '%s' via MqttManager", command);
  } else {
    ESP_LOGE(TAG, "Failed to publish command response: %s", esp_err_to_name(ret));
  }
}

// MQTT message callback wrapper
static void mqttMessageCallback(const char *topic, const uint8_t *payload, size_t payloadLen) {
  processMessage(topic, payload, payloadLen);
}

void processMessage(const char *topic, const uint8_t *payload, size_t payloadLen) {
  if (!s_initialized || !s_mqttManager || !s_handlerActive) {
    ESP_LOGW(TAG, "Handler not active or not initialized");
    return;
  }

  // Check if we're still connected
  if (!s_mqttManager->isConnected()) {
    ESP_LOGW(TAG, "MQTT not connected, ignoring message");
    return;
  }

  // Null-terminate payload for JSON parsing
  char *payloadStr = (char *)malloc(payloadLen + 1);
  if (!payloadStr) {
    ESP_LOGE(TAG, "Failed to allocate memory for payload");
    return;
  }

  memcpy(payloadStr, payload, payloadLen);
  payloadStr[payloadLen] = '\0';

  ESP_LOGI(TAG, "Received MQTT command from topic '%s': %.*s", topic,
           (int)(payloadLen > 128 ? 128 : payloadLen), payloadStr);

  // Parse JSON
  cJSON *json = cJSON_Parse(payloadStr);
  if (!json) {
    ESP_LOGE(TAG, "Failed to parse JSON: %s", cJSON_GetErrorPtr());
    free(payloadStr);
    return;
  }

  // Check device ID target (if specified)
  const char *targetDeviceId = getJsonString(json, "deviceId", nullptr);
  if (targetDeviceId && strlen(targetDeviceId) > 0) {
    // Command has a target device ID - check if it's for this device
    if (strlen(s_deviceId) > 0 && strcmp(targetDeviceId, s_deviceId) != 0) {
      ESP_LOGD(TAG, "Command ignored - target device ID '%s' does not match this device '%s'",
               targetDeviceId, s_deviceId);
      cJSON_Delete(json);
      free(payloadStr);
      return;
    }
    ESP_LOGI(TAG, "Command targeted for this device (ID: %s)", s_deviceId);
  } else {
    // No device ID specified - command is ignored for security
    ESP_LOGW(TAG, "Command ignored - missing 'deviceId' field (required for security)");
    cJSON_Delete(json);
    free(payloadStr);
    return;
  }

  // Extract command fields
  const char *command = getJsonString(json, "command");
  const char *args = getJsonString(json, "args");
  const char *requestId = getJsonString(json, "id", nullptr);

  if (!command || strlen(command) == 0) {
    ESP_LOGE(TAG, "Missing 'command' field in JSON");
    cJSON_Delete(json);
    free(payloadStr);
    return;
  }

  // Build command string
  char cmdStr[128];
  if (args && strlen(args) > 0) {
    snprintf(cmdStr, sizeof(cmdStr), "%s %s", command, args);
  } else {
    strncpy(cmdStr, command, sizeof(cmdStr) - 1);
    cmdStr[sizeof(cmdStr) - 1] = '\0';
  }

  // Execute command through CommandSystem
  CommandSystem::CommandResult result = 
      CommandSystem::executeCommand(CommandSystem::Medium::MQTT, cmdStr);

  // Publish response
  publishResponse(requestId, command, &result);

  // Cleanup
  cJSON_Delete(json);
  free(payloadStr);
}

// Connection callback to activate/deactivate handler
static void onMqttConnectionChanged(bool connected) {
  if (!s_initialized || !s_mqttManager) {
    return;
  }

  if (connected) {
    // Connection established - activate handler
    if (!s_handlerActive) {
      ESP_LOGI(TAG, "MQTT connected - Activating command handler");
      
      // Register message callback via MqttManager
      s_mqttManager->setMessageCallback(mqttMessageCallback);
      
      // Subscribe to command topic via MqttManager API
      if (strlen(s_commandTopic) > 0) {
        if (s_mqttManager->subscribe(s_commandTopic, 1) == ESP_OK) {
          ESP_LOGI(TAG, "Subscribed to command topic: %s", s_commandTopic);
        } else {
          ESP_LOGE(TAG, "Failed to subscribe to command topic: %s", s_commandTopic);
        }
      }
      
      s_handlerActive = true;
    }
  } else {
    // Connection lost - deactivate handler
    if (s_handlerActive) {
      ESP_LOGI(TAG, "MQTT disconnected - Deactivating command handler");
      
      // Unregister message callback
      s_mqttManager->setMessageCallback(nullptr);
      
      s_handlerActive = false;
    }
  }
}

esp_err_t init(MqttManager *mqttManager) {
  if (s_initialized) {
    ESP_LOGW(TAG, "Already initialized");
    return ESP_OK;
  }

  if (!mqttManager) {
    ESP_LOGE(TAG, "Invalid MqttManager pointer");
    return ESP_ERR_INVALID_ARG;
  }

  s_mqttManager = mqttManager;
  s_handlerActive = false; // Handler starts inactive

  // Load MQTT configuration to determine topics and device ID
  // Use static config pointer to avoid stack overflow - get only needed fields
  ConfigManager::FullConfig *configPtr = (ConfigManager::FullConfig *)malloc(sizeof(ConfigManager::FullConfig));
  if (!configPtr) {
    ESP_LOGE(TAG, "Failed to allocate memory for config");
    // Use defaults
    snprintf(s_responseTopic, sizeof(s_responseTopic), "datalogger/telemetry/response");
    snprintf(s_commandTopic, sizeof(s_commandTopic), "datalogger/commands");
    ConfigManager::generateDeviceId(s_deviceId, sizeof(s_deviceId));
  } else {
    if (ConfigManager::getConfig(configPtr) == ESP_OK) {
      // Use publish topic as response topic, but append /response
      if (strlen(configPtr->mqtt.topicPub) > 0) {
        snprintf(s_responseTopic, sizeof(s_responseTopic), "%s/response",
                 configPtr->mqtt.topicPub);
      } else {
        snprintf(s_responseTopic, sizeof(s_responseTopic), "datalogger/telemetry/response");
      }
      // Command topic is the subscription topic
      if (strlen(configPtr->mqtt.topicSub) > 0) {
        snprintf(s_commandTopic, sizeof(s_commandTopic), "%s",
                 configPtr->mqtt.topicSub);
      } else {
        snprintf(s_commandTopic, sizeof(s_commandTopic), "datalogger/commands");
      }
      // Get device ID and name for command filtering and responses
      if (strlen(configPtr->device.id) > 0) {
        strncpy(s_deviceId, configPtr->device.id, sizeof(s_deviceId) - 1);
        s_deviceId[sizeof(s_deviceId) - 1] = '\0';
      } else {
        ConfigManager::generateDeviceId(s_deviceId, sizeof(s_deviceId));
      }
      
      // Get device name
      if (strlen(configPtr->device.name) > 0) {
        strncpy(s_deviceName, configPtr->device.name, sizeof(s_deviceName) - 1);
        s_deviceName[sizeof(s_deviceName) - 1] = '\0';
      } else {
        strncpy(s_deviceName, "DataLogger", sizeof(s_deviceName) - 1);
        s_deviceName[sizeof(s_deviceName) - 1] = '\0';
      }
      
      ESP_LOGI(TAG, "Command topic: %s, Response topic: %s, Device ID: %s, Device Name: %s", 
               s_commandTopic, s_responseTopic, s_deviceId, s_deviceName);
    } else {
      // Fallback to defaults
      snprintf(s_responseTopic, sizeof(s_responseTopic), "datalogger/telemetry/response");
      snprintf(s_commandTopic, sizeof(s_commandTopic), "datalogger/commands");
      ConfigManager::generateDeviceId(s_deviceId, sizeof(s_deviceId));
      strncpy(s_deviceName, "DataLogger", sizeof(s_deviceName) - 1);
      s_deviceName[sizeof(s_deviceName) - 1] = '\0';
      ESP_LOGW(TAG, "Failed to load config, using default topics");
    }
    free(configPtr);
  }

  // Register connection callback to activate/deactivate handler
  mqttManager->setConnectionCallback(onMqttConnectionChanged);

  // Register response callback for MQTT medium in CommandSystem
  // (This allows CommandSystem to publish responses directly if needed)
  CommandSystem::registerResponseCallback(
      CommandSystem::Medium::MQTT,
      [](CommandSystem::Medium medium, const CommandSystem::CommandResult *result, void *userCtx) {
        (void)medium; // Always MQTT
        (void)userCtx;
        // Response already published in processMessage
        // This callback is kept for consistency but responses are handled in processMessage
      },
      nullptr);

  s_initialized = true;
  ESP_LOGI(TAG, "MQTT command handler initialized (inactive until connection)");
  
  // If already connected, activate handler immediately
  if (mqttManager->isConnected()) {
    onMqttConnectionChanged(true);
  }

  return ESP_OK;
}

void deinit() {
  if (!s_initialized) {
    return;
  }

  if (s_mqttManager) {
    // Deactivate handler first
    s_handlerActive = false;
    s_mqttManager->setMessageCallback(nullptr);
    s_mqttManager->setConnectionCallback(nullptr);
  }

  CommandSystem::unregisterResponseCallback(CommandSystem::Medium::MQTT);

  s_mqttManager = nullptr;
  s_responseTopic[0] = '\0';
  s_commandTopic[0] = '\0';
  s_deviceId[0] = '\0';
  s_deviceName[0] = '\0';
  s_initialized = false;
  s_handlerActive = false;
  ESP_LOGI(TAG, "MQTT command handler deinitialized");
}

bool isActive() {
  return s_initialized && s_handlerActive && s_mqttManager && s_mqttManager->isConnected();
}

} // namespace MqttCommandHandler

