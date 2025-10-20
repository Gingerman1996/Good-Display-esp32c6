# คู่มือโปรเจ็กต์ Good-Display ESP32-C6 (ภาษาไทย)

โปรเจ็กต์นี้สาธิตการใช้งาน **จอ e-paper GDEQ0426T82-T01C (SSD1677)** คู่กับ **ESP32-C6** โดยใช้ ESP-IDF แบบเต็มรูปแบบ พร้อมผสาน **LVGL v9** ให้เรนเดอร์ UI และส่งผลลัพธ์กลับไปยังจอขาวดำ 1 บิตผ่านไดรเวอร์ที่เขียนเอง (`epd::Driver`). ระบบยังแยกโค้ดฮาร์ดแวร์ออกเป็น component (`components/gde_display`) เพื่อให้ reuse หรือนำไปต่อยอดได้ง่าย

---

## ภาพรวมการทำงาน

```
┌────────────────────────────┐
│ Application Layer          │ → main/main.cpp
│  - ตั้งค่า LVGL            │
│  - อัปเดตตัวเลขทุกวินาที │
│  - เรียก lv_timer_handler │
├────────────────────────────┤
│ LVGL UI Layer              │
│  - จัดการวิดเจ็ต/การเรนเดอร์│
│  - ส่ง callback flush     │
├────────────────────────────┤
│ Hardware Driver (component)│
│  - epd::Driver (SSD1677)   │
│  - drawBitmap / partial    │
│  - assets (bitmap พื้นฐาน)│
├────────────────────────────┤
│ ESP-IDF HAL                │
└────────────────────────────┘
```

### ลำดับเหตุการณ์หลัก
1. `app_main` ตั้งค่าขา SPI/Busy/MOSI ของจอ → `epd::Driver::init`
2. รีเซ็ตจอและล้างหน้าจอ → `hardwareInit()` + `clear()`
3. โหลดภาพพื้นหลัง (`WhileBG`) ให้เป็น base map
4. เรียก `initLvgl()` เพื่อสร้าง display object ของ LVGL และตั้ง `flush_cb`
5. ในลูปหลัก:
   - เพิ่มค่าตัวเลขทุก 1 วินาที
   - อัปเดตข้อความบน label ของ LVGL
   - เรียก `lv_timer_handler()` และหน่วงเวลาเล็กน้อย

เมื่อ LVGL ต้องวาดหน้าจอใหม่จะเรียก `lvglFlushCallback()` ซึ่งจะแปลงบัฟเฟอร์สี 16 บิตเป็นบิตแมป 1 บิต แล้วใช้ `epd::Driver::drawBitmap()` เขียนลงจอแบบ partial refresh

---

## รายละเอียดโค้ดหลัก

### `main/main.cpp`
- กำหนดค่า render buffer ของ LVGL เป็น line buffer 32 บรรทัด เพื่อลดการใช้ RAM (`kLvglBufferLines`)
- สร้าง `LvglDisplayContext` เก็บข้อมูลที่ต้องใช้ใน flush callback (ตัวชี้ไปยัง `epd::Driver`, label, scratch buffer)
- `lvglFlushCallback()`:
  - ตรวจสอบพิกัดจาก LVGL แล้วจัดให้เป็น byte boundary (ต้องหาร 8 ลงตัว)
  - คำนวณความสว่างจากสี RGB565 → เลือกว่าจะพ่นบิตเป็นขาว/ดำ
  - จัดการ mirror แนวนอนให้ตรงกับทิศของฮาร์ดแวร์
  - เรียก `epd::Driver::drawBitmap()` และปิดด้วย `lv_display_flush_ready()`
- `updateCounterLabel()` สร้างสตริงตัวเลข 5 หลัก (ค่าซ้ำกัน) แล้วตั้งข้อความบน label

### `components/gde_display/epd_driver.*`
- ห่อหุ้ม HAL ของ ESP-IDF:
  - `init()` สร้าง bus SPI สำหรับพาแนล
  - `hardwareInit()` ส่งคำสั่งตั้งต้น SSD1677
  - `loadBaseMap()` โหลด frame buffer เต็มจอ (ใช้ตอนเริ่มงาน)
  - `drawBitmap()` (เพิ่มใหม่) เรียก `writePartialWindow()` แล้วสั่ง `partialUpdate()` เพื่ออัปเดตเฉพาะพื้นที่ที่ LVGL ขอ
- `writePartialWindow()` จะตั้งค่าหน้าต่าง RAM บนจอ, เขียนข้อมูล, และสั่ง update

### `main/idf_component.yml` และ `dependencies.lock`
- ระบุการดึง component `lvgl/lvgl` เวอร์ชัน `^9.0.0`
- `dependencies.lock` ถูกสร้างโดย component manager เพื่อ lock เวอร์ชันของ dependency

### `managed_components/lvgl__lvgl/`
- เป็นสำเนาโค้ดของ LVGL ที่ดึงมาจาก component registry (ไม่ควรแก้ไขไฟล์ภายในโดยตรง)

---

## การตั้งค่าสิ่งแวดล้อม & สั่ง Build/Flash

```bash
# เปิดใช้งาน ESP-IDF 5.5.1
source $HOME/esp/v5.5.1/esp-idf/export.sh

# (ครั้งแรก) ติดตั้ง dependency ของ component manager
idf.py reconfigure

# สร้างและอัปโหลดเฟิร์มแวร์
idf.py build
idf.py -p <พอร์ต> flash
idf.py -p <พอร์ต> monitor
```

> หมายเหตุ: ขณะ `idf.py build` component manager จะต้องดาวน์โหลด LVGL จาก `https://components-file.espressif.com` ให้เชื่อมต่ออินเทอร์เน็ต หรือทำการ mirror ไฟล์มาก่อน ถ้าออฟไลน์สามารถคัดลอกโฟลเดอร์ `managed_components/lvgl__lvgl` จากเครื่องที่ดาวน์โหลดสำเร็จมาไว้ล่วงหน้าได้

---

## การทำงานของ UI ตัวอย่าง

### Air Quality Monitor Dashboard
แอปพลิเคชันแสดงข้อมูลคุณภาพอากาศแบบเรียลไทม์:

#### Status Bar (ด้านบน)
- แสดงค่าความชื้น (ซ้าย) และอุณหภูมิ (ขวา)
- อัพเดททุก 5 วินาที
- เส้นขอบด้านล่างแยก status bar จากตาราง

#### ตารางข้อมูลหลัก (4 แถว × 3 คอลัมน์)
```
┌─────────┬─────────┬─────────┐
│  CO2    │ PM2.5   │  VOC    │ ← แถวที่ 1: หัวตาราง
├─────────┼─────────┼─────────┤
│  741    │    0    │  105    │ ← แถวที่ 2: ค่าเซ็นเซอร์
├─────────┼─────────┼─────────┤
│  ppm    │ ug/m3   │  NOx    │ ← แถวที่ 3: หน่วย
├─────────┼─────────┼─────────┤
│         │         │  105    │ ← แถวที่ 4: ค่า NOx
└─────────┴─────────┴─────────┘
```

#### ค่าที่แสดง (อัพเดทอัตโนมัติทุก 5 วินาที)
- **CO2**: 400-800 ppm (คอลัมน์ 1)
- **PM2.5**: 0-10 μg/m³ (คอลัมน์ 2)  
- **VOC**: 100-200 (คอลัมน์ 3)
- **NOx**: 1-5 (คอลัมน์ 3, แถวล่าง)
- **อุณหภูมิ**: 20-30°C (status bar ขวา)
- **ความชื้น**: 40-70% (status bar ซ้าย)

#### คุณสมบัติเทคนิค
- **Partial Refresh Mode**: อัพเดทเฉพาะพื้นที่ที่เปลี่ยน ลดการกระพริบ
- **Time-based Batching**: รวม flush หลายครั้งเป็นครั้งเดียว (200ms delay)
- **Random Value Generation**: ใช้ `std::mt19937` สร้างค่าสุ่มในช่วงที่กำหนด
- **Grid Layout System**: ใช้ LVGL grid แบ่งพื้นที่อัตโนมัติ
- **8px Divider Lines**: เส้นแบ่งตารางหนา 8px สำหรับ e-paper
- **Force Invalidation**: บังคับ redraw เส้นขอบทุกครั้งที่อัพเดท

#### การปรับแต่ง
- แก้ไข `randomRange()` ใน `updateSensorValues()` เพื่อเปลี่ยนช่วงค่า
- ปรับ `kUpdateInterval` เพื่อเปลี่ยนความถี่ในการอัพเดท
- แก้ `kDividerThickness` เพื่อเปลี่ยนความหนาเส้นแบ่ง
- ใช้ `lv_font_montserrat_*` เปลี่ยนขนาดฟอนต์

---

## โครงสร้างไฟล์สำคัญ

```
├── CMakeLists.txt                 # กำหนดโปรเจ็กต์ + macro ของ LVGL
├── dependencies.lock              # lock dependency
├── managed_components/
│   └── lvgl__lvgl/                # โค้ด LVGL ที่ดึงมาจาก registry
├── components/
│   └── gde_display/
│       ├── epd_driver.cpp/.h      # SSD1677 driver + drawBitmap
│       ├── assets.cpp/.h          # bitmap พื้นฐาน (ตัวเลข/พื้นหลัง)
│       └── CMakeLists.txt
└── main/
    ├── idf_component.yml          # ระบุ dependency LVGL
    ├── CMakeLists.txt             # ลงทะเบียน component `main`
    └── main.cpp                   # logic แอป + UI
```

---

## เคล็ดลับและคำแนะนำ

1. **การทดสอบบนฮาร์ดแวร์จริง**  
   - e-paper partial update อาจใช้เวลาประมาณ 400 ms (ตามสเปก) อย่าลืมรอให้ `lvglFlushCallback()` กลับมาพร้อม `lv_display_flush_ready()` เสมอ
   - หากจอดับ/ไม่รีเฟรช ให้ตรวจสอบขา BUSY และการต่อ GND ให้เรียบร้อย

2. **เพิ่มวิดเจ็ตของ LVGL**  
   - สร้างใน `initLvgl()` และเก็บ pointer ไว้ใน `g_lvgl_ctx` หรือสร้าง struct ของตัวเอง
   - เรียก `lv_obj_align`, `lv_obj_set_size`, `lv_obj_add_event_cb` เพื่อควบคุมพฤติกรรม
   - จำไว้ว่า flush callback จะถูกเรียกอัตโนมัติจาก `lv_timer_handler()` ที่เราป้อนในลูปหลัก

3. **เพิ่มแหล่งข้อมูลอื่น**  
   - ถ้าต้องการแสดงข้อมูลเซนเซอร์ ให้ดึงค่ามาอัปเดตวิดเจ็ตในลูปหลัก หรือใช้ `lv_timer` ของ LVGL เพื่อแยก task ภายในเฟรมเวิร์ก

4. **ขยายฟังก์ชันของ driver**  
   - ปัจจุบัน `drawBitmap()` รองรับบิตแมป 1 บิตที่ความกว้างหาร 8 ลงตัว หากต้องการรูปแบบอื่น (เช่น EPD 4 สี) ต้องปรับโค้ดให้รองรับข้อมูลหลายบิตและ waveform ใหม่ของ SSD1677

---

## ปัญหาที่พบบ่อย

| อาการ | แนวทางตรวจสอบ |
|-------|----------------|
| จอไม่รีเฟรช | ตรวจดู log `LVGL flush ...` หรือ log error จาก `drawBitmap`; ตรวจสอบว่ามีการเชื่อม BUSY/RST/CS/MOSI/CLK ถูกต้อง |
| ฟอนต์กลับหัว/กลับด้าน | ปัจจุบันแก้ด้วยการ mirror ภายใน `lvglFlushCallback()` แล้ว; หากเปลี่ยนการหมุนจอเพิ่มเติมให้ปรับที่ LVGL (`lv_display_set_rotation`) |
| Build ไม่ผ่านเพราะดาวน์โหลด LVGL ไม่ได้ | เชื่อมต่อเน็ต หรือคัดลอกโฟลเดอร์ `managed_components/lvgl__lvgl` และ `dependencies.lock` จากเครื่องที่ติดตั้งสำเร็จ |
| หน่วยความจำไม่พอ | ลด `kLvglBufferLines` หรือสร้างวิดเจ็ตให้น้อยลง; สามารถใช้ `LV_MEM_SIZE` ใน `lv_conf.h` ถ้าคอมไพล์แบบกำหนดเอง |

---

## การปรับแต่งต่อยอด

- ถ้าอยากใช้ Touch panel (FT6336) เพิ่ม สามารถนำ driver จาก commit ก่อนหน้ากลับมา หรือสร้าง component เพิ่มแล้วเรียกจาก LVGL (ใช้ input device driver `lv_indev_drv`)
- สามารถเพิ่มหน้า UI หลายหน้าแล้วเรียก `lv_scr_load()` สลับไปมา
- หากต้องการเก็บ log เพิ่มเติม ให้อาศัย `ESP_LOG*` ในโค้ดเพื่อ debug partial update

---

หวังว่าคู่มือนี้จะช่วยให้เข้าใจโครงสร้างและวิธีใช้งานโปรเจ็กต์ LVGL + SSD1677 บน ESP32-C6 ได้อย่างครบถ้วน หากมีคำถามเพิ่มเติมสามารถสอบถามได้เลย! :D

## ตารางแสดงผลข้อมูล

```
┌────────────────┬────────────────┬────────────────┐
│      CO2       │     PM2.5      │      VOC       │ ← แถว 0 (font 24)
├────────────────┼────────────────┼────────────────┤
│      741       │       0        │      105       │ ← แถว 1 (font 48)
├────────────────┼────────────────┼────────────────┤
│      ppm       │     ug/m3      │      NOx       │ ← แถว 2 (font 20)
├────────────────┼────────────────┼────────────────┤
│                │                │      105       │ ← แถว 3 (font 48)
└────────────────┴────────────────┴────────────────┘
```

**โครงสร้าง:**
- **แถว 0**: หัวตาราง (CO2, PM2.5, VOC)
- **แถว 1**: ค่าวัด (741 ppm, 0 ug/m3, 105 VOC)
- **แถว 2**: หน่วยวัด (ppm, ug/m3, NOx label)
- **แถว 3**: ค่า NOx เพิ่มเติม (คอลัมน์ที่ 3 เท่านั้น = 105)
