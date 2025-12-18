#include "UartCapture.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>

static const char *TAG = "UartCapture";

esp_err_t UartCapture::init(const void* config) {
    if (m_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    if (!config) {
        ESP_LOGE(TAG, "Config is null");
        return ESP_ERR_INVALID_ARG;
    }

    m_config = *static_cast<const Config*>(config);
    memset(&m_stats, 0, sizeof(m_stats));

    // Configure UART
    uart_config_t uart_config = {
        .baud_rate = (int)m_config.baudRate,
        .data_bits = m_config.dataBits,
        .parity = m_config.parity,
        .stop_bits = m_config.stopBits,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
        .flags = 0,
    };

    esp_err_t ret = uart_driver_install(m_config.uartPort,
                                        m_config.rxBufSize, // RX buffer size
                                        0,  // TX buffer size (not needed)
                                        20, // Queue size for events
                                        &m_uartQueue,
                                        0 // Interrupt flags
    );
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = uart_param_config(m_config.uartPort, &uart_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(ret));
        uart_driver_delete(m_config.uartPort);
        return ret;
    }

    ret = uart_set_pin(m_config.uartPort, m_config.txPin, m_config.rxPin,
                       UART_PIN_NO_CHANGE, // RTS
                       UART_PIN_NO_CHANGE  // CTS
    );
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(ret));
        uart_driver_delete(m_config.uartPort);
        return ret;
    }

    // Create ring buffer for inter-task communication
    m_ringBuf = xRingbufferCreate(m_config.ringBufSize, RINGBUF_TYPE_BYTEBUF);
    if (!m_ringBuf) {
        ESP_LOGE(TAG, "Failed to create ring buffer");
        uart_driver_delete(m_config.uartPort);
        return ESP_ERR_NO_MEM;
    }

    // Create capture task pinned to Core 0
    // Pass 'this' pointer to task
    BaseType_t taskRet =
        xTaskCreatePinnedToCore(uartTask, "uart_capture",
                                4096, // Stack size
                                this, // Pass instance pointer
                                configMAX_PRIORITIES - 1, // High priority
                                &m_taskHandle,
                                0 // Core 0
        );
    if (taskRet != pdPASS) {
        ESP_LOGE(TAG, "Failed to create task");
        vRingbufferDelete(m_ringBuf);
        m_ringBuf = nullptr;
        uart_driver_delete(m_config.uartPort);
        return ESP_ERR_NO_MEM;
    }

    m_initialized = true;
    ESP_LOGI(TAG, "Initialized: UART%d @ %lu bps, RX=%d, ringBuf=%uKB",
             m_config.uartPort, m_config.baudRate, m_config.rxPin,
             m_config.ringBufSize / 1024);

    return ESP_OK;
}

RingbufHandle_t UartCapture::getRingBuffer() {
    return m_ringBuf;
}

void UartCapture::setBurstCallback(Transport::BurstCallback callback) {
    m_burstCallback = callback;
}

esp_err_t UartCapture::getStats(Transport::Stats* stats) {
    if (!stats) {
        return ESP_ERR_INVALID_ARG;
    }
    *stats = m_stats;
    return ESP_OK;
}

void UartCapture::resetStats() {
    m_stats.totalBytesReceived = 0;
    m_stats.bytesInCurrentBurst = 0;
    m_stats.burstCount = 0;
    m_stats.overflowCount = 0;
    m_stats.burstActive = false;
}

esp_err_t UartCapture::setBaudRate(uint32_t baudRate) {
    if (!m_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    esp_err_t ret = uart_set_baudrate(m_config.uartPort, baudRate);
    if (ret == ESP_OK) {
        m_config.baudRate = baudRate;
        ESP_LOGI(TAG, "Baudrate changed to %lu bps", baudRate);
    } else {
        ESP_LOGE(TAG, "Failed to set baudrate: %s", esp_err_to_name(ret));
    }
    return ret;
}

uint32_t UartCapture::getBaudRate() const {
    return m_config.baudRate;
}

esp_err_t UartCapture::deinit() {
    if (m_initialized) {
        if (m_taskHandle) {
            vTaskDelete(m_taskHandle);
            m_taskHandle = nullptr;
        }
        if (m_ringBuf) {
            vRingbufferDelete(m_ringBuf);
            m_ringBuf = nullptr;
        }
        uart_driver_delete(m_config.uartPort);
        m_initialized = false;
        ESP_LOGI(TAG, "Deinitialized");
    }
    return ESP_OK;
}

// --- Task implementation ---

void UartCapture::uartTask(void *arg) {
    UartCapture* instance = static_cast<UartCapture*>(arg);
    if (!instance) {
        ESP_LOGE(TAG, "Invalid instance pointer");
        vTaskDelete(nullptr);
        return;
    }

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
        if (xQueueReceive(instance->m_uartQueue, &event, pdMS_TO_TICKS(instance->m_config.timeoutMs))) {
            switch (event.type) {
            case UART_DATA: {
                // Data available in UART buffer
                size_t bufferedLen = 0;
                uart_get_buffered_data_len(instance->m_config.uartPort, &bufferedLen);

                if (!instance->m_stats.burstActive && bufferedLen > 0) {
                    instance->m_stats.burstActive = true;
                    instance->m_stats.bytesInCurrentBurst = 0;
                    instance->m_stats.burstCount++;
                    ESP_LOGD(TAG, "Burst %lu started", instance->m_stats.burstCount);
                }

                // Read all available data
                while (bufferedLen > 0) {
                    size_t toRead = (bufferedLen > 512) ? 512 : bufferedLen;
                    int len = uart_read_bytes(instance->m_config.uartPort, tempBuf, toRead, 0);

                    if (len > 0) {
                        // Send to ring buffer (non-blocking)
                        BaseType_t sent = xRingbufferSend(instance->m_ringBuf, tempBuf, len, 0);
                        if (sent != pdTRUE) {
                            instance->m_stats.overflowCount++;
                            ESP_LOGW(TAG, "Ring buffer overflow! Lost %d bytes", len);
                        } else {
                            instance->m_stats.totalBytesReceived += len;
                            instance->m_stats.bytesInCurrentBurst += len;
                        }
                    }

                    uart_get_buffered_data_len(instance->m_config.uartPort, &bufferedLen);
                }
                break;
            }

            case UART_FIFO_OVF:
                ESP_LOGE(TAG, "UART FIFO overflow!");
                instance->m_stats.overflowCount++;
                uart_flush_input(instance->m_config.uartPort);
                xQueueReset(instance->m_uartQueue);
                break;

            case UART_BUFFER_FULL:
                ESP_LOGE(TAG, "UART buffer full!");
                instance->m_stats.overflowCount++;
                uart_flush_input(instance->m_config.uartPort);
                xQueueReset(instance->m_uartQueue);
                break;

            default:
                ESP_LOGD(TAG, "UART event type: %d", event.type);
                break;
            }
        } else {
            // Timeout - check if burst ended
            if (instance->m_stats.burstActive) {
                size_t bufferedLen = 0;
                uart_get_buffered_data_len(instance->m_config.uartPort, &bufferedLen);

                if (bufferedLen == 0) {
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

    free(tempBuf);
}

