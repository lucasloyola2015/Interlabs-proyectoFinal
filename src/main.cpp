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
#include <cstdint>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#include "config/AppConfig.h"
#include "config/ConfigManager.h"
#include "esp_event.h"
#include "network/INetworkInterface.h"
#include "network/ethernet/EthernetW5500.h"
#include "network/wifi/WifiInterface.h"
#include "pipeline/DataPipeline.h"
#include "storage/FlashRing.h"
#include "transport/IDataSource.h"
#include "transport/parallel/ParallelPortCapture.h"
#include "transport/uart/UartCapture.h"
#include "webserver/WebServer.h"

#include "utils/ButtonMonitor.h"
#include "utils/CommandSystem.h"
#include "utils/LedManager.h"
#include "mqtt/MqttManager.h"
#include "utils/MqttCommandHandler.h"

static const char *TAG = "DataLogger";

// Global instances
static IDataSource *g_dataSource = nullptr;
static INetworkInterface *g_networkInterface = nullptr;
static MqttManager g_mqttManager;  // Global to avoid stack overflow

// Burst callback - called when a data burst ends
static void onBurstEnd(bool ended, size_t bytes) {
  if (ended)
    DataPipeline::flush();
}

extern "C" void app_main(void) {
  // 0. Initialize LED Manager early (Startup State)
  LedManager::init();
  LedManager::setState(LedManager::State::STARTUP);

  ESP_LOGI(TAG, "======================================");
  ESP_LOGI(TAG, "  ESP32 DataLogger - Startup");
  ESP_LOGI(TAG, "======================================");

  // 1. Core System Components
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  ESP_ERROR_CHECK(ConfigManager::init());
  ESP_ERROR_CHECK(FlashRing::init("datalog"));

  // 2. Load Configuration
  ConfigManager::FullConfig appConfig;
  if (ConfigManager::getConfig(&appConfig) != ESP_OK) {
    ESP_LOGE(TAG, "FALLO CRÍTICO: No se pudo cargar la configuración.");
  }

  // 2.1 Check for SAFE MODE
  bool safeModeActive = ConfigManager::getSafeMode();
  if (safeModeActive) {
    ESP_LOGW(TAG, "========================================");
    ESP_LOGW(TAG, "  !!! SAFE MODE DETECTED !!!");
    ESP_LOGW(TAG, "========================================");

    // Clear safe mode flag immediately to prevent loop
    ConfigManager::setSafeMode(false);

    // Disable LAN and WLAN-OP in RAM for this session
    appConfig.network.lan.enabled = false;
    appConfig.network.wlanOp.enabled = false;

    ESP_LOGW(TAG, "Safe Mode: LAN/WLAN-OP deshabilitados temporalmente.");
  } else {
    ESP_LOGI(TAG, "Arranque Normal. Configuración actual:");
    ESP_LOGI(TAG, "  - LAN: %s (IP: %d.%d.%d.%d)",
             appConfig.network.lan.enabled ? "SI" : "NO",
             appConfig.network.lan.staticIp.addr[0],
             appConfig.network.lan.staticIp.addr[1],
             appConfig.network.lan.staticIp.addr[2],
             appConfig.network.lan.staticIp.addr[3]);
    ESP_LOGI(TAG, "  - WiFi OP: %s (SSID: %s)",
             appConfig.network.wlanOp.enabled ? "SI" : "NO",
             appConfig.network.wlanOp.ssid);
  }

  // 3. Initialize Transport
  // Skip transport initialization in safe mode
  if (!safeModeActive) {
    // TODO: Transport needs refactoring to work with new config structure
    // For now, transport is disabled until proper integration
    ESP_LOGW(TAG, "Transport initialization temporarily disabled");
  } else {
    ESP_LOGW(TAG, "Transport/DataPipeline disabled in SAFE MODE");
  }

  // 4. Initialize Data Pipeline
  // Only initialize if transport is available and NOT in safe mode
  if (g_dataSource && !safeModeActive) {
    DataPipeline::Config pipeConfig = {
        .writeChunkSize = 12288, .flushTimeoutMs = 500, .autoStart = true};
    ESP_ERROR_CHECK(DataPipeline::init(pipeConfig, g_dataSource));
  } else {
    if (safeModeActive) {
      ESP_LOGW(TAG, "DataPipeline disabled in SAFE MODE");
    } else {
      ESP_LOGW(TAG,
               "DataPipeline initialization skipped - no transport available");
    }
  }

  // 5. Initialize Network (Ethernet + WiFi)
  // We'll use either Ethernet, WiFi STA, or WiFi AP (Safe Mode)
  static EthernetW5500 ethernet;
  static WifiInterface wifi;

  // Initialize Ethernet if LAN is enabled
  if (appConfig.network.lan.enabled) {
    EthernetW5500::Config ethCfg; // USE CORRECT CLASS CONFIG
    ethCfg.ipMode = appConfig.network.lan.useDhcp ? Network::IpMode::DHCP
                                                  : Network::IpMode::STATIC;
    ethCfg.staticIp = appConfig.network.lan.staticIp;
    ethCfg.staticNetmask = appConfig.network.lan.netmask;
    ethCfg.staticGateway = appConfig.network.lan.gateway;

    // Use default pins defined in EthernetW5500.h or customize here if needed
    // CS=21, RST=22, INT=25, SCLK=18, MISO=19, MOSI=23

    ESP_LOGI(TAG, "Iniciando LAN W5500 (%d.%d.%d.%d)...",
             ethCfg.staticIp.addr[0], ethCfg.staticIp.addr[1],
             ethCfg.staticIp.addr[2], ethCfg.staticIp.addr[3]);

    if (ethernet.init(&ethCfg) == ESP_OK && ethernet.start() == ESP_OK) {
      g_networkInterface = &ethernet;
      ESP_LOGI(TAG, "LAN lista.");
    } else {
      ESP_LOGE(TAG, "ERROR al iniciar LAN (Hardware W5500 no responde?)");
    }
  }

  // Initialize WiFi (STA o AP)
  // SOLO inicia AP si el Safe Mode está activo.
  bool startAP = safeModeActive;

  if (startAP || appConfig.network.wlanOp.enabled) {
    WifiInterface::Config wifiCfg;

    if (startAP) {
      // Initialize WLAN-SAFE (AP Mode)
      wifiCfg.enabled = true;
      wifiCfg.apMode = true;
      strncpy(wifiCfg.apSsid, appConfig.network.wlanSafe.ssid,
              sizeof(wifiCfg.apSsid) - 1);
      strncpy(wifiCfg.apPassword, appConfig.network.wlanSafe.password,
              sizeof(wifiCfg.apPassword) - 1);
      wifiCfg.apChannel = appConfig.network.wlanSafe.channel;
      wifiCfg.staticIp = appConfig.network.wlanSafe.apIp;

      ESP_LOGW(TAG, "Iniciando WiFi AP (%s) como %s", wifiCfg.apSsid,
               safeModeActive ? "MODO SEGURO" : "MODO FALLBACK");
    } else {
      // Initialize WLAN-OP (STA Mode)
      wifiCfg.enabled = true;
      wifiCfg.apMode = false;
      strncpy(wifiCfg.ssid, appConfig.network.wlanOp.ssid,
              sizeof(wifiCfg.ssid) - 1);
      strncpy(wifiCfg.password, appConfig.network.wlanOp.password,
              sizeof(wifiCfg.password) - 1);
      wifiCfg.ipMode = appConfig.network.wlanOp.useDhcp
                           ? Network::IpMode::DHCP
                           : Network::IpMode::STATIC;
      wifiCfg.staticIp = appConfig.network.wlanOp.staticIp;
      wifiCfg.staticNetmask = appConfig.network.wlanOp.netmask;
      wifiCfg.staticGateway = appConfig.network.wlanOp.gateway;

      ESP_LOGI(TAG, "Iniciando WiFi STA (%s)...", wifiCfg.ssid);
    }

    if (wifi.init(&wifiCfg) == ESP_OK && wifi.start() == ESP_OK) {
      if (!g_networkInterface) {
        g_networkInterface = &wifi;
      }
      ESP_LOGI(TAG, "WiFi interface initialized (%s)",
               wifiCfg.apMode ? "AP" : "STA");
    } else {
      ESP_LOGE(TAG, "Failed to initialize WiFi");
    }
  }

  // 6. Initialize Web Server
  if (g_networkInterface) {
    if (WebServer::init(&ethernet, &wifi, appConfig.network.webServerPort) ==
        ESP_OK) {
      WebServer::DataLoggerCallbacks callbacks = {
          .getFlashStats =
              [](void *s) {
                return FlashRing::getStats((FlashRing::Stats *)s);
              },
          .getTransportStats =
              [](void *s) {
                return g_dataSource
                           ? g_dataSource->getStats((Transport::Stats *)s)
                           : ESP_FAIL;
              },
          .getPipelineStats =
              [](void *s) {
                return DataPipeline::getStats((DataPipeline::Stats *)s);
              },
          .getTransportTypeName =
              []() {
                return g_dataSource
                           ? (g_dataSource->getType() == Transport::Type::UART
                                  ? "uart"
                                  : "parallel_port")
                           : "none";
              },
          .formatFlash =
              []() {
                esp_err_t r = FlashRing::erase();
                if (r == ESP_OK) {
                  if (g_dataSource)
                    g_dataSource->resetStats();
                  DataPipeline::resetStats();
                }
                return r;
              },
          .readFlash = [](uint32_t o, uint32_t l, uint8_t *b,
                          size_t *r) { return FlashRing::readAt(o, b, l, r); }};
      WebServer::setDataLoggerCallbacks(&callbacks);
      ESP_LOGI(TAG, "Web Server ready");
    }
  }

  // 7. Start UI/CLI Interfaces
  CommandSystem::initialize(g_dataSource);

  // 8. Initialize MQTT (if network is available - MQTT can work for both COORDINADOR and ENDPOINT)
  if (g_networkInterface) {
    if (g_mqttManager.init() == ESP_OK) {
      ESP_LOGI(TAG, "MQTT Manager initialized");
      
      // Initialize MQTT command handler (pass MqttManager, not MqttClient)
      if (MqttCommandHandler::init(&g_mqttManager) == ESP_OK) {
        ESP_LOGI(TAG, "MQTT Command Handler initialized");
      } else {
        ESP_LOGW(TAG, "MQTT Command Handler initialization failed");
      }
      
      // Try to connect MQTT
      if (g_mqttManager.connect() == ESP_OK) {
        ESP_LOGI(TAG, "MQTT connecting...");
      } else {
        ESP_LOGW(TAG, "MQTT connection failed");
      }
    } else {
      ESP_LOGW(TAG, "MQTT Manager initialization failed");
    }
  }

  // 9. Start Button Monitor (for SAFE MODE trigger)
  ESP_ERROR_CHECK(ButtonMonitor::init());

  ESP_LOGI(TAG, "System Ready. Free heap: %lu bytes", esp_get_free_heap_size());
  esp_log_level_set("*", ESP_LOG_INFO);

  // Initialization finished - LED to IDLE
  LedManager::setState(LedManager::State::IDLE);

  // Main Monitoring Loop
  uint32_t uptime = 0;
  while (true) {
    bool connected = g_networkInterface && g_networkInterface->isConnected();
    if (connected && !WebServer::isRunning()) {
      ESP_LOGI(TAG, "Network UP - Starting Web Server");
      WebServer::start();
    }

    if (uptime % 60 == 0) {
      ESP_LOGI(TAG, "Heartbeat: Uptime=%lu s, Heap=%lu, Net=%s", uptime,
               esp_get_free_heap_size(), connected ? "UP" : "DOWN");
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
    uptime++;
  }
}
