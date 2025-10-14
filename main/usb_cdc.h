#pragma once

#include <cstdarg>

namespace usb_cdc {

/** @brief Initialise the TinyUSB CDC layer (no-op on unsupported targets). */
void init();
/** @brief Send a null-terminated string to the virtual COM port. */
void print(const char *message);
/** @brief printf-style helper that writes formatted data to the virtual COM port. */
void printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace usb_cdc
