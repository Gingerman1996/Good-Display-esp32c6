#include <algorithm>
#include <cstdint>
#include <vector>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "assets.h"
#include "epd_driver.h"
#include "lvgl.h"

namespace {

constexpr const char *TAG = "app";
constexpr size_t kLvglBufferLines = 32;
constexpr lv_color_format_t kLvglColorFormat = LV_COLOR_FORMAT_RGB565;
constexpr size_t kLvglBufferSize =
    LV_DRAW_BUF_SIZE(epd::kWidth, kLvglBufferLines, kLvglColorFormat);
constexpr TickType_t kMessageInterval = pdMS_TO_TICKS(5000);

constexpr const char *kMessages[] = {"Airgradient", "Welcome"};
constexpr size_t kMessageCount = sizeof(kMessages) / sizeof(kMessages[0]);

struct LvglDisplayContext {
  epd::Driver *epd{nullptr};
  lv_obj_t *label{nullptr};
  std::vector<uint8_t> scratch{};
};

esp_timer_handle_t g_lvgl_tick_timer = nullptr;
lv_display_t *g_lvgl_display = nullptr;
LvglDisplayContext g_lvgl_ctx{};

alignas(LV_DRAW_BUF_ALIGN) uint8_t g_lvgl_buf1[kLvglBufferSize];
alignas(LV_DRAW_BUF_ALIGN) uint8_t g_lvgl_buf2[kLvglBufferSize];

void lvglTickCallback(void *) { lv_tick_inc(1); }

void lvglFlushCallback(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
  (void)px_map;

  auto *ctx = static_cast<LvglDisplayContext *>(lv_display_get_user_data(disp));
  if (ctx == nullptr || ctx->epd == nullptr || area == nullptr) {
    lv_display_flush_ready(disp);
    return;
  }

  const int32_t x_start = std::clamp<int32_t>(area->x1, 0, epd::kWidth - 1);
  const int32_t y_start = std::clamp<int32_t>(area->y1, 0, epd::kHeight - 1);
  const int32_t x_end = std::clamp<int32_t>(area->x2, 0, epd::kWidth - 1);
  const int32_t y_end = std::clamp<int32_t>(area->y2, 0, epd::kHeight - 1);

  const int32_t width = (x_end - x_start) + 1;
  const int32_t height = (y_end - y_start) + 1;
  if (width <= 0 || height <= 0) {
    lv_display_flush_ready(disp);
    return;
  }

  ESP_LOGI(TAG, "LVGL flush (%d,%d) -> (%d,%d) size %dx%d ptr %p", x_start, y_start, x_end,
           y_end, width, height, static_cast<void *>(px_map));

  const int32_t aligned_x_start = x_start - (x_start % 8);
  const int32_t leading_padding = x_start - aligned_x_start;
  int32_t aligned_width = width + leading_padding;
  const int32_t trailing_padding = (8 - (aligned_width % 8)) % 8;
  aligned_width += trailing_padding;

  const size_t bit_count =
      static_cast<size_t>(aligned_width) * static_cast<size_t>(height);
  ctx->scratch.assign((bit_count + 7) / 8, 0xFF);

  size_t black_pixels = 0;

  const lv_color_format_t color_format = lv_display_get_color_format(disp);
  const uint32_t pixel_size = lv_color_format_get_size(color_format);
  const uint32_t row_stride =
      lv_draw_buf_width_to_stride(width, color_format ? color_format : kLvglColorFormat);

  for (int32_t row = 0; row < height; ++row) {
    const uint8_t *row_ptr = px_map
                                 ? px_map + static_cast<size_t>(row) * static_cast<size_t>(row_stride)
                                 : nullptr;
    if (row_ptr == nullptr) {
      lv_draw_buf_t *draw_buf = lv_display_get_buf_active(disp);
      if (draw_buf == nullptr) {
        lv_display_flush_ready(disp);
        return;
      }
      row_ptr = draw_buf->data + static_cast<size_t>(row) * draw_buf->header.stride;
    }

    for (int32_t col = 0; col < width; ++col) {
      const uint8_t *pixel_ptr =
          row_ptr + static_cast<size_t>(col) * static_cast<size_t>(pixel_size);

      uint8_t red = 0;
      uint8_t green = 0;
      uint8_t blue = 0;

      if (color_format == LV_COLOR_FORMAT_RGB565 ||
          color_format == LV_COLOR_FORMAT_RGB565A8 ||
          color_format == LV_COLOR_FORMAT_RGB565_SWAPPED) {
        const uint16_t value =
            static_cast<uint16_t>(pixel_ptr[0]) |
            (static_cast<uint16_t>(pixel_ptr[1]) << 8);
        red = static_cast<uint8_t>(((value >> 11) & 0x1F) * 255 / 31);
        green = static_cast<uint8_t>(((value >> 5) & 0x3F) * 255 / 63);
        blue = static_cast<uint8_t>((value & 0x1F) * 255 / 31);
      } else {
        // Assume little-endian BGR[A].
        blue = pixel_ptr[0];
        if (pixel_size > 1) {
          green = pixel_ptr[1];
        }
        if (pixel_size > 2) {
          red = pixel_ptr[2];
        }
      }

      const uint16_t luminance =
          static_cast<uint16_t>(red) * 30 + static_cast<uint16_t>(green) * 59 +
          static_cast<uint16_t>(blue) * 11;
      const bool is_black = luminance < (128U * 100U);

      const size_t dst_index =
          static_cast<size_t>(row) * static_cast<size_t>(aligned_width) +
          static_cast<size_t>(leading_padding + (width - 1 - col));
      const size_t byte_index = dst_index / 8;
      const uint8_t bit_mask = static_cast<uint8_t>(1U << (7 - (dst_index % 8)));

      if (is_black) {
        ctx->scratch[byte_index] &= static_cast<uint8_t>(~bit_mask);
        ++black_pixels;
      } else {
        ctx->scratch[byte_index] |= bit_mask;
      }
    }
  }

  const esp_err_t result = ctx->epd->drawBitmap(
      static_cast<uint16_t>(aligned_x_start), static_cast<uint16_t>(y_start),
      ctx->scratch.data(), static_cast<uint16_t>(aligned_width),
      static_cast<uint16_t>(height));
  if (result != ESP_OK) {
    ESP_LOGE(TAG, "drawBitmap failed: %s", esp_err_to_name(result));
  } else {
    ESP_LOGI(TAG, "flush done, black pixels: %u", static_cast<unsigned>(black_pixels));
  }

  lv_display_flush_ready(disp);
}

void initLvgl(epd::Driver &epd_driver) {
  lv_init();

  g_lvgl_ctx.epd = &epd_driver;
  g_lvgl_display = lv_display_create(epd::kWidth, epd::kHeight);
  lv_display_set_color_format(g_lvgl_display, kLvglColorFormat);
  lv_display_set_buffers(g_lvgl_display, g_lvgl_buf1, g_lvgl_buf2, kLvglBufferSize,
                         LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_user_data(g_lvgl_display, &g_lvgl_ctx);
  lv_display_set_flush_cb(g_lvgl_display, lvglFlushCallback);
  lv_display_set_default(g_lvgl_display);

  const esp_timer_create_args_t tick_timer_args = {
      .callback = &lvglTickCallback,
      .arg = nullptr,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "lvgl_tick",
      .skip_unhandled_events = false,
  };
  ESP_ERROR_CHECK(esp_timer_create(&tick_timer_args, &g_lvgl_tick_timer));
  ESP_ERROR_CHECK(esp_timer_start_periodic(g_lvgl_tick_timer, 1000));

  lv_obj_t *screen = lv_scr_act();
  lv_obj_set_style_bg_color(screen, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

  g_lvgl_ctx.label = lv_label_create(screen);
  lv_obj_set_style_text_color(g_lvgl_ctx.label, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_text_font(g_lvgl_ctx.label, &lv_font_montserrat_48, LV_PART_MAIN);
  lv_label_set_text(g_lvgl_ctx.label, kMessages[0]);
  lv_obj_center(g_lvgl_ctx.label);
}

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

  initLvgl(epd_driver);
  size_t message_index = 0;
  TickType_t last_switch = xTaskGetTickCount();

  while (true) {
    TickType_t now = xTaskGetTickCount();
    if (now - last_switch >= kMessageInterval) {
      last_switch = now;
      message_index = (message_index + 1) % kMessageCount;
      if (g_lvgl_ctx.label != nullptr) {
        lv_label_set_text(g_lvgl_ctx.label, kMessages[message_index]);
        lv_obj_center(g_lvgl_ctx.label);
      }
    }
    lv_timer_handler();
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}
