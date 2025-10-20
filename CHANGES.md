# การแก้ไขปัญหา Ghosting ใน E-Paper Display

## ปัญหาเดิม
- จอ e-paper กระพริบทุกครั้งที่ LVGL ส่ง flush (ทีละ 32 บรรทัด)
- เกิด ghosting: แถวคู่หายไป แถวคี่แสดง (สลับกันไปมา)
- สาเหตุ: แต่ละ `drawBitmap()` ทำ partial refresh ทันที

## วิธีแก้ไข (Batched Refresh)

### 1. เพิ่ม Counter ใน `LvglDisplayContext`
```cpp
struct LvglDisplayContext {
  // ...existing fields...
  int32_t flush_count{0};        // นับจำนวนครั้งของ flush
  int32_t expected_flushes{0};   // จำนวน flush ที่คาดหวัง
};
```

### 2. คำนวณ Expected Flushes ใน `initLvgl()`
```cpp
// คำนวณจำนวน flush ที่คาดหวัง = จอสูง / buffer lines
g_lvgl_ctx.expected_flushes = (kDisplayHeight + kLvglBufferLines - 1) / kLvglBufferLines;
// สำหรับ 800 / 32 = 25 ครั้ง
```

### 3. แก้ไข `lvglFlushCallback()` ให้รอจนครบ
```cpp
// นับครั้ง
ctx->flush_count++;
const bool is_last_flush = (ctx->flush_count >= ctx->expected_flushes);

// ส่งข้อมูลไปจอ แต่ skip refresh ถ้ายังไม่ใช่ flush สุดท้าย
ctx->epd->drawBitmap(..., !is_last_flush);  // skip_refresh = !is_last_flush

// รีเซ็ต counter เมื่อเสร็จ
if (is_last_flush) {
    ctx->flush_count = 0;
}
```

### 4. เพิ่ม Parameter `skip_refresh` ใน `drawBitmap()`
**epd_driver.h:**
```cpp
esp_err_t drawBitmap(uint16_t x_start, uint16_t y_start, const uint8_t *bitmap,
                     uint16_t width_bits, uint16_t height_rows, 
                     bool skip_refresh = false);
```

**epd_driver.cpp:**
```cpp
esp_err_t Driver::drawBitmap(..., bool skip_refresh) {
    // ...upload data...
    
    // ถ้า skip_refresh = true จะไม่ refresh จอ
    if (!skip_refresh) {
        return partialUpdate();
    }
    return ESP_OK;
}
```

## ผลลัพธ์
- ✅ จอจะ refresh **เพียงครั้งเดียว** หลังจาก LVGL วาดครบทั้งหมด
- ✅ ไม่มี ghosting
- ✅ ไม่มีภาพกระพริบระหว่างวาด
- ✅ เร็วกว่า (25 ครั้ง → 1 ครั้ง)

## การทำงาน
```
Flush 1/25 → upload data, skip refresh
Flush 2/25 → upload data, skip refresh
Flush 3/25 → upload data, skip refresh
...
Flush 24/25 → upload data, skip refresh
Flush 25/25 → upload data, DO REFRESH ← จอ refresh ครั้งเดียว
```

## ข้อควรระวัง
- ต้องใช้ `LV_DISPLAY_RENDER_MODE_FULL` เพื่อให้ LVGL วาดครบทั้งจอในคราวเดียว
- `expected_flushes` ต้องคำนวณให้ถูกต้อง มิฉะนั้นจะไม่ refresh เลย
