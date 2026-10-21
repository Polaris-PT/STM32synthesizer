/* RAM overlay 模块：ST7735 状态屏（128x160，BOE 竖屏）+ PC13 按键翻页。
 *
 * 这个文件**不链接进固件**，而是单独编译成一个裸二进制（ui.bin），放进 SD 卡的
 * /synth/ui.bin，由宿主开机导入 QSPI（0x7F0000）、校验 CRC 后整段拷到 AXI SRAM
 * 的 0x24070000 执行。所以：
 *   - 不能调用宿主固件里的任何函数，只能通过 entry() 拿到的 API 表；
 *   - 没有 C 运行时：不跑 startup、不拷 .data，静态变量的零由宿主拷贝镜像时一并写入；
 *   - 不用 HAL（内部 flash 已 99%），SPI 用 GPIO 位敲，寄存器地址直接写死。
 *
 * 硬件（和参考工程 WM8978_Player 的 1.8" 屏一致）：
 *   PE12 = SCK, PE14 = MOSI（位敲），PE11 = RST, PE13 = DC, PE15 = CS, PE10 = 背光（GPIO 高 = 亮）。
 * 背光先直接拉高（阶段二再做 PWM 调光），优先保证"能看到东西"。
 *
 * 重要设计约束（音频优先）：宿主在主循环里调 tick()，而音频预渲染环只有 ~13 ms 的余量，
 * 所以**每次 tick 最多画一个"小项"**（一行字 ≈ 3 ms），整页分几十次画完。绝不允许
 * 一次刷完整屏（40 KB ≈ 几十毫秒）把音频喂水的节奏打断。
 */
#include "module_api.h"
#include "ui_font.h"

/* ---------------------------------------------------------------- 全局状态 */

/* 宿主 API 表：entry() 里拿到后存这儿，后面所有绘制/查询都走它 */
static const synth_module_api_t *g_api;
static const synth_module_cbs_t *g_cbs;
static synth_module_status_t g_st;
static uint8_t  g_page;
static uint8_t  g_dirty = 1u;      /* 1 = 需要重画当前页 */
static uint8_t  g_item;            /* 下一个要画的小项序号 */
static uint8_t  g_ready;
static uint32_t g_last_item_ms;
static uint32_t g_last_periodic_ms;
static uint32_t g_key_events;
static uint32_t g_ticks;

/* ---------------------------------------------------------------- 寄存器 */

#define REG32(a) (*(volatile uint32_t *)(uintptr_t)(a))

#define RCC_BASE      0x58024400u
#define RCC_AHB4ENR   REG32(RCC_BASE + 0x0E0u)

#define GPIOE_BASE    0x58021000u
#define GPIOE_MODER   REG32(GPIOE_BASE + 0x00u)
#define GPIOE_OTYPER  REG32(GPIOE_BASE + 0x04u)
#define GPIOE_OSPEEDR REG32(GPIOE_BASE + 0x08u)
#define GPIOE_PUPDR   REG32(GPIOE_BASE + 0x0Cu)
#define GPIOE_BSRR    REG32(GPIOE_BASE + 0x18u)

#define PIN_BL    10u   /* 背光，先当 GPIO 用 */
#define PIN_RST   11u
#define PIN_SCK   12u
#define PIN_MOSI  14u
#define PIN_DC    13u
#define PIN_CS    15u

#define BSET(p)   (1u << (p))
#define BCLR(p)   (1u << ((p) + 16u))

static inline void pin_hi(uint32_t p) { GPIOE_BSRR = BSET(p); }
static inline void pin_lo(uint32_t p) { GPIOE_BSRR = BCLR(p); }

/* ---------------------------------------------------------------- libc 垫片 */

/* 模块里没有 libc：这几个符号 GCC 可能自己生成调用（比如把 strlen 的循环直接替换成
 * 一次 strlen 调用），所以必须自己定义，并且用 -fno-builtin 编译（见 CMakeLists）。 */
void *memset(void *dst, int v, unsigned n) {
    unsigned char *d = (unsigned char *)dst;
    while (n--) *d++ = (unsigned char)v;
    return dst;
}

void *memcpy(void *dst, const void *src, unsigned n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
    return dst;
}

unsigned long strlen(const char *s) {
    const char *p = s;
    while (*p != 0) p++;
    return (unsigned long)(p - s);
}

/* ---------------------------------------------------------------- 小工具 */

static unsigned u32_to_dec(uint32_t v, char *out) {
    char tmp[11];
    unsigned n = 0, i;
    if (v == 0u) { out[0] = '0'; out[1] = 0; return 1u; }
    while (v != 0u && n < sizeof(tmp)) { tmp[n++] = (char)('0' + (v % 10u)); v /= 10u; }
    for (i = 0; i < n; i++) out[i] = tmp[n - 1u - i];
    out[n] = 0;
    return n;
}

static unsigned u32_to_hex(uint32_t v, char *out, unsigned digits) {
    static const char kHex[] = "0123456789ABCDEF";
    for (unsigned i = 0; i < digits; i++) out[i] = kHex[(v >> (4u * (digits - 1u - i))) & 0xFu];
    out[digits] = 0;
    return digits;
}

static char *str_append(char *p, const char *s) {
    while (*s != 0) *p++ = *s++;
    *p = 0;
    return p;
}

static char *str_append_dec(char *p, uint32_t v) {
    p += u32_to_dec(v, p);
    return p;
}

/* ---------------------------------------------------------------- ST7735 */

#define LCD_W 128u
#define LCD_H 160u

#define C_BLACK  0x0000u
#define C_WHITE  0xFFFFu
#define C_GREY   0x8410u
#define C_DARK   0x18E3u
#define C_GREEN  0x07E0u
#define C_CYAN   0x07FFu
#define C_YELLOW 0xFFE0u
#define C_ORANGE 0xFD20u
#define C_RED    0xF800u

/* 位敲 SPI。ST7735 串行写周期最小约 66 ns，这里刻意上了一点 NOP 延时，宁慢勿快。 */
static void spi_byte(uint8_t b) {
    for (unsigned i = 0; i < 8u; i++) {
        if ((b & 0x80u) != 0u) pin_hi(PIN_MOSI); else pin_lo(PIN_MOSI);
        pin_hi(PIN_SCK);
        __asm__ volatile("nop; nop; nop; nop; nop; nop; nop; nop");
        pin_lo(PIN_SCK);
        b = (uint8_t)(b << 1);
    }
}

static void lcd_cmd(uint8_t c) {
    pin_lo(PIN_CS);
    pin_lo(PIN_DC);
    spi_byte(c);
    pin_hi(PIN_CS);
}

static void lcd_data(const uint8_t *d, uint32_t n) {
    pin_lo(PIN_CS);
    pin_hi(PIN_DC);
    while (n-- != 0u) spi_byte(*d++);
    pin_hi(PIN_CS);
}

static void lcd_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    uint8_t c[4];
    lcd_cmd(0x2Au);                        /* CASET */
    c[0] = (uint8_t)(x0 >> 8); c[1] = (uint8_t)x0;
    c[2] = (uint8_t)(x1 >> 8); c[3] = (uint8_t)x1;
    lcd_data(c, 4u);
    lcd_cmd(0x2Bu);                        /* RASET */
    c[0] = (uint8_t)(y0 >> 8); c[1] = (uint8_t)y0;
    c[2] = (uint8_t)(y1 >> 8); c[3] = (uint8_t)y1;
    lcd_data(c, 4u);
    lcd_cmd(0x2Cu);                        /* RAMWR */
}

/* 填充矩形（含端点） */
static void lcd_fill(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t color) {
    uint32_t n;
    uint8_t px[2];
    if (x1 >= LCD_W) x1 = LCD_W - 1u;
    if (y1 >= LCD_H) y1 = LCD_H - 1u;
    if (x0 > x1 || y0 > y1) return;
    lcd_window(x0, y0, x1, y1);
    px[0] = (uint8_t)(color >> 8);
    px[1] = (uint8_t)color;
    n = (uint32_t)(x1 - x0 + 1u) * (uint32_t)(y1 - y0 + 1u);
    pin_lo(PIN_CS);
    pin_hi(PIN_DC);
    while (n-- != 0u) { spi_byte(px[0]); spi_byte(px[1]); }
    pin_hi(PIN_CS);
}

/* 画一整行。关键：CASET/RASET 设好窗口后，RAMWR 的数据是**按行**（x 递增、然后 y+1）
 * 填进窗口的，而字模是按**列**存的（每列 2 字节、MSB 在上）——所以必须在这里转置：
 * 外层 row、内层 char/col。之前按列推数据，屏幕上就是一堆散点。 */
static void lcd_line(uint16_t x, uint16_t y, const char *s, uint16_t fg, uint16_t bg) {
    unsigned len = (unsigned)strlen(s);
    unsigned maxlen = (unsigned)((LCD_W - x) / UI_FONT_W);
    uint8_t hi_fg = (uint8_t)(fg >> 8), lo_fg = (uint8_t)fg;
    uint8_t hi_bg = (uint8_t)(bg >> 8), lo_bg = (uint8_t)bg;

    if (len > maxlen) len = maxlen;
    /* 先刷整行背景（行尾到屏边也算），避免旧字符残留 */
    lcd_fill(x, y, (uint16_t)(LCD_W - 1u), (uint16_t)(y + UI_FONT_H - 1u), bg);
    if (len == 0u) return;

    lcd_window(x, y, (uint16_t)(x + len * UI_FONT_W - 1u), (uint16_t)(y + UI_FONT_H - 1u));
    pin_lo(PIN_CS);
    pin_hi(PIN_DC);
    for (unsigned row = 0; row < UI_FONT_H; row++) {
        const unsigned shift_top = (row < 8u) ? (7u - row) : 0u;
        const unsigned shift_bot = (row < 8u) ? 0u : (15u - row);
        for (unsigned i = 0; i < len; i++) {
            unsigned ch = (unsigned char)s[i];
            unsigned idx = (ch >= UI_FONT_FIRST && ch < UI_FONT_FIRST + UI_FONT_COUNT)
                               ? (ch - UI_FONT_FIRST)
                               : (unsigned)('?' - UI_FONT_FIRST);
            const unsigned char *g = k_ui_font[idx];
            for (unsigned col = 0; col < UI_FONT_W; col++) {
                uint8_t bits = g[col * 2u + (row < 8u ? 0u : 1u)];
                uint8_t on = (uint8_t)((bits >> (row < 8u ? shift_top : shift_bot)) & 1u);
                if (on != 0u) { spi_byte(hi_fg); spi_byte(lo_fg); }
                else          { spi_byte(hi_bg); spi_byte(lo_bg); }
            }
        }
    }
    pin_hi(PIN_CS);
}

static void lcd_init(void) {
    static const uint8_t init_seq[] = {
        0x01, 0,                                        /* SWRESET */
        0x11, 0,                                        /* SLPOUT */
        0xB1, 3, 0x01, 0x2C, 0x2D,                      /* FRMCTR1 */
        0xB2, 3, 0x01, 0x2C, 0x2D,                      /* FRMCTR2 */
        0xB3, 6, 0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D,    /* FRMCTR3 */
        0xB4, 1, 0x07,                                  /* INVCTR */
        0xC0, 3, 0xA2, 0x02, 0x84,                      /* PWCTR1 */
        0xC1, 1, 0xC5,                                  /* PWCTR2 */
        0xC2, 2, 0x0A, 0x00,                            /* PWCTR3 */
        0xC3, 2, 0x8A, 0x2A,                            /* PWCTR4 */
        0xC4, 2, 0x8A, 0xEE,                            /* PWCTR5 */
        0xC5, 1, 0x0E,                                  /* VMCTR1 */
        0x20, 0,                                        /* INVOFF（BOE 面板） */
        0x3A, 1, 0x05,                                  /* COLMOD = RGB565 */
        0xE0, 16, 0x02, 0x1C, 0x07, 0x12, 0x37, 0x32, 0x29, 0x2D,
                  0x29, 0x25, 0x2B, 0x39, 0x00, 0x01, 0x03, 0x10,   /* GMCTRP1 */
        0xE1, 16, 0x03, 0x1D, 0x07, 0x06, 0x2E, 0x2C, 0x29, 0x2D,
                  0x2E, 0x2E, 0x37, 0x3F, 0x00, 0x00, 0x02, 0x10,   /* GMCTRN1 */
        0x13, 0,                                        /* NORON */
        0x29, 0,                                        /* DISPON */
        0x36, 1, 0xC0,                                  /* MADCTL：竖屏 */
    };
    uint32_t t0;

    /* 硬件复位：RST 低 >= 20 ms 再放高 */
    pin_lo(PIN_RST);
    t0 = g_api->get_tick();
    while ((g_api->get_tick() - t0) < 20u) { }
    pin_hi(PIN_RST);
    t0 = g_api->get_tick();
    while ((g_api->get_tick() - t0) < 120u) { }

    for (unsigned i = 0; i < sizeof(init_seq); ) {
        uint8_t cmd = init_seq[i++];
        uint8_t n = init_seq[i++];
        lcd_cmd(cmd);
        if (n != 0u) { lcd_data(&init_seq[i], n); i = (unsigned)(i + n); }
        if (cmd == 0x01u || cmd == 0x11u) {
            t0 = g_api->get_tick();
            while ((g_api->get_tick() - t0) < 120u) { }
        }
    }
}

/* ---------------------------------------------------------------- 画页面 */

/* 每页若干"小项"，tick() 每次最多画一项。item 号从 0 开始，返回 0 = 画完了。 */

static void draw_header(const char *title, uint8_t page) {
    char line[24];
    char *p = line;
    char num[5];
    p = str_append(p, title);
    while ((unsigned)(p - line) < 14u) *p++ = ' ';
    *p = 0;
    lcd_line(2, 2, line, C_BLACK, C_CYAN);
    num[0] = (char)('1' + page);
    num[1] = '/'; num[2] = '3'; num[3] = 0;
    lcd_line((uint16_t)(LCD_W - 3 * UI_FONT_W - 2), 2, num, C_BLACK, C_CYAN);
    lcd_fill(0, 15, LCD_W - 1u, 15, C_DARK);
}

static void line_kv(uint16_t y, const char *key, const char *val, uint16_t vcolor) {
    char line[24];
    char *p = line;
    p = str_append(p, key);
    while ((unsigned)(p - line) < 8u) *p++ = ' ';
    *p = 0;
    lcd_line(2, y, line, C_GREY, C_BLACK);
    lcd_line((uint16_t)(2 + 8 * UI_FONT_W), y, val, vcolor, C_BLACK);
}

static unsigned build_status_item0(char *buf) {           /* CPU */
    char *p = buf;
    p = str_append_dec(p, g_st.cpu_permille / 10u);
    *p++ = '.';
    *p++ = (char)('0' + (g_st.cpu_permille % 10u));
    p = str_append(p, "%");
    *p = 0;
    line_kv(20, "CPU", buf, g_st.cpu_permille > 800u ? C_RED : C_GREEN);
    return 0;
}

static void draw_item(void) {
    char buf[24];
    char *p;

    switch (g_page) {
    case 0:                                   /* 状态仪表盘 */
        switch (g_item) {
        case 0: draw_header("STATUS", 0); break;
        case 1: (void)build_status_item0(buf); break;
        case 2:                               /* 声部 */
            p = buf;
            p = str_append_dec(p, g_st.active_voices);
            *p++ = '/';
            p = str_append_dec(p, g_st.peak_voices);
            *p = 0;
            line_kv(34, "VOICES", buf, C_WHITE);
            break;
        case 3:                               /* 欠载 */
            p = buf;
            p = str_append_dec(p, g_st.underruns);
            p = str_append(p, " UR ");
            p = str_append_dec(p, g_st.xruns);
            *p = 0;
            line_kv(48, "XRUN", buf, (g_st.underruns | g_st.xruns) != 0u ? C_ORANGE : C_GREEN);
            break;
        case 4:                               /* MIDI */
            p = buf;
            p = str_append_dec(p, g_st.midi_bytes);
            p = str_append(p, " DRP ");
            p = str_append_dec(p, g_st.midi_dropped);
            *p = 0;
            line_kv(62, "MIDI", buf, C_WHITE);
            break;
        case 5:                               /* 音色 */
            p = buf;
            p = str_append(p, "PC ");
            p = str_append_dec(p, g_st.patch_index);
            *p = 0;
            line_kv(76, "PATCH", buf, C_YELLOW);
            break;
        case 6:                               /* 存储 */
            p = buf;
            p = str_append(p, g_api->sd_present() != 0 ? "SD OK" : "SD --");
            p = str_append(p, g_st.lib_count != 0u ? " LIB OK" : " LIB --");
            *p = 0;
            line_kv(90, "STORE", buf, C_WHITE);
            break;
        case 7: {                             /* 运行时间 */
            uint32_t s = g_st.uptime_ms / 1000u;
            p = buf;
            p = str_append_dec(p, s / 3600u);
            *p++ = ':';
            if ((s / 60u) % 60u < 10u) *p++ = '0';
            p = str_append_dec(p, (s / 60u) % 60u);
            *p++ = ':';
            if (s % 60u < 10u) *p++ = '0';
            p = str_append_dec(p, s % 60u);
            *p = 0;
            line_kv(104, "UPTIME", buf, C_WHITE);
            break;
        }
        case 8:                               /* 页脚 */
            lcd_line(2, 132, "KEY: next page", C_GREY, C_BLACK);
            p = buf;
            p = str_append(p, "KEYS ");
            p = str_append_dec(p, g_key_events);
            *p = 0;
            lcd_line(2, 146, buf, C_DARK, C_BLACK);
            break;
        default: return;
        }
        break;

    case 1:                                   /* 采样库 */
        switch (g_item) {
        case 0: draw_header("SAMPLES", 1); break;
        case 1:
            p = buf;
            p = str_append(p, "INSTR ");
            p = str_append_dec(p, g_st.lib_count);
            *p = 0;
            line_kv(20, "LIB", buf, g_st.lib_count != 0u ? C_GREEN : C_ORANGE);
            break;
        case 2:
            p = buf;
            p = str_append(p, "0x");
            p += u32_to_hex(g_st.lib_serial, p, 8u);
            *p = 0;
            line_kv(34, "SERIAL", buf, C_WHITE);
            break;
        case 3: case 4: case 5: case 6: {      /* 前 4 件乐器 */
            char name[16];
            uint16_t pc = 0xFFFFu, zones = 0u;
            unsigned i = (unsigned)(g_item - 3u);
            uint16_t y = (uint16_t)(52u + i * 20u);
            if (g_api->lib_info((int)i, name, (uint16_t)sizeof(name), &pc, &zones) == 0) {
                lcd_line(2, y, name, C_CYAN, C_BLACK);
                p = buf;
                p = str_append(p, "PC ");
                p = str_append_dec(p, pc);
                p = str_append(p, " ZONES ");
                p = str_append_dec(p, zones);
                *p = 0;
                lcd_line(2, (uint16_t)(y + UI_FONT_H), buf, C_GREY, C_BLACK);
            }
            break;
        }
        case 7:
            lcd_line(2, 146, "KEY: next page", C_GREY, C_BLACK);
            break;
        default: return;
        }
        break;

    default:                                  /* 设置页（只读提示） */
        switch (g_item) {
        case 0: draw_header("SETTINGS", 2); break;
        case 1: lcd_line(2, 20, "MIDI CC CONTROL", C_WHITE, C_BLACK); break;
        case 2: lcd_line(2, 36, " CC7  CH VOL", C_GREY, C_BLACK); break;
        case 3: lcd_line(2, 50, " CC11 MASTER", C_GREY, C_BLACK); break;
        case 4: lcd_line(2, 64, " CC91 DELAY", C_GREY, C_BLACK); break;
        case 5: lcd_line(2, 78, " CC93 CHORUS", C_GREY, C_BLACK); break;
        case 6: lcd_line(2, 92, " NRPN0 CUTOFF", C_GREY, C_BLACK); break;
        case 7: lcd_line(2, 106, " NRPN1 RESON", C_GREY, C_BLACK); break;
        case 8:
            p = buf;
            p = str_append(p, "CH0 ");
            p = str_append_dec(p, g_st.chan_patch[0] == 0xFFFFu ? 0u : g_st.chan_patch[0]);
            p = str_append(p, "  SR ");
            p = str_append_dec(p, g_st.sample_rate / 1000u);
            p = str_append(p, "K");
            *p = 0;
            lcd_line(2, 122, buf, C_YELLOW, C_BLACK);
            lcd_line(2, 146, "KEY: next page", C_GREY, C_BLACK);
            break;
        default: return;
        }
        break;
    }
}

static uint8_t page_item_count(uint8_t page) {
    if (page == 0u) return 10u;   /* 0..8 有效，9 = 结束标记 */
    if (page == 1u) return 8u;
    return 10u;
}

/* ---------------------------------------------------------------- 回调 */

static void ui_tick(uint32_t now_ms) {
    g_ticks++;
    if (g_ready == 0u) return;

    /* 每 10 ms 最多画一项（一项 ≈ 一行字 ≈ 2~3 ms），画完一页就停 */
    if (g_dirty != 0u && (now_ms - g_last_item_ms) >= 10u) {
        g_last_item_ms = now_ms;
        draw_item();
        g_item++;
        if (g_item >= page_item_count(g_page)) {
            g_dirty = 0u;
            g_item = 0u;
        }
    }

    /* 状态页每 1 s 自动刷新一遍（只在没有待画项时启动）。
       注意：**不能**在这里整片刷黑（36 KB ≈ 几十毫秒，会把音频喂水的节奏打断），
       每一行由 lcd_line() 自己刷自己的背景就够了。 */
    if (g_page == 0u && g_dirty == 0u && (now_ms - g_last_periodic_ms) >= 1000u) {
        g_api->get_status(&g_st);
        g_last_periodic_ms = now_ms;
        g_dirty = 1u;
        g_item = 1u;                 /* 标题不动，从第一项数据开始刷 */
    }

    /* 探针：0 = tick 计数，1 = 按键次数（没有屏幕也能从 status.ps1 看出来） */
    g_api->set_probe(0u, g_ticks);
    g_api->set_probe(1u, g_key_events);
}

static void ui_key(uint8_t pressed) {
    if (pressed == 0u) return;
    g_key_events++;
    g_page = (uint8_t)((g_page + 1u) % 3u);
    g_dirty = 1u;
    g_item = 0u;
}

static void gpio_init(void) {
    /* GPIOE 时钟 + 六个脚：全部输出推挽、高速、无上下拉 */
    RCC_AHB4ENR |= (1u << 4);                    /* GPIOEEN */
    const uint32_t pins = (1u << PIN_BL) | (1u << PIN_RST) | (1u << PIN_SCK) |
                          (1u << PIN_MOSI) | (1u << PIN_DC) | (1u << PIN_CS);
    for (unsigned p = 0; p < 16u; p++) {
        if ((pins & (1u << p)) == 0u) continue;
        GPIOE_MODER   = (GPIOE_MODER & ~(3u << (2u * p))) | (1u << (2u * p));  /* 01 = 输出 */
        GPIOE_OTYPER &= ~(1u << p);                                            /* 推挽 */
        GPIOE_OSPEEDR |= (3u << (2u * p));                                     /* 很高 */
        GPIOE_PUPDR   &= ~(3u << (2u * p));                                    /* 无上下拉 */
    }
    pin_hi(PIN_CS);
    pin_hi(PIN_DC);
    pin_hi(PIN_RST);
    pin_hi(PIN_BL);                              /* 背光常亮 */
}

const synth_module_cbs_t *ui_entry(const synth_module_api_t *api) {
    static synth_module_cbs_t cbs;

    if (api == 0 || api->abi != SYNTH_MODULE_ABI) return 0;
    g_api = api;

    gpio_init();
    lcd_init();
    lcd_fill(0, 0, LCD_W - 1u, LCD_H - 1u, C_BLACK);
    lcd_line(2, 40, "STM32 SYNTH", C_CYAN, C_BLACK);
    lcd_line(2, 56, "TFT UI MODULE", C_WHITE, C_BLACK);
    lcd_line(2, 72, "PRESS KEY", C_GREY, C_BLACK);

    api->get_status(&g_st);
    g_ready = 1u;
    g_dirty = 1u;
    g_item = 0u;
    g_last_item_ms = api->get_tick();

    cbs.abi  = SYNTH_MODULE_ABI;
    cbs.tick = ui_tick;
    cbs.key  = ui_key;
    g_cbs = &cbs;
    api->set_probe(0u, 0u);
    api->set_probe(1u, 0u);
    return g_cbs;
}
