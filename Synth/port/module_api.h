/* RAM overlay 模块的宿主<->模块契约（本文件同时被宿主固件和模块工程 include）。
 *
 * 为什么要有这个契约：模块是一个**独立编译、独立链接到固定 RAM 地址**的裸二进制，
 * 里面没有符号表，所以它和宿主之间只能靠"固定地址 + 函数指针表"通信。
 * 任何一侧改了这里的结构体（字段顺序/类型/ABI 号），另一侧必须重编 —— ABI 号对不上
 * 时宿主会拒绝加载（见 module.c 的 -2 分支），不会带着错位的函数指针跑飞。
 *
 * 布局（QSPI 8 MB 的顶部 64 KB，见 qspi_flash.h）：
 *   0x7F0000  ┌────────────────────────────┐
 *             │ synth_module_hdr_t (36 B)  │
 *             ├────────────────────────────┤
 *             │ 镜像：.text/.rodata/.data/ │  ← 上电 memcpy 到 SYNTH_MODULE_RAM_BASE
 *             │       .bss（零填充也在内） │
 *             └────────────────────────────┘
 *   0x7FF000  保留给 QSPI 写自检扇区（别再往后放东西）
 *
 * 为什么模块要自带 .bss 的零：模块没有 C 运行时（没有 startup、不跑 .data 拷贝），
 * 所以宿主把"整段镜像"（含 .bss 的零）一次性拷进 RAM，模块里所有静态变量才是干净的。
 */
#ifndef SYNTH_PORT_MODULE_API_H
#define SYNTH_PORT_MODULE_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SYNTH_MODULE_MAGIC    0x444F4D53u   /* 'SMOD'（小端：魔数在低地址） */
#define SYNTH_MODULE_VERSION  1u
#define SYNTH_MODULE_ABI      1u            /* 契约版本：改了本文件的结构体就 +1 */

/* USB 上传标记：tools\upload_module.py 会把头部的 reserved[1]（偏移 32）改成这个值，
 * 表示"板上这份是 USB 灌进去的"。开机时 synth_port_module_import_from_card() 看到
 * 标记就不再拿 SD 卡上的 ui.bin 覆盖它（否则每次上电都会被卡里的旧镜像顶回去）。
 * 注意 reserved[0]（偏移 28）也认 —— 早期版本写错了位置，两个都接受更保险。 */
#define SYNTH_MODULE_USB_FLAG 0x55534255u   /* 'USB\0'（小端：55 42 53 55） */

/* QSPI 里的模块区：8 MB 的顶部 64 KB，避开采样库（0x4000 起往下长）与写自检扇区（0x7FF000） */
#define SYNTH_QSPI_MODULE_OFFSET  0x7F0000u
#define SYNTH_QSPI_MODULE_MAX     (0xF000u)         /* 60 KB */

/* 模块执行区：AXI SRAM 顶部 64 KB（链接脚本 MODULE_RAM 段；宿主 .bss 已被限制在 448 KB 以内） */
#define SYNTH_MODULE_RAM_BASE     0x24070000u
#define SYNTH_MODULE_RAM_BYTES    0x10000u

/* QSPI 里的模块头（36 字节，紧跟其后是镜像） */
typedef struct {
    uint32_t magic;         /* SYNTH_MODULE_MAGIC */
    uint16_t version;       /* SYNTH_MODULE_VERSION */
    uint16_t hdr_bytes;     /* sizeof(synth_module_hdr_t) */
    uint32_t abi;           /* SYNTH_MODULE_ABI */
    uint32_t build_id;      /* 模块构建签名：变了 => 卡上的新镜像要重装 */
    uint32_t image_bytes;   /* 镜像长度（含 .bss 零填充），4 字节对齐 */
    uint32_t entry_offset;  /* entry 函数相对镜像起点的偏移 */
    uint32_t crc32;         /* 镜像的 CRC32（多项式 0xEDB88320，初值/异或 0xFFFFFFFF） */
    uint32_t reserved[2];
} synth_module_hdr_t;

/* 模块向宿主回报的快照（UI 用的都是这些；故意只放标量，避免模块依赖引擎头文件） */
typedef struct {
    uint32_t cpu_permille;      /* 0..1000+，引擎渲染占实时预算的比例 */
    uint32_t uptime_ms;         /* 上电以来的毫秒数（HAL_GetTick） */
    uint32_t midi_bytes;        /* 收到过的 MIDI 字节总数 */
    uint32_t midi_dropped;      /* 环形缓冲丢弃的字节数 */
    uint32_t notes_on;          /* 累计 NoteOn 次数 */
    uint32_t steals;            /* 抢声部次数 */
    uint32_t xruns;             /* 音频 DMA 欠载（错误回调）次数 */
    uint32_t underruns;         /* 预渲染环空导致的重复块数 */
    uint32_t lib_count;         /* QSPI 采样库里的乐器数（0 = 没有库） */
    uint32_t lib_serial;        /* 采样库 serial（打包器算的 CRC32） */
    uint16_t sample_rate;       /* SYNTH_SR_DEFAULT，当前 48000 */
    uint16_t patch_index;       /* 最近一次 Program Change 的音色号 */
    uint16_t chan_patch[16];    /* 每通道音色号（0xFFFF = 没用过） */
    uint16_t active_voices;     /* 当前正在响的声部数 */
    uint16_t peak_voices;       /* 历史峰值 */
} synth_module_status_t;

/* 宿主提供给模块的 API（模块只通过这张表碰宿主，不直接引用宿主的符号） */
typedef struct synth_module_api {
    uint32_t abi;                                                   /* = SYNTH_MODULE_ABI */
    uint32_t (*get_tick)(void);                                     /* HAL_GetTick() */
    void     (*get_status)(synth_module_status_t *out);
    int      (*set_patch)(uint16_t index);                          /* 全局切换（16 通道都换） */
    int      (*set_channel_patch)(uint8_t ch, uint16_t index);      /* 只换一个通道 */
    int      (*lib_info)(int index, char *name, uint16_t name_len,
                         uint16_t *pc, uint16_t *zones);            /* 0 = OK */
    int      (*sd_present)(void);                                   /* 1 = 卡已挂载 */
    void     (*log)(const char *msg);                               /* 串口一行（发布构建里是空操作） */
    void     (*set_probe)(uint16_t idx, uint32_t value);            /* 回报两个探针值，宿主存进 g_app_status */
} synth_module_api_t;

/* 模块导出的回调表（entry 返回它；宿主每圈调 tick，按键事件调 key） */
typedef struct {
    uint32_t abi;
    void (*tick)(uint32_t now_ms);
    void (*key)(uint8_t pressed);       /* 1 = 按下, 0 = 抬起；没有按键需求可以不实现 */
} synth_module_cbs_t;

/* 模块入口：签名固定。返回 NULL = 初始化失败（宿主会记 module_state = -5）。 */
typedef const synth_module_cbs_t *(*synth_module_entry_fn)(const synth_module_api_t *api);

#ifdef __cplusplus
}
#endif
#endif /* SYNTH_PORT_MODULE_API_H */
