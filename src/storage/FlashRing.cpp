#include "FlashRing.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <algorithm>
#include <cstring>

static const char *TAG = "FlashRing";

// NVS namespace and keys
static const char *NVS_NAMESPACE = "flashring";
static const char *NVS_KEY_META = "meta";

// Magic number for validation
static const uint32_t MAGIC_NUMBER = 0x464C5249; // "FLRI" (new version with pre-erase)

namespace FlashRing {

// Module state
static const esp_partition_t *s_partition = nullptr;
static Metadata s_meta = {};
static size_t s_partitionSize = 0;
static size_t s_totalPages = 0;
static bool s_initialized = false;

// Pre-erase task state
static TaskHandle_t s_eraseTaskHandle = nullptr;
static SemaphoreHandle_t s_eraseMutex = nullptr;
static size_t s_erasedPages[PRE_ERASE_PAGES] = {SIZE_MAX, SIZE_MAX};
static volatile bool s_eraseTaskRunning = false;

// Forward declarations
static esp_err_t loadMetadata();
static esp_err_t saveMetadata();
static size_t getUsedBytes();
static size_t getFreeBytes();
static void eraseTask(void *arg);
static bool isPageErased(size_t pageNum);
static void markPageErased(size_t pageNum);
static esp_err_t ensurePageErased(size_t pageNum);

esp_err_t init(const char *partitionLabel) {
  if (s_initialized) {
    ESP_LOGW(TAG, "Already initialized");
    return ESP_OK;
  }

  // Initialize NVS
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_LOGW(TAG, "NVS partition truncated, erasing...");
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  // Find the data partition
  s_partition = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, static_cast<esp_partition_subtype_t>(0x80),
      partitionLabel);

  if (!s_partition) {
    ESP_LOGE(TAG, "Partition '%s' not found", partitionLabel);
    return ESP_ERR_NOT_FOUND;
  }

  s_partitionSize = s_partition->size;
  s_totalPages = s_partitionSize / PAGE_SIZE;
  
  ESP_LOGI(TAG, "Found partition '%s': size=%lu bytes, %u pages",
           partitionLabel, s_partitionSize, s_totalPages);

  // Create mutex for erase coordination
  s_eraseMutex = xSemaphoreCreateMutex();
  if (!s_eraseMutex) {
    ESP_LOGE(TAG, "Failed to create mutex");
    return ESP_ERR_NO_MEM;
  }

  // Load or initialize metadata
  ret = loadMetadata();
  if (ret != ESP_OK || s_meta.magic != MAGIC_NUMBER) {
    ESP_LOGW(TAG, "No valid metadata, initializing fresh");
    s_meta.magic = MAGIC_NUMBER;
    s_meta.head = 0;
    s_meta.tail = 0;
    s_meta.totalWritten = 0;
    s_meta.wrapCount = 0;
    
    // Initialize erased pages array
    for (size_t i = 0; i < PRE_ERASE_PAGES; i++) {
      s_meta.erasedPages[i] = SIZE_MAX; // Empty
    }
    
    // Erase first few pages for fresh start
    ESP_LOGI(TAG, "Erasing initial pages...");
    for (size_t i = 0; i < PRE_ERASE_PAGES + 1; i++) {
      esp_partition_erase_range(s_partition, i * PAGE_SIZE, PAGE_SIZE);
      markPageErased(i);
    }
    saveMetadata();
  } else {
    // Load erased pages from metadata to RAM cache
    xSemaphoreTake(s_eraseMutex, portMAX_DELAY);
    for (size_t i = 0; i < PRE_ERASE_PAGES; i++) {
      s_erasedPages[i] = s_meta.erasedPages[i];
    }
    xSemaphoreGive(s_eraseMutex);
  }

  ESP_LOGI(TAG, "Initialized: head=%lu, tail=%lu, wraps=%lu", s_meta.head,
           s_meta.tail, s_meta.wrapCount);

  // Start pre-erase task
  s_eraseTaskRunning = true;
  xTaskCreatePinnedToCore(eraseTask, "flash_erase", 4096, nullptr, 
                          tskIDLE_PRIORITY + 1, &s_eraseTaskHandle, 1);

  s_initialized = true;
  return ESP_OK;
}

esp_err_t write(const uint8_t *data, size_t len) {
  if (!s_initialized) {
    return ESP_ERR_INVALID_STATE;
  }
  if (len == 0) {
    return ESP_OK;
  }
  if (len > s_partitionSize) {
    ESP_LOGE(TAG, "Write size %u exceeds partition size %u", len,
             s_partitionSize);
    return ESP_ERR_INVALID_SIZE;
  }

  size_t bytesWritten = 0;

  while (bytesWritten < len) {
    // Calculate bytes to end of current page
    size_t offsetInPage = s_meta.head % PAGE_SIZE;
    size_t bytesToPageEnd = PAGE_SIZE - offsetInPage;
    
    // Calculate how much to write in this iteration
    size_t toEndOfPartition = s_partitionSize - s_meta.head;
    size_t remaining = len - bytesWritten;
    size_t chunkSize = std::min({remaining, bytesToPageEnd, toEndOfPartition});

    // Calculate new head position
    size_t newHead = (s_meta.head + chunkSize) % s_partitionSize;

    // Check if we need to advance tail (overwrite old data)
    if (getUsedBytes() + chunkSize >= s_partitionSize) {
      s_meta.tail = (s_meta.tail + chunkSize) % s_partitionSize;
      if (s_meta.wrapCount == 0 || newHead < s_meta.head) {
        s_meta.wrapCount++;
        ESP_LOGD(TAG, "Buffer wrapped, count=%lu", s_meta.wrapCount);
      }
    }

    // Ensure the page we're writing to is erased
    size_t currentPage = s_meta.head / PAGE_SIZE;
    ensurePageErased(currentPage);

    // Write the chunk
    esp_err_t ret = esp_partition_write(s_partition, s_meta.head, 
                                         data + bytesWritten, chunkSize);
    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "esp_partition_write failed at offset %lu: %s", s_meta.head,
               esp_err_to_name(ret));
      return ret;
    }

    s_meta.head = newHead;
    s_meta.totalWritten += chunkSize;
    bytesWritten += chunkSize;
    
    // If we completed a page, it's no longer "erased" (it has data now)
    // The erase task will handle erasing ahead
  }

  ESP_LOGD(TAG, "Wrote %u bytes, head=%lu, tail=%lu", len, s_meta.head,
           s_meta.tail);
  return ESP_OK;
}

esp_err_t read(uint8_t *data, size_t len, size_t *bytesRead) {
  return readAt(0, data, len, bytesRead);
}

esp_err_t readAt(size_t offset, uint8_t *data, size_t len, size_t *bytesRead) {
  if (!s_initialized) {
    return ESP_ERR_INVALID_STATE;
  }

  size_t available = getUsedBytes();
  if (offset >= available) {
    *bytesRead = 0;
    return ESP_OK;
  }

  size_t toRead = std::min(len, available - offset);
  size_t readPos = (s_meta.tail + offset) % s_partitionSize;
  size_t totalRead = 0;

  while (totalRead < toRead) {
    size_t toEndOfPartition = s_partitionSize - readPos;
    size_t chunkSize = std::min(toRead - totalRead, toEndOfPartition);

    esp_err_t ret = esp_partition_read(s_partition, readPos, 
                                        data + totalRead, chunkSize);
    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "esp_partition_read failed at offset %u: %s", readPos,
               esp_err_to_name(ret));
      return ret;
    }

    readPos = (readPos + chunkSize) % s_partitionSize;
    totalRead += chunkSize;
  }

  *bytesRead = totalRead;
  return ESP_OK;
}

esp_err_t consume(size_t len) {
  if (!s_initialized) {
    return ESP_ERR_INVALID_STATE;
  }

  size_t available = getUsedBytes();
  size_t toConsume = std::min(len, available);
  s_meta.tail = (s_meta.tail + toConsume) % s_partitionSize;

  ESP_LOGD(TAG, "Consumed %u bytes, tail=%lu", toConsume, s_meta.tail);
  return ESP_OK;
}

esp_err_t getStats(Stats *stats) {
  if (!s_initialized || !stats) {
    return ESP_ERR_INVALID_STATE;
  }

  stats->partitionSize = s_partitionSize;
  stats->usedBytes = getUsedBytes();
  stats->freeBytes = getFreeBytes();
  stats->wrapCount = s_meta.wrapCount;
  stats->totalWritten = s_meta.totalWritten;

  return ESP_OK;
}

esp_err_t erase() {
  if (!s_initialized) {
    return ESP_ERR_INVALID_STATE;
  }

  ESP_LOGI(TAG, "Erasing all data...");

  // Erase the entire partition
  esp_err_t ret = esp_partition_erase_range(s_partition, 0, s_partitionSize);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "esp_partition_erase_range failed: %s", esp_err_to_name(ret));
    return ret;
  }

  // Reset metadata
  s_meta.head = 0;
  s_meta.tail = 0;
  s_meta.totalWritten = 0;
  s_meta.wrapCount = 0;

  // Mark first pages as erased
  xSemaphoreTake(s_eraseMutex, portMAX_DELAY);
  for (size_t i = 0; i < PRE_ERASE_PAGES; i++) {
    s_erasedPages[i] = i;
    s_meta.erasedPages[i] = i;
  }
  xSemaphoreGive(s_eraseMutex);

  return saveMetadata();
}

esp_err_t flushMetadata() {
  if (!s_initialized) {
    return ESP_ERR_INVALID_STATE;
  }
  return saveMetadata();
}

size_t getHead() {
  return s_meta.head;
}

size_t getBytesToPageEnd() {
  size_t offsetInPage = s_meta.head % PAGE_SIZE;
  return PAGE_SIZE - offsetInPage;
}

void deinit() {
  if (s_initialized) {
    // Stop erase task
    s_eraseTaskRunning = false;
    if (s_eraseTaskHandle) {
      vTaskDelay(pdMS_TO_TICKS(100));
      vTaskDelete(s_eraseTaskHandle);
      s_eraseTaskHandle = nullptr;
    }
    
    if (s_eraseMutex) {
      vSemaphoreDelete(s_eraseMutex);
      s_eraseMutex = nullptr;
    }
    
    saveMetadata();
    s_partition = nullptr;
    s_initialized = false;
    ESP_LOGI(TAG, "Deinitialized");
  }
}

// --- Private functions ---

static esp_err_t loadMetadata() {
  nvs_handle_t handle;
  esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
  if (ret != ESP_OK) {
    return ret;
  }

  size_t size = sizeof(Metadata);
  ret = nvs_get_blob(handle, NVS_KEY_META, &s_meta, &size);
  nvs_close(handle);

  return ret;
}

static esp_err_t saveMetadata() {
  nvs_handle_t handle;
  esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
    return ret;
  }

  ret = nvs_set_blob(handle, NVS_KEY_META, &s_meta, sizeof(Metadata));
  if (ret == ESP_OK) {
    ret = nvs_commit(handle);
  }
  nvs_close(handle);

  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to save metadata: %s", esp_err_to_name(ret));
  }
  return ret;
}

static size_t getUsedBytes() {
  if (s_meta.head >= s_meta.tail) {
    return s_meta.head - s_meta.tail;
  } else {
    return s_partitionSize - s_meta.tail + s_meta.head;
  }
}

static size_t getFreeBytes() {
  return s_partitionSize - getUsedBytes() - 1;
}

static bool isPageErased(size_t pageNum) {
  xSemaphoreTake(s_eraseMutex, portMAX_DELAY);
  bool erased = false;
  for (size_t i = 0; i < PRE_ERASE_PAGES; i++) {
    if (s_erasedPages[i] == pageNum) {
      erased = true;
      break;
    }
  }
  xSemaphoreGive(s_eraseMutex);
  return erased;
}

static void markPageErased(size_t pageNum) {
  xSemaphoreTake(s_eraseMutex, portMAX_DELAY);
  // Find empty slot or oldest entry
  for (size_t i = 0; i < PRE_ERASE_PAGES; i++) {
    if (s_erasedPages[i] == SIZE_MAX || s_erasedPages[i] == pageNum) {
      s_erasedPages[i] = pageNum;
      // Update metadata
      s_meta.erasedPages[i] = pageNum;
      xSemaphoreGive(s_eraseMutex);
      return;
    }
  }
  // Replace first slot if all full
  s_erasedPages[0] = pageNum;
  s_meta.erasedPages[0] = pageNum;
  xSemaphoreGive(s_eraseMutex);
}

static esp_err_t ensurePageErased(size_t pageNum) {
  if (isPageErased(pageNum)) {
    return ESP_OK;
  }
  
  // Page not pre-erased, need to erase now (blocking)
  ESP_LOGW(TAG, "Page %u not pre-erased, erasing now...", pageNum);
  esp_err_t ret = esp_partition_erase_range(s_partition, 
                                             pageNum * PAGE_SIZE, PAGE_SIZE);
  if (ret == ESP_OK) {
    markPageErased(pageNum);
  }
  return ret;
}

static void eraseTask(void *arg) {
  ESP_LOGI(TAG, "Pre-erase task started");
  
  while (s_eraseTaskRunning) {
    if (!s_initialized) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    
    // Get current page
    size_t currentPage = s_meta.head / PAGE_SIZE;
    
    // Check and erase pages ahead
    for (size_t i = 1; i <= PRE_ERASE_PAGES; i++) {
      size_t targetPage = (currentPage + i) % s_totalPages;
      
      if (!isPageErased(targetPage)) {
        ESP_LOGD(TAG, "Pre-erasing page %u", targetPage);
        esp_err_t ret = esp_partition_erase_range(s_partition, 
                                                   targetPage * PAGE_SIZE, 
                                                   PAGE_SIZE);
        if (ret == ESP_OK) {
          markPageErased(targetPage);
        } else {
          ESP_LOGE(TAG, "Failed to pre-erase page %u: %s", 
                   targetPage, esp_err_to_name(ret));
        }
        // Only erase one page per iteration to not block too long
        break;
      }
    }
    
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  
  ESP_LOGI(TAG, "Pre-erase task stopped");
  vTaskDelete(nullptr);
}

} // namespace FlashRing
