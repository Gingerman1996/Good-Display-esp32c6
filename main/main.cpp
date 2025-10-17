#include <cstdint>

#include "esp_err.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "assets.h"
#include "epd_driver.h"

namespace {

constexpr const char *TAG = "app";

/**
 * @brief Draw the initial layout with the current digit on the e-paper display.
 *
 * @param epd_driver Reference to the e-paper driver handling SPI transfers.
 * @param current_value Single digit (0-9) to show on screen.
 */
void showInitialFrame(epd::Driver &epd_driver, int current_value) {
  ESP_ERROR_CHECK(epd_driver.displayDigits(360, 124 + 48 * 0, Num[current_value], //
                                           360, 124 + 48 * 1, Num[current_value], //
                                           360, 124 + 48 * 2, Num[current_value], //
                                           360, 124 + 48 * 3, Num[current_value], //
                                           360, 124 + 48 * 4, Num[current_value], 48,
                                           104));
}

constexpr TickType_t kAutoIncrementInterval = pdMS_TO_TICKS(1000);

} // namespace

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
  epd_cfg.clk_speed_hz = 20 * 1000 * 1000;

  epd::Driver epd_driver;

  ESP_LOGI(TAG, "USB CDC support disabled");

  ESP_LOGI(TAG, "initialising peripherals");
  ESP_ERROR_CHECK(epd_driver.init(epd_cfg));

  ESP_LOGI(TAG, "display full refresh for clean start");
  ESP_ERROR_CHECK(epd_driver.hardwareInit(false));
  ESP_ERROR_CHECK(epd_driver.clear(0xFF));
  vTaskDelay(pdMS_TO_TICKS(1000));

  ESP_ERROR_CHECK(epd_driver.hardwareInit(true));
  ESP_ERROR_CHECK(epd_driver.loadBaseMap(WhileBG, true));

  int current_value = 0;
  showInitialFrame(epd_driver, current_value);
  TickType_t last_increment = xTaskGetTickCount();

  while (true) {
    TickType_t now = xTaskGetTickCount();
    if (now - last_increment >= kAutoIncrementInterval) {
      last_increment = now;
      current_value = (current_value + 1) % 10;
      showInitialFrame(epd_driver, current_value);
    }
    vTaskDelay(pdMS_TO_TICKS(15));
  }
}
