#include "fx_memory.h"

namespace ssynth {

static uint8_t SYNTH_AUDIO_RAM g_pool[kFxPoolBytes];
static uint32_t g_used = 0u;

void FxPoolInit() { g_used = 0u; }

void *FxAlloc(uint32_t bytes) {
    const uint32_t aligned = (bytes + 3u) & ~3u;
    if (g_used + aligned > kFxPoolBytes) return nullptr;
    void *p = &g_pool[g_used];
    g_used += aligned;
    return p;
}

uint32_t FxPoolUsed() { return g_used; }
uint32_t FxPoolSize() { return kFxPoolBytes; }

}  // namespace ssynth
