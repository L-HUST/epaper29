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
#include <LittleFS.h>
#include <Preferences.h>
#include "epd.h"
#include "font_ascii.h"
#include "keys.h"
#include "book_pack.h"

EPD epd;

static uint8_t frame[EPD_BYTES];
static uint32_t last_activity = 0;

// 当前打开的书/页；v1 先只读内部 flash 的 /books 下第一本 .epg
static BookPack g_book;
static uint32_t g_page = 0;
static uint32_t g_since_full = 0;
static bool g_home = true;       // 当前显示的是首页还是阅读页
static bool s_fs_ready = false;  // LittleFS 只挂一次

// ---------------- M5 UI / settings ----------------
static bool g_menu = false;
static uint8_t g_menu_sel = 0;
static bool g_settings = false;
static uint8_t g_settings_sel = 0;
static bool g_goto = false;
static uint8_t g_goto_digits[6] = {0};
static uint8_t g_goto_len = 4;
static uint8_t g_goto_pos = 3;
static uint8_t g_refresh_every = 20;
static uint32_t g_sleep_after_ms = 0;  // 0 = 关闭

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

static void library_scan();
static void library_draw(bool full);
static bool load_book_progress(const char *path, uint32_t &page, uint32_t &size);
static void save_progress();
static void clear_progress();
static bool resume_progress();
static void menu_open();
static void menu_draw(bool full);
static void goto_init();
static void goto_draw(bool full);
static void settings_draw(bool full);
static void settings_load();
static void settings_save();

static void home(bool full) {
    // M4-3：首页就是书库；具体绘制在 library_draw() 里。
    library_draw(full);
}

static uint8_t digits_for(uint32_t n) {
    uint8_t d = 1;
    while (n >= 10 && d < 9) { n /= 10; d++; }
    return d;
}

static void settings_load() {
    Preferences p;
    if (!p.begin("settings", true)) return;
    uint8_t r = (uint8_t)p.getUInt("refresh", 20);
    uint8_t s = (uint8_t)p.getUInt("sleep", 0);
    p.end();
    g_refresh_every = (r == 10 || r == 20 || r == 30) ? r : 20;
    if (s != 0 && s != 1 && s != 5 && s != 10) s = 0;
    g_sleep_after_ms = (uint32_t)s * 60000UL;
}

static void settings_save() {
    Preferences p;
    if (!p.begin("settings", false)) return;
    p.putUInt("refresh", g_refresh_every);
    p.putUInt("sleep", g_sleep_after_ms / 60000UL);
    p.end();
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

// 休眠页：和首页明显不同，避免"醒没醒"分不清
static void sleep_screen() {
    epd.clear(EPD_WHITE);
    epd.draw_rect(0, 0, EPD_WIDTH, EPD_HEIGHT, EPD_BLACK);

#if EPD_HEIGHT >= 200
    epd.draw_text(20, 18, "SLEEP", EPD_BLACK, 8);
    epd.draw_text(EPD_WIDTH - 130, 22, "z z z", EPD_BLACK, 4);
    epd.draw_hline(20, 105, EPD_WIDTH - 40, EPD_BLACK);
    epd.draw_text(20, 135, "DEEP SLEEP", EPD_BLACK, 4);
#if EPD_WIDTH >= 600
    epd.draw_text(20, 215, "Press any key to wake", EPD_BLACK, 3);
#else
    epd.draw_text(20, 215, "Press any key to wake", EPD_BLACK, 2);
#endif
#else
    epd.draw_cjk_text(10, 8, "休眠", EPD_BLACK, 15);
    epd.draw_text(224, 8, "z z z", EPD_BLACK, 2);
    epd.draw_hline(10, 36, EPD_WIDTH - 20, EPD_BLACK);
    epd.draw_cjk_text(118, 50, "深度休眠", EPD_BLACK, 15);
    epd.draw_cjk_text(112, 82, "按任意键唤醒", EPD_BLACK, 12);
#endif

    epd.display_full();
}

static void go_sleep() {
    Serial.println("sleep...");
    Serial.flush();
    if (g_book.isOpen()) save_progress();

    // 先把 SLEEP 页画出来，长按 POWER 的人马上能看到反馈；
    // 再等松手，否则 ext1 会在按住时立刻把板子唤醒。
    sleep_screen();
    if (!keys_wait_all_released(5000)) {
        Serial.println("[sleep] key still held, canceled");
        g_home = true;
        home(true);
        return;
    }

    epd.sleep();

    uint64_t mask = keys_wake_mask();
    if (mask) {
#if BOARD_SEL == BOARD_S3_DEVKITC1
        keys_prepare_sleep();
        esp_err_t err = esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_LOW);
        if (err == ESP_OK) {
            esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
            Serial.println("[sleep] wake on any key");
        } else {
            Serial.printf("[sleep] ext1 failed (%d), timer fallback\n", (int)err);
            esp_sleep_enable_timer_wakeup((uint64_t)WAKE_INTERVAL_S * 1000000ULL);
        }
#else
        esp_sleep_enable_timer_wakeup((uint64_t)WAKE_INTERVAL_S * 1000000ULL);
#endif
    } else {
        esp_sleep_enable_timer_wakeup((uint64_t)WAKE_INTERVAL_S * 1000000ULL);
    }
    Serial.flush();
    esp_deep_sleep_start();
}

// ---------------- 阅读器（v1：内部 flash 第一本 .epg） ----------------
static bool ensure_fs() {
    if (s_fs_ready) return true;
    if (!LittleFS.begin(false, "/littlefs", 10, "spiffs")) {
        Serial.println("[fs] LittleFS mount failed (不自动格式化)");
        return false;
    }
    s_fs_ready = true;
    return true;
}

static bool open_first_book() {
    if (g_book.isOpen()) return true;
    if (!ensure_fs()) return false;
    if (!LittleFS.exists("/books")) {
        LittleFS.mkdir("/books");
        return false;
    }

    fs::File dir = LittleFS.open("/books");
    if (!dir || !dir.isDirectory()) {
        Serial.println("[fs] /books not a directory");
        return false;
    }

    fs::File f = dir.openNextFile();
    while (f) {
        String name = f.name();
        if (name.endsWith(".epg")) {
            String path = f.path();
            f.close();
            dir.close();
            return g_book.open(LittleFS, path.c_str());
        }
        f.close();
        f = dir.openNextFile();
    }
    dir.close();
    Serial.println("[fs] no .epg in /books");
    return false;
}

// ---------------- M4-3 书库 ----------------
#define LIB_MAX 32
#define LIB_ROWS 4
#define LIB_ROW_H 24
#define LIB_TITLE_H 16

struct BookEntry {
    char path[64];
    uint32_t size;
    uint8_t strip[EPD_STRIDE * LIB_TITLE_H];
};

static BookEntry g_lib[LIB_MAX];
static uint8_t g_lib_count = 0;
static uint8_t g_lib_sel = 0;
static uint8_t g_lib_top = 0;

// 书名条是从阅读页页眉裁出来的，自带阅读页的 1px 外框；书库行里
// 不能再带这圈线，否则每本书看起来都有黑色框线。
static void strip_clear_border(uint8_t *strip) {
    memset(strip, 0xFF, EPD_STRIDE);          // 顶部页边框
    for (int r = 0; r < LIB_TITLE_H; r++) {
        uint8_t *row = strip + r * EPD_STRIDE;
        row[0] |= 0x80;                        // 左页边框
        row[EPD_STRIDE - 1] |= 0x01;           // 右页边框
    }
}

static void library_scan() {
    g_lib_count = 0;
    g_lib_sel = 0;
    g_lib_top = 0;
    if (!ensure_fs()) return;

    fs::File dir = LittleFS.open("/books");
    if (!dir || !dir.isDirectory()) {
        Serial.println("[lib] /books missing");
        return;
    }

    static uint8_t pagebuf[EPD_BYTES];
    fs::File f = dir.openNextFile();
    while (f && g_lib_count < LIB_MAX) {
        String name = f.name();
        if (!f.isDirectory() && name.endsWith(".epg")) {
            String path = f.path();
            BookEntry &e = g_lib[g_lib_count];
            strncpy(e.path, path.c_str(), sizeof(e.path) - 1);
            e.path[sizeof(e.path) - 1] = 0;
            e.size = (uint32_t)f.size();
            memset(e.strip, 0xFF, sizeof(e.strip));

            BookPack bp;
            if (bp.open(LittleFS, e.path)) {
                uint32_t page = 0, oldSize = 0;
                load_book_progress(e.path, page, oldSize);
                if ((oldSize != 0 && oldSize != e.size) || page >= bp.pageCount()) {
                    page = 0;
                }
                if (bp.readPage(page, pagebuf)) {
                    memcpy(e.strip, pagebuf, sizeof(e.strip));
                    strip_clear_border(e.strip);
                    Serial.printf("[lib] %s  p%lu/%lu\n", e.path,
                                  (unsigned long)(page + 1),
                                  (unsigned long)bp.pageCount());
                }
            }
            bp.close();
            g_lib_count++;
        }
        f.close();
        f = dir.openNextFile();
    }
    dir.close();

    // 稳定顺序，避免 LittleFS 目录顺序变化导致选中的书跳来跳去
    for (uint8_t i = 0; i < g_lib_count; i++) {
        for (uint8_t j = i + 1; j < g_lib_count; j++) {
            if (strcmp(g_lib[i].path, g_lib[j].path) > 0) {
                BookEntry tmp = g_lib[i];
                g_lib[i] = g_lib[j];
                g_lib[j] = tmp;
            }
        }
    }
    Serial.printf("[lib] %u book(s)\n", g_lib_count);
}

static void draw_strip(int16_t y, const uint8_t *strip, bool invert) {
    uint8_t *fb = epd.buffer();
    for (int r = 0; r < LIB_TITLE_H; r++) {
        int16_t yy = y + r;
        if (yy < 0 || yy >= EPD_HEIGHT) continue;
        const uint8_t *src = strip + r * EPD_STRIDE;
        uint8_t *dst = fb + yy * EPD_STRIDE;
        for (int b = 0; b < EPD_STRIDE; b++) {
            dst[b] = invert ? (uint8_t)~src[b] : src[b];
        }
    }
}

static bool strip_has_ink(const uint8_t *strip) {
    for (int i = 0; i < (int)sizeof(((BookEntry *)0)->strip); i++) {
        if (strip[i] != 0xFF) return true;
    }
    return false;
}

static void library_draw(bool full) {
    epd.clear(EPD_WHITE);
    epd.draw_rect(0, 0, EPD_WIDTH, EPD_HEIGHT, EPD_BLACK);
    epd.draw_cjk_text(4, 2, "书库", EPD_BLACK, 15);

    char count[24];
    snprintf(count, sizeof(count), "%u 本", g_lib_count);
    epd.draw_cjk_text(EPD_WIDTH - epd.cjk_text_width(count, 12) - 4, 4,
                      count, EPD_BLACK, 12);
    epd.draw_hline(0, 18, EPD_WIDTH, EPD_BLACK);

    const int16_t row_y = 20;
    for (uint8_t r = 0; r < LIB_ROWS; r++) {
        uint8_t idx = g_lib_top + r;
        if (idx >= g_lib_count) break;
        int16_t y = row_y + r * LIB_ROW_H;
        bool sel = (idx == g_lib_sel);
        // 选中底色只覆盖书名位图条的同一 16px 区域，避免 24px 黑块
        // 和 16px 位图条上下错位。
        if (sel) epd.fill_rect(0, y + 4, EPD_WIDTH, LIB_TITLE_H, EPD_BLACK);

        const BookEntry &e = g_lib[idx];
        if (strip_has_ink(e.strip)) {
            draw_strip(y + 4, e.strip, sel);
        } else {
            char fallback[24];
            snprintf(fallback, sizeof(fallback), "书 %u", (unsigned)(idx + 1));
            epd.draw_cjk_text(4, y + 5, fallback,
                             sel ? EPD_WHITE : EPD_BLACK, 15);
        }
    }

    if (g_lib_count == 0) {
        epd.draw_cjk_text(12, 48, "没有书", EPD_BLACK, 15);
        epd.draw_cjk_text(12, 78, "请用数据线传书", EPD_BLACK, 12);
    }

    epd.draw_hline(0, 114, EPD_WIDTH, EPD_BLACK);
    const char *foot = g_lib_count ? "确定=打开  电源=休眠" : "请用数据线传书";
    epd.draw_cjk_text(4, 116, foot, EPD_BLACK, 12);

    // 书名条是整行位图，会覆盖左右外框；最后补画一次外框，保证未选中行也不断线。
    epd.draw_rect(0, 0, EPD_WIDTH, EPD_HEIGHT, EPD_BLACK);

    if (full) epd.display_full();
    else epd.display_partial_margin(0, 0, EPD_WIDTH, EPD_HEIGHT, 4);
}

static void library_move(int dir) {
    if (g_lib_count == 0) return;
    uint8_t old = g_lib_sel;
    if (dir < 0 && g_lib_sel > 0) g_lib_sel--;
    if (dir > 0 && g_lib_sel + 1 < g_lib_count) g_lib_sel++;
    if (g_lib_sel < g_lib_top) g_lib_top = g_lib_sel;
    if (g_lib_sel >= g_lib_top + LIB_ROWS) g_lib_top = g_lib_sel - LIB_ROWS + 1;
    if (g_lib_sel != old) library_draw(false);
}

static bool library_open_selected();

static uint32_t book_file_size(const char *path) {
    fs::File f = LittleFS.open(path, "r");
    if (!f) return 0;
    uint32_t n = (uint32_t)f.size();
    f.close();
    return n;
}

static uint32_t book_id(const char *path) {
    uint32_t h = 2166136261u;
    while (*path) {
        h ^= (uint8_t)*path++;
        h *= 16777619u;
    }
    return h;
}

static void progress_key(char *out, size_t n, char prefix, uint32_t id) {
    snprintf(out, n, "%c%08lX", prefix, (unsigned long)id);
}

static bool load_book_progress(const char *path, uint32_t &page, uint32_t &size) {
    Preferences p;
    if (!p.begin("reader", true)) return false;
    char kp[16], ks[16];
    progress_key(kp, sizeof(kp), 'p', book_id(path));
    progress_key(ks, sizeof(ks), 's', book_id(path));
    page = p.getUInt(kp, 0);
    size = p.getUInt(ks, 0);
    p.end();
    return true;
}

static void save_progress() {
    if (!g_book.isOpen() || !g_book.path()[0]) return;
    Preferences p;
    if (!p.begin("reader", false)) return;
    uint32_t sz = book_file_size(g_book.path());
    char kp[16], ks[16];
    progress_key(kp, sizeof(kp), 'p', book_id(g_book.path()));
    progress_key(ks, sizeof(ks), 's', book_id(g_book.path()));

    // 只写变化项：正常翻页只写 page 一个 entry，size/path 不变就不碰。
    if (p.getString("path", "") != g_book.path()) {
        p.putString("path", g_book.path());
    }
    if (p.getUInt(ks, 0) != sz) {
        p.putUInt(ks, sz);
    }
    if (p.getUInt(kp, 0xFFFFFFFFu) != g_page) {
        p.putUInt(kp, g_page);
    }
    p.end();
}

static void clear_book_progress(const char *path) {
    Preferences p;
    if (!p.begin("reader", false)) return;
    char kp[16], ks[16];
    progress_key(kp, sizeof(kp), 'p', book_id(path));
    progress_key(ks, sizeof(ks), 's', book_id(path));
    p.remove(kp);
    p.remove(ks);
    if (p.getString("path", "") == path) {
        p.remove("path");
        p.remove("page");
        p.remove("size");
    }
    p.end();
}

static void clear_progress() {
    Preferences p;
    if (!p.begin("reader", false)) return;
    p.clear();
    p.end();
}

// ---------------- 串口维护命令：内存 / 文件 / 上传 ----------------
static uint16_t crc16_update(uint16_t crc, uint8_t b) {
    crc ^= (uint16_t)b << 8;
    for (int i = 0; i < 8; i++)
        crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                             : (uint16_t)(crc << 1);
    return crc;
}

static void print_mem() {
    Serial.printf("[mem] heap free=%.2f MB min=%.2f MB total=%.2f MB maxalloc=%.2f MB\n",
                  ESP.getFreeHeap() / 1048576.0,
                  ESP.getMinFreeHeap() / 1048576.0,
                  ESP.getHeapSize() / 1048576.0,
                  ESP.getMaxAllocHeap() / 1048576.0);
    if (psramFound()) {
        Serial.printf("[mem] psram free=%.2f MB total=%.2f MB\n",
                      ESP.getFreePsram() / 1048576.0,
                      ESP.getPsramSize() / 1048576.0);
    } else {
        Serial.println("[mem] psram none");
    }
    if (ensure_fs()) {
        uint32_t total = (uint32_t)LittleFS.totalBytes();
        uint32_t used = (uint32_t)LittleFS.usedBytes();
        Serial.printf("[fs] used=%.2f MB total=%.2f MB free=%.2f MB\n",
                      used / 1048576.0, total / 1048576.0,
                      (total - used) / 1048576.0);
    }
}

static void list_dir(const char *path) {
    if (!ensure_fs()) return;
    fs::File dir = LittleFS.open(path);
    if (!dir || !dir.isDirectory()) {
        Serial.printf("[fs] %s: not a directory\n", path);
        return;
    }
    Serial.printf("[fs] %s:\n", path);
    uint32_t n = 0;
    fs::File f = dir.openNextFile();
    while (f) {
        Serial.printf("  %-24s %8.2f MB%s\n", f.name(),
                      f.size() / 1048576.0, f.isDirectory() ? "/" : "");
        n++;
        f.close();
        f = dir.openNextFile();
    }
    dir.close();
    Serial.printf("[fs] %lu entries\n", (unsigned long)n);
}

static void upload_file(String args) {
    if (!ensure_fs()) {
        Serial.println("ERR fs");
        return;
    }
    if (args.startsWith("|")) args.remove(0, 1);

    int p1 = args.indexOf('|');
    int p2 = (p1 >= 0) ? args.indexOf('|', p1 + 1) : -1;
    if (p1 < 0 || p2 < 0) {
        Serial.println("ERR header");
        return;
    }
    String path = args.substring(0, p1);
    String sizeS = args.substring(p1 + 1, p2);
    String crcS = args.substring(p2 + 1);

    if (!path.startsWith("/books/") || !path.endsWith(".epg") ||
        path.length() >= 60 || path.indexOf("..") >= 0 ||
        path.indexOf('\\') >= 0) {
        Serial.println("ERR path");
        return;
    }

    uint32_t size = (uint32_t)strtoul(sizeS.c_str(), nullptr, 10);
    uint16_t wantCrc = (uint16_t)strtoul(crcS.c_str(), nullptr, 16);
    if (size == 0 || size > 8UL * 1024UL * 1024UL) {
        Serial.println("ERR size");
        return;
    }

    uint32_t total = (uint32_t)LittleFS.totalBytes();
    uint32_t used = (uint32_t)LittleFS.usedBytes();
    if (size + 4096 > total - used) {
        Serial.println("ERR space");
        return;
    }

    if (!LittleFS.exists("/books")) LittleFS.mkdir("/books");
    String tmp = path + ".tmp";

    fs::File out = LittleFS.open(tmp, "w", true);
    if (!out) {
        Serial.println("ERR open");
        return;
    }
    // 4KB 从 USB 收，攒 64KB 再写 flash；下面非落盘块会先 ACK。

    // 4KB 从 USB 收，先攒 64KB 再落 flash；非落盘块立刻 ACK，
    // 让 USB 接收和 flash 写入重叠。
    Serial.printf("READY %s %lu\n", path.c_str(), (unsigned long)size);
    Serial.flush();

    Serial.setTimeout(30000);
    static uint8_t buf[4096];
    static uint8_t big[65536];
    size_t blen = 0;
    uint32_t got = 0;
    uint16_t crc = 0xFFFF;
    bool ok = true;

    while (got < size) {
        size_t want = size - got;
        if (want > sizeof(buf)) want = sizeof(buf);
        size_t n = Serial.readBytes(buf, want);
        if (n != want) {
            Serial.printf("[upload] timeout got=%lu/%lu n=%u want=%u\n",
                          (unsigned long)got, (unsigned long)size,
                          (unsigned)n, (unsigned)want);
            Serial.println("ERR timeout");
            ok = false;
            break;
        }
        for (size_t i = 0; i < n; i++) crc = crc16_update(crc, buf[i]);
        got += n;
        memcpy(big + blen, buf, n);
        blen += n;

        bool flush = (blen + sizeof(buf) > sizeof(big)) || (got == size);
        if (flush) {
            if (out.write(big, blen) != blen) {
                Serial.println("ERR write");
                ok = false;
                break;
            }
            blen = 0;
        }
        Serial.write(0x06);       // 每 4KB ACK；落盘块等写完再 ACK
    }
    out.close();
    Serial.setTimeout(5000);

    if (!ok) {
        LittleFS.remove(tmp.c_str());
        return;
    }
    if (got != size || crc != wantCrc) {
        Serial.printf("ERR crc got=%lu/%04X want=%lu/%04X\n",
                      (unsigned long)got, crc, (unsigned long)size, wantCrc);
        LittleFS.remove(tmp.c_str());
        return;
    }

    bool same = g_book.isOpen() && strcmp(g_book.path(), path.c_str()) == 0;
    if (same) g_book.close();
    LittleFS.remove(path.c_str());
    if (!LittleFS.rename(tmp.c_str(), path.c_str())) {
        Serial.println("ERR rename");
        LittleFS.remove(tmp.c_str());
        if (same) open_first_book();
        return;
    }
    if (same) {
        g_page = 0;
        g_since_full = 0;
        clear_book_progress(path.c_str());
        open_first_book();
    }
    if (g_home) {
        library_scan();
        home(true);
    }

    Serial.printf("OK upload %s %lu %04X\n", path.c_str(),
                  (unsigned long)size, wantCrc);
}

static void delete_file(String args) {
    if (!ensure_fs()) {
        Serial.println("ERR fs");
        return;
    }
    if (args.startsWith("|")) args.remove(0, 1);
    args.trim();
    if (!args.startsWith("/")) args = "/books/" + args;
    if (!args.startsWith("/books/") || args.indexOf("..") >= 0 ||
        args.indexOf('\\') >= 0 || args.length() < 8) {
        Serial.println("ERR path");
        return;
    }

    bool same = g_book.isOpen() && strcmp(g_book.path(), args.c_str()) == 0;
    if (same) g_book.close();

    if (!LittleFS.remove(args.c_str())) {
        Serial.printf("ERR delete %s\n", args.c_str());
        if (same) {
            library_scan();
            g_home = true;
            home(true);
        }
        return;
    }

    if (!same) clear_book_progress(args.c_str());

    if (same) {
        g_page = 0;
        g_since_full = 0;
        g_home = true;
        clear_book_progress(args.c_str());
        library_scan();
        home(true);
    } else if (g_home) {
        library_scan();
        home(true);
    }
    Serial.printf("OK delete %s\n", args.c_str());
}

static bool show_page(bool full) {
    if (!g_book.isOpen()) return false;
    if (!g_book.readPage(g_page, epd.buffer())) {
        Serial.printf("[reader] read page %lu failed\n", (unsigned long)g_page);
        return false;
    }

    if (full) {
        epd.display_full();
        g_since_full = 0;
    } else {
        // 第 20 页直接全刷，避免"先局刷再全刷"闪两次
        if (++g_since_full >= g_refresh_every) {
            epd.display_full();
            g_since_full = 0;
        } else {
            epd.display_partial_margin(0, 0, EPD_WIDTH, EPD_HEIGHT, 4);
        }
    }
    Serial.printf("[reader] %s  page %lu/%lu%s\n",
                  g_book.title(), (unsigned long)(g_page + 1),
                  (unsigned long)g_book.pageCount(), full ? "  full" : "");
    g_home = false;
    save_progress();
    return true;
}

static bool library_open_selected() {
    if (g_lib_sel >= g_lib_count) return false;
    g_menu = g_settings = g_goto = false;
    if (!g_book.open(LittleFS, g_lib[g_lib_sel].path)) {
        Serial.printf("[lib] open failed: %s\n", g_lib[g_lib_sel].path);
        return false;
    }
    uint32_t page = 0, oldSize = 0;
    load_book_progress(g_lib[g_lib_sel].path, page, oldSize);
    uint32_t nowSize = book_file_size(g_lib[g_lib_sel].path);
    if ((oldSize != 0 && oldSize != nowSize) || page >= g_book.pageCount()) page = 0;
    g_page = page;
    g_since_full = 0;
    g_home = false;
    if (!show_page(true)) {
        g_book.close();
        g_home = true;
        return false;
    }
    return true;
}

static bool resume_progress() {
    Preferences p;
    if (!p.begin("reader", true)) return false;
    String path = p.getString("path", "");
    uint32_t page = p.getUInt("page", 0);
    uint32_t size = p.getUInt("size", 0);
    p.end();

    if (!path.startsWith("/books/") || !path.endsWith(".epg")) return false;

    // 新格式：每本书独立 page/size；旧 NVS 的全局 page/size 兜底
    uint32_t bp = 0, bs = 0;
    load_book_progress(path.c_str(), bp, bs);
    if (bs != 0 || bp != 0) {
        page = bp;
        size = bs;
    }
    uint32_t nowSize = book_file_size(path.c_str());
    if (nowSize == 0) return false;
    if (size != 0 && size != nowSize) {
        Serial.println("[resume] book changed, start from page 1");
        clear_book_progress(path.c_str());
        return false;
    }
    if (!g_book.open(LittleFS, path.c_str())) return false;
    if (page >= g_book.pageCount()) page = 0;
    g_page = page;
    g_since_full = 0;
    g_home = false;
    Serial.printf("[resume] %s page %lu/%lu\n", g_book.title(),
                  (unsigned long)(g_page + 1),
                  (unsigned long)g_book.pageCount());
    return true;
}

static const char *MENU_ITEMS[] = {
    "阅读设置", "跳转页面", "全刷", "返回书库",
};
static const uint8_t MENU_COUNT = 4;

static void menu_open() {
    g_menu = true;
    g_menu_sel = 0;
    g_settings = false;
    g_goto = false;
    Serial.println("[ui] menu");
    menu_draw(true);
}

static void menu_draw(bool full) {
    epd.clear(EPD_WHITE);
    epd.draw_rect(0, 0, EPD_WIDTH, EPD_HEIGHT, EPD_BLACK);
    epd.draw_cjk_text(4, 2, "菜单", EPD_BLACK, 15);

    char page[40];
    snprintf(page, sizeof(page), "第 %lu/%lu 页",
             (unsigned long)(g_page + 1),
             (unsigned long)g_book.pageCount());
    epd.draw_cjk_text(EPD_WIDTH - epd.cjk_text_width(page, 12) - 4, 4,
                      page, EPD_BLACK, 12);
    epd.draw_hline(0, 18, EPD_WIDTH, EPD_BLACK);

    for (uint8_t i = 0; i < MENU_COUNT; i++) {
        int16_t y = 20 + i * 24;
        bool sel = (i == g_menu_sel);
        if (sel) epd.fill_rect(0, y + 2, EPD_WIDTH, 20, EPD_BLACK);
        epd.draw_cjk_text(6, y + 4, MENU_ITEMS[i],
                         sel ? EPD_WHITE : EPD_BLACK, 15);
    }

    epd.draw_hline(0, 114, EPD_WIDTH, EPD_BLACK);
    epd.draw_cjk_text(4, 116, "确定=选择  电源=返回", EPD_BLACK, 12);
    epd.draw_rect(0, 0, EPD_WIDTH, EPD_HEIGHT, EPD_BLACK);

    if (full) epd.display_full();
    else epd.display_partial_margin(0, 0, EPD_WIDTH, EPD_HEIGHT, 4);
}

static uint32_t goto_value() {
    uint32_t v = 0;
    for (uint8_t i = 0; i < g_goto_len; i++) v = v * 10 + g_goto_digits[i];
    return v;
}

static void goto_draw(bool full) {
    epd.clear(EPD_WHITE);
    epd.draw_rect(0, 0, EPD_WIDTH, EPD_HEIGHT, EPD_BLACK);
    epd.draw_cjk_text(4, 2, "跳转页面", EPD_BLACK, 15);

    char total[40];
    snprintf(total, sizeof(total), "共 %lu 页",
             (unsigned long)g_book.pageCount());
    epd.draw_cjk_text(EPD_WIDTH - epd.cjk_text_width(total, 12) - 4, 4,
                      total, EPD_BLACK, 12);
    epd.draw_hline(0, 18, EPD_WIDTH, EPD_BLACK);

    const int16_t cell = 24;
    int16_t x0 = (EPD_WIDTH - g_goto_len * cell + 4) / 2;
    for (uint8_t i = 0; i < g_goto_len; i++) {
        int16_t x = x0 + i * cell;
        bool active = (i == g_goto_pos);
        if (active) {
            epd.fill_rect(x - 2, 27, 24, 34, EPD_BLACK);
        }
        epd.draw_char(x, 30, (char)('0' + g_goto_digits[i]),
                      active ? EPD_WHITE : EPD_BLACK, 4);
    }

    char cur[40];
    snprintf(cur, sizeof(cur), "当前 %lu / %lu",
             (unsigned long)(g_page + 1),
             (unsigned long)g_book.pageCount());
    epd.draw_cjk_text(4, 64, cur, EPD_BLACK, 12);
    epd.draw_cjk_text(4, 80, "上下=改数字  确定=换位", EPD_BLACK, 12);
    epd.draw_cjk_text(4, 96, "长按确定=跳转", EPD_BLACK, 12);

    epd.draw_hline(0, 114, EPD_WIDTH, EPD_BLACK);
    epd.draw_cjk_text(4, 116, "电源=取消", EPD_BLACK, 12);
    epd.draw_rect(0, 0, EPD_WIDTH, EPD_HEIGHT, EPD_BLACK);

    if (full) epd.display_full();
    else epd.display_partial_margin(0, 0, EPD_WIDTH, EPD_HEIGHT, 4);
}

static void goto_init() {
    g_goto = true;
    g_goto_len = digits_for(g_book.pageCount());
    if (g_goto_len > 6) g_goto_len = 6;
    uint32_t target = g_page + 1;
    if (target > g_book.pageCount()) target = g_book.pageCount();
    for (int i = (int)g_goto_len - 1; i >= 0; i--) {
        g_goto_digits[i] = target % 10;
        target /= 10;
    }
    g_goto_pos = g_goto_len - 1;
    Serial.printf("[ui] goto len=%u target=%lu\n", g_goto_len,
                  (unsigned long)(g_page + 1));
}

static void settings_draw(bool full) {
    epd.clear(EPD_WHITE);
    epd.draw_rect(0, 0, EPD_WIDTH, EPD_HEIGHT, EPD_BLACK);
    epd.draw_cjk_text(4, 2, "阅读设置", EPD_BLACK, 15);
    epd.draw_hline(0, 18, EPD_WIDTH, EPD_BLACK);

    const char *labels[3] = {"刷新间隔", "休眠时间", "返回"};
    char value0[24], value1[24];
    snprintf(value0, sizeof(value0), "%u 页", g_refresh_every);
    uint32_t mins = g_sleep_after_ms / 60000UL;
    if (mins == 0) snprintf(value1, sizeof(value1), "关闭");
    else snprintf(value1, sizeof(value1), "%lu 分钟", (unsigned long)mins);

    for (uint8_t i = 0; i < 3; i++) {
        int16_t y = 20 + i * 24;
        bool sel = (i == g_settings_sel);
        if (sel) epd.fill_rect(0, y + 2, EPD_WIDTH, 20, EPD_BLACK);
        epd.draw_cjk_text(6, y + 4, labels[i],
                         sel ? EPD_WHITE : EPD_BLACK, 15);
        if (i == 0 || i == 1) {
            const char *v = (i == 0) ? value0 : value1;
            epd.draw_cjk_text(EPD_WIDTH - epd.cjk_text_width(v, 12) - 6,
                              y + 6, v, sel ? EPD_WHITE : EPD_BLACK, 12);
        }
    }

    epd.draw_hline(0, 114, EPD_WIDTH, EPD_BLACK);
    epd.draw_cjk_text(4, 116, "确定=修改  电源=返回", EPD_BLACK, 12);
    epd.draw_rect(0, 0, EPD_WIDTH, EPD_HEIGHT, EPD_BLACK);

    if (full) epd.display_full();
    else epd.display_partial_margin(0, 0, EPD_WIDTH, EPD_HEIGHT, 4);
}

static void handle_goto_key(const KeyEvent &e) {
    if (e.type == KEY_EV_DOWN || e.type == KEY_EV_UP) return;
    if (e.id == KEY_POWER && e.type == KEY_EV_LONG) {
        go_sleep();
        return;
    }
    if (e.id == KEY_POWER && e.type == KEY_EV_SHORT) {
        g_goto = false;
        g_menu = true;
        g_menu_sel = 1;
        menu_draw(false);
        return;
    }
    if (e.type == KEY_EV_REPEAT) return;

    if (e.type == KEY_EV_LONG && e.id == KEY_OK) {
        uint32_t target = goto_value();
        if (target < 1) target = 1;
        if (target > g_book.pageCount()) target = g_book.pageCount();
        Serial.printf("[ui] jump to %lu\n", (unsigned long)target);
        g_page = target - 1;
        g_goto = false;
        g_menu = false;
        show_page(true);
        return;
    }
    if (e.type != KEY_EV_SHORT) return;

    if (e.id == KEY_PREV || e.id == KEY_NEXT) {
        uint8_t &d = g_goto_digits[g_goto_pos];
        d = (e.id == KEY_PREV) ? (uint8_t)((d + 9) % 10)
                               : (uint8_t)((d + 1) % 10);
        goto_draw(false);
    } else if (e.id == KEY_OK) {
        g_goto_pos = (g_goto_pos + g_goto_len - 1) % g_goto_len;
        goto_draw(false);
    }
}

static void handle_menu_key(const KeyEvent &e) {
    if (e.type == KEY_EV_DOWN || e.type == KEY_EV_UP) return;
    if (e.id == KEY_POWER && e.type == KEY_EV_LONG) {
        go_sleep();
        return;
    }
    if (e.id == KEY_POWER && e.type == KEY_EV_SHORT) {
        g_menu = false;
        show_page(true);
        return;
    }
    if (e.type == KEY_EV_REPEAT) return;
    if (e.type != KEY_EV_SHORT) return;

    if (e.id == KEY_PREV) {
        g_menu_sel = (g_menu_sel + MENU_COUNT - 1) % MENU_COUNT;
        menu_draw(false);
    } else if (e.id == KEY_NEXT) {
        g_menu_sel = (g_menu_sel + 1) % MENU_COUNT;
        menu_draw(false);
    } else if (e.id == KEY_OK) {
        if (g_menu_sel == 0) {
            g_menu = false;
            g_settings = true;
            g_settings_sel = 0;
            settings_draw(false);
        } else if (g_menu_sel == 1) {
            g_menu = false;
            goto_init();
            goto_draw(false);
        } else if (g_menu_sel == 2) {
            g_menu = false;
            show_page(true);
        } else {
            g_menu = false;
            g_home = true;
            library_scan();
            home(true);
        }
    }
}

static void handle_settings_key(const KeyEvent &e) {
    if (e.type == KEY_EV_DOWN || e.type == KEY_EV_UP) return;
    if (e.id == KEY_POWER && e.type == KEY_EV_LONG) {
        go_sleep();
        return;
    }
    if (e.id == KEY_POWER && e.type == KEY_EV_SHORT) {
        g_settings = false;
        g_menu = true;
        g_menu_sel = 0;
        menu_draw(false);
        return;
    }
    if (e.type == KEY_EV_REPEAT) return;
    if (e.type != KEY_EV_SHORT) return;

    if (e.id == KEY_PREV) {
        g_settings_sel = (g_settings_sel + 2) % 3;
        settings_draw(false);
    } else if (e.id == KEY_NEXT) {
        g_settings_sel = (g_settings_sel + 1) % 3;
        settings_draw(false);
    } else if (e.id == KEY_OK) {
        if (g_settings_sel == 0) {
            g_refresh_every = (g_refresh_every == 10) ? 20 :
                              (g_refresh_every == 20) ? 30 : 10;
            settings_save();
            settings_draw(false);
        } else if (g_settings_sel == 1) {
            uint32_t m = g_sleep_after_ms / 60000UL;
            m = (m == 0) ? 1 : (m == 1) ? 5 : (m == 5) ? 10 : 0;
            g_sleep_after_ms = m * 60000UL;
            settings_save();
            settings_draw(false);
        } else {
            g_settings = false;
            g_menu = true;
            g_menu_sel = 0;
            menu_draw(false);
        }
    }
}


static void handle_reader_key(const KeyEvent &e) {
    if (e.type == KEY_EV_DOWN || e.type == KEY_EV_UP) return;
    if (e.id == KEY_POWER && e.type == KEY_EV_LONG) {
        go_sleep();
        return;
    }
    if (e.type == KEY_EV_REPEAT) return;

    uint32_t step = (e.type == KEY_EV_LONG) ? 10 : 1;
    uint32_t old = g_page;

    if (e.id == KEY_PREV) {
        g_page = (g_page >= step) ? (g_page - step) : 0;
        if (g_page != old) show_page(false);
    } else if (e.id == KEY_NEXT) {
        uint32_t last = g_book.pageCount() - 1;
        g_page = (g_page + step <= last) ? (g_page + step) : last;
        if (g_page != old) show_page(false);
    } else if (e.id == KEY_OK && e.type == KEY_EV_SHORT) {
        menu_open();
    }
}

#if EPD_HAS_KEYS
// ---------------- 按键测试界面 ----------------
// 第一次按键全刷一页 KEY TEST，之后局刷计数；串口同时打印事件。
static uint32_t s_key_hits[KEY_COUNT];
static bool     s_key_screen = false;
static char     s_key_last[32] = "PRESS A KEY";

static void keys_busy_hook() {
    // 屏忙时只扫描/入队，不在这里刷屏
    keys_scan(millis());
}

static void key_test_draw(bool full) {
    const char *names[KEY_COUNT] = {"PREV", "NEXT", "OK", "POWER"};

    epd.clear(EPD_WHITE);
    epd.draw_rect(0, 0, EPD_WIDTH, EPD_HEIGHT, EPD_BLACK);

#if EPD_HEIGHT >= 200
    epd.draw_text(14, 10, "KEY TEST", EPD_BLACK, 4);
    epd.draw_hline(14, 55, EPD_WIDTH - 28, EPD_BLACK);
    for (int i = 0; i < KEY_COUNT; i++) {
        char b[32];
        snprintf(b, sizeof(b), "%s   %lu", names[i],
                 (unsigned long)s_key_hits[i]);
        epd.draw_text(20, 75 + i * 45, b, EPD_BLACK, 3);
    }
    epd.draw_text(20, EPD_HEIGHT - 28, s_key_last, EPD_BLACK, 2);
#else
    epd.draw_text(8, 5, "KEY TEST", EPD_BLACK, 2);
    epd.draw_hline(8, 27, EPD_WIDTH - 16, EPD_BLACK);
    for (int i = 0; i < KEY_COUNT; i++) {
        char b[28];
        snprintf(b, sizeof(b), "%s %lu", names[i],
                 (unsigned long)s_key_hits[i]);
        epd.draw_text(10, 34 + i * 20, b, EPD_BLACK, 2);
    }
    epd.draw_text(8, 116, s_key_last, EPD_BLACK, 1);
#endif

    if (full) {
        epd.display_full();
    } else {
        epd.display_partial_margin(0, 0, EPD_WIDTH, EPD_HEIGHT, 4);
    }
}

static void handle_key(const KeyEvent &e) {
    Serial.printf("[key] %s %s %ums\n",
                  key_name(e.id), key_event_name(e.type), e.heldMs);
    last_activity = millis();

    // 只对短按/长按/重复刷新界面；down/up 只打日志，避免刷两次
    if (e.type == KEY_EV_DOWN || e.type == KEY_EV_UP) return;

    // M5：菜单/设置/跳页优先处理
    if (g_goto) {
        handle_goto_key(e);
        return;
    }
    if (g_settings) {
        handle_settings_key(e);
        return;
    }
    if (g_menu) {
        handle_menu_key(e);
        return;
    }

    // 书库：PREV/NEXT 选书，OK 打开，POWER 长按休眠
    if (g_home) {
        if (e.id == KEY_PREV && e.type == KEY_EV_SHORT) {
            library_move(-1);
        } else if (e.id == KEY_NEXT && e.type == KEY_EV_SHORT) {
            library_move(1);
        } else if (e.id == KEY_OK && e.type == KEY_EV_SHORT) {
            library_open_selected();
        } else if (e.id == KEY_POWER && e.type == KEY_EV_LONG) {
            go_sleep();
        }
        return;
    }

    // 阅读页：POWER 短按回书库
    if (g_book.isOpen()) {
        if (e.id == KEY_POWER && e.type == KEY_EV_SHORT) {
            g_menu = g_settings = g_goto = false;
            g_home = true;
            library_scan();
            home(true);
            return;
        }
        handle_reader_key(e);
        return;
    }

    // 兜底：没有打开的书，仍按书库处理
    if (e.id == KEY_OK && e.type == KEY_EV_SHORT) {
        library_scan();
        if (!library_open_selected()) home(true);
        return;
    }
    if (e.id == KEY_POWER && e.type == KEY_EV_LONG) {
        go_sleep();
        return;
    }
    if (e.id == KEY_OK && e.type == KEY_EV_LONG) {
        s_key_screen = true;
        key_test_draw(true);
        return;
    }
}
#endif  // EPD_HAS_KEYS

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
    Serial.setRxBufferSize(8192);
    Serial.begin(115200);
    Serial.setTimeout(5000);   // 收 4736 字节时别用默认 1 秒，USB-CDC 容易超时
    // 原生 USB CDC：等枚举完成，最多 3 秒，否则前几行 banner 会丢
    {
        uint32_t t0 = millis();
        while (!Serial && millis() - t0 < 3000) delay(10);
    }
    delay(200);
    bool woke_by_key = false;
#if BOARD_SEL == BOARD_S3_DEVKITC1
    woke_by_key = (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1);
#endif
    boot_count++;
    Serial.printf("\n=== Ink firmware (%s / %s) boot #%lu ===\n",
                  EPD_NAME, EPD_CTRL, (unsigned long)boot_count);
    Serial.printf("framebuffer %d bytes, free heap %u bytes\n",
                  (int)EPD_BYTES, (unsigned)ESP.getFreeHeap());

#if OT_ENABLE
    official_test();
    while (true) delay(1000);   // 测试模式：不再跑正常流程
#endif

#if EPD_HAS_KEYS
    keys_begin();           // 4 键：GPIO4/5/6/15，内部上拉，按下为低
    if (woke_by_key) {
        // 唤醒时按键还按着：先等松手，避免它又被当成一次长按
        Serial.println("[keys] wake by key, wait release...");
        keys_wait_all_released(5000);
        keys_flush();
    }
    epd.set_busy_hook(keys_busy_hook);   // 全刷 2 秒期间也扫键，不丢按键
#endif
    epd.begin();            // 初始化 SPI + 屏（跳过它 SPI 都没初始化）
    keys_dump_pins();       // 开机打印一次原始电平，便于确认接线

    settings_load();
    g_menu = g_settings = g_goto = false;
    library_scan();
    bool resumed = resume_progress();

    // 有上次进度就直接回阅读页；否则进书库。自检页只在 SHOW_SPLASH=1 时画。
    {
        uint32_t t0 = millis();
#if SHOW_SPLASH
        splash();
#endif
        if (resumed && show_page(true)) {
            g_home = false;
        } else {
            if (g_book.isOpen()) g_book.close();
            g_page = 0;
            g_since_full = 0;
            g_home = true;
            home(true);
        }
        g_splash_ms = millis() - t0;
    }
    last_activity = millis();
    if (g_book.isOpen()) {
        Serial.printf("READY. book: %s  page %lu/%lu  |  keys: prev/next/ok/power\n",
                      g_book.title(), (unsigned long)(g_page + 1),
                      (unsigned long)g_book.pageCount());
    } else {
        Serial.printf("READY. library: %u book(s)  |  keys: prev/next/ok/power\n",
                      g_lib_count);
    }
    Serial.println("        image: python tools/push.py --help");
}

void loop() {
#if EPD_HAS_KEYS
    // 先扫按键：事件只入队，这里统一处理；刷新期间也不会把状态机弄乱
    keys_scan(millis());
    KeyEvent ke;
    while (keys_pop(ke)) handle_key(ke);
#endif

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
        } else if (c == 'I') {
            Serial.read();
            keys_dump_pins();
            if (g_book.isOpen()) {
                Serial.printf("book=%s page=%lu/%lu\n", g_book.title(),
                              (unsigned long)(g_page + 1),
                              (unsigned long)g_book.pageCount());
            } else {
                Serial.println("book=none");
            }
            Serial.printf("uptime=%lu s\n", (unsigned long)(millis() / 1000));
            last_activity = millis();
        } else if (c == 'M' || c == 'm') {
            Serial.read();
            print_mem();
            last_activity = millis();
        } else if (c == 'L' || c == 'l') {
            Serial.read();
            list_dir("/");
            list_dir("/books");
            last_activity = millis();
        } else if (c == 'U') {
            Serial.read();                    // 吃掉 'U'
            String a = Serial.readStringUntil('\n');
            a.trim();
            upload_file(a);
            last_activity = millis();
        } else if (c == 'D' || c == 'd') {
            Serial.read();                    // 吃掉 'D'
            String a = Serial.readStringUntil('\n');
            a.trim();
            delete_file(a);
            last_activity = millis();
#if EPD_HAS_KEYS
        } else if (c == 'K') {
            // 调试/HIL：K|prev / K|next / K|ok / K|power，模拟一次短按
            Serial.read();                    // 吃掉 'K'
            String s = Serial.readStringUntil('\n');
            s.trim();
            if (s.startsWith("|")) s.remove(0, 1);
            KeyEvent ev;
            ev.type = KEY_EV_SHORT;
            ev.heldMs = 100;
            bool ok = true;
            if (s == "prev") ev.id = KEY_PREV;
            else if (s == "next") ev.id = KEY_NEXT;
            else if (s == "ok") ev.id = KEY_OK;
            else if (s == "oklong") {
                ev.id = KEY_OK;
                ev.type = KEY_EV_LONG;
                ev.heldMs = 900;
            }
            else if (s == "power") ev.id = KEY_POWER;
            else ok = false;
            if (ok) {
                handle_key(ev);
                Serial.println("OK key");
            } else {
                Serial.println("ERR key");
            }
            last_activity = millis();
#endif
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

    if (g_sleep_after_ms > 0 && millis() - last_activity > g_sleep_after_ms) go_sleep();
}
