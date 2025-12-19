#include "CommandSystem.h"
#include "config/ConfigManager.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pipeline/DataPipeline.h"
#include "storage/FlashRing.h"
#include "transport/uart/UartCapture.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "CommandSystem";

namespace CommandSystem {

// Maximum number of registered commands
static constexpr size_t MAX_COMMANDS = 32;
static constexpr size_t MAX_RESPONSE_DATA = 1024;

// Command registry
static Command s_commands[MAX_COMMANDS];
static size_t s_commandCount = 0;

// Response callbacks per medium
struct ResponseCallbackEntry {
  Medium medium;
  ResponseCallback callback;
  void *userCtx;
};

static ResponseCallbackEntry s_responseCallbacks[3]; // DEBUG, WEB, MQTT
static size_t s_callbackCount = 0;

// Global data source reference
static IDataSource *s_dataSource = nullptr;

// UART CLI task
static TaskHandle_t s_cliTaskHandle = nullptr;

// Static response data buffer (for commands that return data)
static char s_responseDataBuffer[MAX_RESPONSE_DATA];

// --- Helper Functions ---

static const char *mediumToString(Medium medium) {
  switch (medium) {
  case Medium::DEBUG:
    return "DEBUG";
  case Medium::WEB:
    return "WEB";
  case Medium::MQTT:
    return "MQTT";
  default:
    return "UNKNOWN";
  }
}

static void sendResponse(Medium medium, const CommandResult *result) {
  // Try to find registered callback
  for (size_t i = 0; i < s_callbackCount; i++) {
    if (s_responseCallbacks[i].medium == medium &&
        s_responseCallbacks[i].callback) {
      s_responseCallbacks[i].callback(medium, result,
                                       s_responseCallbacks[i].userCtx);
      return;
    }
  }
  
  // If no callback registered, default behavior based on medium
  if (medium == Medium::DEBUG) {
    // For DEBUG/UART, print to console
    printf("%s", result->message);
    if (result->status != ESP_OK) {
      printf(": %s", esp_err_to_name(result->status));
    }
    if (result->data && result->dataLen > 0) {
      printf(" %s", result->data);
    }
    printf("\n");
  } else {
    // For other mediums, just log
    ESP_LOGI(TAG, "[%s] %s: %s", mediumToString(medium), result->message,
             esp_err_to_name(result->status));
  }
}

static const char *parseCommandName(const char *cmdStr, char *outName,
                                     size_t nameSize) {
  const char *space = strchr(cmdStr, ' ');
  if (space) {
    size_t len = space - cmdStr;
    if (len >= nameSize)
      len = nameSize - 1;
    strncpy(outName, cmdStr, len);
    outName[len] = '\0';
    return space + 1; // Return pointer to arguments
  } else {
    strncpy(outName, cmdStr, nameSize - 1);
    outName[nameSize - 1] = '\0';
    return nullptr; // No arguments
  }
}

// --- Command Handlers ---

static esp_err_t handleFormat(const char *args, size_t argsLen,
                              CommandResult *result) {
  (void)args;
  (void)argsLen;

  ESP_LOGI(TAG, "Erasing flash and resetting stats...");
  if (FlashRing::erase() == ESP_OK) {
    if (s_dataSource) {
      s_dataSource->resetStats();
    }
    DataPipeline::resetStats();
    result->status = ESP_OK;
    result->message = "FORMAT_OK";
    result->data = nullptr;
    result->dataLen = 0;
    ESP_LOGI(TAG, "Flash erased and stats reset!");
  } else {
    result->status = ESP_FAIL;
    result->message = "FORMAT_FAIL";
    result->data = nullptr;
    result->dataLen = 0;
    ESP_LOGE(TAG, "Flash erase failed!");
  }
  return result->status;
}

static esp_err_t handleStats(const char *args, size_t argsLen,
                             CommandResult *result) {
  (void)args;
  (void)argsLen;

  FlashRing::Stats fs;
  FlashRing::getStats(&fs);

  // Format stats as JSON for WEB/MQTT, plain text for DEBUG
  float usedPercent = fs.partitionSize > 0 
      ? (100.0f * fs.usedBytes / fs.partitionSize)
      : 0.0f;
  snprintf(s_responseDataBuffer, MAX_RESPONSE_DATA,
           "{\"flash\":{\"usedBytes\":%u,\"partitionSize\":%u,"
           "\"freeBytes\":%u,\"wrapCount\":%u,\"totalWritten\":%u,"
           "\"usedPercent\":%.1f}",
           (unsigned int)fs.usedBytes, (unsigned int)fs.partitionSize,
           (unsigned int)fs.freeBytes, (unsigned int)fs.wrapCount,
           (unsigned int)fs.totalWritten, usedPercent);

  if (s_dataSource) {
    Transport::Stats ts;
    if (s_dataSource->getStats(&ts) == ESP_OK) {
      snprintf(s_responseDataBuffer + strlen(s_responseDataBuffer),
               MAX_RESPONSE_DATA - strlen(s_responseDataBuffer),
               ",\"transport\":{\"totalBytesReceived\":%zu,"
               "\"burstCount\":%lu,\"overflowCount\":%lu,\"burstActive\":%s}",
               ts.totalBytesReceived, (unsigned long)ts.burstCount, (unsigned long)ts.overflowCount,
               ts.burstActive ? "true" : "false");
    }
  }

  DataPipeline::Stats ps;
  if (DataPipeline::getStats(&ps) == ESP_OK) {
    snprintf(s_responseDataBuffer + strlen(s_responseDataBuffer),
             MAX_RESPONSE_DATA - strlen(s_responseDataBuffer),
             ",\"pipeline\":{\"bytesWrittenToFlash\":%zu,"
             "\"bytesDropped\":%zu,\"writeOperations\":%lu,"
             "\"flushOperations\":%lu,\"running\":%s}",
             ps.bytesWrittenToFlash, ps.bytesDropped, (unsigned long)ps.writeOperations,
             (unsigned long)ps.flushOperations, ps.running ? "true" : "false");
  }

  strcat(s_responseDataBuffer, "}");

  result->status = ESP_OK;
  result->message = "STATS_DATA";
  result->data = s_responseDataBuffer;
  result->dataLen = strlen(s_responseDataBuffer);

  ESP_LOGI(TAG, "Flash: %u/%u bytes (%u%%), wraps=%lu", fs.usedBytes,
           fs.partitionSize, (fs.usedBytes * 100) / fs.partitionSize,
           fs.wrapCount);

  return ESP_OK;
}

static esp_err_t handleRead(const char *args, size_t argsLen,
                            CommandResult *result) {
  unsigned int offset = 0, len = 0;
  if (sscanf(args, "%u %u", &offset, &len) != 2) {
    result->status = ESP_ERR_INVALID_ARG;
    result->message = "READ_USAGE";
    result->data = "Usage: read <offset> <length>";
    result->dataLen = strlen(result->data);
    return ESP_ERR_INVALID_ARG;
  }

  if (len > 256)
    len = 256;

  uint8_t *buf = (uint8_t *)malloc(len);
  if (!buf) {
    result->status = ESP_ERR_NO_MEM;
    result->message = "READ_FAIL";
    result->data = "Memory allocation failed";
    result->dataLen = strlen(result->data);
    return ESP_ERR_NO_MEM;
  }

  size_t bytesRead = 0;
  esp_err_t ret = FlashRing::readAt(offset, buf, len, &bytesRead);

  if (ret == ESP_OK) {
    // Format as hex dump
    size_t hexLen = 0;
    for (size_t i = 0; i < bytesRead && hexLen < MAX_RESPONSE_DATA - 100;
         i += 16) {
      char line[80];
      int lineLen = snprintf(line, sizeof(line), "%04X: ", (unsigned int)(i + offset));
      
      for (size_t j = 0; j < 16 && (i + j) < bytesRead; j++) {
        lineLen += snprintf(line + lineLen, sizeof(line) - lineLen, "%02X ", buf[i + j]);
      }
      lineLen += snprintf(line + lineLen, sizeof(line) - lineLen, "\n");
      
      if (hexLen + lineLen < MAX_RESPONSE_DATA - 1) {
        memcpy(s_responseDataBuffer + hexLen, line, lineLen);
        hexLen += lineLen;
      }
    }
    s_responseDataBuffer[hexLen] = '\0';

    result->status = ESP_OK;
    result->message = "READ_OK";
    result->data = s_responseDataBuffer;
    result->dataLen = hexLen;
  } else {
    result->status = ret;
    result->message = "READ_FAIL";
    result->data = "Flash read failed";
    result->dataLen = strlen(result->data);
  }

  free(buf);
  return result->status;
}

static esp_err_t handleBaud(const char *args, size_t argsLen,
                            CommandResult *result) {
  if (argsLen == 0 || args[0] == '\0') {
    // Get current baudrate
    if (s_dataSource && s_dataSource->getType() == Transport::Type::UART) {
      UartCapture *uart = static_cast<UartCapture *>(s_dataSource);
      uint32_t currentBaud = uart->getBaudRate();
      snprintf(s_responseDataBuffer, MAX_RESPONSE_DATA, "%lu", currentBaud);
      result->status = ESP_OK;
      result->message = "BAUD";
      result->data = s_responseDataBuffer;
      result->dataLen = strlen(s_responseDataBuffer);
      ESP_LOGI(TAG, "Current baudrate: %lu", currentBaud);
    } else {
      result->status = ESP_ERR_NOT_SUPPORTED;
      result->message = "BAUD_FAIL";
      result->data = "Baudrate command only available for UART transport";
      result->dataLen = strlen(result->data);
    }
    return result->status;
  }

  // Set baudrate
  if (s_dataSource && s_dataSource->getType() == Transport::Type::UART) {
    unsigned int newBaud = 0;
    if (sscanf(args, "%u", &newBaud) == 1) {
      UartCapture *uart = static_cast<UartCapture *>(s_dataSource);
      if (uart->setBaudRate(newBaud) == ESP_OK) {
        ConfigManager::FullConfig fullConfig;
        if (ConfigManager::getConfig(&fullConfig) == ESP_OK) {
          ConfigManager::saveConfig(&fullConfig);
        }
        snprintf(s_responseDataBuffer, MAX_RESPONSE_DATA, "%u", newBaud);
        result->status = ESP_OK;
        result->message = "BAUD_OK";
        result->data = s_responseDataBuffer;
        result->dataLen = strlen(s_responseDataBuffer);
        ESP_LOGI(TAG, "Baudrate set to %u", newBaud);
      } else {
        result->status = ESP_FAIL;
        result->message = "BAUD_FAIL";
        result->data = "Failed to set baudrate";
        result->dataLen = strlen(result->data);
      }
    } else {
      result->status = ESP_ERR_INVALID_ARG;
      result->message = "BAUD_USAGE";
      result->data = "Usage: baud <baudrate>";
      result->dataLen = strlen(result->data);
    }
  } else {
    result->status = ESP_ERR_NOT_SUPPORTED;
    result->message = "BAUD_FAIL";
    result->data = "Baudrate command only available for UART transport";
    result->dataLen = strlen(result->data);
  }
  return result->status;
}

static esp_err_t handleConfig(const char *args, size_t argsLen,
                              CommandResult *result) {
  (void)args;
  (void)argsLen;

  ConfigManager::FullConfig config;
  if (ConfigManager::getConfig(&config) == ESP_OK) {
    // Format as JSON
    snprintf(s_responseDataBuffer, MAX_RESPONSE_DATA,
             "{\"device\":{\"name\":\"%s\",\"id\":\"%s\",\"type\":%d},"
             "\"network\":{\"lan\":{\"enabled\":%s,"
             "\"staticIp\":\"%d.%d.%d.%d\"},"
             "\"wlanOp\":{\"enabled\":%s,\"ssid\":\"%s\"},"
             "\"wlanSafe\":{\"ssid\":\"%s\",\"channel\":%d}}}",
             config.device.name, config.device.id,
             (int)config.device.type,
             config.network.lan.enabled ? "true" : "false",
             config.network.lan.staticIp.addr[0],
             config.network.lan.staticIp.addr[1],
             config.network.lan.staticIp.addr[2],
             config.network.lan.staticIp.addr[3],
             config.network.wlanOp.enabled ? "true" : "false",
             config.network.wlanOp.ssid, config.network.wlanSafe.ssid,
             config.network.wlanSafe.channel);

    result->status = ESP_OK;
    result->message = "CONFIG_DATA";
    result->data = s_responseDataBuffer;
    result->dataLen = strlen(s_responseDataBuffer);

    ESP_LOGI(TAG, "Device: %s (ID: %s)", config.device.name, config.device.id);
  } else {
    result->status = ESP_FAIL;
    result->message = "CONFIG_FAIL";
    result->data = "Failed to load configuration";
    result->dataLen = strlen(result->data);
  }
  return result->status;
}

static esp_err_t handleReset(const char *args, size_t argsLen,
                             CommandResult *result) {
  (void)args;
  (void)argsLen;

  result->status = ESP_OK;
  result->message = "RESET_OK";
  result->data = nullptr;
  result->dataLen = 0;

  ESP_LOGW(TAG, "Rebooting system...");
  
  // Note: Response will be sent by the caller (executeCommand) through
  // the appropriate medium callback before we reboot
  // We delay a bit to allow response to be sent
  vTaskDelay(pdMS_TO_TICKS(500));
  esp_restart();

  return ESP_OK;
}

static esp_err_t handleHelp(const char *args, size_t argsLen,
                            CommandResult *result) {
  (void)args;
  (void)argsLen;

  size_t helpLen = 0;
  helpLen = snprintf(s_responseDataBuffer, MAX_RESPONSE_DATA,
                     "Available commands:\n");

  for (size_t i = 0; i < s_commandCount && helpLen < MAX_RESPONSE_DATA - 100;
       i++) {
    int len = snprintf(s_responseDataBuffer + helpLen,
                       MAX_RESPONSE_DATA - helpLen, "  %s - %s\n",
                       s_commands[i].name, s_commands[i].description);
    if (len > 0)
      helpLen += len;
  }

  result->status = ESP_OK;
  result->message = "HELP";
  result->data = s_responseDataBuffer;
  result->dataLen = helpLen;

  return ESP_OK;
}

// --- Public API Implementation ---

esp_err_t initialize(IDataSource *dataSource) {
  s_dataSource = dataSource;
  s_commandCount = 0;
  s_callbackCount = 0;

  // Register built-in commands
  registerCommand({.name = "format",
                   .handler = handleFormat,
                   .allowedMediums =
                       (MediumMask)Medium::DEBUG | (MediumMask)Medium::WEB,
                   .description = "Erase flash and reset statistics"});

  registerCommand({.name = "erase",
                   .handler = handleFormat, // Same handler as format
                   .allowedMediums =
                       (MediumMask)Medium::DEBUG | (MediumMask)Medium::WEB,
                   .description = "Erase flash and reset statistics (alias)"});

  registerCommand({.name = "stats",
                   .handler = handleStats,
                   .allowedMediums = (MediumMask)Medium::DEBUG |
                                     (MediumMask)Medium::WEB |
                                     (MediumMask)Medium::MQTT,
                   .description = "Get system statistics"});

  registerCommand({.name = "read",
                   .handler = handleRead,
                   .allowedMediums =
                       (MediumMask)Medium::DEBUG | (MediumMask)Medium::WEB,
                   .description = "Read data from flash (usage: read <offset> <length>)"});

  registerCommand({.name = "baud",
                   .handler = handleBaud,
                   .allowedMediums =
                       (MediumMask)Medium::DEBUG | (MediumMask)Medium::WEB,
                   .description =
                       "Get or set UART baudrate (usage: baud [rate])"});

  registerCommand({.name = "config",
                   .handler = handleConfig,
                   .allowedMediums = (MediumMask)Medium::DEBUG |
                                     (MediumMask)Medium::WEB |
                                     (MediumMask)Medium::MQTT,
                   .description = "Get device configuration"});

  registerCommand({.name = "reset",
                   .handler = handleReset,
                   .allowedMediums =
                       (MediumMask)Medium::DEBUG | (MediumMask)Medium::WEB,
                   .description = "Reboot the system"});

  registerCommand({.name = "reboot",
                   .handler = handleReset, // Same handler as reset
                   .allowedMediums =
                       (MediumMask)Medium::DEBUG | (MediumMask)Medium::WEB,
                   .description = "Reboot the system (alias)"});

  registerCommand({.name = "help",
                   .handler = handleHelp,
                   .allowedMediums = (MediumMask)Medium::DEBUG |
                                     (MediumMask)Medium::WEB |
                                     (MediumMask)Medium::MQTT,
                   .description = "Show available commands"});

  // Start UART CLI task
  BaseType_t ret = xTaskCreate(
      [](void *arg) {
        (void)arg;
        char cmdBuf[64];
        int cmdIdx = 0;

        while (true) {
          int c = getchar();
          if (c != EOF) {
            if (c == '\n' || c == '\r') {
              if (cmdIdx > 0) {
                cmdBuf[cmdIdx] = '\0';
                // executeCommand will send response automatically
                executeCommand(Medium::DEBUG, cmdBuf);
                cmdIdx = 0;
              }
            } else if (cmdIdx < (int)sizeof(cmdBuf) - 1) {
              cmdBuf[cmdIdx++] = (char)c;
            }
          }
          vTaskDelay(pdMS_TO_TICKS(10));
        }
      },
      "cli_task", 4096, nullptr, 5, &s_cliTaskHandle);

  return (ret == pdPASS) ? ESP_OK : ESP_FAIL;
}

void deinit() {
  if (s_cliTaskHandle) {
    vTaskDelete(s_cliTaskHandle);
    s_cliTaskHandle = nullptr;
  }
  s_commandCount = 0;
  s_callbackCount = 0;
  s_dataSource = nullptr;
}

esp_err_t registerCommand(const Command &cmd) {
  if (s_commandCount >= MAX_COMMANDS) {
    ESP_LOGE(TAG, "Command registry full");
    return ESP_ERR_NO_MEM;
  }

  if (!cmd.name || !cmd.handler) {
    ESP_LOGE(TAG, "Invalid command structure");
    return ESP_ERR_INVALID_ARG;
  }

  s_commands[s_commandCount++] = cmd;
  ESP_LOGI(TAG, "Registered command: %s", cmd.name);
  return ESP_OK;
}

CommandResult executeCommand(Medium medium, const char *cmdStr) {
  CommandResult result = {};
  result.status = ESP_ERR_NOT_FOUND;
  result.message = "COMMAND_NOT_FOUND";
  result.data = "Unknown command";
  result.dataLen = strlen(result.data);

  if (!cmdStr || strlen(cmdStr) == 0) {
    result.status = ESP_ERR_INVALID_ARG;
    result.message = "INVALID_COMMAND";
    result.data = "Empty command string";
    result.dataLen = strlen(result.data);
    // Log to DEBUG UART
    ESP_LOGW(TAG, "[%s] Command execution failed: Empty command string",
             mediumToString(medium));
    return result;
  }

  char cmdName[32];
  const char *args = parseCommandName(cmdStr, cmdName, sizeof(cmdName));
  size_t argsLen = args ? strlen(args) : 0;

  // Log command execution to DEBUG UART (always, regardless of origin medium)
  if (args && argsLen > 0) {
    ESP_LOGI(TAG, "[%s] Executing command: %s %.*s",
             mediumToString(medium), cmdName, (int)(argsLen > 32 ? 32 : argsLen), args);
  } else {
    ESP_LOGI(TAG, "[%s] Executing command: %s", mediumToString(medium), cmdName);
  }

  // Find command
  const Command *cmd = nullptr;
  for (size_t i = 0; i < s_commandCount; i++) {
    if (strcmp(s_commands[i].name, cmdName) == 0) {
      cmd = &s_commands[i];
      break;
    }
  }

  if (!cmd) {
    ESP_LOGW(TAG, "[%s] Unknown command: %s", mediumToString(medium), cmdName);
    return result;
  }

  // Check permissions
  MediumMask mediumMask = (MediumMask)medium;
  if ((cmd->allowedMediums & mediumMask) == 0) {
    result.status = ESP_ERR_INVALID_STATE;
    result.message = "PERMISSION_DENIED";
    result.data = "Command not allowed from this medium";
    result.dataLen = strlen(result.data);
    ESP_LOGW(TAG, "[%s] Command %s not allowed from this medium",
             mediumToString(medium), cmdName);
    return result;
  }

  // Execute command
  esp_err_t ret = cmd->handler(args ? args : "", argsLen, &result);
  if (ret != ESP_OK && result.status == ESP_ERR_NOT_FOUND) {
    result.status = ret;
  }

  // Log result to DEBUG UART (always, regardless of origin medium)
  if (result.status == ESP_OK) {
    ESP_LOGI(TAG, "[%s] Command %s executed successfully: %s",
             mediumToString(medium), cmdName, result.message);
  } else {
    ESP_LOGE(TAG, "[%s] Command %s failed: %s (%s)",
             mediumToString(medium), cmdName, result.message,
             esp_err_to_name(result.status));
  }

  // Send response through registered callback
  // Note: For reset/reboot, the handler will delay before rebooting to allow
  // the response to be sent first
  sendResponse(medium, &result);

  return result;
}

esp_err_t registerResponseCallback(Medium medium, ResponseCallback callback,
                                   void *userCtx) {
  if (s_callbackCount >= 3) { // DEBUG, WEB, MQTT
    ESP_LOGE(TAG, "Response callback registry full");
    return ESP_ERR_NO_MEM;
  }

  // Check if already registered
  for (size_t i = 0; i < s_callbackCount; i++) {
    if (s_responseCallbacks[i].medium == medium) {
      // Update existing
      s_responseCallbacks[i].callback = callback;
      s_responseCallbacks[i].userCtx = userCtx;
      ESP_LOGI(TAG, "Updated response callback for medium %d", (int)medium);
      return ESP_OK;
    }
  }

  // Add new
  s_responseCallbacks[s_callbackCount].medium = medium;
  s_responseCallbacks[s_callbackCount].callback = callback;
  s_responseCallbacks[s_callbackCount].userCtx = userCtx;
  s_callbackCount++;
  ESP_LOGI(TAG, "Registered response callback for medium %d", (int)medium);
  return ESP_OK;
}

void unregisterResponseCallback(Medium medium) {
  for (size_t i = 0; i < s_callbackCount; i++) {
    if (s_responseCallbacks[i].medium == medium) {
      // Remove by shifting
      for (size_t j = i; j < s_callbackCount - 1; j++) {
        s_responseCallbacks[j] = s_responseCallbacks[j + 1];
      }
      s_callbackCount--;
      ESP_LOGI(TAG, "Unregistered response callback for medium %d", (int)medium);
      return;
    }
  }
}

size_t getAvailableCommands(const Command **commands, size_t maxCommands) {
  size_t count = (s_commandCount < maxCommands) ? s_commandCount : maxCommands;
  for (size_t i = 0; i < count; i++) {
    commands[i] = &s_commands[i];
  }
  return count;
}

size_t getAvailableCommandsForMedium(Medium medium, const Command **commands,
                                     size_t maxCommands) {
  MediumMask mediumMask = (MediumMask)medium;
  size_t count = 0;

  for (size_t i = 0; i < s_commandCount && count < maxCommands; i++) {
    if (s_commands[i].allowedMediums & mediumMask) {
      commands[count++] = &s_commands[i];
    }
  }

  return count;
}

} // namespace CommandSystem
