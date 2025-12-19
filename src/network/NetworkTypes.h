#pragma once

#include "esp_err.h"
#include <cstdint>
#include <cstddef>

/**
 * @brief Network types and common definitions
 */

namespace Network {

/// Network interface types
enum class Type {
    ETHERNET,
    WIFI
};

/// Network connection status
enum class Status {
    DISCONNECTED,
    CONNECTING,
    CONNECTED,
    ERROR
};

/// IP configuration mode
enum class IpMode {
    DHCP,      ///< Dynamic IP (DHCP)
    STATIC     ///< Static IP configuration
};

/// IP address structure
struct IpAddress {
    uint8_t addr[4];  ///< IPv4 address (e.g., {192, 168, 1, 100})
    
    IpAddress() : addr{0, 0, 0, 0} {}
    IpAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d) : addr{a, b, c, d} {}
    
    bool operator==(const IpAddress& other) const {
        return addr[0] == other.addr[0] && addr[1] == other.addr[1] &&
               addr[2] == other.addr[2] && addr[3] == other.addr[3];
    }
    
    bool isZero() const {
        return addr[0] == 0 && addr[1] == 0 && addr[2] == 0 && addr[3] == 0;
    }
};

/// Network statistics
struct Stats {
    uint64_t bytesReceived;
    uint64_t bytesSent;
    uint32_t packetsReceived;
    uint32_t packetsSent;
    uint32_t errors;
};

} // namespace Network

