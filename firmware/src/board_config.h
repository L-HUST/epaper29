// board_config.h —— 板级配置：换屏 / 换引脚 / 换主控只改这一个文件
//
// 目前支持（都是 4 线 SPI，命令集接近，差别在分辨率寄存器、VCOM 和波形）：
//   EPD_MODEL_29  : 2.9寸  296x128  UC8151D/SSD1680
//   EPD_MODEL_42  : 4.2寸  400x300  UC8176       <-- 默认
//   EPD_MODEL_426 : 4.26寸 800x480  UC8176       (显存 48KB/屏，必须 S3 + PSRAM)
//
// 切换方式（三选一）：
//   1) 改下面 EPD_MODEL 的默认值
//   2) 用 platformio.ini 里预置的环境：pio run -e esp32-c3-29 / esp32-s3-426
//   3) 自己写环境时在 build_flags 加 -D EPD_MODEL=29
//   注意：PlatformIO 不会因为 .h 内容变化而自动重编，换屏后先 pio run -t clean
#pragma once

// ---------------------------------------------------------------- 屏型号
#define EPD_MODEL_29  29
#define EPD_MODEL_42  42
#define EPD_MODEL_426 426

#ifndef EPD_MODEL
#define EPD_MODEL EPD_MODEL_42
#endif

#if EPD_MODEL == EPD_MODEL_29
  #define EPD_NAME      "2.9inch 296x128"
  #define EPD_WIDTH     296
  #define EPD_HEIGHT    128
  #define EPD_CTRL      "UC8151D/SSD1680"
  // 0x61 分辨率的 4 个字节，照官方 2.9inch V2/V3 例程（epd2in9_V2）：
  //   0x80 0x01 0x28 0x01
  // 之前这里写成 0x00 0x80 0x01 0x28，整体错了一位，导致控制器拿不到合法
  // 分辨率、直接不刷新（表现为刷屏只花 1ms、屏完全不动）。
  #define EPD_RES_XL    0x80
  #define EPD_RES_XH    0x01
  #define EPD_RES_YL    0x28
  #define EPD_RES_YH    0x01
  #define EPD_VCOM_DC   0x22      // 官方 2.9 寸 V2 用的是 0x22
  #define EPD_VCOM_INTERVAL 0x97  // 0x50 寄存器，官方 2.9 寸是 0x97
  #define EPD_BOOSTER_3B    1     // 升压 0x06 是 3 字节（0x07 0x07 0x17）
  #define EPD_WRITE_WAVEFORM 0   // V3 把波形烧进屏内 OTP 了，不用下发

#elif EPD_MODEL == EPD_MODEL_42
  #define EPD_NAME      "4.2inch 400x300"
  #define EPD_WIDTH     400
  #define EPD_HEIGHT    300
  #define EPD_CTRL      "UC8176"
  #define EPD_VCOM_INTERVAL 0x87  // 0x50 寄存器，4.2 寸用 0x87
  #define EPD_BOOSTER_3B    0
  // 400 = 0x0190, 300 = 0x012C
  #define EPD_RES_XL    0x90
  #define EPD_RES_XH    0x01
  #define EPD_RES_YL    0x2C
  #define EPD_RES_YH    0x01
  #define EPD_VCOM_DC   0x12
  // 4.2 寸建议下发官方 LUT（屏内 OTP 不一定和你的屏批次匹配）。
  // 打开后必须把官方 demo 的 LUT 填进 epd.cpp 的 write_lut_full()，见 README 第 6 节第 8 条。
  // 保守做法：先设 0 用 OTP 波形试一次，能出图就先不动。
  #define EPD_WRITE_WAVEFORM 0

#elif EPD_MODEL == EPD_MODEL_426
  #define EPD_NAME      "4.26inch 800x480"
  #define EPD_WIDTH     800
  #define EPD_HEIGHT    480
  #define EPD_CTRL      "UC8176 (需按官方例程核对寄存器)"
  // 800 = 0x0320, 480 = 0x01E0
  #define EPD_RES_XL    0x20
  #define EPD_RES_XH    0x03
  #define EPD_RES_YL    0xE0
  #define EPD_RES_YH    0x01
  #define EPD_VCOM_DC   0x12
  #define EPD_VCOM_INTERVAL 0x87
  #define EPD_BOOSTER_3B    0
  #define EPD_WRITE_WAVEFORM 0
#else
  #error "未知的 EPD_MODEL"
#endif

// 显存：每行字节数按 8 位对齐，1 = 白，0 = 黑
#define EPD_STRIDE ((EPD_WIDTH + 7) / 8)
#define EPD_BYTES  (EPD_STRIDE * EPD_HEIGHT)

// ---------------------------------------------------------------- 接线
// 引脚表跟着"开发板"走，不跟着"屏"走。换板改 BOARD_SEL，换屏改 EPD_MODEL。
//
//   BOARD_C3_SUPERMINI : ¥15，11 个可用 GPIO 已被本工程占满，没法扩展
//                        （只适合点亮屏 / 最小验证）
//   BOARD_S3_DEVKITC1  : ¥45，30+ GPIO、8MB PSRAM、16MB Flash、内置 BLE
//                        ← 做产品用这个
//
// 切板：platformio.ini 里 esp32-s3 / esp32-s3-426 环境已经带 -D BOARD_SEL=2，
//       命令行不要自己加 -D（PlatformIO 不支持），改环境或改下面的默认值。
#define BOARD_C3_SUPERMINI 1
#define BOARD_S3_DEVKITC1  2

#ifndef BOARD_SEL
#define BOARD_SEL BOARD_C3_SUPERMINI
#endif

#if BOARD_SEL == BOARD_S3_DEVKITC1
  // ESP32-S3-DevKitC-1 (N16R8)，脚位和微雪 ESP32-S3 例程一致
  // 禁用脚：GPIO19/20(USB) 0/3/45/46(strapping) 26~32(Flash) 33~37(八线 PSRAM)
  #define PIN_EPD_CS   10
  #define PIN_EPD_DC   9
  #define PIN_EPD_RST  8
  #define PIN_EPD_BUSY 7
  #define PIN_EPD_MOSI 11
  #define PIN_EPD_SCK  12

  #define PIN_KEY_PREV 4
  #define PIN_KEY_NEXT 5

  // microSD 与墨水屏共用 SCK/MOSI（屏只写不读，不会抢 MISO），只多 2 根线
  #define PIN_SD_CS    13
  #define PIN_SD_SCK   PIN_EPD_SCK
  #define PIN_SD_MOSI  PIN_EPD_MOSI
  #define PIN_SD_MISO  14

  // 扩展口：现在不用，先把脚位固定下来，接线/打板不会再改
  // 注意避开了 GPIO39~42（JTAG），留一个能挂调试器的口
  #define PIN_I2C_SDA   47    // 传感器、RTC、IO 扩展
  #define PIN_I2C_SCL   48
  #define PIN_VBAT_ADC  3     // 电池电压，1M/1M 分压
  #define PIN_PWR_HOLD  21    // 软开关：拉高维持供电，拉低自断
  #define PIN_EXT_TX    17    // 空闲串口，接 GPS / 串口屏 / 外挂蓝牙模块
  #define PIN_EXT_RX    18
  #define PIN_EXT_IO1   6     // 备用数字 IO（非 ADC）
  #define PIN_EXT_IO2   15    // 备用数字 IO（非 ADC）
  #define HAS_EXT_HEADER 1

#else
  // ESP32-C3 SuperMini（引脚已接近用满，扩展口一行都没有）
  #define PIN_EPD_CS   7
  #define PIN_EPD_DC   1
  #define PIN_EPD_RST  4
  #define PIN_EPD_BUSY 5
  #define PIN_EPD_MOSI 3
  #define PIN_EPD_SCK  2

  #define PIN_KEY_PREV 6
  #define PIN_KEY_NEXT 10

  // 注意：GPIO9 在 C3 SuperMini 上是 BOOT 键，接 MISO 时不要按着那个键上电
  #define PIN_SD_CS    8
  #define PIN_SD_SCK   20
  #define PIN_SD_MOSI  21
  #define PIN_SD_MISO  9

  // C3 没有多余 GPIO，这几个是占位（-1 = 不可用）
  #define PIN_I2C_SDA   -1
  #define PIN_I2C_SCL   -1
  #define PIN_VBAT_ADC  -1
  #define PIN_PWR_HOLD  -1
  #define PIN_EXT_TX    -1
  #define PIN_EXT_RX    -1
  #define PIN_EXT_IO1   -1
  #define PIN_EXT_IO2   -1
  #define HAS_EXT_HEADER 0
#endif

// SPI 时钟：屏能上 10MHz，但线长/供电一般时降到 4~8MHz 更稳
// 2.9 寸先用 1MHz 排除杜邦线太长导致的时序问题，通了再往上提
#if EPD_MODEL == EPD_MODEL_29
  #define EPD_SPI_HZ   2000000
#else
  #define EPD_SPI_HZ   8000000
#endif

// ---------------------------------------------------------------- 功能开关
// 按键接 GND 触发（引脚见上面的按板分支）；不接就设 0
#define EPD_HAS_KEYS  1
// microSD：S3 上和墨水屏共用 SCK/MOSI，只多 MISO + CS（引脚见上面按板分支）
#define EPD_HAS_SD    0

// ---------------------------------------------------------------- 应用参数
// 串口空闲多久后深睡；0 = 不休眠（调试期间用 0，产品化再改成 30000）
#define IDLE_SLEEP_MS     0
#define WAKE_INTERVAL_S   30      // 深睡眠唤醒周期（秒，仅在 IDLE_SLEEP_MS > 0 时生效）
