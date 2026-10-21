/* USB SysEx 上传模块镜像 → 写 QSPI → 重新加载（协议见 module_upload.h） */
#include "module_upload.h"

#include <string.h>

#include "module.h"
#include "qspi_flash.h"

#define UP_ID     0x7Du
#define UP_NO_RUN 0x100

enum {
    UP_IDLE = 0,
    UP_WAIT_ID,   /* 刚收到 F0，等 0x7D */
    UP_CMD,       /* 等命令字 */
    UP_LEN,       /* 收 8 个十六进制字符（总长） */
    UP_OFF,       /* 收 6 个十六进制字符（块偏移） */
    UP_DATA,      /* 收数据十六进制字符，直到 F7 */
    UP_SKIP       /* 别的 SysEx：吞到 F7 */
};

static uint8_t  s_state;
static uint8_t  s_cmd;
static uint32_t s_acc;          /* 十六进制累加 */
static uint8_t  s_hex_n;        /* 已收的十六进制字符数 */
static uint32_t s_nibble;       /* 半个字节的暂存 */
static uint32_t s_total;        /* 本次上传总长 */
static uint32_t s_off;          /* 当前数据块的写偏移（镜像内） */
static int      s_result = UP_NO_RUN;
static int      s_err;          /* 首次失败码（后面继续收字节但不再写） */

/* 页暂存：QSPI 页编程必须页对齐，所以按页攒 */
static uint8_t  s_page[SYNTH_QSPI_PAGE_BYTES];
static uint32_t s_page_base;    /* 镜像内 256 对齐偏移 */
static uint32_t s_page_used;

static void up_flush(void) {
    if (s_page_used == 0u) return;
    if (s_err == 0) {
        if (synth_port_qspi_write_enable() < 0 ||
            synth_port_qspi_page_program(SYNTH_QSPI_MODULE_OFFSET + s_page_base, s_page, s_page_used) < 0) {
            s_err = -3;
        }
    }
    s_page_used = 0u;
}

static void up_reset_page(uint32_t off) {
    if ((off & ~(uint32_t)(SYNTH_QSPI_PAGE_BYTES - 1u)) != s_page_base || s_page_used == 0u) {
        up_flush();
        s_page_base = off & ~(uint32_t)(SYNTH_QSPI_PAGE_BYTES - 1u);
        s_page_used = 0u;
    }
}

static void up_put(uint8_t v) {
    if (s_off >= s_total) { s_err = -4; return; }
    up_reset_page(s_off);
    const uint32_t in_page = s_off - s_page_base;
    s_page[in_page] = v;
    if (in_page + 1u > s_page_used) s_page_used = in_page + 1u;
    s_off++;
    if (s_page_used == SYNTH_QSPI_PAGE_BYTES) up_flush();
}

/* 增量 CRC32（多项式与 module.c / make_module.py 一致） */
static uint32_t s_crc;
static void crc_feed(const uint8_t *p, uint32_t n) {
    uint32_t c = s_crc;
    while (n-- > 0u) {
        c ^= *p++;
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    s_crc = c;
}

/* 缓存失效 + 回到内存映射，然后校验并重新加载模块。
   校验**走间接读**（synth_port_qspi_read）：QSPI 映射区是可缓存的，用映射指针校验会
   读到 CPU 缓存里的旧行（症状：flash 内容明明已经正确，commit 却老是失败）。 */
static int up_commit(void) {
    up_flush();
    if (s_err != 0) return s_err;
    if (s_total < sizeof(synth_module_hdr_t)) return -2;

    synth_module_hdr_t h;
    if (synth_port_qspi_read(SYNTH_QSPI_MODULE_OFFSET, &h, (uint32_t)sizeof(h)) != 0) return -2;
    if (h.magic != SYNTH_MODULE_MAGIC || h.version != SYNTH_MODULE_VERSION ||
        h.abi != SYNTH_MODULE_ABI || h.hdr_bytes != (uint16_t)sizeof(synth_module_hdr_t) ||
        h.image_bytes == 0u || (h.image_bytes & 3u) != 0u ||
        (uint32_t)h.hdr_bytes + h.image_bytes > SYNTH_QSPI_MODULE_MAX ||
        (uint32_t)h.hdr_bytes + h.image_bytes > s_total) {
        return -2;
    }

    uint32_t off  = SYNTH_QSPI_MODULE_OFFSET + (uint32_t)sizeof(h);
    uint32_t left = h.image_bytes;
    s_crc = 0xFFFFFFFFu;
    while (left > 0u) {
        uint32_t n = (left > SYNTH_QSPI_PAGE_BYTES) ? SYNTH_QSPI_PAGE_BYTES : left;
        if (synth_port_qspi_read(off, s_page, n) != 0) return -3;
        crc_feed(s_page, n);
        off  += n;
        left -= n;
    }
    if ((s_crc ^ 0xFFFFFFFFu) != h.crc32) return -3;

    synth_port_qspi_cache_invalidate();
    if (synth_port_qspi_ensure_mmap() != 0) return -7;

    /* 关键：synth_module_init() 是幂等的（已加载就直接返回 0），所以必须先卸载，
       否则 USB 上传后跑的还是上一次拷进 RAM 的旧镜像（症状：module_crc 不变、屏不刷新）。 */
    synth_module_unload();
    return synth_module_init();     /* 0 = 新模块已经跑起来了 */
}

static void up_begin_erase(uint32_t total) {
    if (total == 0u || total > SYNTH_QSPI_MODULE_MAX) { s_err = -5; return; }
    s_total = total;
    s_page_base = 0u;
    s_page_used = 0u;
    s_off = 0u;
    const uint32_t sectors = (total + SYNTH_QSPI_SECTOR_BYTES - 1u) / SYNTH_QSPI_SECTOR_BYTES;
    for (uint32_t i = 0; i < sectors; ++i) {
        if (synth_port_qspi_erase_sector(SYNTH_QSPI_MODULE_OFFSET + i * SYNTH_QSPI_SECTOR_BYTES) < 0) {
            s_err = -3;
            break;
        }
    }
}

int synth_port_module_upload_byte(uint8_t b) {
    /* 系统实时字节（F8/FA/FC/FE…）可以在任何时刻插进来，直接吞掉 */
    if (b >= 0xF8u) return 1;
    if (b == 0xF7u) {
        switch (s_state) {
        case UP_DATA:
            up_flush();
            s_state = UP_IDLE;
            return 1;
        case UP_IDLE:
            return 0;
        default:
            s_state = UP_IDLE;      /* 半截消息：结束但不算提交 */
            return 1;
        }
    }
    if (b == 0xF0u) { s_state = UP_WAIT_ID; return 1; }

    switch (s_state) {
    case UP_IDLE:
        return 0;                    /* 普通 MIDI，照常送给引擎 */
    case UP_WAIT_ID:
        s_state = (b == UP_ID) ? UP_CMD : UP_SKIP;
        return 1;
    case UP_SKIP:
        return 1;
    case UP_CMD:
        s_cmd = b;
        s_acc = 0u;
        s_hex_n = 0u;
        s_nibble = 0u;
        if (b == 0x00u)      { s_state = UP_LEN; }
        else if (b == 0x01u) { s_state = UP_OFF; }
        else if (b == 0x02u) { s_result = up_commit(); s_state = UP_IDLE; }
        else                 { s_state = UP_SKIP; }
        return 1;
    case UP_LEN: {
        uint8_t v;
        if (b >= '0' && b <= '9')      v = (uint8_t)(b - '0');
        else if (b >= 'A' && b <= 'F') v = (uint8_t)(b - 'A' + 10u);
        else if (b >= 'a' && b <= 'f') v = (uint8_t)(b - 'a' + 10u);
        else { s_state = UP_SKIP; return 1; }
        s_acc = (s_acc << 4) | v;
        if (++s_hex_n == 8u) {
            s_err = 0;
            up_begin_erase(s_acc);
            s_state = UP_SKIP;       /* 等 F7 */
        }
        return 1;
    }
    case UP_OFF: {
        uint8_t v;
        if (b >= '0' && b <= '9')      v = (uint8_t)(b - '0');
        else if (b >= 'A' && b <= 'F') v = (uint8_t)(b - 'A' + 10u);
        else if (b >= 'a' && b <= 'f') v = (uint8_t)(b - 'a' + 10u);
        else { s_state = UP_SKIP; return 1; }
        s_acc = (s_acc << 4) | v;
        if (++s_hex_n == 6u) {
            s_off = s_acc;
            s_hex_n = 0u;        /* 关键：偏移收完必须把"半个字节"状态清零，
                                    否则数据阶段会按 1 个字符 = 1 字节错位配对
                                    （症状：写进 flash 的都是错位垃圾，斜坡测试
                                     00010203… 变成 00 10 20 30…） */
            s_nibble = 0u;
            s_state = UP_DATA;
        }
        return 1;
    }
    case UP_DATA: {
        uint8_t v;
        if (b >= '0' && b <= '9')      v = (uint8_t)(b - '0');
        else if (b >= 'A' && b <= 'F') v = (uint8_t)(b - 'A' + 10u);
        else if (b >= 'a' && b <= 'f') v = (uint8_t)(b - 'a' + 10u);
        else { s_state = UP_SKIP; return 1; }
        if (s_hex_n == 0u) { s_nibble = v; s_hex_n = 1u; }
        else { up_put((uint8_t)((s_nibble << 4) | v)); s_hex_n = 0u; }
        return 1;
    }
    default:
        s_state = UP_SKIP;
        return 1;
    }
}

int synth_port_module_upload_result(void) { return s_result; }

uint32_t synth_port_module_upload_state(void) {
    const uint32_t st  = (uint32_t)(s_state & 0xFFu);
    const uint32_t er  = (uint32_t)(uint8_t)(int8_t)(s_err > -128 ? s_err : -128);
    const uint32_t res = (uint32_t)(uint8_t)(int8_t)(s_result > -128 ? s_result : -128);
    return st | (er << 8) | (res << 16);
}
