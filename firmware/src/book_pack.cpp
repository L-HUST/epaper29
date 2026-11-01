#include "book_pack.h"

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// 压缩页最多比原始页大一点；留 256B 余量
static uint8_t s_page_buf[EPD_BYTES + 256];

BookPack::BookPack()
    : _open(false), _flags(0), _pageBytes(0), _pageCount(0),
      _pageTableOffset(0), _dataOffset(0) {
    _title[0] = 0;
    _path[0] = 0;
}

BookPack::~BookPack() {
    close();
}

bool BookPack::open(fs::FS &fs, const char *path) {
    close();
    _file = fs.open(path, "r");
    if (!_file) {
        Serial.printf("[book] open failed: %s\n", path);
        return false;
    }
    if (!readHeader()) {
        close();
        return false;
    }
    strncpy(_path, path, sizeof(_path) - 1);
    _path[sizeof(_path) - 1] = 0;
    _open = true;
    Serial.printf("[book] %s  pages=%lu  flags=%u\n",
                  _title, (unsigned long)_pageCount, (unsigned)_flags);
    return true;
}

void BookPack::close() {
    if (_file) _file.close();
    _open = false;
}

bool BookPack::readHeader() {
    uint8_t h[128];
    if (_file.size() < sizeof(h)) return false;
    _file.seek(0);
    if (_file.read(h, sizeof(h)) != sizeof(h)) return false;

    if (memcmp(h, "EPG1", 4) != 0) {
        Serial.println("[book] bad magic");
        return false;
    }
    uint16_t version = rd16(h + 4);
    uint16_t headerSize = rd16(h + 6);
    uint16_t width = rd16(h + 8);
    uint16_t height = rd16(h + 10);
    uint16_t stride = rd16(h + 12);
    _flags = rd16(h + 14);
    _pageBytes = rd32(h + 16);
    _pageCount = rd32(h + 20);
    _pageTableOffset = rd32(h + 24);
    _dataOffset = rd32(h + 28);

    if (version != 1 || headerSize != 128 ||
        width != EPD_WIDTH || height != EPD_HEIGHT || stride != EPD_STRIDE ||
        _pageBytes != EPD_BYTES || _pageCount == 0 || _pageCount > 100000) {
        Serial.printf("[book] bad header v%u %ux%u stride%u page%lu\n",
                      version, width, height, stride, (unsigned long)_pageBytes);
        return false;
    }
    if (_pageTableOffset != 128 ||
        _dataOffset < _pageTableOffset + _pageCount * 8 ||
        _dataOffset > _file.size()) {
        Serial.println("[book] bad table/data offset");
        return false;
    }

    memcpy(_title, h + 40, 64);
    _title[64] = 0;
    // 保证 NUL 结尾，防止恶意/损坏文件把标题带出界
    for (int i = 0; i < 64; i++) {
        if (_title[i] == 0) break;
        if (i == 63) _title[64] = 0;
    }
    if (_title[0] == 0) strcpy(_title, "UNTITLED");
    return true;
}

bool BookPack::readPage(uint32_t index, uint8_t *out) {
    if (!_open || index >= _pageCount || out == nullptr) return false;

    uint8_t e[8];
    _file.seek(_pageTableOffset + index * 8);
    if (_file.read(e, sizeof(e)) != sizeof(e)) return false;

    uint32_t off = rd32(e);
    uint16_t stored = rd16(e + 4);
    uint16_t wantCrc = rd16(e + 6);

    if (stored == 0 || stored > sizeof(s_page_buf) ||
        off < _dataOffset || off + stored > _file.size()) {
        Serial.printf("[book] bad page %lu\n", (unsigned long)index);
        return false;
    }

    _file.seek(off);
    if (_file.read(s_page_buf, stored) != stored) return false;

    if (_flags & 1) {
        if (!unpack(s_page_buf, stored, out, EPD_BYTES)) {
            Serial.printf("[book] unpack failed page %lu (off=%lu stored=%u)\n",
                          (unsigned long)index, (unsigned long)off,
                          (unsigned)stored);
            return false;
        }
    } else {
        if (stored != EPD_BYTES) return false;
        memcpy(out, s_page_buf, EPD_BYTES);
    }

    if (crc16(out, EPD_BYTES) != wantCrc) {
        Serial.printf("[book] crc failed page %lu\n", (unsigned long)index);
        return false;
    }
    return true;
}

bool BookPack::unpack(const uint8_t *src, uint16_t n,
                      uint8_t *dst, uint32_t dstLen) {
    uint32_t i = 0, o = 0;
    while (i < n && o < dstLen) {
        uint8_t c = src[i++];
        if (c < 128) {
            uint16_t cnt = (uint16_t)c + 1;
            if (i + cnt > n || o + cnt > dstLen) {
                Serial.printf("[unpack] lit overflow i=%lu o=%lu c=%u n=%u dst=%lu\n",
                              (unsigned long)i, (unsigned long)o, (unsigned)c,
                              (unsigned)n, (unsigned long)dstLen);
                return false;
            }
            memcpy(dst + o, src + i, cnt);
            i += cnt;
            o += cnt;
        } else if (c > 128) {
            uint16_t cnt = (uint16_t)(257 - c);
            if (i >= n || o + cnt > dstLen) {
                Serial.printf("[unpack] run overflow i=%lu o=%lu c=%u n=%u dst=%lu\n",
                              (unsigned long)i, (unsigned long)o, (unsigned)c,
                              (unsigned)n, (unsigned long)dstLen);
                return false;
            }
            memset(dst + o, src[i++], cnt);
            o += cnt;
        }
        // c == 128: no-op
    }
    if (o != dstLen) {
        Serial.printf("[unpack] short o=%lu i=%lu n=%u dst=%lu\n",
                      (unsigned long)o, (unsigned long)i, (unsigned)n,
                      (unsigned long)dstLen);
    }
    return o == dstLen;
}

uint16_t BookPack::crc16(const uint8_t *p, uint32_t n) {
    uint16_t crc = 0xFFFF;
    for (uint32_t i = 0; i < n; i++) {
        crc ^= (uint16_t)p[i] << 8;
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                                 : (uint16_t)(crc << 1);
    }
    return crc;
}
