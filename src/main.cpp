/**
 * ESP32 DataLogger - High Speed Data Capture to Flash
 *
 * Captures data from various transports (UART, Parallel Port) and stores
 * to internal flash using a circular buffer with wear leveling.
 *
 * Architecture:
 * - Core 0: Transport capture task (UART/PP)
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

#include "config/AppConfig.h"
#include "config/ConfigManager.h"
#include "pipeline/DataPipeline.h"
#include "storage/FlashRing.h"
#include "transport/IDataSource.h"
#include "transport/uart/UartCapture.h"
#include "transport/parallel/ParallelPortCapture.h"
#include "utils/LogFormatter.h"

static const char *TAG = "DataLogger";

// Global transport instance
static IDataSource* g_dataSource = nullptr;

// Command processing via debug UART (UART0/USB)
static void processCommand(const char *cmd) {
  if (strcmp(cmd, "format") == 0 || strcmp(cmd, "erase") == 0) {
    ESP_LOGI(TAG, "Erasing flash and resetting stats...");
    if (FlashRing::erase() == ESP_OK) {
      if (g_dataSource) {
        g_dataSource->resetStats();
      }
      DataPipeline::resetStats();
      ESP_LOGI(TAG, "Flash erased and stats reset!");
    } else {
      ESP_LOGE(TAG, "Flash erase failed!");
    }
  } else if (strcmp(cmd, "stats") == 0) {
    FlashRing::Stats fs;
    FlashRing::getStats(&fs);
    ESP_LOGI(TAG, "Flash: %u/%u bytes (%u%%), wraps=%lu",
             fs.usedBytes, fs.partitionSize,
             (fs.usedBytes * 100) / fs.partitionSize, fs.wrapCount);
    
    if (g_dataSource) {
      Transport::Stats ts;
      if (g_dataSource->getStats(&ts) == ESP_OK) {
        ESP_LOGI(TAG, "Transport: total=%lu, bursts=%lu, overflows=%lu",
                 ts.totalBytesReceived, ts.burstCount, ts.overflowCount);
      }
    }
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
    // Parse: baud <baudrate> (UART only)
    if (g_dataSource && g_dataSource->getType() == Transport::Type::UART) {
      unsigned int newBaud = 0;
      if (sscanf(cmd + 5, "%u", &newBaud) == 1) {
        UartCapture* uart = static_cast<UartCapture*>(g_dataSource);
        if (uart->setBaudRate(newBaud) == ESP_OK) {
          // Update config in NVS
          ConfigManager::UartConfig uartConfig;
          if (ConfigManager::getUartConfig(&uartConfig) == ESP_OK) {
            uartConfig.baudRate = newBaud;
            ConfigManager::saveUartConfig(&uartConfig);
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
    if (g_dataSource && g_dataSource->getType() == Transport::Type::UART) {
      UartCapture* uart = static_cast<UartCapture*>(g_dataSource);
      uint32_t currentBaud = uart->getBaudRate();
      ESP_LOGI(TAG, "Current baudrate: %lu", currentBaud);
      printf("BAUD %lu\n", currentBaud);
    } else {
      ESP_LOGW(TAG, "Baudrate command only available for UART transport");
    }
  } else if (strcmp(cmd, "config") == 0) {
    // Show current configuration
    ConfigManager::AppConfig config;
    if (ConfigManager::getConfig(&config) == ESP_OK) {
      ESP_LOGI(TAG, "Transport: %s", 
               config.transportType == Transport::Type::UART ? "UART" : "PARALLEL_PORT");
      if (config.transportType == Transport::Type::UART) {
        ESP_LOGI(TAG, "UART: port=%d, rx=%d, tx=%d, baud=%lu, data=%d, parity=%d, stop=%d",
                 config.uart.uartPort, config.uart.rxPin, config.uart.txPin,
                 config.uart.baudRate, config.uart.dataBits, config.uart.parity, config.uart.stopBits);
      } else {
        ESP_LOGI(TAG, "PP: strobe=%d, active=%s, data=[%d,%d,%d,%d,%d,%d,%d,%d]",
                 config.parallelPort.strobePin,
                 config.parallelPort.strobeActiveHigh ? "high" : "low",
                 config.parallelPort.dataPins[0], config.parallelPort.dataPins[1],
                 config.parallelPort.dataPins[2], config.parallelPort.dataPins[3],
                 config.parallelPort.dataPins[4], config.parallelPort.dataPins[5],
                 config.parallelPort.dataPins[6], config.parallelPort.dataPins[7]);
      }
    }
  } else if (strncmp(cmd, "transport ", 10) == 0) {
    // Parse: transport <uart|pp>
    const char* typeStr = cmd + 10;
    Transport::Type newType;
    if (strcmp(typeStr, "uart") == 0) {
      newType = Transport::Type::UART;
    } else if (strcmp(typeStr, "pp") == 0 || strcmp(typeStr, "parallel") == 0) {
      newType = Transport::Type::PARALLEL_PORT;
    } else {
      ESP_LOGW(TAG, "Usage: transport <uart|pp>");
      return;
    }
    
    if (ConfigManager::setTransportType(newType) == ESP_OK) {
      ESP_LOGI(TAG, "Transport type set to %s (reboot required)", 
               newType == Transport::Type::UART ? "UART" : "PARALLEL_PORT");
      printf("TRANSPORT_OK %s\n", newType == Transport::Type::UART ? "UART" : "PP");
    } else {
      ESP_LOGE(TAG, "Failed to set transport type");
      printf("TRANSPORT_FAIL\n");
    }
  } else if (strcmp(cmd, "help") == 0) {
    ESP_LOGI(TAG, "Commands: format, stats, read <offset> <len>, baud [rate], config, transport <uart|pp>, help");
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

  // Initialize ConfigManager (loads from NVS)
  ESP_LOGI(TAG, "Initializing ConfigManager...");
  esp_err_t ret = ConfigManager::init();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "ConfigManager init failed!");
    return;
  }

  // Initialize FlashRing
  ESP_LOGI(TAG, "Initializing FlashRing...");
  ret = FlashRing::init("datalog");
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "FlashRing init failed!");
    return;
  }

  // Print initial flash stats
  FlashRing::Stats flashStats;
  FlashRing::getStats(&flashStats);
  ESP_LOGI(TAG, "Flash partition: %u bytes, used: %u bytes",
           flashStats.partitionSize, flashStats.usedBytes);

  // Load configuration from NVS
  ConfigManager::AppConfig appConfig;
  ret = ConfigManager::getConfig(&appConfig);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to load configuration!");
    return;
  }

  // Initialize transport based on configuration from NVS
  ESP_LOGI(TAG, "Initializing transport...");
  
  if (appConfig.transportType == Transport::Type::UART) {
    // Initialize UART transport
    static UartCapture uartCapture;
    UartCapture::Config uartConfig;
    uartConfig.uartPort = appConfig.uart.uartPort;
    uartConfig.rxPin = appConfig.uart.rxPin;
    uartConfig.txPin = appConfig.uart.txPin;
    uartConfig.baudRate = appConfig.uart.baudRate;
    uartConfig.dataBits = appConfig.uart.dataBits;
    uartConfig.parity = appConfig.uart.parity;
    uartConfig.stopBits = appConfig.uart.stopBits;
    uartConfig.rxBufSize = appConfig.uart.rxBufSize;
    uartConfig.ringBufSize = appConfig.uart.ringBufSize;
    uartConfig.timeoutMs = appConfig.uart.timeoutMs;

    ret = uartCapture.init(&uartConfig);
    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "UART transport init failed!");
      return;
    }

    uartCapture.setBurstCallback(onBurstEnd);
    g_dataSource = &uartCapture;
    ESP_LOGI(TAG, "UART transport initialized: UART%d @ %lu bps, RX=%d, data=%d, parity=%d, stop=%d",
             uartConfig.uartPort, uartConfig.baudRate, uartConfig.rxPin,
             uartConfig.dataBits, uartConfig.parity, uartConfig.stopBits);
  } else if (appConfig.transportType == Transport::Type::PARALLEL_PORT) {
    // Initialize Parallel Port transport
    static ParallelPortCapture parallelCapture;
    ParallelPortCapture::Config parallelConfig;
    
    // Copy data pins from config
    for (int i = 0; i < 8; i++) {
      parallelConfig.dataPins[i] = appConfig.parallelPort.dataPins[i];
    }
    
    parallelConfig.strobePin = appConfig.parallelPort.strobePin;
    parallelConfig.strobeActiveHigh = appConfig.parallelPort.strobeActiveHigh;
    parallelConfig.ringBufSize = appConfig.parallelPort.ringBufSize;
    parallelConfig.timeoutMs = appConfig.parallelPort.timeoutMs;

    ret = parallelCapture.init(&parallelConfig);
    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "Parallel Port transport init failed!");
      return;
    }

    parallelCapture.setBurstCallback(onBurstEnd);
    g_dataSource = &parallelCapture;
    ESP_LOGI(TAG, "Parallel Port transport initialized: Data pins [%d,%d,%d,%d,%d,%d,%d,%d], Strobe=%d",
             parallelConfig.dataPins[0], parallelConfig.dataPins[1], parallelConfig.dataPins[2], 
             parallelConfig.dataPins[3], parallelConfig.dataPins[4], parallelConfig.dataPins[5],
             parallelConfig.dataPins[6], parallelConfig.dataPins[7], parallelConfig.strobePin);
  }

  // Initialize DataPipeline
  ESP_LOGI(TAG, "Initializing DataPipeline...");
  DataPipeline::Config pipeConfig;
  pipeConfig.writeChunkSize = 12288;  // 12KB buffer to accumulate while writing
  pipeConfig.flushTimeoutMs = 500;
  pipeConfig.autoStart = true;

  ret = DataPipeline::init(pipeConfig, g_dataSource);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "DataPipeline init failed!");
    return;
  }

  // Start command task for debug UART
  xTaskCreate(cmdTask, "cmd", 2048, NULL, 5, NULL);

  ESP_LOGI(TAG, "======================================");
  ESP_LOGI(TAG, "  DataLogger Ready!");
  if (appConfig.transportType == Transport::Type::UART) {
    ESP_LOGI(TAG, "  Transport: UART%d RX: GPIO%d @ %lu bps",
             appConfig.uart.uartPort,
             appConfig.uart.rxPin,
             appConfig.uart.baudRate);
  } else if (appConfig.transportType == Transport::Type::PARALLEL_PORT) {
    ESP_LOGI(TAG, "  Transport: Parallel Port (8-bit + Strobe)");
    ESP_LOGI(TAG, "    Data pins: [%d,%d,%d,%d,%d,%d,%d,%d]",
             appConfig.parallelPort.dataPins[0],
             appConfig.parallelPort.dataPins[1],
             appConfig.parallelPort.dataPins[2],
             appConfig.parallelPort.dataPins[3],
             appConfig.parallelPort.dataPins[4],
             appConfig.parallelPort.dataPins[5],
             appConfig.parallelPort.dataPins[6],
             appConfig.parallelPort.dataPins[7]);
    ESP_LOGI(TAG, "    Strobe pin: GPIO%d (%s edge)",
             appConfig.parallelPort.strobePin,
             appConfig.parallelPort.strobeActiveHigh ? "rising" : "falling");
  }
  ESP_LOGI(TAG, "  Commands: format, stats, config, transport, help");
  ESP_LOGI(TAG, "======================================");
  ESP_LOGI(TAG, "Free heap after init: %lu bytes", esp_get_free_heap_size());
  printf("READY\n");

  // Main loop - keep alive
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
