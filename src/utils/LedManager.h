#pragma once

#include "esp_err.h"
#include <cstdint>


namespace LedManager {

/**
 * @brief LED States for the system state machine
 */
enum class State {
  IDLE,          ///< OFF
  STARTUP,       ///< Continuous ON (100% duty)
  DATA_ACTIVITY, ///< Blink 50% duty, 100ms period (50ms ON / 50ms OFF)
  HOLD_3S,       ///< 300ms ON / 300ms OFF (Button 0-3s)
  HOLD_8S,       ///< 1.25s ON / 1.25s OFF (Button 3-8s, Freq 0.4Hz)
  FACTORY_READY  ///< Continuous ON (Button >8s)
};

/**
 * @brief Initialize the LED manager
 *
 * Sets up the GPIO and the hardware timer to control LED patterns.
 * Default LED GPIO is 2 (common for many ESP32 boards).
 *
 * @return ESP_OK on success
 */
esp_err_t init();

/**
 * @brief Set the current LED state
 *
 * @param state New state to apply
 */
void setState(State state);

/**
 * @brief Get the current LED state
 *
 * @return Current State
 */
State getState();

/**
 * @brief Signal data activity
 *
 * Temporarily switches to DATA_ACTIVITY state if currently IDLE.
 * Should be called by transport/pipeline when data is flowing.
 *
 * @param active true if data is flowing
 */
void setDataActivity(bool active);

} // namespace LedManager
