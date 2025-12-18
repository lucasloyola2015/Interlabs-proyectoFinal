#include "UartCapture.h"
#include "esp_log.h"
#include <cstring>

static const char *TAG = "UartCapture";

namespace UartCapture {

// Module state
static Config s_config;
static RingbufHandle_t s_ringBuf = nullptr;
static TaskHandle_t s_taskHandle = nullptr;
static QueueHandle_t s_uartQueue = nullptr;
static BurstCallback s_burstCallback = nullptr;
static bool s_initialized = false;

// Statistics
static Stats s_stats = {};

// Task function
static void uartTask(void *arg);

esp_err_t init(const Config &config) {
  if (s_initialized) {
    ESP_LOGW(TAG, "Already initialized");
    return ESP_OK;
  }

  s_config = config;
  memset(&s_stats, 0, sizeof(s_stats));

  // Configure UART
  uart_config_t uart_config = {
      .baud_rate = (int)config.baudRate,
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .rx_flow_ctrl_thresh = 0,
      .source_clk = UART_SCLK_DEFAULT,
  };

  esp_err_t ret = uart_driver_install(config.uartPort,
                                      config.rxBufSize, // RX buffer size
                                      0,  // TX buffer size (not needed)
                                      20, // Queue size for events
                                      &s_uartQueue,
                                      0 // Interrupt flags
  );
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(ret));
    return ret;
  }

  ret = uart_param_config(config.uartPort, &uart_config);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(ret));
    uart_driver_delete(config.uartPort);
    return ret;
  }

  ret = uart_set_pin(config.uartPort, config.txPin, config.rxPin,
                     UART_PIN_NO_CHANGE, // RTS
                     UART_PIN_NO_CHANGE  // CTS
  );
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(ret));
    uart_driver_delete(config.uartPort);
    return ret;
  }

  // Create ring buffer for inter-task communication
  s_ringBuf = xRingbufferCreate(config.ringBufSize, RINGBUF_TYPE_BYTEBUF);
  if (!s_ringBuf) {
    ESP_LOGE(TAG, "Failed to create ring buffer");
    uart_driver_delete(config.uartPort);
    return ESP_ERR_NO_MEM;
  }

  // Create capture task pinned to Core 0
  BaseType_t taskRet =
      xTaskCreatePinnedToCore(uartTask, "uart_capture",
                              4096, // Stack size
                              nullptr,
                              configMAX_PRIORITIES - 1, // High priority
                              &s_taskHandle,
                              0 // Core 0
      );
  if (taskRet != pdPASS) {
    ESP_LOGE(TAG, "Failed to create task");
    vRingbufferDelete(s_ringBuf);
    s_ringBuf = nullptr;
    uart_driver_delete(config.uartPort);
    return ESP_ERR_NO_MEM;
  }

  s_initialized = true;
  ESP_LOGI(TAG, "Initialized: UART%d @ %lu bps, RX=%d, ringBuf=%uKB",
           config.uartPort, config.baudRate, config.rxPin,
           config.ringBufSize / 1024);

  return ESP_OK;
}

RingbufHandle_t getRingBuffer() { return s_ringBuf; }

void setBurstCallback(BurstCallback callback) { s_burstCallback = callback; }

esp_err_t getStats(Stats *stats) {
  if (!stats) {
    return ESP_ERR_INVALID_ARG;
  }
  *stats = s_stats;
  return ESP_OK;
}

void resetStats() {
  s_stats.totalBytesReceived = 0;
  s_stats.bytesInCurrentBurst = 0;
  s_stats.burstCount = 0;
  s_stats.overflowCount = 0;
  s_stats.burstActive = false;
}

esp_err_t setBaudRate(uint32_t baudRate) {
  if (!s_initialized) {
    return ESP_ERR_INVALID_STATE;
  }
  
  esp_err_t ret = uart_set_baudrate(s_config.uartPort, baudRate);
  if (ret == ESP_OK) {
    s_config.baudRate = baudRate;
    ESP_LOGI(TAG, "Baudrate changed to %lu bps", baudRate);
  } else {
    ESP_LOGE(TAG, "Failed to set baudrate: %s", esp_err_to_name(ret));
  }
  return ret;
}

uint32_t getBaudRate() {
  return s_config.baudRate;
}

void deinit() {
  if (s_initialized) {
    if (s_taskHandle) {
      vTaskDelete(s_taskHandle);
      s_taskHandle = nullptr;
    }
    if (s_ringBuf) {
      vRingbufferDelete(s_ringBuf);
      s_ringBuf = nullptr;
    }
    uart_driver_delete(s_config.uartPort);
    s_initialized = false;
    ESP_LOGI(TAG, "Deinitialized");
  }
}

// --- Task implementation ---

static void uartTask(void *arg) {
  uart_event_t event;
  uint8_t *tempBuf = (uint8_t *)malloc(512); // Temporary buffer for reads

  if (!tempBuf) {
    ESP_LOGE(TAG, "Failed to allocate temp buffer");
    vTaskDelete(nullptr);
    return;
  }

  ESP_LOGI(TAG, "UART capture task started on Core %d", xPortGetCoreID());

  while (true) {
    // Wait for UART event with timeout
    if (xQueueReceive(s_uartQueue, &event, pdMS_TO_TICKS(s_config.timeoutMs))) {
      switch (event.type) {
      case UART_DATA: {
        // Data available in UART buffer
        size_t bufferedLen = 0;
        uart_get_buffered_data_len(s_config.uartPort, &bufferedLen);

        if (!s_stats.burstActive && bufferedLen > 0) {
          s_stats.burstActive = true;
          s_stats.bytesInCurrentBurst = 0;
          s_stats.burstCount++;
          ESP_LOGD(TAG, "Burst %lu started", s_stats.burstCount);
        }

        // Read all available data
        while (bufferedLen > 0) {
          size_t toRead = (bufferedLen > 512) ? 512 : bufferedLen;
          int len = uart_read_bytes(s_config.uartPort, tempBuf, toRead, 0);

          if (len > 0) {
            // Send to ring buffer (non-blocking)
            BaseType_t sent = xRingbufferSend(s_ringBuf, tempBuf, len, 0);
            if (sent != pdTRUE) {
              s_stats.overflowCount++;
              ESP_LOGW(TAG, "Ring buffer overflow! Lost %d bytes", len);
            } else {
              s_stats.totalBytesReceived += len;
              s_stats.bytesInCurrentBurst += len;
            }
          }

          uart_get_buffered_data_len(s_config.uartPort, &bufferedLen);
        }
        break;
      }

      case UART_FIFO_OVF:
        ESP_LOGE(TAG, "UART FIFO overflow!");
        s_stats.overflowCount++;
        uart_flush_input(s_config.uartPort);
        xQueueReset(s_uartQueue);
        break;

      case UART_BUFFER_FULL:
        ESP_LOGE(TAG, "UART buffer full!");
        s_stats.overflowCount++;
        uart_flush_input(s_config.uartPort);
        xQueueReset(s_uartQueue);
        break;

      default:
        ESP_LOGD(TAG, "UART event type: %d", event.type);
        break;
      }
    } else {
      // Timeout - check if burst ended
      if (s_stats.burstActive) {
        size_t bufferedLen = 0;
        uart_get_buffered_data_len(s_config.uartPort, &bufferedLen);

        if (bufferedLen == 0) {
          // No more data, burst ended
          s_stats.burstActive = false;
          ESP_LOGI(TAG, "Burst %lu ended: %lu bytes", s_stats.burstCount,
                   s_stats.bytesInCurrentBurst);

          if (s_burstCallback) {
            s_burstCallback(true, s_stats.bytesInCurrentBurst);
          }
        }
      }
    }
  }

  free(tempBuf);
}

} // namespace UartCapture
