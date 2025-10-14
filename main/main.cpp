#include <cstdint>

#include "esp_err.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "assets.h"
#include "epd_driver.h"
#include "ft6336.h"

namespace {

constexpr const char *TAG = "app";

/**
 * @brief Draw the initial layout with the current digit on the e-paper display.
 *
 * @param epd_driver Reference to the e-paper driver handling SPI transfers.
 * @param current_value Single digit (0-9) to show on screen.
 */
void showInitialFrame(epd::Driver &epd_driver, int current_value) {
    ESP_ERROR_CHECK(epd_driver.displayDigits(360, 124 + 48 * 0, Num[0],  //
                                             360, 124 + 48 * 1, Num[0],  //
                                             360, 124 + 48 * 2, Num[0],  //
                                             360, 124 + 48 * 3, Num[0],  //
                                             360, 124 + 48 * 4, Num[current_value], 48, 104));
}

/** @brief Return true when the touch point is inside the increment area. */
bool isIncrementArea(const ft6336::Point &point) {
    return point.x > 240 && point.x < 480 && point.y > 400 && point.y < 800;
}

/** @brief Return true when the touch point is inside the decrement area. */
bool isDecrementArea(const ft6336::Point &point) {
    return point.x > 0 && point.x <= 240 && point.y > 400 && point.y < 800;
}

constexpr TickType_t kAutoIncrementInterval = pdMS_TO_TICKS(1000);

/**
 * @brief Scan the I²C bus and log every address that acknowledges.
 *
 * @param port I²C port to scan.
 */
void scanI2CBus(i2c_port_t port) {
    ESP_LOGI(TAG, "Scanning I2C bus on port %d", static_cast<int>(port));
    for (uint8_t addr = 1; addr < 0x7F; ++addr) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        esp_err_t err = i2c_master_cmd_begin(port, cmd, pdMS_TO_TICKS(50));
        i2c_cmd_link_delete(cmd);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "I2C device found at 0x%02X", addr);
        } else if (err != ESP_ERR_TIMEOUT && err != ESP_FAIL) {
            ESP_LOGW(TAG, "I2C scan address 0x%02X returned %s", addr, esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

}  // namespace

/** @brief Application entry point created by ESP-IDF. */
extern "C" void app_main(void) {
    epd::Config epd_cfg;
    epd_cfg.host = SPI2_HOST;
    epd_cfg.mosi = GPIO_NUM_0;
    epd_cfg.sclk = GPIO_NUM_1;
    epd_cfg.cs = GPIO_NUM_21;
    epd_cfg.dc = GPIO_NUM_15;
    epd_cfg.rst = GPIO_NUM_23;
    epd_cfg.busy = GPIO_NUM_20;
    epd_cfg.clk_speed_hz = 10 * 1000 * 1000;

    ft6336::Config ft_cfg;
    ft_cfg.port = I2C_NUM_0;
    ft_cfg.sda = GPIO_NUM_7;
    ft_cfg.scl = GPIO_NUM_6;
    ft_cfg.rst = GPIO_NUM_18;
    ft_cfg.interrupt = GPIO_NUM_19;
    ft_cfg.clk_speed_hz = 100000;

    epd::Driver epd_driver;
    ft6336::Driver touch_driver;

    ESP_LOGI(TAG, "USB CDC support disabled");

    ESP_LOGI(TAG, "initialising peripherals");
    ESP_ERROR_CHECK(epd_driver.init(epd_cfg));
    esp_err_t touch_init_err = touch_driver.init(ft_cfg);
    if (touch_init_err != ESP_OK) {
        ESP_LOGE(TAG, "touch init failed: %s", esp_err_to_name(touch_init_err));
        scanI2CBus(ft_cfg.port);
    }

    ESP_LOGI(TAG, "display full refresh for clean start");
    ESP_ERROR_CHECK(epd_driver.hardwareInit(false));
    ESP_ERROR_CHECK(epd_driver.clear(0xFF));
    vTaskDelay(pdMS_TO_TICKS(1000));

    ESP_ERROR_CHECK(epd_driver.hardwareInit(true));
    ESP_ERROR_CHECK(epd_driver.loadBaseMap(gImage_basemapT, true));

    int current_value = 0;
    showInitialFrame(epd_driver, current_value);
    TickType_t last_increment = xTaskGetTickCount();

    while (true) {
        if (touch_init_err == ESP_OK && touch_driver.touchReady()) {
            ft6336::TouchData touch{};
            esp_err_t err = touch_driver.scan(touch);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "touch scan failed: %s", esp_err_to_name(err));
            } else if (touch.count > 0) {
                const ft6336::Point *active_point = nullptr;
                for (uint8_t i = 0; i < touch.count; ++i) {
                    if (touch.points[i].valid) {
                        active_point = &touch.points[i];
                        break;
                    }
                }

                if (active_point != nullptr) {
                    bool updated = false;
                    if (isIncrementArea(*active_point)) {
                        current_value = (current_value + 1) % 10;
                        updated = true;
                    } else if (isDecrementArea(*active_point)) {
                        current_value = (current_value == 0) ? 9 : current_value - 1;
                        updated = true;
                    }

                    if (updated) {
                        ESP_LOGI(TAG, "touch (%u,%u) -> value %d", active_point->x,
                                 active_point->y, current_value);
                        showInitialFrame(epd_driver, current_value);
                        vTaskDelay(pdMS_TO_TICKS(150));
                    }
                }
            }
        }

        TickType_t now = xTaskGetTickCount();
        if (now - last_increment >= kAutoIncrementInterval) {
            last_increment = now;
            current_value = (current_value + 1) % 10;
            showInitialFrame(epd_driver, current_value);
        }
        vTaskDelay(pdMS_TO_TICKS(15));
    }
}
