/* TinyUSB 配置：STM32H750 的 OTG_FS（dwc2 驱动），裸机，只做 MIDI 设备。 */
#ifndef SYNTH_TUSB_CONFIG_H
#define SYNTH_TUSB_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

#define CFG_TUSB_MCU                OPT_MCU_STM32H7
#define CFG_TUSB_OS                 OPT_OS_NONE
#define CFG_TUSB_DEBUG              0

#define CFG_TUSB_RHPORT0_MODE       (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)

#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN          __attribute__((aligned(4)))
#endif

/* 设备描述符 */
#define CFG_TUD_ENABLED             1
#define CFG_TUD_ENDPOINT0_SIZE      64

/* 只要 MIDI 类 */
#define CFG_TUD_CDC                 0
#define CFG_TUD_MSC                 0
#define CFG_TUD_HID                 0
#define CFG_TUD_MIDI                1
#define CFG_TUD_AUDIO               0
#define CFG_TUD_VENDOR              0

/* RX 放大到 1 KB：用 SysEx 上传模块镜像时主机会一口气灌几 KB，
   64 B 的 FIFO 容易溢出丢包（协议本身可重发，但没必要丢） */
#define CFG_TUD_MIDI_RX_BUFSIZE     1024
#define CFG_TUD_MIDI_TX_BUFSIZE     64
#define CFG_TUD_MIDI_EP_BUFSIZE     64

#ifdef __cplusplus
}
#endif
#endif /* SYNTH_TUSB_CONFIG_H */
