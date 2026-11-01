#include "keys.h"
#include "board_config.h"

#if EPD_HAS_KEYS

#if BOARD_SEL == BOARD_S3_DEVKITC1
#include <driver/rtc_io.h>
#endif

static const int8_t s_pins[KEY_COUNT] = {
    PIN_KEY_PREV, PIN_KEY_NEXT, PIN_KEY_OK, PIN_KEY_POWER
};

static const uint32_t DEBOUNCE_MS   = 20;
static const uint32_t LONG_MS       = 800;
static const uint32_t REPEAT_MS     = 500;

struct KeyState {
    bool     raw;          // 最近一次读到的电平（true = 按下）
    bool     stable;       // 消抖后的状态
    uint32_t last_change;  // 原始电平变化的时间
    uint32_t pressed_at;   // 消抖后按下的时间
    bool     long_sent;
    uint32_t repeat_at;
};

static KeyState s_state[KEY_COUNT];

// 事件队列：足够应付一次刷新期间的连按
static const uint8_t Q_SIZE = 16;
static KeyEvent s_queue[Q_SIZE];
static uint8_t s_head = 0;
static uint8_t s_tail = 0;

static void push_event(KeyId id, KeyEventType type, uint32_t held) {
    uint8_t next = (uint8_t)((s_head + 1) % Q_SIZE);
    if (next == s_tail) return;          // 满了就丢新事件，避免覆盖正在处理的事件
    s_queue[s_head].id = id;
    s_queue[s_head].type = type;
    s_queue[s_head].heldMs = (uint16_t)(held > 65535 ? 65535 : held);
    s_head = next;
}

void keys_begin() {
    uint32_t now = millis();
    for (uint8_t i = 0; i < KEY_COUNT; i++) {
        KeyState &s = s_state[i];
        s.raw = false;
        s.stable = false;
        s.last_change = now;
        s.pressed_at = now;
        s.long_sent = false;
        s.repeat_at = now;
        if (s_pins[i] >= 0) {
            pinMode((uint8_t)s_pins[i], INPUT_PULLUP);
            s.raw = (digitalRead((uint8_t)s_pins[i]) == LOW);
        }
    }
    s_head = s_tail = 0;
}

void keys_scan(uint32_t now) {
    for (uint8_t i = 0; i < KEY_COUNT; i++) {
        if (s_pins[i] < 0) continue;
        KeyState &s = s_state[i];

        bool raw = (digitalRead((uint8_t)s_pins[i]) == LOW);
        if (raw != s.raw) {
            s.raw = raw;
            s.last_change = now;
        }

        if ((uint32_t)(now - s.last_change) >= DEBOUNCE_MS && s.stable != s.raw) {
            s.stable = s.raw;
            if (s.stable) {
                s.pressed_at = now;
                s.long_sent = false;
                s.repeat_at = now + REPEAT_MS;
                push_event((KeyId)i, KEY_EV_DOWN, 0);
            } else {
                uint32_t held = now - s.pressed_at;
                push_event((KeyId)i, KEY_EV_UP, held);
                if (!s.long_sent) {
                    push_event((KeyId)i, KEY_EV_SHORT, held);
                }
            }
        }

        if (s.stable) {
            uint32_t held = now - s.pressed_at;
            if (!s.long_sent && held >= LONG_MS) {
                s.long_sent = true;
                s.repeat_at = now + REPEAT_MS;
                push_event((KeyId)i, KEY_EV_LONG, held);
            } else if (s.long_sent &&
                       (int32_t)(now - s.repeat_at) >= 0) {
                s.repeat_at += REPEAT_MS;
                push_event((KeyId)i, KEY_EV_REPEAT, held);
            }
        }
    }
}

bool keys_pop(KeyEvent &out) {
    if (s_tail == s_head) return false;
    out = s_queue[s_tail];
    s_tail = (uint8_t)((s_tail + 1) % Q_SIZE);
    return true;
}

void keys_dump_pins() {
    Serial.print("[keys] pins:");
    for (uint8_t i = 0; i < KEY_COUNT; i++) {
        if (s_pins[i] < 0) {
            Serial.printf(" %s=--", key_name((KeyId)i));
        } else {
            Serial.printf(" %s=%d", key_name((KeyId)i),
                          (int)digitalRead((uint8_t)s_pins[i]));
        }
    }
    Serial.println("  (1=released, 0=pressed)");
}

uint64_t keys_wake_mask() {
#if BOARD_SEL == BOARD_S3_DEVKITC1
    uint64_t mask = 0;
    for (uint8_t i = 0; i < KEY_COUNT; i++) {
        if (s_pins[i] >= 0) mask |= (1ULL << (uint8_t)s_pins[i]);
    }
    return mask;
#else
    return 0;   // C3 构建不启用 ext1 唤醒，走定时唤醒兜底
#endif
}

void keys_prepare_sleep() {
#if BOARD_SEL == BOARD_S3_DEVKITC1
    // 深睡时数字域掉电，必须让 RTC 域继续给按键上拉，否则 ext1 会浮空误唤醒
    for (uint8_t i = 0; i < KEY_COUNT; i++) {
        if (s_pins[i] < 0) continue;
        rtc_gpio_pullup_en((gpio_num_t)s_pins[i]);
        rtc_gpio_pulldown_dis((gpio_num_t)s_pins[i]);
    }
#endif
}

bool keys_wait_all_released(uint32_t timeoutMs) {
    uint32_t t0 = millis();
    while ((uint32_t)(millis() - t0) < timeoutMs) {
        bool any = false;
        for (uint8_t i = 0; i < KEY_COUNT; i++) {
            if (s_pins[i] < 0) continue;
            if (digitalRead((uint8_t)s_pins[i]) == LOW) {
                any = true;
                break;
            }
        }
        if (!any) return true;
        delay(5);
    }
    return false;
}

void keys_flush() {
    s_head = s_tail = 0;
}

const char *key_name(KeyId id) {
    switch (id) {
        case KEY_PREV:  return "prev";
        case KEY_NEXT:  return "next";
        case KEY_OK:    return "ok";
        case KEY_POWER: return "power";
        default:        return "?";
    }
}

const char *key_event_name(KeyEventType type) {
    switch (type) {
        case KEY_EV_DOWN:   return "down";
        case KEY_EV_UP:     return "up";
        case KEY_EV_SHORT:  return "short";
        case KEY_EV_LONG:   return "long";
        case KEY_EV_REPEAT: return "repeat";
        default:            return "?";
    }
}

#else   // EPD_HAS_KEYS == 0

void keys_begin() {}
void keys_scan(uint32_t) {}
bool keys_pop(KeyEvent &) { return false; }
void keys_dump_pins() {}
uint64_t keys_wake_mask() { return 0; }
void keys_prepare_sleep() {}
bool keys_wait_all_released(uint32_t) { return true; }
void keys_flush() {}
const char *key_name(KeyId) { return "?"; }
const char *key_event_name(KeyEventType) { return "?"; }

#endif
