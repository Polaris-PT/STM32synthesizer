/* 渲染耗时基准（PC 端）：测 RenderBlock 在 128 帧/块下的成本，并与 M7 的 2.667 ms 预算对比。
   用法：ssynth_bench [--frames 128] [--blocks 2000] [--patch N]
   目的：定位"复音一多就滋滋/卡死"是不是 CPU 超预算（音频中断里渲染超时 → DMA 欠载）。 */
#include "synth/synth.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

double NowUs() {
    using C = std::chrono::steady_clock;
    static const auto t0 = C::now();
    return std::chrono::duration<double, std::micro>(C::now() - t0).count();
}

const char *EngineName(uint16_t idx) {
    (void)idx;
    return "";
}

int PatchCount() { return (int)synth_factory_bank_size; }

void NoteOn(uint8_t ch, uint8_t note, uint8_t vel) {
    uint8_t m[3] = {(uint8_t)(0x90u | (ch & 0x0Fu)), note, vel};
    synth_midi_in(m, 3);
}

void AllOff() {
    for (uint8_t n = 0; n < 128; ++n) {
        uint8_t m[3] = {0x80u, n, 0};
        synth_midi_in(m, 3);
    }
}

struct Result { double us; double us_peak; int nan; int peak_abs; };

Result Measure(int patch, int voices, int blocks, int frames, bool fx_heavy) {
    synth_config_t cfg{};
    cfg.sample_rate = 48000;
    cfg.max_voices = SYNTH_MAX_VOICES;
    cfg.factory_bank = nullptr;
    cfg.factory_bank_size = 0;
    if (synth_init(&cfg) != 0) { std::printf("init failed\n"); return {0, 0, 0, 0}; }
    synth_set_patch((uint16_t)patch);
    if (fx_heavy) {
        /* 打开合唱+延迟：CC91 = 混响量、CC93 = 合唱量（引擎按 0..127 映射） */
        uint8_t cc[3] = {0xB0u, 93u, 127u};
        synth_midi_in(cc, 3);
        cc[1] = 91u;
        synth_midi_in(cc, 3);
    }
    /* 16 声部：从 40 号音起铺开，避免同一个音重复触发导致被"同音替换" */
    for (int n = 0; n < voices; ++n) NoteOn(0, (uint8_t)(40 + n * 3), 100);

    std::vector<int32_t> buf((size_t)frames * 2u, 0);
    for (int i = 0; i < 200; ++i) synth_render_i32(buf.data(), (uint32_t)frames);  /* 预热 + 让包络进入稳态 */

    const double t0 = NowUs();
    double peak = 0.0;
    for (int i = 0; i < blocks; ++i) {
        const double a = NowUs();
        synth_render_i32(buf.data(), (uint32_t)frames);
        const double d = NowUs() - a;
        if (d > peak) peak = d;
    }
    const double total = NowUs() - t0;

    int nan = 0, absmax = 0;
    for (int32_t v : buf) {
        const float f = (float)v * (1.0f / 2147483648.0f);
        if (!(f == f)) ++nan;
        const int a = (v < 0) ? -v : v;
        if (a > absmax) absmax = a;
    }
    AllOff();
    synth_deinit();
    return {total / (double)blocks, peak, nan, absmax};
}

}  // namespace

int main(int argc, char **argv) {
    int frames = 128, blocks = 2000, only_patch = -1;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) frames = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--blocks") && i + 1 < argc) blocks = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--patch") && i + 1 < argc) only_patch = std::atoi(argv[++i]);
    }

    const double budget_us = 1e6 * (double)frames / 48000.0;   /* 48 kHz 下这一块的实时预算 */
    std::printf("== 预算：%d 帧 / 48 kHz = %.0f us/块（M7 上这就是音频中断的截止时间）==\n", frames, budget_us);

    const int npatches = PatchCount();
    const int vlist[] = {1, 4, 8, 12, 16};

    for (int p = 0; p < npatches; ++p) {
        if (only_patch >= 0 && p != only_patch) continue;
        for (int vi = 0; vi < 5; ++vi) {
            const int nv = vlist[vi];
            for (int heavy = 0; heavy < 2; ++heavy) {
                const Result r = Measure(p, nv, blocks, frames, heavy != 0);
                std::printf("patch %2d  voice %2d  %s  %8.1f us/块  %5.1f%% 预算  峰值 %8.1f us  nan=%d  peak=%.3f\n",
                            p, nv, heavy ? "FX  " : "干声", r.us, 100.0 * r.us / budget_us, r.us_peak, r.nan,
                            (double)r.peak_abs / 2147483648.0);
            }
        }
    }
    return 0;
}
