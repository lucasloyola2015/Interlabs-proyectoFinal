#pragma once

#include "../network/INetworkInterface.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include <cstddef>
#include <cstdint>


/**
 * @brief Web Server Module
 *
 * HTTP web server that works with any network interface (Ethernet, WiFi, etc.)
 * Provides REST API endpoints for data logger control and status.
 */

namespace WebServer {

/// HTTP handler function type
typedef esp_err_t (*HttpHandler)(httpd_req_t *req);

/// URI handler registration structure
struct UriHandler {
  const char *uri;
  httpd_method_t method;
  HttpHandler handler;
  bool userCtx = false; ///< If true, pass user context to handler
  void *userCtxData = nullptr;
};

/**
 * @brief Initialize web server
 * @param ethInterface Ethernet network interface (optional)
 * @param wifiInterface WiFi network interface (optional)
 * @param port HTTP server port (default: 80)
 * @return ESP_OK on success
 */
esp_err_t init(INetworkInterface *ethInterface,
               INetworkInterface *wifiInterface, uint16_t port = 80);

/**
 * @brief Start web server
 * @return ESP_OK on success
 */
esp_err_t start();

/**
 * @brief Stop web server
 * @return ESP_OK on success
 */
esp_err_t stop();

/**
 * @brief Deinitialize web server
 * @return ESP_OK on success
 */
esp_err_t deinit();

/**
 * @brief Register URI handler
 * @param handler Handler structure
 * @return ESP_OK on success
 */
esp_err_t registerUri(const UriHandler &handler);

/**
 * @brief Get server handle (for advanced usage)
 * @return httpd_handle_t or nullptr if not initialized
 */
httpd_handle_t getHandle();

/**
 * @brief Check if server is running
 * @return true if running
 */
bool isRunning();

/**
 * @brief Set callback functions to access DataLogger data
 * These callbacks allow the webserver to access DataLogger information
 * without direct dependencies
 */
struct DataLoggerCallbacks {
  // Get flash statistics
  esp_err_t (*getFlashStats)(void *stats);

  // Get transport statistics
  esp_err_t (*getTransportStats)(void *stats);

  // Get pipeline statistics
  esp_err_t (*getPipelineStats)(void *stats);

  // Get transport type name
  const char *(*getTransportTypeName)();

  // Format flash (returns ESP_OK on success)
  esp_err_t (*formatFlash)();

  // Read data from flash
  esp_err_t (*readFlash)(uint32_t offset, uint32_t length, uint8_t *buffer,
                         size_t *bytesRead);

  // User context (optional)
  void *userCtx = nullptr;
};

void setDataLoggerCallbacks(const DataLoggerCallbacks *callbacks);

} // namespace WebServer
