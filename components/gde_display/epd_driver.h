#pragma once

#include <cstddef>
#include <cstdint>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"

namespace epd {

constexpr int kWidth = 480;
constexpr int kHeight = 800;
constexpr int kBufferSize = kWidth * kHeight / 8;

/** @brief SPI + GPIO configuration required by the e-paper panel. */
struct Config {
    spi_host_device_t host = SPI2_HOST;
    gpio_num_t mosi = GPIO_NUM_NC;
    gpio_num_t sclk = GPIO_NUM_NC;
    gpio_num_t cs = GPIO_NUM_NC;
    gpio_num_t dc = GPIO_NUM_NC;
    gpio_num_t rst = GPIO_NUM_NC;
    gpio_num_t busy = GPIO_NUM_NC;
    int clk_speed_hz = 10 * 1000 * 1000;
};

class Driver {
  public:
    Driver() = default;
    /** @brief Automatically calls deinit() when the driver is destroyed. */
    ~Driver();

    /** @brief Prepare SPI bus and associated GPIO lines. */
    esp_err_t init(const Config &config);
    /** @brief Release SPI resources if they were previously acquired. */
    void deinit();

    /** @brief Send the panel initialisation sequence. */
    esp_err_t hardwareInit(bool fast_mode = false);
    /** @brief Clear the frame buffer with a single byte pattern. */
    esp_err_t clear(uint8_t fill_byte);
    /** @brief Upload an entire frame and trigger a refresh. */
    esp_err_t loadBaseMap(const uint8_t *data, bool fast_mode);
    /** @brief Render five bitmap regions positioned to show a multi-digit value. */
    esp_err_t displayDigits(uint16_t x_startA, uint16_t y_startA, const uint8_t *datasA,
                            uint16_t x_startB, uint16_t y_startB, const uint8_t *datasB,
                            uint16_t x_startC, uint16_t y_startC, const uint8_t *datasC,
                            uint16_t x_startD, uint16_t y_startD, const uint8_t *datasD,
                            uint16_t x_startE, uint16_t y_startE, const uint8_t *datasE,
                            uint16_t part_column, uint16_t part_line);
    /** @brief Upload a single-bit bitmap and trigger a partial refresh. 
     *  @param skip_refresh If true, only upload data without triggering refresh (for batching).
     */
    esp_err_t drawBitmap(uint16_t x_start, uint16_t y_start, const uint8_t *bitmap,
                         uint16_t width_bits, uint16_t height_rows, bool skip_refresh = false);
    /** @brief Trigger a partial refresh without uploading new data. */
    esp_err_t triggerRefresh();
    /** @brief Request the display controller to enter deep sleep. */
    esp_err_t deepSleep();

  private:
    Config cfg_{};
    spi_device_handle_t spi_{nullptr};
    bool initialised_{false};

    /** @brief Toggle the reset pin low/high with the required delay. */
    void reset() const;
    /** @brief Poll the BUSY pin until the controller is ready. */
    void waitWhileBusy() const;
    /** @brief Send a single command byte over SPI. */
    esp_err_t sendCommand(uint8_t cmd);
    /** @brief Send a command followed by an optional payload. */
    esp_err_t sendCommand(uint8_t cmd, const uint8_t *data, size_t len);
    /** @brief Send a raw data buffer with the DC pin set high. */
    esp_err_t sendData(const uint8_t *data, size_t len);
    /** @brief Load the default LUT table (temperature-based). */
    esp_err_t writeLutDefault();
    /** @brief Load the fast update LUT table. */
    esp_err_t writeLutFast();
    /** @brief Write a LUT blob to the controller registers. */
    esp_err_t writeLut(const uint8_t *waveform);
    /** @brief Trigger a display update sequence. */
    esp_err_t updatePanel(bool fast_mode);
    /** @brief Trigger a partial update sequence. */
    esp_err_t partialUpdate();
    /** @brief Upload a bitmap into a selected RAM window. */
    esp_err_t writePartialWindow(uint16_t x_start, uint16_t y_start, const uint8_t *datas,
                                 uint16_t part_column, uint16_t part_line);
};

}  // namespace epd
