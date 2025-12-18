#include "DataPipeline.h"
#include "FlashRing.h"
#include "UartCapture.h"
#include "esp_log.h"
#include "freertos/semphr.h"
#include <cstring>

static const char *TAG = "DataPipeline";

namespace DataPipeline {

// Module state
static Config s_config;
static TaskHandle_t s_taskHandle = nullptr;
static SemaphoreHandle_t s_flushSem = nullptr;
static volatile bool s_running = false;
static volatile bool s_stopRequested = false;
static bool s_initialized = false;

// Statistics
static Stats s_stats = {};

// Task function
static void writerTask(void *arg);

esp_err_t init(const Config &config) {
  if (s_initialized) {
    ESP_LOGW(TAG, "Already initialized");
    return ESP_OK;
  }

  s_config = config;
  memset(&s_stats, 0, sizeof(s_stats));
  s_stopRequested = false;

  // Create flush semaphore
  s_flushSem = xSemaphoreCreateBinary();
  if (!s_flushSem) {
    ESP_LOGE(TAG, "Failed to create semaphore");
    return ESP_ERR_NO_MEM;
  }

  // Create writer task pinned to Core 1
  BaseType_t taskRet = xTaskCreatePinnedToCore(
      writerTask, "flash_writer",
      4096, // Stack size
      nullptr,
      configMAX_PRIORITIES - 2, // High priority but below UART
      &s_taskHandle,
      1 // Core 1
  );
  if (taskRet != pdPASS) {
    ESP_LOGE(TAG, "Failed to create writer task");
    vSemaphoreDelete(s_flushSem);
    s_flushSem = nullptr;
    return ESP_ERR_NO_MEM;
  }

  s_initialized = true;

  if (config.autoStart) {
    s_running = true;
  }

  ESP_LOGI(TAG, "Initialized: chunkSize=%u, flushTimeout=%lu ms",
           config.writeChunkSize, config.flushTimeoutMs);

  return ESP_OK;
}

esp_err_t start() {
  if (!s_initialized) {
    return ESP_ERR_INVALID_STATE;
  }
  s_running = true;
  ESP_LOGI(TAG, "Started");
  return ESP_OK;
}

esp_err_t stop() {
  if (!s_initialized) {
    return ESP_ERR_INVALID_STATE;
  }
  s_running = false;
  ESP_LOGI(TAG, "Stopped");
  return ESP_OK;
}

esp_err_t flush() {
  if (!s_initialized) {
    return ESP_ERR_INVALID_STATE;
  }

  // Signal the writer task to flush
  xSemaphoreGive(s_flushSem);

  // Wait a bit for flush to complete
  vTaskDelay(pdMS_TO_TICKS(100));

  // Flush flash metadata
  FlashRing::flushMetadata();
  s_stats.flushOperations++;

  return ESP_OK;
}

esp_err_t getStats(Stats *stats) {
  if (!stats) {
    return ESP_ERR_INVALID_ARG;
  }
  s_stats.running = s_running;
  *stats = s_stats;
  return ESP_OK;
}

void resetStats() {
  s_stats.bytesWrittenToFlash = 0;
  s_stats.bytesDropped = 0;
  s_stats.writeOperations = 0;
  s_stats.flushOperations = 0;
}

void deinit() {
  if (s_initialized) {
    s_stopRequested = true;
    s_running = false;

    if (s_taskHandle) {
      // Give task time to exit
      vTaskDelay(pdMS_TO_TICKS(200));
      vTaskDelete(s_taskHandle);
      s_taskHandle = nullptr;
    }

    if (s_flushSem) {
      vSemaphoreDelete(s_flushSem);
      s_flushSem = nullptr;
    }

    s_initialized = false;
    ESP_LOGI(TAG, "Deinitialized");
  }
}

// --- Task implementation ---

static void writerTask(void *arg) {
  RingbufHandle_t ringBuf = UartCapture::getRingBuffer();

  if (!ringBuf) {
    ESP_LOGE(TAG, "No ring buffer available!");
    vTaskDelete(nullptr);
    return;
  }

  // Allocate write buffer (12KB to hold data while writing to flash)
  uint8_t *writeBuf = (uint8_t *)malloc(s_config.writeChunkSize);
  if (!writeBuf) {
    ESP_LOGE(TAG, "Failed to allocate write buffer");
    vTaskDelete(nullptr);
    return;
  }

  ESP_LOGI(TAG, "Flash writer task started on Core %d", xPortGetCoreID());

  size_t pendingBytes = 0;
  TickType_t lastDataTime = xTaskGetTickCount();

  while (!s_stopRequested) {
    if (!s_running) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    // Try to receive data from ring buffer
    size_t itemSize;
    void *item = xRingbufferReceiveUpTo(
        ringBuf, &itemSize,
        pdMS_TO_TICKS(50), // Short timeout for responsive flush
        s_config.writeChunkSize - pendingBytes);

    if (item && itemSize > 0) {
      // Copy to write buffer
      memcpy(writeBuf + pendingBytes, item, itemSize);
      pendingBytes += itemSize;

      // Return item to ring buffer
      vRingbufferReturnItem(ringBuf, item);

      lastDataTime = xTaskGetTickCount();

      // Page-aligned writing: write when we can complete current page + full pages
      size_t bytesToPageEnd = FlashRing::getBytesToPageEnd();
      
      // If we have enough to complete current page, write it
      if (pendingBytes >= bytesToPageEnd && bytesToPageEnd > 0) {
        // Write to complete current page
        esp_err_t ret = FlashRing::write(writeBuf, bytesToPageEnd);
        if (ret == ESP_OK) {
          s_stats.bytesWrittenToFlash += bytesToPageEnd;
          s_stats.writeOperations++;
          ESP_LOGD(TAG, "Wrote %u bytes (page completion)", bytesToPageEnd);
        } else {
          s_stats.bytesDropped += bytesToPageEnd;
          ESP_LOGE(TAG, "Flash write failed: %s", esp_err_to_name(ret));
        }
        
        // Shift remaining data
        size_t remaining = pendingBytes - bytesToPageEnd;
        if (remaining > 0) {
          memmove(writeBuf, writeBuf + bytesToPageEnd, remaining);
        }
        pendingBytes = remaining;
      }
      
      // Write full pages (4096 bytes each)
      while (pendingBytes >= FlashRing::PAGE_SIZE) {
        esp_err_t ret = FlashRing::write(writeBuf, FlashRing::PAGE_SIZE);
        if (ret == ESP_OK) {
          s_stats.bytesWrittenToFlash += FlashRing::PAGE_SIZE;
          s_stats.writeOperations++;
          ESP_LOGD(TAG, "Wrote %u bytes (full page)", FlashRing::PAGE_SIZE);
        } else {
          s_stats.bytesDropped += FlashRing::PAGE_SIZE;
          ESP_LOGE(TAG, "Flash write failed: %s", esp_err_to_name(ret));
        }
        
        // Shift remaining data
        size_t remaining = pendingBytes - FlashRing::PAGE_SIZE;
        if (remaining > 0) {
          memmove(writeBuf, writeBuf + FlashRing::PAGE_SIZE, remaining);
        }
        pendingBytes = remaining;
      }
    }

    // Check for flush signal or timeout
    bool shouldFlush = false;

    if (xSemaphoreTake(s_flushSem, 0) == pdTRUE) {
      shouldFlush = true;
    }

    // Flush on timeout if we have pending data
    if (pendingBytes > 0) {
      TickType_t elapsed = xTaskGetTickCount() - lastDataTime;
      if (elapsed > pdMS_TO_TICKS(s_config.flushTimeoutMs)) {
        shouldFlush = true;
      }
    }

    if (shouldFlush && pendingBytes > 0) {
      // Calculate RAM buffer usage percentage
      float usagePercent = (float)pendingBytes * 100.0f / (float)s_config.writeChunkSize;
      ESP_LOGI(TAG, "Flushing %u bytes (used: %.1f%% RAM)", 
               pendingBytes, usagePercent);

      esp_err_t ret = FlashRing::write(writeBuf, pendingBytes);
      if (ret == ESP_OK) {
        s_stats.bytesWrittenToFlash += pendingBytes;
        s_stats.writeOperations++;
      } else {
        s_stats.bytesDropped += pendingBytes;
        ESP_LOGE(TAG, "Flash write failed on flush: %s", esp_err_to_name(ret));
      }
      pendingBytes = 0;

      // Also flush metadata
      FlashRing::flushMetadata();
      s_stats.flushOperations++;
    }
  }

  free(writeBuf);
  ESP_LOGI(TAG, "Writer task exiting");
  vTaskDelete(nullptr);
}

} // namespace DataPipeline
