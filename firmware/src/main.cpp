// main.cpp —— 墨水屏应用固件（ESP32-C3 / ESP32-S3）
//
// 目标屏型由 src/board_config.h 决定（默认 4.2 寸 400x300），
// 换屏只改那一个文件，本文件不含任何尺寸相关常量。
//
// 功能：
//   1. 冷启动自检图案 -> 首页，肉眼确认接线/驱动都对
//   2. 首页：标题 + 运行秒数 + “采样值”（局刷更新，每 10 秒一次）
//   3. 串口接收上位机推来的整屏位图，全刷
//   4. 串口接收多行文本，用固件自带 5x7 ASCII 字库排版后局刷（翻页场景）
//   5. 30 秒没数据就自动深睡眠
//
// 串口协议（115200 8N1）：
//   位图：上位机发 0xAA 0x55 -> 设备回 0x06(ACK) ->
//         上位机发 512 字节头（前 4 字节小端长度，须等于 EPD_BYTES）+
//         位图数据 + 2 字节 CRC16-CCITT 大端 -> 设备回 0x06 / 0x15
//   文本：上位机发 "T|x,y,scale|第一行|第二行|...\n" -> 设备排版局刷并回 OK
//   单字节 'F'：立刻做一次全刷（清残影）；'S'：立刻休眠
#include <Arduino.h>
#include <SPI.h>
#include <esp_sleep.h>
#include "epd.h"
#include "font_ascii.h"

EPD epd;

static uint8_t frame[EPD_BYTES];
static uint32_t last_activity = 0;

// RTC 内存：深睡眠不丢，用来区分“冷启动”和“定时唤醒”
RTC_DATA_ATTR static uint32_t boot_count = 0;

static uint16_t crc16(const uint8_t *p, uint32_t n) {
    uint16_t crc = 0xFFFF;
    for (uint32_t i = 0; i < n; i++) {
        crc ^= (uint16_t)p[i] << 8;
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

// ---------------- 界面 ----------------
// 大屏（4.2/4.26 寸，>=200px 高）沿用原来的坐标；
// 小屏（2.9 寸 296x128）单独一套紧凑布局，否则下半截全画在屏外。
static void splash() {
#if EPD_HEIGHT >= 200
    epd.clear(EPD_WHITE);
    epd.draw_rect(0, 0, EPD_WIDTH, EPD_HEIGHT, EPD_BLACK);
    epd.draw_rect(3, 3, EPD_WIDTH - 6, EPD_HEIGHT - 6, EPD_BLACK);

    epd.draw_text(20, 24, EPD_NAME, EPD_BLACK, 4);
    epd.draw_hline(20, 70, EPD_WIDTH - 40, EPD_BLACK);
    epd.draw_text(20, 84, EPD_CTRL, EPD_BLACK, 3);
    epd.draw_text(20, 130, "SELF-TEST OK", EPD_BLACK, 3);

    char b[32];
    snprintf(b, sizeof(b), "framebuffer: %d bytes", (int)EPD_BYTES);
    epd.draw_text(20, 178, b, EPD_BLACK, 2);
    epd.draw_text(20, 220, "WAITING SERIAL DATA...", EPD_BLACK, 2);
    epd.display_full();
#else
    // 2.9 寸 296x128：四行，字号统一 2（10x14 像素）
    epd.clear(EPD_WHITE);
    epd.draw_rect(0, 0, EPD_WIDTH, EPD_HEIGHT, EPD_BLACK);
    epd.draw_rect(3, 3, EPD_WIDTH - 6, EPD_HEIGHT - 6, EPD_BLACK);
    epd.draw_text(10, 10, EPD_NAME, EPD_BLACK, 2);
    epd.draw_hline(10, 40, EPD_WIDTH - 20, EPD_BLACK);
    epd.draw_text(10, 50, EPD_CTRL, EPD_BLACK, 2);
    char b[32];
    snprintf(b, sizeof(b), "fb %d B", (int)EPD_BYTES);
    epd.draw_text(10, 78, b, EPD_BLACK, 2);
    epd.draw_text(10, 104, "SELF-TEST OK", EPD_BLACK, 2);
    epd.display_full();
#endif
}

#if EPD_HEIGHT >= 200
// 只重画"值"那一块，用于局刷演示（仅大屏布局用）
static void draw_value(int16_t x, int16_t y, const char *v, int16_t w) {
    epd.fill_rect(x, y, w, 7 * 3 + 6, EPD_WHITE);
    epd.draw_text(x, y, v, EPD_BLACK, 3);
}
#endif

static void home(bool full) {
#if EPD_HEIGHT >= 200
    epd.clear(EPD_WHITE);
    epd.draw_rect(0, 0, EPD_WIDTH, EPD_HEIGHT, EPD_BLACK);

    epd.draw_text(16, 16, "INK READER", EPD_BLACK, 4);
    epd.draw_hline(16, 58, EPD_WIDTH - 32, EPD_BLACK);

    epd.draw_text(16, 78, "UPTIME(s)", EPD_BLACK, 3);
    epd.draw_text(16, 120, "SAMPLES", EPD_BLACK, 3);
    epd.draw_text(16, 178, "SERIAL: 115200 8N1", EPD_BLACK, 2);
    epd.draw_text(16, 210, "TEXT: T|x,y,scale|L1|L2", EPD_BLACK, 2);
    epd.draw_text(16, 246, "BOOT#", EPD_BLACK, 2);

    char b[24];
    snprintf(b, sizeof(b), "%lu", (unsigned long)(millis() / 1000));
    draw_value(16 + 9 * 18, 78, b, 240);
    snprintf(b, sizeof(b), "%lu", (unsigned long)boot_count);
    epd.draw_text(16 + 5 * 12, 246, b, EPD_BLACK, 2);

    if (full) epd.display_full();
#else
    epd.clear(EPD_WHITE);
    epd.draw_rect(0, 0, EPD_WIDTH, EPD_HEIGHT, EPD_BLACK);
    epd.draw_text(10, 8, "INK READER", EPD_BLACK, 2);
    epd.draw_hline(10, 34, EPD_WIDTH - 20, EPD_BLACK);

    char b[24];
    snprintf(b, sizeof(b), "UP %lu s", (unsigned long)(millis() / 1000));
    epd.draw_text(10, 48, b, EPD_BLACK, 2);
    snprintf(b, sizeof(b), "BOOT %lu", (unsigned long)boot_count);
    epd.draw_text(10, 74, b, EPD_BLACK, 2);
    epd.draw_text(10, 100, "115200 8N1", EPD_BLACK, 2);

    if (full) epd.display_full();
#endif
}

// ---------------- 串口收图 ----------------
static bool read_full_frame() {
    uint8_t magic[2] = {0, 0};
    if (Serial.readBytes(magic, 2) != 2) return false;
    if (magic[0] != 0xAA || magic[1] != 0x55) return false;
    Serial.write(0x06);                       // ACK，请上位机开始发头

    uint8_t head[512];
    if (Serial.readBytes(head, sizeof(head)) != sizeof(head)) return false;
    uint32_t len = (uint32_t)head[0] | ((uint32_t)head[1] << 8) |
                   ((uint32_t)head[2] << 16) | ((uint32_t)head[3] << 24);
    if (len != EPD_BYTES) {
        Serial.printf("ERR len=%lu expect=%u\n", (unsigned long)len, (unsigned)EPD_BYTES);
        return false;
    }

    uint32_t got = 0;
    uint32_t t0 = millis();
    while (got < EPD_BYTES && millis() - t0 < 10000) {
        int n = Serial.readBytes(frame + got, EPD_BYTES - got);
        if (n > 0) { got += n; t0 = millis(); }
    }
    if (got != EPD_BYTES) {
        Serial.printf("ERR short got=%lu/%u\n", (unsigned long)got, (unsigned)EPD_BYTES);
        return false;
    }

    uint8_t cb[2];
    if (Serial.readBytes(cb, 2) != 2) return false;
    uint16_t rx = ((uint16_t)cb[0] << 8) | cb[1];
    uint16_t calc = crc16(frame, EPD_BYTES);
    if (rx != calc) {
        Serial.printf("ERR crc rx=%04X calc=%04X\n", rx, calc);
        return false;
    }
    return true;
}

// 文本命令： T|x,y,scale|第一行|第二行...
static void do_text_line(char *line) {
    char *p1 = strchr(line, '|');
    if (!p1) return;
    int x = 16, y = 20, scale = 3;
    sscanf(p1 + 1, "%d,%d,%d", &x, &y, &scale);
    char *body = strchr(p1 + 1, '|');
    if (!body) return;
    if (scale < 1) scale = 1;
    if (scale > 6) scale = 6;
    body++;

    epd.clear(EPD_WHITE);                       // 整屏重排，避免上一页残留
    epd.draw_rect(0, 0, EPD_WIDTH, EPD_HEIGHT, EPD_BLACK);
    epd.draw_page(body, x, y, (uint8_t)scale, 6);
    // 翻页用局刷：不闪、快。连续刷 20 次建议由上位机发一次 'F' 做全刷清残影
    epd.display_partial_margin(0, 0, EPD_WIDTH, EPD_HEIGHT, 4);
}

static void go_sleep() {
    Serial.println("sleep...");
    Serial.flush();
    epd.sleep();
    esp_sleep_enable_timer_wakeup((uint64_t)WAKE_INTERVAL_S * 1000000ULL);
    esp_deep_sleep_start();
}

// 调试用：记录最近一次全刷耗时。正常全刷约 5000ms；
// 如果只有几毫秒，说明屏根本没被驱动（供电/接线问题）
static uint32_t g_splash_ms = 0;
static uint32_t g_refresh_ms = 0;

// ================================================================
// 官方例程原样测试（epd2in9_V2 的命令序列一个字不改，只换引脚号）
// 用来一刀切开：屏/接线有问题 vs 我的驱动写错
// ================================================================
#ifndef OT_ENABLE
#define OT_ENABLE 0
#endif

// 启动时是否先画一页自检图（调试接线用；正常启动关掉可以少闪一次）
#ifndef SHOW_SPLASH
#define SHOW_SPLASH 0
#endif

// 空闲时每隔多少毫秒补一次全刷（去残影用；0 = 不自动刷，省得一直闪）
#ifndef REFRESH_EVERY_MS
#define REFRESH_EVERY_MS 0
#endif

#if OT_ENABLE
#define OT_CS 10
#define OT_DC 9
#define OT_RST 8
#define OT_BUSY 7

static void ot_cmd(uint8_t c) {
    digitalWrite(OT_DC, LOW);  digitalWrite(OT_CS, LOW);
    SPI.transfer(c);           digitalWrite(OT_CS, HIGH);
}
static void ot_data(uint8_t d) {
    digitalWrite(OT_DC, HIGH); digitalWrite(OT_CS, LOW);
    SPI.transfer(d);           digitalWrite(OT_CS, HIGH);
}
static void ot_busy(void) {
    uint32_t t = millis();
    while (digitalRead(OT_BUSY) == HIGH && millis() - t < 8000) delay(1);
}

static void official_test(void) {
    pinMode(OT_CS, OUTPUT); pinMode(OT_DC, OUTPUT);
    pinMode(OT_RST, OUTPUT); pinMode(OT_BUSY, INPUT);
    digitalWrite(OT_CS, HIGH); digitalWrite(OT_DC, HIGH);
    SPI.begin(PIN_EPD_SCK, -1, PIN_EPD_MOSI, -1);
    SPI.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
    Serial.println("[official] 开始，命令序列照抄 epd2in9_V2");

    digitalWrite(OT_RST, LOW); delay(10);
    digitalWrite(OT_RST, HIGH); delay(10);

    ot_cmd(0x00); ot_data(0x1f); ot_data(0x0d);
    ot_cmd(0x01); ot_data(0x03); ot_data(0x00);
                  ot_data(0x2b); ot_data(0x2b); ot_data(0x09);
    ot_cmd(0x06); ot_data(0x07); ot_data(0x07); ot_data(0x17);
    ot_cmd(0x61); ot_data(0x80); ot_data(0x01); ot_data(0x28); ot_data(0x01);
    ot_cmd(0x50); ot_data(0x97);
    ot_cmd(0x82); ot_data(0x22);
    ot_cmd(0x04); ot_busy();
    Serial.println("[official] 初始化完成，写显存...");

    // 白/黑交替，每 6 秒一次：肉眼能直接看出屏到底在不在变。
    // 全白 = 全 0xFF，全黑 = 全 0x00；即使行宽算错，整屏也该是纯色。
    for (int round = 0; round < 6; round++) {
        uint8_t fill = (round & 1) ? 0x00 : 0xFF;   // 0xFF=白, 0x00=黑
        ot_cmd(0x24);
        digitalWrite(OT_DC, HIGH); digitalWrite(OT_CS, LOW);
        for (int i = 0; i < 4736; i++) SPI.transfer(fill);
        digitalWrite(OT_CS, HIGH);

        ot_cmd(0x22); ot_data(0xF7);
        ot_cmd(0x20);
        Serial.printf("[official] 第 %d 次刷新：%s\n", round + 1,
                      fill == 0xFF ? "全白" : "全黑");
        delay(6000);
    }
    Serial.println("[official] 结束。如果屏一直是同一副噪点，说明数据没进屏");
}
#endif
// ================================================================

void setup() {
    Serial.begin(115200);
    Serial.setTimeout(5000);   // 收 4736 字节时别用默认 1 秒，USB-CDC 容易超时
    // 原生 USB CDC：等枚举完成，最多 3 秒，否则前几行 banner 会丢
    {
        uint32_t t0 = millis();
        while (!Serial && millis() - t0 < 3000) delay(10);
    }
    delay(200);
    boot_count++;
    Serial.printf("\n=== Ink firmware (%s / %s) boot #%lu ===\n",
                  EPD_NAME, EPD_CTRL, (unsigned long)boot_count);
    Serial.printf("framebuffer %d bytes, free heap %u bytes\n",
                  (int)EPD_BYTES, (unsigned)ESP.getFreeHeap());

#if OT_ENABLE
    official_test();
    while (true) delay(1000);   // 测试模式：不再跑正常流程
#endif

    epd.begin();            // 初始化 SPI + 屏（跳过它 SPI 都没初始化）

    // 一次全刷出首页。自检页只在 SHOW_SPLASH=1 时才画（调试接线用），
    // 否则每次开机都要闪两下。
    {
        uint32_t t0 = millis();
#if SHOW_SPLASH
        splash();
#endif
        home(true);
        g_splash_ms = millis() - t0;
    }
    last_activity = millis();
    Serial.println("READY. 文本: T|16,20,3|LINE1|LINE2   图像: python tools/push.py --help");
}

void loop() {
    // 心跳：每 3 秒打一行。串口接晚了也能看到，说明程序在跑
    static uint32_t hb_ms = 0;
    if (millis() - hb_ms > 3000) {
        hb_ms = millis();
        Serial.printf("[alive] %lu s  BUSY=%d  splash=%lums  refresh=%lums\n",
                      (unsigned long)(millis() / 1000),
                      (int)digitalRead(PIN_EPD_BUSY),
                      (unsigned long)g_splash_ms, (unsigned long)g_refresh_ms);
    }
    if (Serial.available()) {
        int c = Serial.peek();
        if (c == 'T') {
            String s = Serial.readStringUntil('\n');
            do_text_line((char *)s.c_str());
            Serial.println("OK");
            last_activity = millis();
        } else if (c == 'F') {
            Serial.read();
            epd.display_full();          // 手动清残影
            Serial.println("OK full");
            last_activity = millis();
        } else if (c == 0xAA) {
            if (read_full_frame()) {
                memcpy(epd.buffer(), frame, EPD_BYTES);
                epd.display_full();
                Serial.println("OK frame");
            } else {
                Serial.write(0x15);
                Serial.println("NAK frame");
            }
            last_activity = millis();
        } else if (c == 'S') {
            Serial.read();
            go_sleep();
        } else {
            Serial.read();               // 丢掉无法识别的字节
        }
    }

#if REFRESH_EVERY_MS > 0
    // 调试期才开：定期全刷，屏会闪一下，用来确认"屏还在被驱动"
    static uint32_t t = 0;
    if (millis() - t > REFRESH_EVERY_MS) {
        t = millis();
        uint32_t t1 = millis();
        epd.display_full();
        g_refresh_ms = millis() - t1;
    }
#endif

    if (IDLE_SLEEP_MS > 0 && millis() - last_activity > IDLE_SLEEP_MS) go_sleep();
}
