#include "epd_driver.h"

#include <algorithm>
#include <array>
#include <cstring>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace epd {
namespace {

constexpr const char *TAG = "epd_driver";
constexpr size_t kSpiMaxChunkBytes = 4096;

/** @brief Validate that the supplied GPIO number is inside the supported range. */
bool gpioIsValid(gpio_num_t gpio) {
    return gpio >= 0 && gpio < GPIO_NUM_MAX;
}

/** @brief Helper that returns a bit mask for the given GPIO pin. */
uint64_t maskFor(gpio_num_t gpio) {
    return 1ULL << static_cast<uint32_t>(gpio);
}

constexpr std::array<uint8_t, 112> kWaveform20_80 = {
    0xA0, 0x48, 0x54, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x50, 0x48, 0xA8, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xA0, 0x48, 0x54, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x50, 0x48,
    0xA8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x1A, 0x14, 0x00, 0x00, 0x00, 0x0D, 0x01, 0x0D, 0x01, 0x02, 0x0A, 0x0A, 0x03, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x01, 0x22, 0x22, 0x22, 0x22, 0x22, 0x17, 0x41, 0xA8, 0x32, 0x48, 0x00, 0x00,
};

constexpr std::array<uint8_t, 112> kWaveform80_127 = {
    0xA8, 0x00, 0x55, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x54, 0x00, 0xAA, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xA8, 0x00, 0x55, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x54, 0x00,
    0xAA, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x0C, 0x0D, 0x0B, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0A, 0x0A, 0x05, 0x0B,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x01, 0x22, 0x22, 0x22, 0x22, 0x22, 0x17, 0x41, 0xA8, 0x32, 0x30, 0x00, 0x00,
};

}  // namespace

/** @brief Ensure underlying resources are released when the driver is destroyed. */
Driver::~Driver() {
    deinit();
}

/**
 * @brief Configure GPIO and SPI resources for the e-paper panel.
 *
 * The call must succeed before any other public API is used.
 */
esp_err_t Driver::init(const Config &config) {
    ESP_RETURN_ON_FALSE(!initialised_, ESP_ERR_INVALID_STATE, TAG, "driver already initialised");
    ESP_RETURN_ON_FALSE(gpioIsValid(config.mosi) && gpioIsValid(config.sclk) &&
                            gpioIsValid(config.cs) && gpioIsValid(config.dc) &&
                            gpioIsValid(config.rst) && gpioIsValid(config.busy),
                        ESP_ERR_INVALID_ARG, TAG, "invalid GPIO assignment");

    cfg_ = config;
    if (cfg_.clk_speed_hz <= 0) {
        cfg_.clk_speed_hz = 10 * 1000 * 1000;
    }

    gpio_config_t out_conf = {};
    out_conf.pin_bit_mask = maskFor(cfg_.dc) | maskFor(cfg_.rst);
    out_conf.mode = GPIO_MODE_OUTPUT;
    out_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    out_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    out_conf.intr_type = GPIO_INTR_DISABLE;
    ESP_RETURN_ON_ERROR(gpio_config(&out_conf), TAG, "output gpio config failed");
    gpio_set_level(cfg_.dc, 1);
    gpio_set_level(cfg_.rst, 1);

    gpio_config_t busy_conf = {};
    busy_conf.pin_bit_mask = maskFor(cfg_.busy);
    busy_conf.mode = GPIO_MODE_INPUT;
    busy_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    busy_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    busy_conf.intr_type = GPIO_INTR_DISABLE;
    ESP_RETURN_ON_ERROR(gpio_config(&busy_conf), TAG, "busy gpio config failed");

    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num = cfg_.mosi;
    buscfg.miso_io_num = -1;
    buscfg.sclk_io_num = cfg_.sclk;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    buscfg.max_transfer_sz = kBufferSize + 16;
    ESP_RETURN_ON_ERROR(spi_bus_initialize(cfg_.host, &buscfg, SPI_DMA_CH_AUTO), TAG,
                        "spi bus init failed");

    spi_device_interface_config_t devcfg = {};
    devcfg.clock_speed_hz = cfg_.clk_speed_hz;
    devcfg.mode = 0;
    devcfg.spics_io_num = cfg_.cs;
    devcfg.queue_size = 7;
    devcfg.flags = SPI_DEVICE_NO_DUMMY;
    ESP_RETURN_ON_ERROR(spi_bus_add_device(cfg_.host, &devcfg, &spi_), TAG,
                        "spi add device failed");

    initialised_ = true;
    ESP_LOGI(TAG, "initialised, SPI clock %d Hz", cfg_.clk_speed_hz);
    return ESP_OK;
}

/** @brief Tear down SPI resources and mark the driver as uninitialised. */
void Driver::deinit() {
    if (!initialised_) {
        return;
    }
    if (spi_) {
        spi_bus_remove_device(spi_);
        spi_ = nullptr;
    }
    spi_bus_free(cfg_.host);
    initialised_ = false;
}

/**
 * @brief Run the panel hardware initialisation sequence.
 *
 * @param fast_mode The underlying sequence is shared for both modes but retained for API parity.
 */
esp_err_t Driver::hardwareInit(bool fast_mode) {
    (void)fast_mode;
    ESP_RETURN_ON_FALSE(initialised_, ESP_ERR_INVALID_STATE, TAG, "driver not initialised");

    reset();

    waitWhileBusy();
    ESP_RETURN_ON_ERROR(sendCommand(0x12), TAG, "SWRESET failed");
    waitWhileBusy();

    const std::array<uint8_t, 1> cmd18 = {0x80};
    ESP_RETURN_ON_ERROR(sendCommand(0x18, cmd18.data(), cmd18.size()), TAG, "CMD 0x18 failed");

    const std::array<uint8_t, 5> cmd0c = {0xAE, 0xC7, 0xC3, 0xC0, 0x80};
    ESP_RETURN_ON_ERROR(sendCommand(0x0C, cmd0c.data(), cmd0c.size()), TAG, "CMD 0x0C failed");

    const std::array<uint8_t, 3> cmd01 = {
        static_cast<uint8_t>((kWidth - 1) & 0xFF),
        static_cast<uint8_t>((kWidth - 1) >> 8),
        0x02,
    };
    ESP_RETURN_ON_ERROR(sendCommand(0x01, cmd01.data(), cmd01.size()), TAG, "CMD 0x01 failed");

    const std::array<uint8_t, 1> cmd3c = {0x01};
    ESP_RETURN_ON_ERROR(sendCommand(0x3C, cmd3c.data(), cmd3c.size()), TAG, "CMD 0x3C failed");

    const std::array<uint8_t, 1> cmd11 = {0x03};
    ESP_RETURN_ON_ERROR(sendCommand(0x11, cmd11.data(), cmd11.size()), TAG, "CMD 0x11 failed");

    const std::array<uint8_t, 4> cmd44 = {
        0x00, 0x00,
        static_cast<uint8_t>((kHeight - 1) & 0xFF),
        static_cast<uint8_t>((kHeight - 1) >> 8),
    };
    ESP_RETURN_ON_ERROR(sendCommand(0x44, cmd44.data(), cmd44.size()), TAG, "CMD 0x44 failed");

    const std::array<uint8_t, 4> cmd45 = {
        0x00, 0x00,
        static_cast<uint8_t>((kWidth - 1) & 0xFF),
        static_cast<uint8_t>((kWidth - 1) >> 8),
    };
    ESP_RETURN_ON_ERROR(sendCommand(0x45, cmd45.data(), cmd45.size()), TAG, "CMD 0x45 failed");

    const std::array<uint8_t, 2> cmd4e = {0x00, 0x00};
    ESP_RETURN_ON_ERROR(sendCommand(0x4E, cmd4e.data(), cmd4e.size()), TAG, "CMD 0x4E failed");

    const std::array<uint8_t, 2> cmd4f = {0x00, 0x00};
    ESP_RETURN_ON_ERROR(sendCommand(0x4F, cmd4f.data(), cmd4f.size()), TAG, "CMD 0x4F failed");
    waitWhileBusy();

    const std::array<uint8_t, 1> cmd1a = {0x5A};
    ESP_RETURN_ON_ERROR(sendCommand(0x1A, cmd1a.data(), cmd1a.size()), TAG, "CMD 0x1A failed");

    const std::array<uint8_t, 1> cmd22 = {0x91};
    ESP_RETURN_ON_ERROR(sendCommand(0x22, cmd22.data(), cmd22.size()), TAG, "CMD 0x22 failed");
    ESP_RETURN_ON_ERROR(sendCommand(0x20), TAG, "CMD 0x20 failed");

    waitWhileBusy();
    return ESP_OK;
}

/**
 * @brief Fill the entire display memory with a single byte value,
 *        typically 0xFF (white) or 0x00 (black).
 */
esp_err_t Driver::clear(uint8_t fill_byte) {
    ESP_RETURN_ON_FALSE(initialised_, ESP_ERR_INVALID_STATE, TAG, "driver not initialised");

    ESP_RETURN_ON_ERROR(sendCommand(0x24), TAG, "CMD 0x24 failed");

    std::array<uint8_t, 128> buffer{};
    buffer.fill(fill_byte);

    size_t remaining = kBufferSize;
    while (remaining > 0) {
        size_t chunk = std::min(remaining, buffer.size());
        ESP_RETURN_ON_ERROR(sendData(buffer.data(), chunk), TAG, "fill chunk failed");
        remaining -= chunk;
    }

    return updatePanel(false);
}

/**
 * @brief Upload a full frame image and trigger the appropriate LUT.
 *
 * @param data Pointer to EPD_BUFFER_SIZE bytes.
 * @param fast_mode Choose LUT suitable for fast or full update.
 */
esp_err_t Driver::loadBaseMap(const uint8_t *data, bool fast_mode) {
    ESP_RETURN_ON_FALSE(initialised_, ESP_ERR_INVALID_STATE, TAG, "driver not initialised");
    ESP_RETURN_ON_FALSE(data != nullptr, ESP_ERR_INVALID_ARG, TAG, "data pointer null");

    ESP_RETURN_ON_ERROR(sendCommand(0x24), TAG, "CMD 0x24 failed");
    ESP_RETURN_ON_ERROR(sendData(data, kBufferSize), TAG, "write base map (0x24) failed");

    ESP_RETURN_ON_ERROR(sendCommand(0x26), TAG, "CMD 0x26 failed");
    ESP_RETURN_ON_ERROR(sendData(data, kBufferSize), TAG, "write base map (0x26) failed");

    return updatePanel(fast_mode);
}

/**
 * @brief Write five digit sprites into predefined positions using partial refresh flow.
 */
esp_err_t Driver::displayDigits(uint16_t x_startA, uint16_t y_startA, const uint8_t *datasA,
                                uint16_t x_startB, uint16_t y_startB, const uint8_t *datasB,
                                uint16_t x_startC, uint16_t y_startC, const uint8_t *datasC,
                                uint16_t x_startD, uint16_t y_startD, const uint8_t *datasD,
                                uint16_t x_startE, uint16_t y_startE, const uint8_t *datasE,
                                uint16_t part_column, uint16_t part_line) {
    ESP_RETURN_ON_FALSE(initialised_, ESP_ERR_INVALID_STATE, TAG, "driver not initialised");

    ESP_RETURN_ON_ERROR(writePartialWindow(x_startA, y_startA, datasA, part_column, part_line), TAG,
                        "partial A failed");
    ESP_RETURN_ON_ERROR(writePartialWindow(x_startB, y_startB, datasB, part_column, part_line), TAG,
                        "partial B failed");
    ESP_RETURN_ON_ERROR(writePartialWindow(x_startC, y_startC, datasC, part_column, part_line), TAG,
                        "partial C failed");
    ESP_RETURN_ON_ERROR(writePartialWindow(x_startD, y_startD, datasD, part_column, part_line), TAG,
                        "partial D failed");
    ESP_RETURN_ON_ERROR(writePartialWindow(x_startE, y_startE, datasE, part_column, part_line), TAG,
                        "partial E failed");

    return partialUpdate();
}

esp_err_t Driver::drawBitmap(uint16_t x_start, uint16_t y_start, const uint8_t *bitmap,
                             uint16_t width_bits, uint16_t height_rows) {
    ESP_RETURN_ON_FALSE(initialised_, ESP_ERR_INVALID_STATE, TAG, "driver not initialised");
    ESP_RETURN_ON_FALSE(bitmap != nullptr, ESP_ERR_INVALID_ARG, TAG, "bitmap null");
    ESP_RETURN_ON_FALSE(width_bits != 0 && (width_bits % 8u) == 0, ESP_ERR_INVALID_ARG, TAG,
                        "width must be multiple of 8 bits");
    ESP_RETURN_ON_FALSE(height_rows != 0, ESP_ERR_INVALID_ARG, TAG, "height 0");

    ESP_RETURN_ON_ERROR(writePartialWindow(x_start, y_start, bitmap, height_rows, width_bits), TAG,
                        "partial bitmap failed");
    return partialUpdate();
}

/** @brief Put the panel into deep sleep mode to reduce power consumption. */
esp_err_t Driver::deepSleep() {
    ESP_RETURN_ON_FALSE(initialised_, ESP_ERR_INVALID_STATE, TAG, "driver not initialised");
    const std::array<uint8_t, 1> payload = {0x01};
    ESP_RETURN_ON_ERROR(sendCommand(0x10, payload.data(), payload.size()), TAG, "deep sleep failed");
    vTaskDelay(pdMS_TO_TICKS(100));
    return ESP_OK;
}

/** @brief Pulse the reset line according to the datasheet timing. */
void Driver::reset() const {
    gpio_set_level(cfg_.rst, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(cfg_.rst, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
}

/** @brief Block until the BUSY pin drops low, signalling command completion. */
void Driver::waitWhileBusy() const {
    const TickType_t delay_ticks = pdMS_TO_TICKS(10);
    while (gpio_get_level(cfg_.busy) == 1) {
        vTaskDelay(delay_ticks);
    }
}

/** @brief Write a single command byte on the SPI bus. */
esp_err_t Driver::sendCommand(uint8_t cmd) {
    gpio_set_level(cfg_.dc, 0);
    spi_transaction_t t = {};
    t.length = 8;
    t.flags = SPI_TRANS_USE_TXDATA;
    t.tx_data[0] = cmd;
    return spi_device_polling_transmit(spi_, &t);
}

/**
 * @brief Helper that writes a command byte followed by an optional payload.
 */
esp_err_t Driver::sendCommand(uint8_t cmd, const uint8_t *data, size_t len) {
    ESP_RETURN_ON_ERROR(sendCommand(cmd), TAG, "send command 0x%02X failed", static_cast<int>(cmd));
    if (data != nullptr && len > 0) {
        ESP_RETURN_ON_ERROR(sendData(data, len), TAG, "send data for 0x%02X failed",
                            static_cast<int>(cmd));
    }
    return ESP_OK;
}

/** @brief Stream arbitrary data bytes over SPI while DC is asserted high. */
esp_err_t Driver::sendData(const uint8_t *data, size_t len) {
    if (len == 0) {
        return ESP_OK;
    }

    gpio_set_level(cfg_.dc, 1);
    while (len > 0) {
        size_t chunk = std::min(len, kSpiMaxChunkBytes);
        spi_transaction_t t = {};
        t.length = chunk * 8;
        t.tx_buffer = data;
        ESP_RETURN_ON_ERROR(spi_device_polling_transmit(spi_, &t), TAG, "spi write failed");
        data += chunk;
        len -= chunk;
    }
    return ESP_OK;
}

/** @brief Load the temperature-compensated default waveform. */
esp_err_t Driver::writeLutDefault() {
    return writeLut(kWaveform20_80.data());
}

/** @brief Load the fast-update waveform table. */
esp_err_t Driver::writeLutFast() {
    return writeLut(kWaveform80_127.data());
}

/** @brief Transfer a LUT blob into the controller registers. */
esp_err_t Driver::writeLut(const uint8_t *waveform) {
    ESP_RETURN_ON_FALSE(waveform != nullptr, ESP_ERR_INVALID_ARG, TAG, "waveform null");

    ESP_RETURN_ON_ERROR(sendCommand(0x32, waveform, 105), TAG, "write LUT main failed");
    waitWhileBusy();

    ESP_RETURN_ON_ERROR(sendCommand(0x03, waveform + 105, 1), TAG, "write LUT gate failed");
    ESP_RETURN_ON_ERROR(sendCommand(0x04, waveform + 106, 3), TAG, "write LUT source failed");
    ESP_RETURN_ON_ERROR(sendCommand(0x2C, waveform + 109, 1), TAG, "write LUT vcom failed");

    return ESP_OK;
}

/**
 * @brief Trigger the display update sequence using the selected LUT.
 */
esp_err_t Driver::updatePanel(bool fast_mode) {
    if (fast_mode) {
        ESP_RETURN_ON_ERROR(writeLutFast(), TAG, "fast LUT failed");
    } else {
        ESP_RETURN_ON_ERROR(writeLutDefault(), TAG, "default LUT failed");
    }

    const std::array<uint8_t, 1> control = {0xC7};
    ESP_RETURN_ON_ERROR(sendCommand(0x22, control.data(), control.size()), TAG, "update control");
    ESP_RETURN_ON_ERROR(sendCommand(0x20), TAG, "update trigger");
    waitWhileBusy();
    return ESP_OK;
}

/** @brief Request a partial update sequence using the preloaded buffer. */
esp_err_t Driver::partialUpdate() {
    const std::array<uint8_t, 1> control = {0xFF};
    ESP_RETURN_ON_ERROR(sendCommand(0x22, control.data(), control.size()), TAG,
                        "partial update control");
    ESP_RETURN_ON_ERROR(sendCommand(0x20), TAG, "partial update trigger");
    waitWhileBusy();
    return ESP_OK;
}

/**
 * @brief Configure the RAM window and stream partial image data.
 */
esp_err_t Driver::writePartialWindow(uint16_t x_start, uint16_t y_start, const uint8_t *datas,
                                     uint16_t part_column, uint16_t part_line) {
    ESP_RETURN_ON_FALSE(datas != nullptr, ESP_ERR_INVALID_ARG, TAG, "partial data null");

    uint16_t x_aligned = x_start - (x_start % 8);
    uint16_t x_end = x_aligned + part_line - 1;
    uint16_t y_end = y_start + part_column - 1;

    reset();

    const std::array<uint8_t, 1> cmd18 = {0x80};
    ESP_RETURN_ON_ERROR(sendCommand(0x18, cmd18.data(), cmd18.size()), TAG, "partial cmd 0x18");

    const std::array<uint8_t, 1> cmd3c = {0x80};
    ESP_RETURN_ON_ERROR(sendCommand(0x3C, cmd3c.data(), cmd3c.size()), TAG, "partial cmd 0x3C");

    const std::array<uint8_t, 4> cmd44 = {
        static_cast<uint8_t>(x_aligned & 0xFF),
        static_cast<uint8_t>(x_aligned >> 8),
        static_cast<uint8_t>(x_end & 0xFF),
        static_cast<uint8_t>(x_end >> 8),
    };
    ESP_RETURN_ON_ERROR(sendCommand(0x44, cmd44.data(), cmd44.size()), TAG, "partial cmd 0x44");

    const std::array<uint8_t, 4> cmd45 = {
        static_cast<uint8_t>(y_start & 0xFF),
        static_cast<uint8_t>(y_start >> 8),
        static_cast<uint8_t>(y_end & 0xFF),
        static_cast<uint8_t>(y_end >> 8),
    };
    ESP_RETURN_ON_ERROR(sendCommand(0x45, cmd45.data(), cmd45.size()), TAG, "partial cmd 0x45");

    const std::array<uint8_t, 2> cmd4e = {
        static_cast<uint8_t>(x_aligned & 0xFF),
        static_cast<uint8_t>(x_aligned >> 8),
    };
    ESP_RETURN_ON_ERROR(sendCommand(0x4E, cmd4e.data(), cmd4e.size()), TAG, "partial cmd 0x4E");

    const std::array<uint8_t, 2> cmd4f = {
        static_cast<uint8_t>(y_start & 0xFF),
        static_cast<uint8_t>(y_start >> 8),
    };
    ESP_RETURN_ON_ERROR(sendCommand(0x4F, cmd4f.data(), cmd4f.size()), TAG, "partial cmd 0x4F");

    ESP_RETURN_ON_ERROR(sendCommand(0x24), TAG, "partial cmd 0x24");

    size_t bytes = static_cast<size_t>(part_column) * part_line / 8;
    return sendData(datas, bytes);
}

}  // namespace epd
