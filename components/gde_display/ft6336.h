#pragma once

#include <array>
#include <cstdint>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_err.h"

namespace ft6336 {

/** @brief Parameters needed to configure the FT6336 touch controller. */
struct Config {
    i2c_port_t port = I2C_NUM_0;
    gpio_num_t sda = GPIO_NUM_NC;
    gpio_num_t scl = GPIO_NUM_NC;
    gpio_num_t rst = GPIO_NUM_NC;
    gpio_num_t interrupt = GPIO_NUM_NC;
    uint32_t clk_speed_hz = 400000;
};

/** @brief Represents a single reported touch point. */
struct Point {
    uint16_t x = 0;
    uint16_t y = 0;
    uint8_t id = 0;
    uint8_t event = 0;
    bool valid = false;
};

/** @brief Container for up to two touch points returned by scan(). */
struct TouchData {
    std::array<Point, 2> points{};
    uint8_t count = 0;
};

class Driver {
  public:
    Driver() = default;

    /** @brief Initialise I²C pins and configure the controller registers. */
    esp_err_t init(const Config &config);
    /** @brief Indicate whether the interrupt pin is signalling a touch event. */
    bool touchReady() const;
    /** @brief Read the current touch points into the supplied structure. */
    esp_err_t scan(TouchData &touch);

  private:
    Config cfg_{};
    bool initialised_{false};

    /** @brief Perform a hardware reset on the touch controller. */
    void reset() const;
    /** @brief Validate GPIO numbers prior to configuration. */
    static bool gpioIsValid(gpio_num_t gpio);
    /** @brief Write a single configuration register. */
    esp_err_t writeRegister(uint8_t reg, uint8_t value);
    /** @brief Read one or more bytes from the controller. */
    esp_err_t readRegister(uint8_t reg, uint8_t *data, size_t len);
};

}  // namespace ft6336
