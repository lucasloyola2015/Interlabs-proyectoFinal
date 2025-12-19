#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/ringbuf.h"

/**
 * @brief Common types and definitions for transport layer
 */

namespace Transport {

/// Callback for burst events (start/end of data burst)
using BurstCallback = void (*)(bool burstEnded, size_t bytesInBurst);

/// Statistics structure common to all transports
struct Stats {
    size_t totalBytesReceived;      ///< Total bytes received since init
    size_t bytesInCurrentBurst;      ///< Bytes in current active burst
    uint32_t burstCount;             ///< Number of bursts detected
    uint32_t overflowCount;           ///< Number of buffer overflows
    bool burstActive;                 ///< Whether a burst is currently active
};

/// Transport type enumeration
enum class Type {
    UART,           ///< UART serial interface
    PARALLEL_PORT   ///< 8-bit parallel port with strobe
};

} // namespace Transport

