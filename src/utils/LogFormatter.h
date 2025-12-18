#pragma once

#include "esp_log.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/**
 * @brief Custom log formatter that removes timestamps
 */

// Custom log formatter that removes timestamp
static inline int custom_log_vprintf(const char *fmt, va_list args) {
  char buffer[512];
  int len = vsnprintf(buffer, sizeof(buffer), fmt, args);
  
  if (len <= 0 || len >= (int)sizeof(buffer)) {
    return vprintf(fmt, args);
  }
  
  // Remove timestamp pattern: "I (12345) " -> "I "
  // Format is typically: "LEVEL (TIMESTAMP) TAG: message"
  if (buffer[0] == 'E' || buffer[0] == 'W' || buffer[0] == 'I' || 
      buffer[0] == 'D' || buffer[0] == 'V') {
    // Look for pattern: level char, space, '(', digits, ')', space
    if (len > 2 && buffer[1] == ' ' && buffer[2] == '(') {
      char *closeParen = strchr(buffer + 2, ')');
      if (closeParen && closeParen[1] == ' ') {
        // Remove "(TIMESTAMP) " part: keep level char and space, skip to after ') '
        memmove(buffer + 1, closeParen + 2, strlen(closeParen + 2) + 1);
      }
    }
  }
  
  return printf("%s", buffer);
}

