#pragma once

#include "esp_system.h"
#include "transport/IDataSource.h"
#include <cstdint>
#include <cstddef>

/**
 * @brief Command System - Centralized command manager
 *
 * Handles commands from multiple communication mediums (UART, Web, MQTT)
 * with permission-based access control.
 */

namespace CommandSystem {

/**
 * @brief Communication mediums
 */
enum class Medium : uint8_t {
  DEBUG = 1 << 0,  ///< Debug UART (UART0)
  WEB = 1 << 1,    ///< Web Server (HTTP)
  MQTT = 1 << 2    ///< MQTT broker
};

/**
 * @brief Bitmask for medium permissions
 */
using MediumMask = uint8_t;

/**
 * @brief Command result
 */
struct CommandResult {
  esp_err_t status;      ///< Execution status (ESP_OK on success)
  const char *message;   ///< Result message (e.g., "FORMAT_OK", "STATS_DATA")
  const char *data;      ///< Optional data payload (JSON, text, etc.)
  size_t dataLen;        ///< Data payload length
};

/**
 * @brief Command handler function type
 * @param args Command arguments (parsed from command string)
 * @param argsLen Length of arguments string
 * @param result Output result structure
 * @return ESP_OK if command executed successfully
 */
typedef esp_err_t (*CommandHandler)(const char *args, size_t argsLen,
                                    CommandResult *result);

/**
 * @brief Response callback function type
 * Called to send command response back through the originating medium
 * @param medium Medium that received the command
 * @param result Command execution result
 * @param userCtx User context provided when registering the callback
 */
typedef void (*ResponseCallback)(Medium medium, const CommandResult *result,
                                 void *userCtx);

/**
 * @brief Command registration structure
 */
struct Command {
  const char *name;           ///< Command name (e.g., "format", "stats")
  CommandHandler handler;     ///< Command handler function
  MediumMask allowedMediums;  ///< Bitmask of allowed mediums (Medium::DEBUG | Medium::WEB)
  const char *description;    ///< Command description for help
};

/**
 * @brief Initialize command system
 * @param dataSource Global transport instance for stats and control
 * @return ESP_OK on success
 */
esp_err_t initialize(IDataSource *dataSource);

/**
 * @brief Deinitialize command system
 */
void deinit();

/**
 * @brief Register a command
 * @param cmd Command structure
 * @return ESP_OK on success
 */
esp_err_t registerCommand(const Command &cmd);

/**
 * @brief Execute a command from a specific medium
 * @param medium Medium that originated the command
 * @param cmdStr Full command string (e.g., "format", "stats", "read 0 256")
 * @return CommandResult with execution result
 */
CommandResult executeCommand(Medium medium, const char *cmdStr);

/**
 * @brief Register response callback for a medium
 * @param medium Medium to register callback for
 * @param callback Callback function
 * @param userCtx User context to pass to callback
 * @return ESP_OK on success
 */
esp_err_t registerResponseCallback(Medium medium, ResponseCallback callback,
                                   void *userCtx);

/**
 * @brief Unregister response callback for a medium
 * @param medium Medium to unregister
 */
void unregisterResponseCallback(Medium medium);

/**
 * @brief Get list of available commands (for help, etc.)
 * @param commands Output array of command pointers
 * @param maxCommands Maximum number of commands to return
 * @return Number of commands returned
 */
size_t getAvailableCommands(const Command **commands, size_t maxCommands);

/**
 * @brief Get list of commands available for a specific medium
 * @param medium Medium to check
 * @param commands Output array of command pointers
 * @param maxCommands Maximum number of commands to return
 * @return Number of commands returned
 */
size_t getAvailableCommandsForMedium(Medium medium, const Command **commands,
                                     size_t maxCommands);

} // namespace CommandSystem
