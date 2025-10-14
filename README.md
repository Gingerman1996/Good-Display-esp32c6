# Good-Display ESP32-C6 Port

โปรเจ็กต์นี้เป็นการพอร์ตตัวอย่าง Arduino สำหรับจอ e-paper GDEQ0426T82-T01C และคอนโทรลเลอร์ทัช FT6336 มายัง ESP-IDF สำหรับ ESP32-C6 โดยเพิ่มโครงสร้างเชิงวัตถุ, รองรับการแสดงผลผ่าน EPD, การอ่านทัช, สแกนบัส I²C และการทำงานร่วมกับ USB CDC (บนชิปที่รองรับ TinyUSB) พร้อมแยกโค้ดส่วนฮาร์ดแวร์ออกเป็น ESP-IDF component (`gde_display`) เพื่อให้ `main` โฟกัสที่ลูปแอปพลิเคชันอย่างเดียว

## Design Pattern & Architectural Decisions

- **Component Factorisation** – โค้ดฝั่ง driver/assets ถูกย้ายไปไว้ใน `components/gde_display` ทำให้สามารถ reuse หรือเอาไปใช้กับโปรเจ็กต์อื่นได้ง่ายขึ้น โดย `main` ยังคงเป็น entry point เพียงไฟล์เดียว
- **Driver Abstraction** – คลาส `epd::Driver` และ `ft6336::Driver` ทำหน้าที่เป็น wrapper แบบ Facade เหนือ ESP-IDF HAL ช่วยดึงรายละเอียดการกำหนดค่า GPIO/I²C/SPI ออกจาก `app_main`
- **Namespace Encapsulation** – ใช้ namespace (`epd`, `ft6336`, `usb_cdc`) เพื่อแบ่งขอบเขตโค้ดแต่ละส่วน ทำให้ง่ายต่อการขยายหรือเปลี่ยน implementation ภายหลัง
- **Resource Separation** – ไฟล์ `assets.cpp` ใน component เก็บข้อมูลบิตแมพรูปภาพแยกจากโค้ดหลัก ทำให้ง่ายต่อการอัปเดตรูปโดยไม่ต้องแตะ logic
- **Graceful Degradation** – สำหรับชิปที่ไม่มี TinyUSB จะลิงก์กับ `usb_cdc_stub.cpp` แทน ทำให้ build ผ่านได้แม้ไม่มีฟีเจอร์ CDC

## Layering Overview

```
┌─────────────────────────────┐
│ Application Layer            │ → `main/main.cpp`
│  - ลูปหลัก, การนับตัวเลข,    │
│    การจัดการ touch            │
├─────────────────────────────┤
│ Hardware Abstraction Layer   │
│  - `epd::Driver` / `ft6336::Driver` ใน `components/gde_display`
│  - `usb_cdc` facade ใน `main`
├─────────────────────────────┤
│ Assets / Data Layer          │
│  - `components/gde_display/assets.cpp`
├─────────────────────────────┤
│ ESP-IDF Framework & HAL      │
└─────────────────────────────┘
```

## Simplified Class Diagram

```
          +---------------------+
          | epd::Driver         |
          |---------------------|
          | - cfg_ : Config     |
          | - spi_ : handle     |
          | - initialised_      |
          |---------------------|
          | + init(cfg)         |
          | + hardwareInit()    |
          | + clear()           |
          | + loadBaseMap()     |
          | + displayDigits()   |
          +----------+----------+
                     |
          +----------v----------+
          | ft6336::Driver      |
          |---------------------|
          | - cfg_ : Config     |
          | - initialised_      |
          |---------------------|
          | + init(cfg)         |
          | + touchReady()      |
          | + scan(data)        |
          +----------+----------+
                     |
          +----------v----------+
          | usb_cdc (namespace) |
          |---------------------|
          | + init()            |
          | + print/printf()    |
          +---------------------+

```

## Build & Flash

```bash
. $HOME/esp/v5.5.1/export.sh
idf.py set-target esp32c6
idf.py build
idf.py -p /dev/tty.usbmodem11101 -b 460800 flash
idf.py -p /dev/tty.usbmodem11101 monitor
```

> หาก TinyUSB ไม่พร้อมใช้งานบนแพลตฟอร์มที่ใช้ build จะเลือกใช้สตับให้โดยอัตโนมัติ

## Hardware Connections (ESP32-C6)

| Peripheral                | Pin Mapping  |
|---------------------------|--------------|
| E-Paper BUSY              | GPIO20       |
| E-Paper RST               | GPIO23       |
| E-Paper DC                | GPIO15       |
| E-Paper CS                | GPIO21       |
| E-Paper MOSI              | GPIO0        |
| E-Paper CLK               | GPIO1        |
| FT6336 INT                | GPIO19       |
| FT6336 RST                | GPIO18       |
| FT6336 SDA                | GPIO7        |
| FT6336 SCL                | GPIO6        |

## I²C Bus Diagnostics

- เมื่อ FT6336 initialisation ล้มเหลว ระบบจะสแกนบัส I²C และพิมพ์ address ที่พบ (หรือ error code) เพื่อช่วยตรวจสอบการเชื่อมต่อฮาร์ดแวร์
- สามารถดูผลผ่าน `idf.py monitor`

## Touch & Display Behaviour

- ตัวเลขบนจอจะนับเพิ่มอัตโนมัติทุก 1 วินาที
- การแตะฝั่งขวาของจอ (x > 240) จะนับเพิ่ม, ฝั่งซ้าย (x ≤ 240) จะนับลด
- หมายเลขจะแสดงเฉพาะหลักเดียว (0-9) ตามชุดทรัพยากร bitmap ใน `assets.cpp`

## โครงสร้างแฟ้มหลัก

```
├── CMakeLists.txt           # โปรเจ็กต์ ESP-IDF
├── components/
│   └── gde_display/
│       ├── assets.cpp/.h    # bitmap resources
│       ├── epd_driver.cpp/.h
│       ├── ft6336.cpp/.h
│       └── CMakeLists.txt   # ลงทะเบียน component
├── main/
│   ├── usb_cdc.cpp/.h       # USB CDC facade
│   ├── usb_cdc_stub.cpp     # stub สำหรับชิปที่ไม่รองรับ TinyUSB
│   └── main.cpp             # application logic
└── README.md                # (ไฟล์นี้)
```
