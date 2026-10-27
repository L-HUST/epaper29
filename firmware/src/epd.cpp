// epd.cpp —— 黑白墨水屏驱动实现（4 线 SPI）
// 控制器：UC8151D / SSD1680（2.9"）、UC8176（4.2"、4.26"）
//
// 关键约束（都是踩过的坑）：
//  1) BUSY 高 = 忙，低 = 空闲
//  2) 数据入口/窗口用的是"字节坐标"寄存器 0x44/0x45
//     （2.9" 横向 296px = 37B，4.2" 横向 400px = 50B）
//  3) 局刷和全刷之间切换，必须重新设置窗口（否则图像偏移/花屏）
//  4) 局刷几次以后必须来一次全刷清残影
//  5) 复位低电平别超过 10ms（有些驱动板会把整板掉电）
//  6) 刷完立刻 sleep()，长时间加高压会永久坏屏
#include "epd.h"
#include "font_ascii.h"
#include <SPI.h>

// 分辨率/波形/VCOM 等型号相关参数全部来自 board_config.h
static_assert(EPD_WIDTH % 8 == 0, "屏宽必须是 8 的整数倍，否则显存行宽对不齐");
// 显存 = EPD_BYTES（4.2 寸 15000 字节，4.26 寸 48000 字节），
// main.cpp 里还有一份同样大小的收图缓冲，加上 Arduino 核心约 30KB：
//   4.2 寸 + C3  实测 44952 字节（13.7%）
//   2.9 寸 + C3  实测 24424 字节（7.5%）
//   4.26寸 + S3  实测 115920 字节（35.4%）——所以 4.26 寸必须用 S3

// ---------------------------------------------------------------- 命令表
#define C_PANEL_SETTING     0x00
#define C_POWER_SETTING     0x01
#define C_POWER_OFF         0x02
#define C_POWER_ON          0x04
#define C_BOOSTER_SOFTSTART 0x06
#define C_DEEP_SLEEP        0x07
#define C_DATA_ENTRY        0x11   // 数据入口模式（x 增减 / y 增减）
#define C_DATA_START_TX     0x10   // 写新数据（Old/New 里的 New）
#define C_DATA_START_TX2    0x13   // 写旧数据（局刷基准）
#define C_DISPLAY_REFRESH   0x12
#define C_LUT_VCOM_DC       0x20
#define C_LUT_WW            0x21
#define C_LUT_BW            0x22
#define C_LUT_WB            0x23
#define C_LUT_BB            0x24
#define C_PLL_CONTROL       0x30
#define C_VCOM_INTERVAL     0x50
#define C_TCON_SETTING      0x60
#define C_RESOLUTION        0x61
#define C_VCM_DC_SETTING    0x82
#define C_PARTIAL_IN        0x91
#define C_PARTIAL_OUT       0x92
// UC8151（2.9 寸）专用，和 UC8176 完全不同
#define C_WRITE_RAM_BW      0x24   // 写黑白 RAM
#define C_UPDATE_CTRL2      0x22   // 显示更新控制 2
#define C_MASTER_ACTIVATE   0x20   // 启动刷新序列
#define C_DEEP_SLEEP_8151  0x10   // 深睡（后面跟 0x01）

#define LUT_VCOM_LEN 44
#define LUT_W_LEN    42

#if EPD_WRITE_WAVEFORM
// 占位波形（相位结构对，具体灰阶值必须换成官方 LUT，见 README 第 6 节第 8 条）
// 注意：这组是 2.9" UC8151D 的，4.2" UC8176 请用官方 EPD_4in2_V2.c 里的 LUT 替换
static const uint8_t LUT_VCOM_DC[LUT_VCOM_LEN] = {
    0x00, 0x0A, 0x00, 0x00, 0x00, 0x01, 0x60, 0x14, 0x14, 0x00, 0x00,
    0x01, 0x00, 0x14, 0x00, 0x00, 0x00, 0x01, 0x00, 0x13, 0x0A, 0x01,
    0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static const uint8_t LUT_WW[LUT_W_LEN] = {
    0x40, 0x0A, 0x00, 0x00, 0x00, 0x01, 0x90, 0x14, 0x14, 0x00, 0x00,
    0x01, 0x10, 0x14, 0x0A, 0x00, 0x00, 0x01, 0xA0, 0x13, 0x01, 0x00,
    0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static const uint8_t LUT_BW[LUT_W_LEN] = {
    0x40, 0x0A, 0x00, 0x00, 0x00, 0x01, 0x90, 0x14, 0x14, 0x00, 0x00,
    0x01, 0x00, 0x14, 0x0A, 0x00, 0x00, 0x01, 0x99, 0x0C, 0x01, 0x03,
    0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static const uint8_t LUT_WB[LUT_W_LEN] = {
    0x40, 0x0A, 0x00, 0x00, 0x00, 0x01, 0x90, 0x14, 0x14, 0x00, 0x00,
    0x01, 0x00, 0x14, 0x0A, 0x00, 0x00, 0x01, 0x99, 0x0B, 0x04, 0x04,
    0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static const uint8_t LUT_BB[LUT_W_LEN] = {
    0x80, 0x0A, 0x00, 0x00, 0x00, 0x01, 0x90, 0x14, 0x14, 0x00, 0x00,
    0x01, 0x20, 0x14, 0x0A, 0x00, 0x00, 0x01, 0x50, 0x13, 0x01, 0x00,
    0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
#endif  // EPD_WRITE_WAVEFORM

// ---------------------------------------------------------------- 底层时序
EPD::EPD(int8_t pin_cs, int8_t pin_dc, int8_t pin_rst, int8_t pin_busy,
         int8_t pin_mosi, int8_t pin_sck)
    : _cs(pin_cs), _dc(pin_dc), _rst(pin_rst), _busy(pin_busy),
      _mosi(pin_mosi), _sck(pin_sck),
      _spi_ready(false), _asleep(false) {
    memset(_buf, 0xFF, sizeof(_buf));   // 1 = 白
}

#if EPD_BOOSTER_3B
// ============================================================================
// 2.9 寸专用：命令序列与波形表逐字取自卖家例程
//   E-Paper_ESP32_Driver_Board_Code/.../utility/EPD_2in9_V2.cpp
// 之前"全刷耗时 3.5 秒但屏纹丝不动"的原因就是缺了下面几条：
//   0x12 软复位 / 0x11 数据入口 / 0x44+0x45 窗口 / 0x4E+0x4F 光标
//   波形必须由主机下发（0x32 + 153 字节），0x22 用 0xC7 而不是 0xF7
// ============================================================================
static const uint8_t WS_20_30[159] = {
    0x80, 0x66, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00,
    0x10, 0x66, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00,
    0x80, 0x66, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00,
    0x10, 0x66, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x14, 0x08, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x0A, 0x0A, 0x00, 0x0A, 0x0A, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x14, 0x08, 0x00, 0x01, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x00, 0x00, 0x00,
    0x22, 0x17, 0x41, 0x00, 0x32, 0x36
};

static void e29_cmd(uint8_t c) {
    digitalWrite(PIN_EPD_DC, LOW);
    digitalWrite(PIN_EPD_CS, LOW);
    SPI.transfer(c);
    digitalWrite(PIN_EPD_CS, HIGH);
}
static void e29_data(uint8_t d) {
    digitalWrite(PIN_EPD_DC, HIGH);
    digitalWrite(PIN_EPD_CS, LOW);
    SPI.transfer(d);
    digitalWrite(PIN_EPD_CS, HIGH);
}

static void e29_wait_busy(uint32_t timeout_ms) {
    uint32_t t0 = millis();
    while (digitalRead(PIN_EPD_BUSY) == HIGH) {     // 高 = 忙（SSD1680）
        if (millis() - t0 > timeout_ms) {
            Serial.printf("[ep29] BUSY 超时 %lums\n", (unsigned long)timeout_ms);
            return;
        }
        delay(1);
    }
}

static void e29_init(void) {
    pinMode(PIN_EPD_CS, OUTPUT);
    pinMode(PIN_EPD_DC, OUTPUT);
    pinMode(PIN_EPD_RST, OUTPUT);
    pinMode(PIN_EPD_BUSY, INPUT);
    digitalWrite(PIN_EPD_CS, HIGH);
    digitalWrite(PIN_EPD_DC, HIGH);
    SPI.begin(PIN_EPD_SCK, -1, PIN_EPD_MOSI, -1);
    SPI.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));

    // 复位时序照官方：高 10ms / 低 2ms / 高 10ms，再等 100ms
    digitalWrite(PIN_EPD_RST, HIGH); delay(10);
    digitalWrite(PIN_EPD_RST, LOW);  delay(2);
    digitalWrite(PIN_EPD_RST, HIGH); delay(10);
    delay(100);

    e29_wait_busy(20000);
    e29_cmd(0x12);                                  // 软复位
    e29_wait_busy(20000);

    e29_cmd(0x01); e29_data(0x27); e29_data(0x01); e29_data(0x00);  // 驱动输出
    e29_cmd(0x11); e29_data(0x03);                  // 数据入口：X/Y 递增

    // 窗口：X 单位是"字节"（128 像素 = 16 字节），Y 是行（296 行）
    e29_cmd(0x44); e29_data(0x00); e29_data(0x0F);
    e29_cmd(0x45); e29_data(0x00); e29_data(0x00);
                   e29_data(0x27); e29_data(0x01);  // 295 = 0x0127

    e29_cmd(0x21); e29_data(0x00); e29_data(0x80);  // 显示更新控制
    e29_cmd(0x4E); e29_data(0x00);                  // 光标 X
    e29_cmd(0x4F); e29_data(0x00); e29_data(0x00);  // 光标 Y
    e29_wait_busy(20000);

    // 波形：前 153 字节走 0x32，剩下几个字节分散到 0x3F/0x03/0x04/0x2C
    e29_cmd(0x32);
    for (int i = 0; i < 153; i++) e29_data(WS_20_30[i]);
    e29_wait_busy(20000);
    e29_cmd(0x3F); e29_data(WS_20_30[153]);
    e29_cmd(0x03); e29_data(WS_20_30[154]);
    e29_cmd(0x04); e29_data(WS_20_30[155]); e29_data(WS_20_30[156]);
                   e29_data(WS_20_30[157]);
    e29_cmd(0x2C); e29_data(WS_20_30[158]);

    Serial.println("[ep29] init ok (SSD1680 + host LUT)");
}

// 我方画布是横放 296x128（每行 37 字节），屏内 RAM 却是竖着存的：
// 16 字节宽（128 像素 = 短边）× 296 行（长边）。
// 映射照官方 GUI_Paint 的 rotate=270：用户像素 (x,y) -> RAM(x=y, y=295-x)
static void e29_build_mem(const uint8_t *buf, uint8_t *mem) {
    const uint32_t stride_mem = EPD_HEIGHT / 8;          // 128/8 = 16 字节
    memset(mem, 0x00, EPD_BYTES);
    for (uint16_t y = 0; y < EPD_HEIGHT; y++) {
        const uint8_t *row = buf + (uint32_t)y * EPD_STRIDE;
        const uint32_t mcol = (uint32_t)(y >> 3);
        const uint8_t  mbit = (uint8_t)(0x80 >> (y & 7));
        for (uint16_t x = 0; x < EPD_WIDTH; x++) {
            if (row[x >> 3] & (0x80 >> (x & 7)))
                mem[(uint32_t)(EPD_WIDTH - 1 - x) * stride_mem + mcol] |= mbit;
        }
    }
}

// 局刷之后控制器里装的是局刷波形，下一次全刷必须先重新初始化
static bool s_need_reinit = false;

static void e29_send_ram(uint8_t cmd, const uint8_t *mem) {
    e29_cmd(cmd);
    digitalWrite(PIN_EPD_DC, HIGH);
    digitalWrite(PIN_EPD_CS, LOW);
    SPI.writeBytes((uint8_t *)mem, EPD_BYTES);
    digitalWrite(PIN_EPD_CS, HIGH);
}

static void e29_show(const uint8_t *buf, uint32_t n) {
    static uint8_t mem[EPD_BYTES];

    uint32_t t0 = millis();
    e29_build_mem(buf, mem);

    // 0x24 = 新图；0x26 = 旧图。局刷是靠"新图 vs 旧图"逐像素比对的，
    // 全刷时必须把 0x26 也写成同一张图（官方 Display_Base / Clear 都是这么做的），
    // 否则下一次局刷拿一堆垃圾当旧图比，画面就花了。
    e29_send_ram(0x24, mem);
    e29_send_ram(0x26, mem);

    e29_cmd(0x22); e29_data(0xC7);                   // 官方全刷参数
    e29_cmd(0x20);
    e29_wait_busy(20000);

    Serial.printf("[ep29] refresh %lu bytes, %lu ms\n",
                  (unsigned long)n, (unsigned long)(millis() - t0));
    s_need_reinit = false;
}

// 局刷波形，同样逐字取自卖家例程 _WF_PARTIAL_2IN9
static const uint8_t WF_PARTIAL_2IN9[] = {
    0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x80, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x40, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x00, 0x00, 0x00,
    0x22, 0x17, 0x41, 0xB0, 0x32, 0x36
};

// 局刷：不闪、约 0.4 秒。连续多用会积残影，界面上每隔几十次补一次全刷。
static void e29_partial(const uint8_t *buf) {
    static uint8_t mem[EPD_BYTES];
    uint32_t t0 = millis();
    e29_build_mem(buf, mem);

    // 官方局刷入口先给一个短复位脉冲
    digitalWrite(PIN_EPD_RST, LOW);  delay(1);
    digitalWrite(PIN_EPD_RST, HIGH); delay(2);

    e29_cmd(0x32);
    for (int i = 0; i < 153; i++) e29_data(WF_PARTIAL_2IN9[i]);
    e29_wait_busy(20000);

    static const uint8_t en[10] = {0x00, 0x00, 0x00, 0x00, 0x00,
                                   0x40, 0x00, 0x00, 0x00, 0x00};
    e29_cmd(0x37);
    for (int i = 0; i < 10; i++) e29_data(en[i]);
    e29_cmd(0x3C); e29_data(0x80);

    e29_cmd(0x22); e29_data(0xC0);
    e29_cmd(0x20);
    e29_wait_busy(20000);

    e29_cmd(0x44); e29_data(0x00); e29_data(0x0F);
    e29_cmd(0x45); e29_data(0x00); e29_data(0x00);
                   e29_data(0x27); e29_data(0x01);
    e29_cmd(0x4E); e29_data(0x00);
    e29_cmd(0x4F); e29_data(0x00); e29_data(0x00);

    e29_cmd(0x24);
    digitalWrite(PIN_EPD_DC, HIGH);
    digitalWrite(PIN_EPD_CS, LOW);
    SPI.writeBytes(mem, EPD_BYTES);
    digitalWrite(PIN_EPD_CS, HIGH);

    e29_cmd(0x22); e29_data(0x0F);
    e29_cmd(0x20);
    e29_wait_busy(20000);

    s_need_reinit = true;      // 局刷把全刷波形覆盖掉了，下次全刷要重新初始化
    Serial.printf("[ep29] partial %lu ms\n", (unsigned long)(millis() - t0));
}
#endif

void EPD::cmd(uint8_t c) {
    digitalWrite(_dc, LOW);
    digitalWrite(_cs, LOW);
    SPI.transfer(c);
    digitalWrite(_cs, HIGH);
}

void EPD::data(uint8_t d) {
    digitalWrite(_dc, HIGH);
    digitalWrite(_cs, LOW);
    SPI.transfer(d);
    digitalWrite(_cs, HIGH);
}

void EPD::data_buf(const uint8_t *p, uint32_t n) {
    digitalWrite(_dc, HIGH);
    digitalWrite(_cs, LOW);
    SPI.writeBytes(p, n);
    digitalWrite(_cs, HIGH);
}

void EPD::reset() {
    digitalWrite(_rst, HIGH);
    delay(20);
    digitalWrite(_rst, LOW);
#if EPD_BOOSTER_3B
    delay(10);                      // 2.9 寸照官方例程：低 10ms
#else
    delay(2);                       // 4.2 寸：别超过 10ms
#endif
    digitalWrite(_rst, HIGH);
    delay(20);
}

void EPD::wait_busy(const char *what, uint32_t timeout_ms) {
    uint32_t t0 = millis();
    while (digitalRead(_busy) == HIGH) {      // HIGH = 忙
        if (millis() - t0 > timeout_ms) {
            Serial.printf("[epd] BUSY 超时 @%s (>%lums)\n", what,
                          (unsigned long)timeout_ms);
            return;
        }
        delay(1);
    }
}

// ---------------------------------------------------------------- 初始化
void EPD::begin() {
#if EPD_BOOSTER_3B
    e29_init();
    _asleep = false;
    return;                 // 2.9 寸走独立实现
#endif
    if (!_spi_ready) {
        pinMode(_cs, OUTPUT);
        pinMode(_dc, OUTPUT);
        pinMode(_rst, OUTPUT);
        pinMode(_busy, INPUT_PULLUP);   // 上拉：屏没电/线断时读 1，屏正常会主动拉低读 0
        digitalWrite(_cs, HIGH);
        digitalWrite(_dc, HIGH);
        SPI.begin(_sck, -1, _mosi, -1);
        SPI.beginTransaction(SPISettings(EPD_SPI_HZ, MSBFIRST, SPI_MODE0));
        _spi_ready = true;
    }

    reset();

    cmd(C_PANEL_SETTING);    data(0x1F); data(0x0D);          // LUT from OTP, KWR
    cmd(C_POWER_SETTING);    data(0x03); data(0x00); data(0x2B); data(0x2B); data(0x09);
#if EPD_BOOSTER_3B
    // ---------- 2.9 寸 UC8151：严格照官方 epd2in9_V2 ----------
    // 没有 0x30(PLL) 和 0x03(TCON)：0x03 在 UC8151 上是掉电时序，写了会卡忙
    cmd(C_BOOSTER_SOFTSTART); data(0x07); data(0x07); data(0x17);
    cmd(C_RESOLUTION); data(EPD_RES_XL); data(EPD_RES_XH);
                       data(EPD_RES_YL); data(EPD_RES_YH);
    cmd(C_VCOM_INTERVAL);  data(EPD_VCOM_INTERVAL);
    cmd(C_VCM_DC_SETTING); data(EPD_VCOM_DC);
    cmd(C_POWER_ON);                          // 官方例程在 init 末尾上电
    delay(300);                               // BUSY 极性不可靠，用固定等待
#else
    // ---------- 4.2 / 4.26 寸 UC8176 ----------
    cmd(C_BOOSTER_SOFTSTART); data(0x17); data(0x17); data(0x17); data(0x17);
    cmd(C_PLL_CONTROL);      data(0x3C);                      // 100Hz
    cmd(C_TCON_SETTING);     data(0x22);
    cmd(C_RESOLUTION);                                        // 0x61，参数见 board_config.h
    data(EPD_RES_XL); data(EPD_RES_XH); data(EPD_RES_YL); data(EPD_RES_YH);
    cmd(C_VCM_DC_SETTING);   data(EPD_VCOM_DC);               // VCOM，太深调小、太浅调大

#if EPD_WRITE_WAVEFORM
    write_lut_full();
#endif

    cmd(C_VCOM_INTERVAL);    data(EPD_VCOM_INTERVAL);         // 边框跟随 VCOM
    set_window(0, 0, EPD_WIDTH - 1, EPD_HEIGHT - 1);
    wait_busy("begin", 5000);
#endif
    _asleep = false;

    Serial.printf("[epd] %s / %s  init ok. %dx%d, %d 字节显存, BUSY=%d\n",
                  EPD_NAME, EPD_CTRL, EPD_WIDTH, EPD_HEIGHT, (int)EPD_BYTES,
                  digitalRead(_busy));
}

void EPD::set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    uint16_t bx0 = x0 / 8, bx1 = x1 / 8;      // 0x44/0x45 是字节坐标
    cmd(C_DATA_ENTRY); data(0x03);            // X 递增、Y 递增
    cmd(0x44); data(bx0 & 0xFF); data(bx1 & 0xFF);
    cmd(0x45); data(y0 & 0xFF); data((y0 >> 8) & 0xFF);
               data(y1 & 0xFF); data((y1 >> 8) & 0xFF);
}

#if EPD_WRITE_WAVEFORM
void EPD::write_lut_full() {
    cmd(C_LUT_VCOM_DC); data_buf(LUT_VCOM_DC, LUT_VCOM_LEN);
    cmd(C_LUT_WW);      data_buf(LUT_WW, LUT_W_LEN);
    cmd(C_LUT_BW);      data_buf(LUT_BW, LUT_W_LEN);
    cmd(C_LUT_WB);      data_buf(LUT_WB, LUT_W_LEN);
    cmd(C_LUT_BB);      data_buf(LUT_BB, LUT_W_LEN);
}

void EPD::write_lut_partial() {
    // 局刷波形官方是另一组表，这里先复用全刷表；要更好的局刷质量见 docs/02-debug.md 第 4 节
    write_lut_full();
}
#endif

// ---------------------------------------------------------------- 画图
void EPD::clear(uint8_t color) {
    memset(_buf, color ? 0xFF : 0x00, sizeof(_buf));
}

void EPD::set_pixel(int16_t x, int16_t y, uint8_t color) {
    if (x < 0 || y < 0 || x >= EPD_WIDTH || y >= EPD_HEIGHT) return;
    uint32_t idx = (uint32_t)y * EPD_STRIDE + (uint32_t)(x >> 3);
    uint8_t mask = 0x80 >> (x & 7);
    if (color) _buf[idx] |= mask;
    else       _buf[idx] &= (uint8_t)~mask;
}

void EPD::draw_hline(int16_t x, int16_t y, int16_t w, uint8_t color) {
    for (int16_t i = 0; i < w; i++) set_pixel(x + i, y, color);
}

void EPD::draw_vline(int16_t x, int16_t y, int16_t h, uint8_t color) {
    for (int16_t i = 0; i < h; i++) set_pixel(x, y + i, color);
}

void EPD::draw_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t color) {
    if (w <= 0 || h <= 0) return;
    draw_hline(x, y, w, color);
    draw_hline(x, y + h - 1, w, color);
    draw_vline(x, y, h, color);
    draw_vline(x + w - 1, y, h, color);
}

void EPD::fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t color) {
    for (int16_t j = 0; j < h; j++)
        for (int16_t i = 0; i < w; i++) set_pixel(x + i, y + j, color);
}

void EPD::draw_char(int16_t x, int16_t y, char c, uint8_t color, uint8_t scale) {
    if (scale < 1) scale = 1;
    if (c < (char)FONT_ASCII_FIRST || c > 0x7E) c = '?';
    const uint8_t *g = &FONT_ASCII[((uint8_t)c - FONT_ASCII_FIRST) * FONT_ASCII_W];
    for (uint8_t col = 0; col < FONT_ASCII_W; col++) {
        uint8_t bits = g[col];
        for (uint8_t row = 0; row < FONT_ASCII_H; row++) {
            if (!((bits >> row) & 1)) continue;
            if (scale == 1) set_pixel(x + col, y + row, color);
            else            fill_rect(x + col * scale, y + row * scale, scale, scale, color);
        }
    }
}

void EPD::draw_text(int16_t x, int16_t y, const char *s, uint8_t color, uint8_t scale) {
    if (scale < 1) scale = 1;
    while (*s) {
        draw_char(x, y, *s++, color, scale);
        x += (FONT_ASCII_W + 1) * scale;
    }
}

int16_t EPD::text_width(const char *s, uint8_t scale) {
    if (scale < 1) scale = 1;
    return (int16_t)strlen(s) * (FONT_ASCII_W + 1) * scale;
}

// 简单排版：按 '|' 或 '\n' 换行，超出屏幕底部的行自动丢掉
// 只支持 0x20~0x7E 的 ASCII；中文请在上位机渲染成位图推过来
void EPD::draw_page(const char *text, int16_t x, int16_t y, uint8_t scale,
                    int16_t line_gap) {
    if (scale < 1) scale = 1;
    const char *p = text;
    int16_t cy = y;
    char line[96];

    while (*p) {
        size_t n = 0;
        while (*p && *p != '|' && *p != '\n' && n < sizeof(line) - 1) line[n++] = *p++;
        line[n] = 0;
        while (*p == '|' || *p == '\n') p++;

        if (cy + 7 * scale > EPD_HEIGHT) break;      // 放不下了
        draw_text(x, cy, line, EPD_BLACK, scale);
        cy += 7 * scale + line_gap;
    }
}

// ---------------------------------------------------------------- 全刷
void EPD::display_full() {
#if EPD_BOOSTER_3B
    if (s_need_reinit) e29_init();          // 上次是局刷 -> 重新装全刷波形
    e29_show(_buf, EPD_BYTES);
    return;                 // 2.9 寸走独立实现
#endif
    if (_asleep) begin();

#if EPD_BOOSTER_3B
    // 2.9 寸：照官方/测试固件，不设窗口，直接写 4736 字节
    cmd(C_WRITE_RAM_BW);
    data_buf(_buf, EPD_BYTES);
    cmd(C_UPDATE_CTRL2); data(0xF7);
    cmd(C_MASTER_ACTIVATE);
    delay(3500);
#else
    set_window(0, 0, EPD_WIDTH - 1, EPD_HEIGHT - 1);

    cmd(C_DATA_START_TX);                     // 新数据
    data_buf(_buf, EPD_BYTES);

    cmd(C_POWER_ON);
    wait_busy("power_on", 5000);

#if EPD_WRITE_WAVEFORM
    write_lut_full();
#endif

    cmd(C_DISPLAY_REFRESH);                   // 未下发波形时用屏内 OTP 波形
    data(0x00);
    wait_busy("full refresh", 20000);

    cmd(C_POWER_OFF);
    wait_busy("power_off", 5000);
#endif
}

// ---------------------------------------------------------------- 局刷
void EPD::display_partial(int16_t x, int16_t y, int16_t w, int16_t h) {
#if EPD_BOOSTER_3B
    // 2.9 寸：走局刷波形，约 0.4 秒、不闪
    (void)x; (void)y; (void)w; (void)h;
    e29_partial(_buf);
    return;
#endif
    if (_asleep) begin();

    int16_t x0 = x, y0 = y, x1 = x + w - 1, y1 = y + h - 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > EPD_WIDTH - 1) x1 = EPD_WIDTH - 1;
    if (y1 > EPD_HEIGHT - 1) y1 = EPD_HEIGHT - 1;
    if (x1 < x0 || y1 < y0) return;

    cmd(C_PARTIAL_IN);  data(0x00);
    cmd(C_PARTIAL_OUT); data(0x00);

    cmd(C_POWER_ON);
    wait_busy("power_on(p)", 5000);

#if EPD_WRITE_WAVEFORM
    write_lut_partial();
#endif

    // 只推窗口内的数据；每行按字节切片
    set_window(x0, y0, x1, y1);
    uint16_t bx0 = x0 / 8, bx1 = x1 / 8, rows = y1 - y0 + 1;

    cmd(C_DATA_START_TX);
    for (uint16_t r = 0; r < rows; r++) {
        const uint8_t *line = &_buf[(uint32_t)(y0 + r) * EPD_STRIDE];
        data_buf(line + bx0, bx1 - bx0 + 1);
    }

    cmd(C_DISPLAY_REFRESH);
    data(0x00);
    wait_busy("partial refresh", 5000);

    cmd(C_POWER_OFF);
    wait_busy("power_off(p)", 5000);
}

void EPD::display_partial_margin(int16_t x, int16_t y, int16_t w, int16_t h,
                                 int16_t margin) {
    display_partial(x - margin, y - margin, w + 2 * margin, h + 2 * margin);
}

void EPD::sleep() {
    cmd(C_POWER_OFF);
    wait_busy("power_off(sleep)", 5000);
    cmd(C_DEEP_SLEEP);
    data(0x01);                              // 保留 RAM 的深睡眠
    delay(100);
    _asleep = true;
    Serial.println("[epd] deep sleep");
}
