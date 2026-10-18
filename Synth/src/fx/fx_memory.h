/* FX 大缓冲内存池：目标端放 AXI SRAM 的 .ram_section（DTCM 只有 128KB 且不能给 DMA）。
   宿主端无需任何属性。若目标链接脚本没有 .ram_section，可定义空的 SYNTH_AUDIO_RAM 关闭。 */
#ifndef SSYNTH_FX_MEMORY_H
#define SSYNTH_FX_MEMORY_H

#include <stdint.h>

#if defined(__arm__) && !defined(SYNTH_AUDIO_RAM)
#define SYNTH_AUDIO_RAM __attribute__((section(".ram_section")))
#endif
#ifndef SYNTH_AUDIO_RAM
#define SYNTH_AUDIO_RAM
#endif

namespace ssynth {

constexpr uint32_t kFxPoolBytes = 192u * 1024u;   /* 目标端 AXI SRAM（延迟 0.6s=128KB + 合唱 16KB） */

void     FxPoolInit();
void    *FxAlloc(uint32_t bytes);                 /* 4 字节对齐，失败返回 nullptr */
uint32_t FxPoolUsed();
uint32_t FxPoolSize();

}  // namespace ssynth
#endif /* SSYNTH_FX_MEMORY_H */
