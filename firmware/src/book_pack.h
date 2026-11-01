// book_pack.h —— .epg 页包读取（v1）
//
// 文件结构见 docs/10-架构与接口.md：
//   128B 头 + pageCount×8B 页表 + 页数据（可选 PackBits 压缩）
//
// 只依赖 fs::FS，不绑定 LittleFS/SD；以后加 SD 直接传 SDFS 进来。
#pragma once

#include <Arduino.h>
#include <FS.h>
#include "board_config.h"

class BookPack {
public:
    BookPack();
    ~BookPack();

    bool open(fs::FS &fs, const char *path);
    void close();

    bool isOpen() const { return _open; }
    uint32_t pageCount() const { return _pageCount; }
    const char *title() const { return _title; }
    const char *path() const { return _path; }

    // 把第 index 页解到 out（EPD_BYTES 字节，1=白 0=黑）
    bool readPage(uint32_t index, uint8_t *out);

private:
    bool readHeader();
    bool unpack(const uint8_t *src, uint16_t n, uint8_t *dst, uint32_t dstLen);
    static uint16_t crc16(const uint8_t *p, uint32_t n);

    fs::File _file;
    bool      _open;
    uint16_t  _flags;
    uint32_t  _pageBytes;
    uint32_t  _pageCount;
    uint32_t  _pageTableOffset;
    uint32_t  _dataOffset;
    char      _title[65];
    char      _path[64];
};
