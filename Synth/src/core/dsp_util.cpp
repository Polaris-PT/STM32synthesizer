#include "dsp_util.h"

namespace ssynth {

float SineTable::table_[SineTable::kSize];

void SineTable::Init() {
    for (int i = 0; i < kSize; ++i) {
        table_[i] = sinf(kTwoPi * (float)i / (float)kSize);
    }
}

}  // namespace ssynth
