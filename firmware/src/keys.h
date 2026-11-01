// keys.h —— 4 键输入：消抖、短按/长按/重复
//
// 按键一端接 GPIO，另一端接 GND；GPIO 用内部上拉，按下为低。
// 引脚在 board_config.h：PIN_KEY_PREV / NEXT / OK / POWER。
//
// 用法：
//   keys_begin();
//   loop: keys_scan(millis());
//         KeyEvent e; while (keys_pop(e)) { ... }
#pragma once

#include <Arduino.h>

enum KeyId : uint8_t {
    KEY_PREV = 0,
    KEY_NEXT,
    KEY_OK,
    KEY_POWER,
    KEY_COUNT
};

enum KeyEventType : uint8_t {
    KEY_EV_DOWN = 0,
    KEY_EV_UP,
    KEY_EV_SHORT,
    KEY_EV_LONG,
    KEY_EV_REPEAT
};

struct KeyEvent {
    KeyId         id;
    KeyEventType  type;
    uint16_t      heldMs;
};

void keys_begin();
void keys_scan(uint32_t nowMs);
bool keys_pop(KeyEvent &out);
void keys_dump_pins();

// 深睡前后用：唤醒掩码、保持上拉、等待松手、清空测试队列
uint64_t keys_wake_mask();
void     keys_prepare_sleep();
bool     keys_wait_all_released(uint32_t timeoutMs);
void     keys_flush();

const char *key_name(KeyId id);
const char *key_event_name(KeyEventType type);
