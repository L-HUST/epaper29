// epd.h —— 黑白墨水屏驱动（4 线 SPI，UC8151D / SSD1680 / UC8176）
//
// 屏幕型号由 board_config.h 决定，驱动本身不绑定任何尺寸：
//   2.9"  296x128  /  4.2" 400x300  /  4.26" 800x480
//
// 屏幕坐标系（和主流例程一致，屏幕横放）：
//   x: 0..EPD_WIDTH-1   水平方向（长边）
//   y: 0..EPD_HEIGHT-1  垂直方向（短边）
//   左上角 = (0,0)
//   framebuffer 按行连续存放，每行 EPD_STRIDE 字节，1 = 白，0 = 黑
//
// 用法：
//   EPD epd;
//   epd.begin();
//   epd.clear(EPD_WHITE);
//   epd.draw_text(8, 8, "HELLO", EPD_BLACK, 2);
//   epd.display_full();
//   epd.sleep();
#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <string.h>
#include "board_config.h"

#define EPD_BLACK 0
#define EPD_WHITE 1u

// 旧名字，兼容早期代码/文档
#define EPD_W EPD_WIDTH
#define EPD_H EPD_HEIGHT

class EPD {
public:
    explicit EPD(int8_t pin_cs = PIN_EPD_CS, int8_t pin_dc = PIN_EPD_DC,
                 int8_t pin_rst = PIN_EPD_RST, int8_t pin_busy = PIN_EPD_BUSY,
                 int8_t pin_mosi = PIN_EPD_MOSI, int8_t pin_sck = PIN_EPD_SCK);

    // 上电初始化；会把型号、BUSY/STATUS 打到串口便于排查
    void begin();

    // ---- 画图（只动内存）----
    void clear(uint8_t color = EPD_WHITE);
    void set_pixel(int16_t x, int16_t y, uint8_t color);
    void draw_hline(int16_t x, int16_t y, int16_t w, uint8_t color);
    void draw_vline(int16_t x, int16_t y, int16_t h, uint8_t color);
    void draw_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t color);
    void fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t color);

    // 字库是 5x7 ASCII（字库大小与屏型无关）
    // scale = 1 -> 5x7 像素；2 -> 10x14；4 -> 20x28
    void draw_char(int16_t x, int16_t y, char c, uint8_t color = EPD_BLACK,
                   uint8_t scale = 1);
    void draw_text(int16_t x, int16_t y, const char *s, uint8_t color = EPD_BLACK,
                   uint8_t scale = 1);
    // 返回这串文字画完后的 x 坐标
    int16_t text_width(const char *s, uint8_t scale = 1);

    // 简单排版：按 '|' 或 '\n' 换行，放不下的行自动丢弃
    void draw_page(const char *text, int16_t x, int16_t y, uint8_t scale,
                   int16_t line_gap = 4);

    // 一行文字占的像素高度
    static int16_t line_height(uint8_t scale) { return 7 * scale + 4; }

    // ---- 推屏 ----
    void display_full();
    void display_partial(int16_t x, int16_t y, int16_t w, int16_t h);
    void display_partial_margin(int16_t x, int16_t y, int16_t w, int16_t h,
                                int16_t margin);
    void sleep();

    uint8_t *buffer() { return _buf; }
    // 初始化时从屏读回的状态字节；0x00 / 0xFF 说明 SPI 没通
    uint8_t status() const { return _status; }

private:
    void reset();
    void cmd(uint8_t c);
    void data(uint8_t d);
    void data_buf(const uint8_t *p, uint32_t n);
    void wait_busy(const char *what, uint32_t timeout_ms = 20000);
    void set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);
    void write_lut_full();
    void write_lut_partial();

    int8_t _cs, _dc, _rst, _busy, _mosi, _sck;
    uint8_t _buf[EPD_BYTES];
    uint8_t _status = 0;
    bool _spi_ready;
    bool _asleep;
};
