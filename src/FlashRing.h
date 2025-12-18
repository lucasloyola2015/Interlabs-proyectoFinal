#pragma once

#include <cstdint>
#include <cstddef>
#include "esp_err.h"
#include "esp_partition.h"

/**
 * @brief FlashRing - Circular buffer on raw flash partition (direct access)
 * 
 * This module provides a persistent circular buffer that stores data directly
 * to a flash partition using direct partition API (no wear leveling).
 * When the buffer is full, oldest data is overwritten.
 * 
 * Features:
 * - Direct flash access for maximum speed
 * - Persistent metadata in NVS (survives reboots)
 * - Block-aligned writes for efficiency
 * - Automatic wrap-around with oldest data discard
 * - Circular writing distributes wear naturally
 */

namespace FlashRing {

/// Block size for flash operations (must match flash page size)
constexpr size_t PAGE_SIZE = 4096;

/// Number of pages to pre-erase ahead of write position
constexpr size_t PRE_ERASE_PAGES = 2;

/// Metadata structure stored in NVS
struct Metadata {
    uint32_t magic;         ///< Validation magic number
    uint32_t head;          ///< Next write position (bytes from partition start)
    uint32_t tail;          ///< Oldest data position (bytes from partition start)
    uint32_t totalWritten;  ///< Total bytes written (lifetime, wraps at 4GB)
    uint32_t wrapCount;     ///< Number of times buffer has wrapped
    uint32_t erasedPages[PRE_ERASE_PAGES]; ///< Pages that are pre-erased (SIZE_MAX = empty)
};

/// Statistics for debugging and monitoring
struct Stats {
    size_t   partitionSize;  ///< Total partition size in bytes
    size_t   usedBytes;      ///< Bytes currently stored
    size_t   freeBytes;      ///< Bytes available before wrap
    uint32_t wrapCount;      ///< Times buffer has wrapped
    uint32_t totalWritten;   ///< Total bytes written (lifetime)
};

/**
 * @brief Initialize the FlashRing module
 * 
 * Mounts the wear leveling partition and loads metadata from NVS.
 * If no valid metadata exists, initializes fresh.
 * 
 * @param partitionLabel Label of the data partition (e.g., "datalog")
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t init(const char* partitionLabel);

/**
 * @brief Write data to the circular buffer
 * 
 * Data is appended at the head position. If there's not enough space,
 * the tail is advanced (discarding oldest data) to make room.
 * 
 * @param data Pointer to data to write
 * @param len  Number of bytes to write
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t write(const uint8_t* data, size_t len);

/**
 * @brief Read data from the circular buffer
 * 
 * Reads from the tail (oldest data) up to len bytes.
 * Does NOT advance the tail - use consume() for that.
 * 
 * @param data     Buffer to read into
 * @param len      Maximum bytes to read
 * @param bytesRead Actual bytes read (output)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t read(uint8_t* data, size_t len, size_t* bytesRead);

/**
 * @brief Read data from a specific offset
 * 
 * @param offset   Offset from tail
 * @param data     Buffer to read into
 * @param len      Maximum bytes to read
 * @param bytesRead Actual bytes read (output)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t readAt(size_t offset, uint8_t* data, size_t len, size_t* bytesRead);

/**
 * @brief Consume (discard) data from the buffer
 * 
 * Advances the tail by the specified number of bytes.
 * 
 * @param len Bytes to consume
 * @return ESP_OK on success
 */
esp_err_t consume(size_t len);

/**
 * @brief Get buffer statistics
 * 
 * @param stats Pointer to stats structure (output)
 * @return ESP_OK on success
 */
esp_err_t getStats(Stats* stats);

/**
 * @brief Erase all data in the buffer
 * 
 * Resets head and tail to zero, erases flash.
 * 
 * @return ESP_OK on success
 */
esp_err_t erase();

/**
 * @brief Flush metadata to NVS
 * 
 * Called periodically or after burst to ensure persistence.
 * 
 * @return ESP_OK on success
 */
esp_err_t flushMetadata();

/**
 * @brief Get current head position
 * @return Head position in bytes
 */
size_t getHead();

/**
 * @brief Get bytes remaining until end of current page
 * @return Bytes until page boundary
 */
size_t getBytesToPageEnd();

/**
 * @brief Deinitialize and unmount
 */
void deinit();

} // namespace FlashRing
