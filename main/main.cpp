#include <algorithm>
#include <cstdint>
#include <vector>
#include <random>

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
constexpr uint32_t kDisplayWidth = epd::kHeight;
constexpr uint32_t kDisplayHeight = epd::kWidth;
constexpr size_t kLvglBufferSize =
    LV_DRAW_BUF_SIZE(kDisplayWidth, kLvglBufferLines, kLvglColorFormat);
constexpr TickType_t kUpdateInterval = pdMS_TO_TICKS(5000);  // อัพเดททุก 5 วินาที

struct LvglDisplayContext {
  epd::Driver *epd{nullptr};
  lv_obj_t *status_bar{nullptr};  // เพิ่ม status_bar reference
  lv_obj_t *status_temp_label{nullptr};
  lv_obj_t *status_humidity_label{nullptr};
  lv_obj_t *table_container{nullptr};
  lv_obj_t *table_headings[3]{};
  lv_obj_t *table_values[3]{};
  lv_obj_t *table_units[3]{};
  lv_obj_t *nox_value_label{nullptr};  // เพิ่ม label สำหรับ NOx
  std::vector<uint8_t> scratch{};
  int32_t flush_count{0};
  int32_t expected_flushes{0};
  TickType_t last_flush_time{0};  // เวลาของ flush ล่าสุด
};

// Global variables - ต้องประกาศก่อนใช้งาน
esp_timer_handle_t g_lvgl_tick_timer = nullptr;
lv_display_t *g_lvgl_display = nullptr;
LvglDisplayContext g_lvgl_ctx{};

alignas(LV_DRAW_BUF_ALIGN) uint8_t g_lvgl_buf1[kLvglBufferSize];
alignas(LV_DRAW_BUF_ALIGN) uint8_t g_lvgl_buf2[kLvglBufferSize];

// Random number generator
std::random_device rd;
std::mt19937 gen(rd());

// ฟังก์ชันสุ่มค่า
int randomRange(int min, int max) {
  std::uniform_int_distribution<> dis(min, max);
  return dis(gen);
}

void updateSensorValues() {
  // สุ่มค่าตาม range ที่กำหนด
  int co2 = randomRange(400, 800);
  int pm25 = randomRange(0, 10);
  int voc = randomRange(100, 200);
  int nox = randomRange(1, 5);
  int temp = randomRange(20, 30);
  int humi = randomRange(40, 70);

  ESP_LOGI(TAG, "Updating values: CO2=%d, PM2.5=%d, VOC=%d, NOx=%d, Temp=%d, Humi=%d",
           co2, pm25, voc, nox, temp, humi);

  // อัพเดท status bar
  char temp_str[16];
  snprintf(temp_str, sizeof(temp_str), "%dC", temp);
  lv_label_set_text(g_lvgl_ctx.status_temp_label, temp_str);  // temp ไปที่ temp_label (ซ้าย)

  char humi_str[16];
  snprintf(humi_str, sizeof(humi_str), "%d%%", humi);
  lv_label_set_text(g_lvgl_ctx.status_humidity_label, humi_str);  // humi ไปที่ humidity_label (ขวา)

  // อัพเดทตาราง
  char co2_str[16];
  snprintf(co2_str, sizeof(co2_str), "%d", co2);
  lv_label_set_text(g_lvgl_ctx.table_values[0], co2_str);

  char pm25_str[16];
  snprintf(pm25_str, sizeof(pm25_str), "%d", pm25);
  lv_label_set_text(g_lvgl_ctx.table_values[1], pm25_str);

  char voc_str[16];
  snprintf(voc_str, sizeof(voc_str), "%d", voc);
  lv_label_set_text(g_lvgl_ctx.table_values[2], voc_str);

  // อัพเดท NOx value
  char nox_str[16];
  snprintf(nox_str, sizeof(nox_str), "%d", nox);
  lv_label_set_text(g_lvgl_ctx.nox_value_label, nox_str);
  
  // Force invalidate ทั้ง container และ status bar เพื่อให้วาดเส้นขอบใหม่
  if (g_lvgl_ctx.status_bar != nullptr) {
    lv_obj_invalidate(g_lvgl_ctx.status_bar);
  }
  if (g_lvgl_ctx.table_container != nullptr) {
    lv_obj_invalidate(g_lvgl_ctx.table_container);
  }
}

void lvglTickCallback(void *) { lv_tick_inc(1); }

void lvglFlushCallback(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
  (void)px_map;

  auto *ctx = static_cast<LvglDisplayContext *>(lv_display_get_user_data(disp));
  if (ctx == nullptr || ctx->epd == nullptr || area == nullptr) {
    lv_display_flush_ready(disp);
    return;
  }

  const int32_t x_start =
      std::clamp<int32_t>(area->x1, 0, static_cast<int32_t>(kDisplayWidth) - 1);
  const int32_t y_start =
      std::clamp<int32_t>(area->y1, 0, static_cast<int32_t>(kDisplayHeight) - 1);
  const int32_t x_end =
      std::clamp<int32_t>(area->x2, 0, static_cast<int32_t>(kDisplayWidth) - 1);
  const int32_t y_end =
      std::clamp<int32_t>(area->y2, 0, static_cast<int32_t>(kDisplayHeight) - 1);

  const int32_t width = (x_end - x_start) + 1;
  const int32_t height = (y_end - y_start) + 1;
  if (width <= 0 || height <= 0) {
    lv_display_flush_ready(disp);
    return;
  }

  // บันทึกเวลาของ flush
  ctx->last_flush_time = xTaskGetTickCount();

  ESP_LOGI(TAG, "LVGL flush (%d,%d) -> (%d,%d) size %dx%d", 
           x_start, y_start, x_end, y_end, width, height);

  const int32_t aligned_x_start = x_start - (x_start % 8);
  const int32_t leading_padding = x_start - aligned_x_start;
  int32_t aligned_width = width + leading_padding;
  const int32_t trailing_padding = (8 - (aligned_width % 8)) % 8;
  aligned_width += trailing_padding;

  const size_t bit_count = static_cast<size_t>(aligned_width) * static_cast<size_t>(height);
  ctx->scratch.assign((bit_count + 7) / 8, 0xFF);

  size_t black_pixels = 0;

  const lv_color_format_t color_format = lv_display_get_color_format(disp);
  const uint32_t pixel_size = lv_color_format_get_size(color_format);
  const uint32_t row_stride =
      lv_draw_buf_width_to_stride(width, color_format ? color_format : kLvglColorFormat);

  for (int32_t row = 0; row < height; ++row) {
    const uint8_t *row_ptr =
        px_map ? px_map + static_cast<size_t>(row) * static_cast<size_t>(row_stride) : nullptr;
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

      if (color_format == LV_COLOR_FORMAT_RGB565 || color_format == LV_COLOR_FORMAT_RGB565A8 ||
          color_format == LV_COLOR_FORMAT_RGB565_SWAPPED) {
        const uint16_t value =
            static_cast<uint16_t>(pixel_ptr[0]) | (static_cast<uint16_t>(pixel_ptr[1]) << 8);
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

      const uint16_t luminance = static_cast<uint16_t>(red) * 30 +
                                 static_cast<uint16_t>(green) * 59 +
                                 static_cast<uint16_t>(blue) * 11;
      const bool is_black = luminance < (128U * 100U);

      const size_t dst_index = static_cast<size_t>(row) * static_cast<size_t>(aligned_width) +
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

  // ส่งข้อมูลไปจอ โดย skip refresh ทุกครั้ง
  // จะ refresh ครั้งเดียวหลังจากไม่มี flush มาสัก 200ms
  const esp_err_t result = ctx->epd->drawBitmap(
      static_cast<uint16_t>(aligned_x_start), static_cast<uint16_t>(y_start), ctx->scratch.data(),
      static_cast<uint16_t>(aligned_width), static_cast<uint16_t>(height), true);  // skip_refresh = true
  
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
  g_lvgl_display = lv_display_create(kDisplayWidth, kDisplayHeight);
  lv_display_set_color_format(g_lvgl_display, kLvglColorFormat);
  lv_display_set_buffers(g_lvgl_display, g_lvgl_buf1, g_lvgl_buf2, kLvglBufferSize,
                         LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_user_data(g_lvgl_display, &g_lvgl_ctx);
  lv_display_set_flush_cb(g_lvgl_display, lvglFlushCallback);
  lv_display_set_default(g_lvgl_display);

  // สำหรับ PARTIAL mode จะไม่รู้จำนวน flush ล่วงหน้า
  // เราจะใช้วิธีรอ delay หลังจาก render เสร็จแทน
  g_lvgl_ctx.flush_count = 0;
  g_lvgl_ctx.expected_flushes = 0;

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

  // g_lvgl_ctx.label = lv_label_create(screen);
  // lv_obj_set_style_text_color(g_lvgl_ctx.label, lv_color_black(), LV_PART_MAIN);
  // lv_obj_set_style_text_font(g_lvgl_ctx.label, &lv_font_montserrat_48, LV_PART_MAIN);
  // lv_label_set_text(g_lvgl_ctx.label, kMessages[0]);
  // lv_obj_center(g_lvgl_ctx.label);

  constexpr lv_coord_t kScreenMargin = 16;
  constexpr lv_coord_t kStatusBarHeight = 64;
  constexpr lv_coord_t kTableGap = 16;
  constexpr lv_coord_t kHeaderRowHeight = 60;
  constexpr lv_coord_t kValueRowHeight = 80;   // ลดลงเพื่อให้มีพื้นที่สำหรับแถวที่ 4
  constexpr lv_coord_t kUnitRowHeight = 50;    // แถวหน่วย
  constexpr lv_coord_t kDividerThickness = 8;  // เพิ่มจาก 4 เป็น 8 px

  const lv_coord_t content_width =
      static_cast<lv_coord_t>(kDisplayWidth) - 2 * kScreenMargin;

  g_lvgl_ctx.status_bar = lv_obj_create(screen);
  lv_obj_set_size(g_lvgl_ctx.status_bar, content_width, kStatusBarHeight);
  lv_obj_set_pos(g_lvgl_ctx.status_bar, kScreenMargin, kScreenMargin);
  lv_obj_set_style_bg_opa(g_lvgl_ctx.status_bar, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_color(g_lvgl_ctx.status_bar, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_border_width(g_lvgl_ctx.status_bar, 2, LV_PART_MAIN);
  lv_obj_set_style_border_side(g_lvgl_ctx.status_bar, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
  lv_obj_set_style_pad_all(g_lvgl_ctx.status_bar, 0, LV_PART_MAIN);

  g_lvgl_ctx.status_temp_label = lv_label_create(g_lvgl_ctx.status_bar);
  lv_obj_set_style_text_color(g_lvgl_ctx.status_temp_label, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_text_font(g_lvgl_ctx.status_temp_label, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_set_style_text_align(g_lvgl_ctx.status_temp_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
  lv_label_set_text(g_lvgl_ctx.status_temp_label, "26.5C");
  lv_obj_align(g_lvgl_ctx.status_temp_label, LV_ALIGN_LEFT_MID, 8, 0);

  g_lvgl_ctx.status_humidity_label = lv_label_create(g_lvgl_ctx.status_bar);
  lv_obj_set_style_text_color(g_lvgl_ctx.status_humidity_label, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_text_font(g_lvgl_ctx.status_humidity_label, &lv_font_montserrat_24,
                             LV_PART_MAIN);
  lv_obj_set_style_text_align(g_lvgl_ctx.status_humidity_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
  lv_label_set_text(g_lvgl_ctx.status_humidity_label, "72%");
  lv_obj_align(g_lvgl_ctx.status_humidity_label, LV_ALIGN_RIGHT_MID, -8, 0);

  const lv_coord_t table_y = kScreenMargin + kStatusBarHeight + kTableGap;
  const lv_coord_t table_height =
      static_cast<lv_coord_t>(kDisplayHeight) - table_y - kScreenMargin;

  g_lvgl_ctx.table_container = lv_obj_create(screen);
  lv_obj_set_size(g_lvgl_ctx.table_container, content_width, table_height);
  lv_obj_set_pos(g_lvgl_ctx.table_container, kScreenMargin, table_y);
  lv_obj_set_style_bg_opa(g_lvgl_ctx.table_container, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_color(g_lvgl_ctx.table_container, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_border_width(g_lvgl_ctx.table_container, 2, LV_PART_MAIN);
  lv_obj_set_style_radius(g_lvgl_ctx.table_container, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(g_lvgl_ctx.table_container, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_row(g_lvgl_ctx.table_container, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_column(g_lvgl_ctx.table_container, 0, LV_PART_MAIN);

  static lv_coord_t column_dsc[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
                                    LV_GRID_TEMPLATE_LAST};
  static lv_coord_t row_dsc[] = {
      kHeaderRowHeight,      // แถวที่ 0: 60px (หัวตาราง: CO2, PM2.5, VOC)
      kValueRowHeight,       // แถวที่ 1: 80px (ค่า: 741, 0, 105)
      kUnitRowHeight,        // แถวที่ 2: 50px (หน่วย: ppm, ug/m3, NOx)
      LV_GRID_FR(1),         // แถวที่ 3: ที่เหลือ (ว่าง, ว่าง, 105)
      LV_GRID_TEMPLATE_LAST
  };
  lv_obj_set_layout(g_lvgl_ctx.table_container, LV_LAYOUT_GRID);
  lv_obj_set_grid_dsc_array(g_lvgl_ctx.table_container, column_dsc, row_dsc);

  const char *const headings[] = {"CO2", "PM2.5", "VOC"};
  const char *const sample_values[] = {"741", "0", "105"};
  const char *const units[] = {"ppm", "ug/m3", "NOx"};
  const char *const nox_value = "105";

  for (size_t i = 0; i < 3; ++i) {
    // แถวที่ 0: หัวตาราง (CO2, PM2.5, VOC)
    g_lvgl_ctx.table_headings[i] = lv_label_create(g_lvgl_ctx.table_container);
    lv_obj_set_style_text_color(g_lvgl_ctx.table_headings[i], lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_text_font(g_lvgl_ctx.table_headings[i], &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_align(g_lvgl_ctx.table_headings[i], LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(g_lvgl_ctx.table_headings[i], headings[i]);
    lv_obj_set_grid_cell(g_lvgl_ctx.table_headings[i], LV_GRID_ALIGN_CENTER, i, 1,
                         LV_GRID_ALIGN_CENTER, 0, 1);

    // แถวที่ 1: ค่าหลัก (741, 0, 105)
    g_lvgl_ctx.table_values[i] = lv_label_create(g_lvgl_ctx.table_container);
    lv_obj_set_style_text_color(g_lvgl_ctx.table_values[i], lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_text_font(g_lvgl_ctx.table_values[i], &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_align(g_lvgl_ctx.table_values[i], LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(g_lvgl_ctx.table_values[i], sample_values[i]);
    lv_obj_set_grid_cell(g_lvgl_ctx.table_values[i], LV_GRID_ALIGN_CENTER, i, 1,
                         LV_GRID_ALIGN_CENTER, 1, 1);

    // แถวที่ 2: หน่วย (ppm, ug/m3, NOx)
    g_lvgl_ctx.table_units[i] = lv_label_create(g_lvgl_ctx.table_container);
    lv_obj_set_style_text_color(g_lvgl_ctx.table_units[i], lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_text_font(g_lvgl_ctx.table_units[i], &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_align(g_lvgl_ctx.table_units[i], LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(g_lvgl_ctx.table_units[i], units[i]);
    lv_obj_set_grid_cell(g_lvgl_ctx.table_units[i], LV_GRID_ALIGN_CENTER, i, 1, 
                         LV_GRID_ALIGN_CENTER, 2, 1);
  }

  // แถวที่ 3: เฉพาะคอลัมน์ที่ 3 (NOx value = 105)
  g_lvgl_ctx.nox_value_label = lv_label_create(g_lvgl_ctx.table_container);
  lv_obj_set_style_text_color(g_lvgl_ctx.nox_value_label, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_text_font(g_lvgl_ctx.nox_value_label, &lv_font_montserrat_48, LV_PART_MAIN);
  lv_obj_set_style_text_align(g_lvgl_ctx.nox_value_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_label_set_text(g_lvgl_ctx.nox_value_label, nox_value);
  lv_obj_set_grid_cell(g_lvgl_ctx.nox_value_label, LV_GRID_ALIGN_CENTER, 2, 1,  // คอลัมน์ที่ 2 (index 2)
                       LV_GRID_ALIGN_CENTER, 3, 1);                                // แถวที่ 3 (index 3)


  // วาดเส้นแบ่งหลังจากสร้าง labels เสร็จแล้ว
  const lv_coord_t column_width = lv_obj_get_width(g_lvgl_ctx.table_container) / 3;
  const lv_coord_t table_width = lv_obj_get_width(g_lvgl_ctx.table_container);
  
  ESP_LOGI(TAG, "Table container size: %dx%d", (int)table_width, (int)table_height);
  ESP_LOGI(TAG, "Column width: %d", (int)column_width);
  
  // 3 เส้นแนวนอน สำหรับแบ่ง 4 แถว
  const lv_coord_t horizontal_positions[] = {
      kHeaderRowHeight,                                  // y = 60
      kHeaderRowHeight + kValueRowHeight,                // y = 60 + 80 = 140
      kHeaderRowHeight + kValueRowHeight + kUnitRowHeight // y = 60 + 80 + 50 = 190
  };

  // ใช้ lv_line สำหรับวาดเส้นแนวตั้ง (ชัดเจนกว่า obj)
  static lv_point_precise_t vertical_line1_points[] = {
      {0, 0}, 
      {0, 0}  // จะ set ใน runtime
  };
  static lv_point_precise_t vertical_line2_points[] = {
      {0, 0}, 
      {0, 0}
  };
  
  // เส้นแนวตั้งที่ 1 (ระหว่าง CO2 กับ PM2.5)
  vertical_line1_points[0].x = column_width;
  vertical_line1_points[0].y = 0;
  vertical_line1_points[1].x = column_width;
  vertical_line1_points[1].y = table_height;
  
  lv_obj_t *vline1 = lv_line_create(g_lvgl_ctx.table_container);
  lv_line_set_points(vline1, vertical_line1_points, 2);
  lv_obj_set_style_line_width(vline1, kDividerThickness, LV_PART_MAIN);
  lv_obj_set_style_line_color(vline1, lv_color_black(), LV_PART_MAIN);
  lv_obj_add_flag(vline1, LV_OBJ_FLAG_IGNORE_LAYOUT);
  lv_obj_move_foreground(vline1);
  
  ESP_LOGI(TAG, "Vertical line 1: x=%d, height=%d", (int)column_width, (int)table_height);
  
  // เส้นแนวตั้งที่ 2 (ระหว่าง PM2.5 กับ VOC)
  vertical_line2_points[0].x = column_width * 2;
  vertical_line2_points[0].y = 0;
  vertical_line2_points[1].x = column_width * 2;
  vertical_line2_points[1].y = table_height;
  
  lv_obj_t *vline2 = lv_line_create(g_lvgl_ctx.table_container);
  lv_line_set_points(vline2, vertical_line2_points, 2);
  lv_obj_set_style_line_width(vline2, kDividerThickness, LV_PART_MAIN);
  lv_obj_set_style_line_color(vline2, lv_color_black(), LV_PART_MAIN);
  lv_obj_add_flag(vline2, LV_OBJ_FLAG_IGNORE_LAYOUT);
  lv_obj_move_foreground(vline2);
  
  ESP_LOGI(TAG, "Vertical line 2: x=%d, height=%d", (int)(column_width * 2), (int)table_height);
  
  // วาดเส้นแนวนอนด้วย obj ธรรมดา (เหมือนเดิม)
  auto add_horizontal_divider = [](lv_obj_t *parent, lv_coord_t y, lv_coord_t width, lv_coord_t thickness) {
    lv_obj_t *line = lv_obj_create(parent);
    lv_obj_set_size(line, width, thickness);
    lv_obj_set_pos(line, 0, y - thickness / 2);
    lv_obj_set_style_bg_color(line, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(line, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(line, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(line, 0, LV_PART_MAIN);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(line, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_move_foreground(line);
    lv_obj_invalidate(line);
  };
  
  // วาดเส้นแนวนอน 3 เส้น (แบ่ง 4 แถว)
  for (lv_coord_t y : horizontal_positions) {
    ESP_LOGI(TAG, "Drawing horizontal line at y=%d, width=%d", (int)y, (int)table_width);
    add_horizontal_divider(g_lvgl_ctx.table_container, y, table_width, kDividerThickness);
  }
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

  // ESP_ERROR_CHECK(epd_driver.hardwareInit(true));
  ESP_ERROR_CHECK(epd_driver.loadBaseMap(WhileBG, true));

  // initLvgl(epd_driver);
  // size_t message_index = 0;
  // TickType_t last_switch = xTaskGetTickCount();

  // while (true) {
  //   TickType_t now = xTaskGetTickCount();
  //   if (now - last_switch >= kMessageInterval) {
  //     last_switch = now;
  //     message_index = (message_index + 1) % kMessageCount;
  //     if (g_lvgl_ctx.label != nullptr) {
  //       lv_label_set_text(g_lvgl_ctx.label, kMessages[message_index]);
  //       lv_obj_center(g_lvgl_ctx.label);
  //     }
  //   }
  //   lv_timer_handler();
  //   vTaskDelay(pdMS_TO_TICKS(50));
  // }

  initLvgl(epd_driver);

  TickType_t last_refresh_check = xTaskGetTickCount();
  TickType_t last_update = xTaskGetTickCount();  // เวลาอัพเดทค่าล่าสุด
  constexpr TickType_t kRefreshCheckInterval = pdMS_TO_TICKS(100);  // ตรวจสอบทุก 100ms
  constexpr TickType_t kRefreshDelay = pdMS_TO_TICKS(200);          // รอ 200ms หลัง flush สุดท้าย

  // อัพเดทค่าเริ่มต้นครั้งแรก
  updateSensorValues();

  while (true) {
    lv_timer_handler();
    
    TickType_t now = xTaskGetTickCount();
    
    // อัพเดทค่าเซ็นเซอร์ทุก 5 วินาที
    if (now - last_update >= kUpdateInterval) {
      last_update = now;
      updateSensorValues();
      ESP_LOGI(TAG, "Sensor values updated");
    }
    
    // ตรวจสอบว่าควร refresh จอหรือไม่
    if (now - last_refresh_check >= kRefreshCheckInterval) {
      last_refresh_check = now;
      
      // ถ้ามี flush มาแล้ว และผ่านไป 200ms โดยไม่มี flush ใหม่
      if (g_lvgl_ctx.last_flush_time > 0 && 
          now - g_lvgl_ctx.last_flush_time >= kRefreshDelay) {
        ESP_LOGI(TAG, "Triggering delayed refresh...");
        
        // เรียก triggerRefresh เพื่อ refresh จอด้วยข้อมูลที่อัพโหลดไปแล้ว
        ESP_ERROR_CHECK(epd_driver.triggerRefresh());
        
        g_lvgl_ctx.last_flush_time = 0;  // รีเซ็ต
        ESP_LOGI(TAG, "Refresh completed");
      }
    }
    
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}
