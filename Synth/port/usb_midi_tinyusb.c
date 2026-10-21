/* USB MIDI（TinyUSB，dwc2 驱动）→ synth_midi_in()
 *
 * 依赖：TinyUSB 源码在 Synth/third_party/tinyusb（见 Synth/port/README.md），构建时加入
 *   tusb.c / tusb_fifo.c / usbd.c / midi_device.c / dcd_dwc2.c / dwc2_common.c 六个文件。
 *   已按 vendored 版本（commit 3c6524e）核对 API：tud_midi_n_available(itf, cable)、
 *   tud_midi_n_packet_read(itf, pkt[4])、tud_midi_rx_cb(itf)、TUD_MIDI_DESCRIPTOR(...)。
 *
 * 注意：CubeMX 里不要启用 ST 的 USB Device 中间件（HAL_PCD/ll_usb），由 TinyUSB 的 dwc2
 *   驱动直接接管 OTG_FS；OTG_FS_IRQHandler 里调 tud_int_handler(0)（在 stm32h7xx_it.c 的
 *   USER CODE 区）。
 */
#include "synth_port_config.h"
#include "synth_port.h"
#include "synth/synth.h"

#if SYNTH_PORT_USB_ENABLE
#include "tusb.h"
#include "debug_uart.h"
#include "module_upload.h"   /* SysEx 上传模块镜像：先过滤协议字节，再喂引擎 */

/* 诊断计数：收到的 MIDI 字节总数。调试器里盯着它，弹一下琴键就会涨；
   不涨 = USB→引擎 这条路没通（枚举失败，或主机没往 OUT 端点发数据）。 */
volatile uint32_t g_synth_midi_rx_bytes = 0;

/* 主机完成枚举 / 掉线的次数：正常情况插一次 mount 恰好 +1 并保持不变。
   反复增长 = 主机在反复重枚举（通常是固件在复位，或供电/线材不稳）。
   TinyUSB 在 tud_task() 里调用这两个回调（主循环上下文，不在中断里）。 */
volatile uint32_t g_synth_usb_mount_count  = 0;
volatile uint32_t g_synth_usb_umount_count = 0;

void tud_mount_cb(void) {
    ++g_synth_usb_mount_count;
    dbg_log("usb", "MOUNT 主机枚举完成 #%lu", (unsigned long)g_synth_usb_mount_count);
}
void tud_umount_cb(void) {
    ++g_synth_usb_umount_count;
    dbg_log("usb", "UMOUNT 掉线 #%lu", (unsigned long)g_synth_usb_umount_count);
}

/* ---------- 描述符 ---------- */
#define USB_VID   0xCafe
#define USB_PID   0x4020
#define USB_BCD   0x0100

#define EPNUM_MIDI_OUT 0x01
#define EPNUM_MIDI_IN  0x81

/* MIDI 描述符集含【两个】接口：0 = AudioControl、1 = MIDIStreaming（TUD_MIDI_DESCRIPTOR(n,…)
 * 把 MIDIStreaming 放在 n+1，AudioControl 头的 baInterfaceNr 也指向 n+1）→ bNumInterfaces 必须是 2。
 * 写成 1 时配置描述符自相矛盾：Windows 能看到设备名，但 usbmidi.sys 启动失败（代码 10），
 * 且不创建 MIDI 端口。 */
#define ITF_NUM_MIDI            0
#define ITF_NUM_MIDI_STREAMING  1
#define ITF_NUM_TOTAL           2
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_MIDI_DESC_LEN)

static const tusb_desc_device_t desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = USB_BCD,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

static const uint8_t desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_MIDI_DESCRIPTOR(ITF_NUM_MIDI, 0, EPNUM_MIDI_OUT, EPNUM_MIDI_IN, 64),
};

static const char *string_desc_arr[] = {
    (const char[]){0x09, 0x04},     /* 0: 语言 = 英语(US) */
    "STM32 Synth",                  /* 1: 厂商 */
    "H750 PCM5102A Synth",          /* 2: 产品 */
    "0001",                         /* 3: 序列号 */
};

/* ---------- TinyUSB 回调 ---------- */
uint8_t const *tud_descriptor_device_cb(void) { return (uint8_t const *)&desc_device; }
uint8_t const *tud_descriptor_configuration_cb(uint8_t index) { (void)index; return desc_configuration; }

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    static uint16_t desc_str[32];
    (void)langid;
    uint8_t chr_count = 0;

    if (index == 0) {
        memcpy(&desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
    } else {
        if (index >= (uint8_t)(sizeof(string_desc_arr) / sizeof(string_desc_arr[0]))) return NULL;
        const char *str = string_desc_arr[index];
        chr_count = (uint8_t)strlen(str);
        if (chr_count > 31) chr_count = 31;
        for (uint8_t i = 0; i < chr_count; ++i) desc_str[1 + i] = (uint16_t)str[i];
    }

    desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2u * chr_count + 2u));
    return desc_str;
}

/* USB-MIDI 事件包长度表（按 CIN 低 4 位） */
static const uint8_t kCinBytes[16] = {
    1, 1, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1,
};

/* MIDI 收到数据：拆包 → 字节流 → 引擎 */
void tud_midi_rx_cb(uint8_t itf) {
    uint8_t pkt[4];
    uint8_t bytes[3];
    while (tud_midi_n_available(itf, 0)) {
        if (!tud_midi_n_packet_read(itf, pkt)) break;    /* 本版本 API 无 cable 参数 */
        const uint8_t cin = (uint8_t)(pkt[0] & 0x0Fu);
        const uint8_t n   = kCinBytes[cin];
        for (uint8_t i = 0; i < n; ++i) bytes[i] = pkt[1 + i];
#if SYNTH_PORT_MIDI_TRACE
        /* 每个事件包一行：CIN + 3 个原始字节。判断"音停不下来"的第一手材料 ——
           note-on（9n）之后有没有对应的 note-off（8n）或 velocity=0 的 9n 到达。 */
        dbg_log("midi", "cin=%X %02X %02X %02X n=%u", (unsigned)cin,
                (unsigned)bytes[0], (unsigned)bytes[1], (unsigned)bytes[2], (unsigned)n);
#endif
        /* SysEx 上传模块镜像的协议字节不送引擎；普通 MIDI 原样通过（顺序不变） */
        uint8_t pass[3];
        uint8_t np = 0u;
        for (uint8_t i = 0; i < n; ++i) {
            if (!synth_port_module_upload_byte(bytes[i])) pass[np++] = bytes[i];
        }
        g_synth_midi_rx_bytes += np;
        if (np != 0u) synth_midi_in(pass, np);
    }
}

/* OTG_FS 中断计数（在 Core/Src/stm32h7xx_it.c 的 OTG_FS_IRQHandler 里 ++）。
   插着线却恒为 0 = DWC2 核没产生中断（时钟/PHY 供电/DP 上拉/中断使能的问题），
   而不是"枚举谈不拢"。主循环抄进 g_app_status.usb_irq_count。 */
volatile uint32_t g_synth_usb_irq_count = 0;

/* ---------- 端口 API ---------- */
int synth_port_usb_init(void) {
    if (!tusb_init()) return -1;
    return 0;
}

void synth_port_usb_task(void) {
    tud_task();                     /* 设备任务：枚举、收发 */
}

#endif /* SYNTH_PORT_USB_ENABLE */
