#include "usb_cdc.h"

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_cdcacm.h"

namespace usb_cdc {
namespace {

constexpr const char *TAG = "usb_cdc";
constexpr size_t kTxBufferSize = 256;

bool g_initialised = false;
bool g_connected = false;

/** @brief Consume incoming data to keep the CDC endpoint from stalling. */
void handle_rx(int itf, cdcacm_event_t *event) {
    (void)event;
    uint8_t buffer[64];
    size_t read = 0;
    esp_err_t err = tinyusb_cdcacm_read(itf, buffer, sizeof(buffer), &read);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CDC read failed: %s", esp_err_to_name(err));
    }
}

/** @brief Track DTR/RTS state changes to know when a host is listening. */
void handle_line_state(int itf, cdcacm_event_t *event) {
    (void)itf;
    g_connected = event->line_state_changed_data.dtr;
    ESP_LOGI(TAG, "CDC line state changed: DTR=%d RTS=%d", event->line_state_changed_data.dtr,
             event->line_state_changed_data.rts);
}

/** @brief Send a raw byte buffer when the CDC connection is open. */
void write_raw(const char *data, size_t len) {
    if (!g_connected || data == nullptr || len == 0) {
        return;
    }

    while (len > 0) {
        size_t chunk = std::min(len, static_cast<size_t>(64));
        esp_err_t err =
            tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0,
                                       reinterpret_cast<const uint8_t *>(data), chunk);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "CDC write queue failed: %s", esp_err_to_name(err));
            return;
        }
        data += chunk;
        len -= chunk;
    }

    esp_err_t flush_err =
        tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, pdMS_TO_TICKS(10));
    if (flush_err != ESP_OK) {
        ESP_LOGW(TAG, "CDC flush failed: %s", esp_err_to_name(flush_err));
    }
}

}  // namespace

/** @brief Initialise TinyUSB CDC-ACM and register callbacks. */
void init() {
    if (g_initialised) {
        return;
    }

    tinyusb_config_t tusb_cfg = {};
    tusb_cfg.device_descriptor = nullptr;
    tusb_cfg.configuration_descriptor = nullptr;
    tusb_cfg.string_descriptor = nullptr;
    tusb_cfg.external_phy = false;

    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

    tinyusb_config_cdcacm_t cdc_cfg = {};
    cdc_cfg.usb_dev = TINYUSB_USBDEV_0;
    cdc_cfg.cdc_port = TINYUSB_CDC_ACM_0;
    cdc_cfg.rx_unread_buf_sz = 64;
    cdc_cfg.callback_rx = handle_rx;
    cdc_cfg.callback_rx_wanted_char = nullptr;
    cdc_cfg.callback_line_state_changed = handle_line_state;
    cdc_cfg.callback_line_coding_changed = nullptr;

    ESP_ERROR_CHECK(tinyusb_cdcacm_init(&cdc_cfg));

    g_initialised = true;
    ESP_LOGI(TAG, "TinyUSB CDC initialised");
}

/** @brief Write a null-terminated string to the CDC endpoint. */
void print(const char *message) {
    if (message == nullptr) {
        return;
    }
    write_raw(message, std::strlen(message));
}

/** @brief printf-style helper that formats into a ring buffer before sending. */
void printf(const char *fmt, ...) {
    if (fmt == nullptr) {
        return;
    }

    std::array<char, kTxBufferSize> buffer{};
    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(buffer.data(), buffer.size(), fmt, args);
    va_end(args);

    if (written <= 0) {
        return;
    }

    size_t len = std::min(static_cast<size_t>(written), buffer.size() - 1);
    write_raw(buffer.data(), len);
}

}  // namespace usb_cdc
