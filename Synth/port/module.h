/* RAM overlay 模块：宿主侧加载器（QSPI 里的裸镜像 → AXI SRAM 顶部 64 KB → 执行）。
 *
 * 分工：
 *   module_api.h  宿主<->模块的函数指针契约（两边共用）
 *   module.c      校验/CRC/拷贝/调用入口/tick 分发（不含文件系统）
 *   patch_storage_sd.c  synth_port_module_import_from_card()（把 SD 上的 /synth/ui.bin 写进 QSPI）
 *
 * 启动顺序要求：**必须**在 QSPI 已进入内存映射模式之后调用（App_QspiEnterMemoryMapped）。
 * 返回码（g_app_status.module_state）：
 *    0  已加载并进入 tick 循环
 *   -1  QSPI 里没有模块（魔数不对）
 *   -2  版本/ABI 不匹配（模块与固件不是同一次构建）
 *   -3  CRC 校验失败（镜像损坏）
 *   -4  长度/入口偏移非法
 *   -5  entry 返回 NULL（模块自己拒绝初始化）
 *   -6  SD 导入失败（见 g_app_status.module_import）
 */
#ifndef SYNTH_PORT_MODULE_H
#define SYNTH_PORT_MODULE_H

#include <stdint.h>
#include "module_api.h"

#ifdef __cplusplus
extern "C" {
#endif

int      synth_module_init(void);
/* 只清"已加载"标记（RAM 里的旧代码不动）：USB 上传新镜像后要能重新 init。
   必须在主循环上下文调用（模块的 tick 只从主循环来）。 */
void     synth_module_unload(void);
void     synth_module_tick(void);
void     synth_module_key(uint8_t pressed);

/* 由 patch_storage_sd.c 提供：卡上 /synth/ui.bin 的 build_id 与板上不同就写进 QSPI。
   返回 0 = 已是最新（没写）、1 = 已导入、负 = 失败。 */
int      synth_port_module_import_from_card(void);

/* 诊断用读数（main.c 拷进 g_app_status；模块自己的探针走 set_probe）。 */
int      synth_module_loaded(void);
/* 和 module.c 内部那份完全一样（多项式 0xEDB88320）—— SD 导入器用它回读校验镜像。 */
uint32_t synth_module_crc32(const void *data, uint32_t len);
/* 同上的细分状态：0 = 已加载；否则是最近一次 init 的失败码（-1..-5，含义见文件头）。 */
int      synth_module_state(void);
uint32_t synth_module_crc(void);
uint32_t synth_module_build_id(void);
uint32_t synth_module_ticks(void);
extern volatile uint32_t g_module_probe[2];

#ifdef __cplusplus
}
#endif
#endif /* SYNTH_PORT_MODULE_H */
