/**
 * ESP32 DataLogger - High Speed UART to Flash
 *
 * Captures data from UART2 at 1Mbps and stores to internal flash
 * using a circular buffer with wear leveling.
 *
 * Architecture:
 * - Core 0: UART capture task
 * - Core 1: Flash writer task
 * - Ring buffer in RAM bridges the two
 */

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "DataPipeline.h"
#include "FlashRing.h"
#include "UartCapture.h"

static const char *TAG = "DataLogger";

// Custom log formatter that removes timestamp
static int custom_log_vprintf(const char *fmt, va_list args) {
  char buffer[512];
  int len = vsnprintf(buffer, sizeof(buffer), fmt, args);
  
  if (len <= 0 || len >= (int)sizeof(buffer)) {
    return vprintf(fmt, args);
  }
  
  // Remove timestamp pattern: "I (12345) " -> "I "
  // Format is typically: "LEVEL (TIMESTAMP) TAG: message"
  if (buffer[0] == 'E' || buffer[0] == 'W' || buffer[0] == 'I' || 
      buffer[0] == 'D' || buffer[0] == 'V') {
    // Look for pattern: level char, space, '(', digits, ')', space
    if (len > 2 && buffer[1] == ' ' && buffer[2] == '(') {
      char *closeParen = strchr(buffer + 2, ')');
      if (closeParen && closeParen[1] == ' ') {
        // Remove "(TIMESTAMP) " part: keep level char and space, skip to after ') '
        memmove(buffer + 1, closeParen + 2, strlen(closeParen + 2) + 1);
      }
    }
  }
  
  return printf("%s", buffer);
}

// Command processing via debug UART (UART0/USB)
static void processCommand(const char *cmd) {
  if (strcmp(cmd, "format") == 0 || strcmp(cmd, "erase") == 0) {
    ESP_LOGI(TAG, "Erasing flash and resetting stats...");
    if (FlashRing::erase() == ESP_OK) {
      UartCapture::resetStats();
      DataPipeline::resetStats();
      ESP_LOGI(TAG, "Flash erased and stats reset!");
    } else {
      ESP_LOGE(TAG, "Flash erase failed!");
    }
  } else if (strcmp(cmd, "stats") == 0) {
    FlashRing::Stats fs;
    FlashRing::getStats(&fs);
    UartCapture::Stats us;
    UartCapture::getStats(&us);
    ESP_LOGI(TAG, "Flash: %u/%u bytes (%u%%), wraps=%lu",
             fs.usedBytes, fs.partitionSize,
             (fs.usedBytes * 100) / fs.partitionSize, fs.wrapCount);
    ESP_LOGI(TAG, "UART: total=%lu, bursts=%lu, overflows=%lu",
             us.totalBytesReceived, us.burstCount, us.overflowCount);
  } else if (strncmp(cmd, "read ", 5) == 0) {
    // Parse: read <offset> <length>
    unsigned int offset = 0, len = 0;
    if (sscanf(cmd + 5, "%u %u", &offset, &len) == 2) {
      if (len > 256) len = 256; // Limit to 256 bytes per read
      uint8_t *buf = (uint8_t *)malloc(len);
      if (buf) {
        size_t bytesRead = 0;
        if (FlashRing::readAt(offset, buf, len, &bytesRead) == ESP_OK) {
          ESP_LOGI(TAG, "Read %u bytes at offset %u:", bytesRead, offset);
          // Print hex dump
          for (size_t i = 0; i < bytesRead; i += 16) {
            char hex[50] = {0}, ascii[17] = {0};
            for (size_t j = 0; j < 16 && (i + j) < bytesRead; j++) {
              sprintf(hex + j * 3, "%02X ", buf[i + j]);
              ascii[j] = (buf[i + j] >= 32 && buf[i + j] < 127) ? buf[i + j] : '.';
            }
            ESP_LOGI(TAG, "%04X: %-48s %s", i + offset, hex, ascii);
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
    // Parse: baud <baudrate>
    unsigned int newBaud = 0;
    if (sscanf(cmd + 5, "%u", &newBaud) == 1) {
      if (UartCapture::setBaudRate(newBaud) == ESP_OK) {
        ESP_LOGI(TAG, "Baudrate set to %u", newBaud);
        printf("BAUD_OK %u\n", newBaud);
      } else {
        ESP_LOGE(TAG, "Failed to set baudrate");
        printf("BAUD_FAIL\n");
      }
    } else {
      ESP_LOGW(TAG, "Usage: baud <baudrate>");
    }
  } else if (strcmp(cmd, "baud") == 0) {
    uint32_t currentBaud = UartCapture::getBaudRate();
    ESP_LOGI(TAG, "Current baudrate: %lu", currentBaud);
    printf("BAUD %lu\n", currentBaud);
  } else if (strcmp(cmd, "help") == 0) {
    ESP_LOGI(TAG, "Commands: format, stats, read <offset> <len>, baud [rate], help");
  } else if (strlen(cmd) > 0) {
    ESP_LOGW(TAG, "Unknown command: %s (type 'help')", cmd);
  }
}

static void cmdTask(void *arg) {
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


// Burst callback - called when a data burst ends
static void onBurstEnd(bool ended, size_t bytes) {
  if (ended) {
    // Trigger flush to ensure data is persisted
    DataPipeline::flush();
  }
}

extern "C" void app_main(void) {
  // Set custom log formatter to remove timestamps
  esp_log_set_vprintf(custom_log_vprintf);
  
  ESP_LOGI(TAG, "======================================");
  ESP_LOGI(TAG, "  ESP32 DataLogger Starting");
  ESP_LOGI(TAG, "======================================");
  ESP_LOGI(TAG, "Free heap: %lu bytes", esp_get_free_heap_size());

  // Initialize FlashRing
  ESP_LOGI(TAG, "Initializing FlashRing...");
  esp_err_t ret = FlashRing::init("datalog");
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "FlashRing init failed!");
    return;
  }

  // Print initial flash stats
  FlashRing::Stats flashStats;
  FlashRing::getStats(&flashStats);
  ESP_LOGI(TAG, "Flash partition: %u bytes, used: %u bytes",
           flashStats.partitionSize, flashStats.usedBytes);

  // Initialize UART capture
  ESP_LOGI(TAG, "Initializing UartCapture...");
  UartCapture::Config uartConfig;
  uartConfig.uartPort = UART_NUM_2;
  uartConfig.rxPin = 16;
  uartConfig.baudRate = 115200; // 115200 bps
  uartConfig.rxBufSize = 32 * 1024;   // 32KB hardware buffer
  uartConfig.ringBufSize = 64 * 1024; // 64KB ring buffer
  uartConfig.timeoutMs = 100;

  ret = UartCapture::init(uartConfig);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "UartCapture init failed!");
    return;
  }

  // Set burst callback
  UartCapture::setBurstCallback(onBurstEnd);

  // Initialize DataPipeline
  ESP_LOGI(TAG, "Initializing DataPipeline...");
  DataPipeline::Config pipeConfig;
  pipeConfig.writeChunkSize = 12288;  // 12KB buffer to accumulate while writing
  pipeConfig.flushTimeoutMs = 500;
  pipeConfig.autoStart = true;

  ret = DataPipeline::init(pipeConfig);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "DataPipeline init failed!");
    return;
  }

  // Start command task for debug UART
  xTaskCreate(cmdTask, "cmd", 2048, NULL, 5, NULL);

  ESP_LOGI(TAG, "======================================");
  ESP_LOGI(TAG, "  DataLogger Ready!");
  ESP_LOGI(TAG, "  UART2 RX: GPIO16 @ 115200 bps");
  ESP_LOGI(TAG, "  Commands: format, stats, help");
  ESP_LOGI(TAG, "======================================");
  ESP_LOGI(TAG, "Free heap after init: %lu bytes", esp_get_free_heap_size());
  printf("READY\n");

  // Main loop - keep alive
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
