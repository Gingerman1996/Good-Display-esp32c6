#include "ft6336.h"

#include <algorithm>
#include <array>
#include <cstring>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace ft6336 {
namespace {

constexpr const char *TAG = "ft6336";
constexpr uint8_t kI2cAddress = 0x38;

/** @brief Return a GPIO bit mask used with gpio_config. */
uint64_t maskFor(gpio_num_t gpio) {
    return 1ULL << static_cast<uint32_t>(gpio);
}

}  // namespace

/** @brief Check whether the provided GPIO number is valid for the current target. */
bool Driver::gpioIsValid(gpio_num_t gpio) {
    return gpio >= 0 && gpio < GPIO_NUM_MAX;
}

/**
 * @brief Configure GPIO and I²C resources required by the FT6336 touch controller.
 */
esp_err_t Driver::init(const Config &config) {
    ESP_RETURN_ON_FALSE(!initialised_, ESP_ERR_INVALID_STATE, TAG, "driver already initialised");
    ESP_RETURN_ON_FALSE(gpioIsValid(config.sda) && gpioIsValid(config.scl) &&
                            gpioIsValid(config.rst) && gpioIsValid(config.interrupt),
                        ESP_ERR_INVALID_ARG, TAG, "invalid GPIO assignment");

    cfg_ = config;
    if (cfg_.clk_speed_hz == 0) {
        cfg_.clk_speed_hz = 100000;
    }

    gpio_config_t rst_conf = {
        .pin_bit_mask = maskFor(cfg_.rst),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&rst_conf), TAG, "rst gpio config failed");
    gpio_set_level(cfg_.rst, 1);

    gpio_config_t int_conf = {
        .pin_bit_mask = maskFor(cfg_.interrupt),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&int_conf), TAG, "int gpio config failed");

    i2c_config_t i2c_conf = {};
    i2c_conf.mode = I2C_MODE_MASTER;
    i2c_conf.sda_io_num = cfg_.sda;
    i2c_conf.scl_io_num = cfg_.scl;
    i2c_conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
    i2c_conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
    i2c_conf.master.clk_speed = cfg_.clk_speed_hz;
    ESP_RETURN_ON_ERROR(i2c_param_config(cfg_.port, &i2c_conf), TAG, "i2c param config failed");
    ESP_RETURN_ON_ERROR(i2c_driver_install(cfg_.port, I2C_MODE_MASTER, 0, 0, 0), TAG,
                        "i2c driver install failed");

    reset();

    ESP_RETURN_ON_ERROR(writeRegister(0x00, 0x00), TAG, "set device mode failed");
    ESP_RETURN_ON_ERROR(writeRegister(0x80, 22), TAG, "set threshold failed");
    ESP_RETURN_ON_ERROR(writeRegister(0x88, 14), TAG, "set period active failed");

    initialised_ = true;
    ESP_LOGI(TAG, "initialised touch controller");
    return ESP_OK;
}

/** @brief Indicate whether the INT pin reports an active touch. */
bool Driver::touchReady() const {
    if (!initialised_) {
        return false;
    }
    return gpio_get_level(cfg_.interrupt) == 0;
}

/**
 * @brief Read available touch points and populate the provided TouchData structure.
 */
esp_err_t Driver::scan(TouchData &touch) {
    ESP_RETURN_ON_FALSE(initialised_, ESP_ERR_INVALID_STATE, TAG, "driver not initialised");

    touch.count = 0;
    for (auto &point : touch.points) {
        point = {};
    }

    uint8_t status = 0;
    ESP_RETURN_ON_ERROR(readRegister(0x02, &status, 1), TAG, "read status failed");
    uint8_t reported = status & 0x0F;
    if (reported == 0) {
        return ESP_OK;
    }
    reported = std::min<uint8_t>(reported, static_cast<uint8_t>(touch.points.size()));

    const std::array<uint8_t, 2> base_regs = {0x03, 0x09};
    for (uint8_t idx = 0; idx < reported; ++idx) {
        uint8_t buf[4] = {0};
        ESP_RETURN_ON_ERROR(readRegister(base_regs[idx], buf, sizeof(buf)), TAG,
                            "read point %u failed", static_cast<unsigned>(idx));
        bool contact = (buf[0] & 0xC0) == 0x80;
        if (!contact) {
            continue;
        }

        Point point{};
        point.x = static_cast<uint16_t>(((buf[0] & 0x0F) << 8) | buf[1]);
        point.y = static_cast<uint16_t>(((buf[2] & 0x0F) << 8) | buf[3]);
        point.id = static_cast<uint8_t>(buf[2] >> 4);
        point.event = static_cast<uint8_t>(buf[0] >> 6);
        point.valid = true;

        if (touch.count < touch.points.size()) {
            touch.points[touch.count] = point;
            touch.count++;
        }
    }

    return ESP_OK;
}

/** @brief Issue a hardware reset sequence with datasheet-compliant delays. */
void Driver::reset() const {
    gpio_set_level(cfg_.rst, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(cfg_.rst, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
}

/** @brief Write a single register in the controller using a blocking I²C call. */
esp_err_t Driver::writeRegister(uint8_t reg, uint8_t value) {
    uint8_t payload[2] = {reg, value};
    return i2c_master_write_to_device(cfg_.port, kI2cAddress, payload, sizeof(payload),
                                      pdMS_TO_TICKS(100));
}

/** @brief Read one or more bytes starting at the provided register. */
esp_err_t Driver::readRegister(uint8_t reg, uint8_t *data, size_t len) {
    return i2c_master_write_read_device(cfg_.port, kI2cAddress, &reg, 1, data, len,
                                        pdMS_TO_TICKS(100));
}

}  // namespace ft6336
