#pragma once

#include "esp_err.h"
#include "../mqtt/MqttManager.h"

/**
 * @brief MQTT Command Handler
 *
 * Handles command execution via MQTT messages.
 * Parses JSON commands from subscribed topics and publishes responses.
 * Uses MqttManager for all MQTT communication to maintain abstraction.
 */

namespace MqttCommandHandler {

/**
 * @brief Initialize MQTT command handler
 * Sets up message callback to process commands from MQTT
 * @param mqttManager Pointer to MqttManager instance
 * @return ESP_OK on success
 */
esp_err_t init(MqttManager *mqttManager);

/**
 * @brief Deinitialize MQTT command handler
 */
void deinit();

/**
 * @brief Check if handler is currently active (MQTT connected)
 * @return true if handler is active and ready to process commands
 */
bool isActive();

/**
 * @brief Process incoming MQTT message
 * Called automatically by message callback
 * @param topic Topic where message was received
 * @param payload Message payload (JSON)
 * @param payloadLen Payload length
 */
void processMessage(const char *topic, const uint8_t *payload, size_t payloadLen);

} // namespace MqttCommandHandler

