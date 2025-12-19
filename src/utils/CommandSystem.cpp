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

static const char *TAG = "CommandSystem";

namespace CommandSystem {

static IDataSource *s_dataSource = nullptr;
static TaskHandle_t s_taskHandle = nullptr;

static void processCommand(const char *cmd) {
  if (strcmp(cmd, "format") == 0 || strcmp(cmd, "erase") == 0) {
    ESP_LOGI(TAG, "Erasing flash and resetting stats...");
    if (FlashRing::erase() == ESP_OK) {
      if (s_dataSource) {
        s_dataSource->resetStats();
      }
      DataPipeline::resetStats();
      ESP_LOGI(TAG, "Flash erased and stats reset!");
    } else {
      ESP_LOGE(TAG, "Flash erase failed!");
    }
  } else if (strcmp(cmd, "stats") == 0) {
    FlashRing::Stats fs;
    FlashRing::getStats(&fs);
    ESP_LOGI(TAG, "Flash: %u/%u bytes (%u%%), wraps=%lu", fs.usedBytes,
             fs.partitionSize, (fs.usedBytes * 100) / fs.partitionSize,
             fs.wrapCount);

    if (s_dataSource) {
      Transport::Stats ts;
      if (s_dataSource->getStats(&ts) == ESP_OK) {
        ESP_LOGI(TAG, "Transport: total=%lu, bursts=%lu, overflows=%lu",
                 ts.totalBytesReceived, ts.burstCount, ts.overflowCount);
      }
    }
  } else if (strncmp(cmd, "read ", 5) == 0) {
    unsigned int offset = 0, len = 0;
    if (sscanf(cmd + 5, "%u %u", &offset, &len) == 2) {
      if (len > 256)
        len = 256;
      uint8_t *buf = (uint8_t *)malloc(len);
      if (buf) {
        size_t bytesRead = 0;
        if (FlashRing::readAt(offset, buf, len, &bytesRead) == ESP_OK) {
          ESP_LOGI(TAG, "Read %u bytes at offset %u:", (unsigned int)bytesRead,
                   offset);
          for (size_t i = 0; i < bytesRead; i += 16) {
            char hex[50] = {0}, ascii[17] = {0};
            for (size_t j = 0; j < 16 && (i + j) < bytesRead; j++) {
              sprintf(hex + j * 3, "%02X ", buf[i + j]);
              ascii[j] =
                  (buf[i + j] >= 32 && buf[i + j] < 127) ? buf[i + j] : '.';
            }
            ESP_LOGI(TAG, "%04X: %-48s %s", (unsigned int)(i + offset), hex,
                     ascii);
          }
        } else {
          ESP_LOGE(TAG, "Read failed");
        }
        free(buf);
      }
    } else {
      ESP_LOGW(TAG, "Usage: read <offset> <length>");
    }
  } else if (strncmp(cmd, "baud ", 5) == 0) {
    if (s_dataSource && s_dataSource->getType() == Transport::Type::UART) {
      unsigned int newBaud = 0;
      if (sscanf(cmd + 5, "%u", &newBaud) == 1) {
        UartCapture *uart = static_cast<UartCapture *>(s_dataSource);
        if (uart->setBaudRate(newBaud) == ESP_OK) {
          ConfigManager::FullConfig fullConfig;
          if (ConfigManager::getConfig(&fullConfig) == ESP_OK) {
            // TODO: Update FullConfig to store UART baudrate when transport is
            // implemented
            ConfigManager::saveConfig(&fullConfig);
          }
          ESP_LOGI(TAG, "Baudrate set to %u", newBaud);
          printf("BAUD_OK %u\n", newBaud);
        } else {
          ESP_LOGE(TAG, "Failed to set baudrate");
          printf("BAUD_FAIL\n");
        }
      } else {
        ESP_LOGW(TAG, "Usage: baud <baudrate>");
      }
    } else {
      ESP_LOGW(TAG, "Baudrate command only available for UART transport");
    }
  } else if (strcmp(cmd, "baud") == 0) {
    if (s_dataSource && s_dataSource->getType() == Transport::Type::UART) {
      UartCapture *uart = static_cast<UartCapture *>(s_dataSource);
      uint32_t currentBaud = uart->getBaudRate();
      ESP_LOGI(TAG, "Current baudrate: %lu", currentBaud);
      printf("BAUD %lu\n", currentBaud);
    } else {
      ESP_LOGW(TAG, "Baudrate command only available for UART transport");
    }
  } else if (strcmp(cmd, "config") == 0) {
    ConfigManager::FullConfig config;
    if (ConfigManager::getConfig(&config) == ESP_OK) {
      ESP_LOGI(TAG, "Device: %s (ID: %s)", config.device.name,
               config.device.id);
      ESP_LOGI(TAG, "LAN: %s, IP: %d.%d.%d.%d",
               config.network.lan.enabled ? "enabled" : "disabled",
               config.network.lan.staticIp.addr[0],
               config.network.lan.staticIp.addr[1],
               config.network.lan.staticIp.addr[2],
               config.network.lan.staticIp.addr[3]);
      ESP_LOGI(TAG, "WLAN-OP: %s, SSID: %s",
               config.network.wlanOp.enabled ? "enabled" : "disabled",
               config.network.wlanOp.ssid);
      ESP_LOGI(TAG, "WLAN-SAFE: SSID: %s, Channel: %d",
               config.network.wlanSafe.ssid, config.network.wlanSafe.channel);
    }
  } else if (strcmp(cmd, "reset") == 0 || strcmp(cmd, "reboot") == 0) {
    ESP_LOGW(TAG, "Rebooting system...");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
  } else if (strcmp(cmd, "help") == 0) {
    ESP_LOGI(TAG, "Commands: format, stats, read <offset> <len>, baud [rate], "
                  "config, reset, help");
  } else if (strlen(cmd) > 0) {
    ESP_LOGW(TAG, "Unknown command: %s (type 'help')", cmd);
  }
}

static void cliTask(void *arg) {
  char cmdBuf[64];
  int cmdIdx = 0;

  while (true) {
    int c = getchar();
    if (c != EOF) {
      if (c == '\n' || c == '\r') {
        if (cmdIdx > 0) {
          cmdBuf[cmdIdx] = '\0';
          processCommand(cmdBuf);
          cmdIdx = 0;
        }
      } else if (cmdIdx < (int)sizeof(cmdBuf) - 1) {
        cmdBuf[cmdIdx++] = (char)c;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

esp_err_t initialize(IDataSource *dataSource) {
  s_dataSource = dataSource;
  BaseType_t ret =
      xTaskCreate(cliTask, "cli_task", 4096, nullptr, 5, &s_taskHandle);
  return (ret == pdPASS) ? ESP_OK : ESP_FAIL;
}

void deinit() {
  if (s_taskHandle) {
    vTaskDelete(s_taskHandle);
    s_taskHandle = nullptr;
  }
}

} // namespace CommandSystem
