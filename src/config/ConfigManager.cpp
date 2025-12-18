#include "ConfigManager.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <cstring>

static const char *TAG = "ConfigManager";

// NVS namespace and key
static const char *NVS_NAMESPACE = "appconfig";
static const char *NVS_KEY_CONFIG = "config";

// Configuration version (increment when structure changes)
static const uint32_t CONFIG_VERSION = 1;

// Default configuration
static const ConfigManager::AppConfig DEFAULT_CONFIG = {
    .transportType = Transport::Type::UART,
    .uart = {
        .uartPort = UART_NUM_2,
        .rxPin = 16,
        .txPin = 17,
        .baudRate = 115200,
        .dataBits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stopBits = UART_STOP_BITS_1,
        .rxBufSize = 32 * 1024,
        .ringBufSize = 64 * 1024,
        .timeoutMs = 100
    },
    .parallelPort = {
        .dataPins = {2, 4, 5, 18, 19, 21, 22, 23},
        .strobePin = 0,
        .strobeActiveHigh = true,
        .ringBufSize = 64 * 1024,
        .timeoutMs = 100
    },
    .version = CONFIG_VERSION
};

// Current configuration (cached in RAM)
static ConfigManager::AppConfig s_config = DEFAULT_CONFIG;
static bool s_initialized = false;

namespace ConfigManager {

esp_err_t init() {
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    // Initialize NVS if not already done
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS partition was truncated and needs to be erased
        ESP_LOGW(TAG, "NVS partition truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize NVS: %s", esp_err_to_name(ret));
        return ret;
    }

    // Load configuration from NVS
    nvs_handle_t handle;
    ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret == ESP_OK) {
        size_t size = sizeof(AppConfig);
        ret = nvs_get_blob(handle, NVS_KEY_CONFIG, &s_config, &size);
        nvs_close(handle);

        if (ret == ESP_OK) {
            // Validate version
            if (s_config.version != CONFIG_VERSION) {
                ESP_LOGW(TAG, "Config version mismatch (%lu vs %lu), using defaults", 
                         s_config.version, CONFIG_VERSION);
                s_config = DEFAULT_CONFIG;
                // Save defaults
                saveConfig(&s_config);
            } else {
                ESP_LOGI(TAG, "Configuration loaded from NVS");
            }
        } else if (ret == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGI(TAG, "No configuration found in NVS, using defaults");
            // Save defaults to NVS
            saveConfig(&DEFAULT_CONFIG);
        } else {
            ESP_LOGE(TAG, "Failed to load config from NVS: %s", esp_err_to_name(ret));
            s_config = DEFAULT_CONFIG;
        }
    } else {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        s_config = DEFAULT_CONFIG;
    }

    s_initialized = true;
    return ESP_OK;
}

esp_err_t getConfig(AppConfig* config) {
    if (!s_initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    *config = s_config;
    return ESP_OK;
}

esp_err_t saveConfig(const AppConfig* config) {
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return ret;
    }

    // Create a copy with current version
    AppConfig configToSave = *config;
    configToSave.version = CONFIG_VERSION;

    ret = nvs_set_blob(handle, NVS_KEY_CONFIG, &configToSave, sizeof(AppConfig));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set config blob: %s", esp_err_to_name(ret));
        nvs_close(handle);
        return ret;
    }

    ret = nvs_commit(handle);
    nvs_close(handle);

    if (ret == ESP_OK) {
        // Update cached config
        s_config = configToSave;
        ESP_LOGI(TAG, "Configuration saved to NVS");
    } else {
        ESP_LOGE(TAG, "Failed to commit config: %s", esp_err_to_name(ret));
    }

    return ret;
}

esp_err_t getUartConfig(UartConfig* config) {
    if (!s_initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    *config = s_config.uart;
    return ESP_OK;
}

esp_err_t saveUartConfig(const UartConfig* config) {
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    s_config.uart = *config;
    return saveConfig(&s_config);
}

esp_err_t getParallelPortConfig(ParallelPortConfig* config) {
    if (!s_initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    *config = s_config.parallelPort;
    return ESP_OK;
}

esp_err_t saveParallelPortConfig(const ParallelPortConfig* config) {
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    s_config.parallelPort = *config;
    return saveConfig(&s_config);
}

esp_err_t setTransportType(Transport::Type type) {
    if (!s_initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    s_config.transportType = type;
    return saveConfig(&s_config);
}

Transport::Type getTransportType() {
    if (!s_initialized) {
        return Transport::Type::UART; // Default
    }
    return s_config.transportType;
}

esp_err_t resetToDefaults() {
    s_config = DEFAULT_CONFIG;
    return saveConfig(&s_config);
}

} // namespace ConfigManager

