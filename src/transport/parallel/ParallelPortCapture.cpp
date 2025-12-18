#include "ParallelPortCapture.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <cstring>

static const char *TAG = "ParallelPort";

esp_err_t ParallelPortCapture::init(const void* config) {
    if (m_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    if (!config) {
        ESP_LOGE(TAG, "Config is null");
        return ESP_ERR_INVALID_ARG;
    }

    const Config* cfg = static_cast<const Config*>(config);
    m_config.strobePin = cfg->strobePin;
    m_config.strobeActiveHigh = cfg->strobeActiveHigh;
    m_config.ringBufSize = cfg->ringBufSize;
    m_config.timeoutMs = cfg->timeoutMs;
    
    // Copy data pins array
    for (int i = 0; i < 8; i++) {
        m_config.dataPins[i] = cfg->dataPins[i];
    }
    
    memset(&m_stats, 0, sizeof(m_stats));

    // Validate GPIO pins
    for (int i = 0; i < 8; i++) {
        if (m_config.dataPins[i] < 0 || m_config.dataPins[i] > GPIO_NUM_MAX) {
            ESP_LOGE(TAG, "Invalid data pin[%d]: %d", i, m_config.dataPins[i]);
            return ESP_ERR_INVALID_ARG;
        }
    }

    if (m_config.strobePin < 0 || m_config.strobePin > GPIO_NUM_MAX) {
        ESP_LOGE(TAG, "Invalid strobe pin: %d", m_config.strobePin);
        return ESP_ERR_INVALID_ARG;
    }

    // Configure data pins as inputs with pull-down
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = 0;
    io_conf.pull_down_en = GPIO_PULLDOWN_ENABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;

    for (int i = 0; i < 8; i++) {
        io_conf.pin_bit_mask |= (1ULL << m_config.dataPins[i]);
    }

    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure data pins: %s", esp_err_to_name(ret));
        return ret;
    }

    // Configure strobe pin as input with interrupt
    io_conf.pin_bit_mask = (1ULL << m_config.strobePin);
    io_conf.intr_type = m_config.strobeActiveHigh ? GPIO_INTR_POSEDGE : GPIO_INTR_NEGEDGE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;

    ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure strobe pin: %s", esp_err_to_name(ret));
        return ret;
    }

    // Create queue for strobe events from ISR
    m_strobeQueue = xQueueCreate(100, sizeof(uint32_t)); // Queue of timestamps
    if (!m_strobeQueue) {
        ESP_LOGE(TAG, "Failed to create strobe queue");
        return ESP_ERR_NO_MEM;
    }

    // Create ring buffer for inter-task communication
    m_ringBuf = xRingbufferCreate(m_config.ringBufSize, RINGBUF_TYPE_BYTEBUF);
    if (!m_ringBuf) {
        ESP_LOGE(TAG, "Failed to create ring buffer");
        vQueueDelete(m_strobeQueue);
        m_strobeQueue = nullptr;
        return ESP_ERR_NO_MEM;
    }

    // Install GPIO ISR service
    ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to install GPIO ISR service: %s", esp_err_to_name(ret));
        vRingbufferDelete(m_ringBuf);
        m_ringBuf = nullptr;
        vQueueDelete(m_strobeQueue);
        m_strobeQueue = nullptr;
        return ret;
    }

    // Hook ISR handler for strobe pin
    ret = gpio_isr_handler_add((gpio_num_t)m_config.strobePin, strobeISR, this);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add ISR handler: %s", esp_err_to_name(ret));
        vRingbufferDelete(m_ringBuf);
        m_ringBuf = nullptr;
        vQueueDelete(m_strobeQueue);
        m_strobeQueue = nullptr;
        return ret;
    }

    // Create capture task pinned to Core 0
    BaseType_t taskRet = xTaskCreatePinnedToCore(
        captureTask, "parallel_capture",
        4096, // Stack size
        this, // Pass instance pointer
        configMAX_PRIORITIES - 1, // High priority
        &m_taskHandle,
        0 // Core 0
    );
    if (taskRet != pdPASS) {
        ESP_LOGE(TAG, "Failed to create task");
        gpio_isr_handler_remove((gpio_num_t)m_config.strobePin);
        vRingbufferDelete(m_ringBuf);
        m_ringBuf = nullptr;
        vQueueDelete(m_strobeQueue);
        m_strobeQueue = nullptr;
        return ESP_ERR_NO_MEM;
    }

    m_initialized = true;
    ESP_LOGI(TAG, "Initialized: Data pins [%d,%d,%d,%d,%d,%d,%d,%d], Strobe=%d (%s edge), ringBuf=%uKB",
             m_config.dataPins[0], m_config.dataPins[1], m_config.dataPins[2], m_config.dataPins[3],
             m_config.dataPins[4], m_config.dataPins[5], m_config.dataPins[6], m_config.dataPins[7],
             m_config.strobePin, m_config.strobeActiveHigh ? "rising" : "falling",
             m_config.ringBufSize / 1024);

    return ESP_OK;
}

RingbufHandle_t ParallelPortCapture::getRingBuffer() {
    return m_ringBuf;
}

void ParallelPortCapture::setBurstCallback(Transport::BurstCallback callback) {
    m_burstCallback = callback;
}

esp_err_t ParallelPortCapture::getStats(Transport::Stats* stats) {
    if (!stats) {
        return ESP_ERR_INVALID_ARG;
    }
    *stats = m_stats;
    return ESP_OK;
}

void ParallelPortCapture::resetStats() {
    m_stats.totalBytesReceived = 0;
    m_stats.bytesInCurrentBurst = 0;
    m_stats.burstCount = 0;
    m_stats.overflowCount = 0;
    m_stats.burstActive = false;
}

esp_err_t ParallelPortCapture::deinit() {
    if (m_initialized) {
        // Remove ISR handler
        gpio_isr_handler_remove((gpio_num_t)m_config.strobePin);

        if (m_taskHandle) {
            vTaskDelete(m_taskHandle);
            m_taskHandle = nullptr;
        }

        if (m_ringBuf) {
            vRingbufferDelete(m_ringBuf);
            m_ringBuf = nullptr;
        }

        if (m_strobeQueue) {
            vQueueDelete(m_strobeQueue);
            m_strobeQueue = nullptr;
        }

        m_initialized = false;
        ESP_LOGI(TAG, "Deinitialized");
    }
    return ESP_OK;
}

// --- ISR Handler ---

void IRAM_ATTR ParallelPortCapture::strobeISR(void* arg) {
    ParallelPortCapture* instance = static_cast<ParallelPortCapture*>(arg);
    if (!instance || !instance->m_strobeQueue) {
        return;
    }

    // Send timestamp to queue (from ISR)
    uint32_t timestamp = xTaskGetTickCountFromISR();
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xQueueSendFromISR(instance->m_strobeQueue, &timestamp, &xHigherPriorityTaskWoken);
    
    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

// --- Task Implementation ---

void ParallelPortCapture::captureTask(void *arg) {
    ParallelPortCapture* instance = static_cast<ParallelPortCapture*>(arg);
    if (!instance) {
        ESP_LOGE(TAG, "Invalid instance pointer");
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGI(TAG, "Parallel port capture task started on Core %d", xPortGetCoreID());

    uint32_t timestamp;

    while (true) {
        // Wait for strobe event with timeout
        if (xQueueReceive(instance->m_strobeQueue, &timestamp, 
                          pdMS_TO_TICKS(instance->m_config.timeoutMs)) == pdTRUE) {
            // Strobe detected - read data byte
            uint8_t data = instance->readDataByte();

            // Check if burst started
            if (!instance->m_stats.burstActive) {
                instance->m_stats.burstActive = true;
                instance->m_stats.bytesInCurrentBurst = 0;
                instance->m_stats.burstCount++;
                ESP_LOGD(TAG, "Burst %lu started", instance->m_stats.burstCount);
            }

            // Send to ring buffer (non-blocking)
            BaseType_t sent = xRingbufferSend(instance->m_ringBuf, &data, 1, 0);
            if (sent != pdTRUE) {
                instance->m_stats.overflowCount++;
                ESP_LOGW(TAG, "Ring buffer overflow! Lost 1 byte");
            } else {
                instance->m_stats.totalBytesReceived++;
                instance->m_stats.bytesInCurrentBurst++;
            }
        } else {
            // Timeout - check if burst ended
            if (instance->m_stats.burstActive) {
                // Check if queue is empty (no more strobes)
                if (uxQueueMessagesWaiting(instance->m_strobeQueue) == 0) {
                    // No more data, burst ended
                    instance->m_stats.burstActive = false;
                    ESP_LOGI(TAG, "Burst %lu ended: %lu bytes", instance->m_stats.burstCount,
                             instance->m_stats.bytesInCurrentBurst);

                    if (instance->m_burstCallback) {
                        instance->m_burstCallback(true, instance->m_stats.bytesInCurrentBurst);
                    }
                }
            }
        }
    }
}

// --- Helper Functions ---

uint8_t ParallelPortCapture::readDataByte() const {
    uint8_t data = 0;
    
    // Read each data pin and construct byte
    for (int i = 0; i < 8; i++) {
        if (gpio_get_level((gpio_num_t)m_config.dataPins[i])) {
            data |= (1 << i);
        }
    }
    
    return data;
}

