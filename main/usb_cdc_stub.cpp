#include "usb_cdc.h"

namespace usb_cdc {

/** @brief Stub init used when TinyUSB is not available. */
void init() {}

/** @brief Stub print that drops all data. */
void print(const char *) {}

/** @brief Stub printf that drops all formatted data. */
void printf(const char *, ...) {}

}  // namespace usb_cdc
