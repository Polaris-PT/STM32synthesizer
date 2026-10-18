/* 宿主端单元测试：振荡器频率/混叠、滤波器、包络、MIDI 解析、引擎复音与偷音。 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <vector>

#include "synth/synth.h"
#include "synth/pcm.h"
#include "../Synth/src/core/dsp_util.h"
#include "../Synth/src/core/wavetable.h"
#include "../Synth/src/core/osc.h"
#include "../Synth/src/core/filter.h"
#include "../Synth/src/core/env.h"
#include "../Synth/src/midi/midi_parser.h"
#include "../Synth/src/fx/chorus.h"
#include "../Synth/src/fx/delay.h"
#include "../Synth/src/fx/fx_chain.h"
#include "../Synth/src/app/patch_text.h"
#include "../Synth/src/engines/fm_dx7.h"
#include "../Synth/src/engines/fm_voices.h"
#include "../Synth/src/engines/sysex.h"
#include "../Synth/src/app/instrument_text.h"
#include "../Synth/src/core/rompler.h"

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++g_fail; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define CHECK_NEAR(a, b, tol, ...) do { double _d = fabs((double)(a) - (double)(b)); if (_d > (tol)) { ++g_fail; printf("  FAIL(%%.4g vs %%.4g): ", (double)(a), (double)(b)); printf(__VA_ARGS__); printf("\n"); } } while (0)

using namespace ssynth;

/* Goertzel：单频点幅度（归一化到 1.0 = 满幅正弦） */
static double Goertzel(const std::vector<float> &x, double freq, double sr) {
    const int    N = (int)x.size();
    const double w = 2.0 * M_PI * freq / sr;
    const double c = 2.0 * cos(w);
    double s0 = 0, s1 = 0, s2 = 0;
    for (int i = 0; i < N; ++i) { s0 = x[i] + c * s1 - s2; s2 = s1; s1 = s0; }
    const double re = s1 - s2 * cos(w);
    const double im = s2 * sin(w);
    return 2.0 * sqrt(re * re + im * im) / (double)N;
}

static void TestOscillator() {
    printf("[osc] 带限波表 频率与混叠\n");
    SineTable::Init();
    WaveTables::Init(48000.0f);
    const double sr = 48000.0;
    const float  f  = 440.0f;
    std::vector<float> x(48000);
    TableOsc o;
    o.Reset(0.0f);
    o.SetFreq(f, (float)sr);
    for (size_t i = 0; i < x.size(); ++i) x[i] = o.Saw();

    int crossings = 0;                       /* 只数正向过零：锯齿每周期一次 */
    for (size_t i = 1; i < x.size(); ++i) if (x[i - 1] < 0.0f && x[i] >= 0.0f) ++crossings;
    CHECK_NEAR(crossings, 440, 3, "锯齿正向过零次数应为 440，实测 %d", crossings);

    const double fund = Goertzel(x, 440.0, sr);
    CHECK(fund > 0.5, "440Hz 基波幅度应 >0.5，实测 %.3f", fund);

    /* 混叠探针 A：2.5kHz 锯齿的第 12 次谐波(30kHz)折叠到 18kHz */
    std::vector<float> y(48000);
    TableOsc o2;
    o2.Reset(0.0f);
    o2.SetFreq(2500.0f, (float)sr);
    for (size_t i = 0; i < y.size(); ++i) y[i] = o2.Saw();
    const double f25 = Goertzel(y, 2500.0, sr);
    const double alias = Goertzel(y, 18000.0, sr);
    const double db = 20.0 * log10((alias + 1e-12) / (f25 + 1e-12));
    printf("       2.5kHz 锯齿：基波 %.3f，18kHz 混叠 %.2e (%.1f dB)\n", f25, alias, db);
    CHECK(db < -55.0, "2.5kHz 混叠应低于 -55dB，实测 %.1f dB", db);

    /* 混叠探针 B：C7 (2093Hz) 的第 12 次谐波(25116Hz)折叠到 22884Hz */
    std::vector<float> z(48000);
    TableOsc o3;
    o3.Reset(0.0f);
    o3.SetFreq(2093.0f, (float)sr);
    for (size_t i = 0; i < z.size(); ++i) z[i] = o3.Saw();
    const double fc7 = Goertzel(z, 2093.0, sr);
    const double alias7 = Goertzel(z, 22884.0, sr);
    const double db7 = 20.0 * log10((alias7 + 1e-12) / (fc7 + 1e-12));
    printf("       C7 锯齿：基波 %.3f，22884Hz 混叠 %.2e (%.1f dB)\n", fc7, alias7, db7);
    CHECK(db7 < -55.0, "C7 混叠应低于 -55dB，实测 %.1f dB", db7);
}

static void TestFilter() {
    printf("[filter] TPT SVF 频响\n");
    const double sr = 48000.0;
    auto measure = [&](double freq) {
        TptSvf sv;
        sv.Reset();
        sv.SetCoeffs(1000.0f, 0.707f, (float)sr);
        std::vector<float> out(24000);
        for (int i = 0; i < 24000; ++i) {
            const float in = (float)sin(2.0 * M_PI * freq * (double)i / sr);
            float lp, bp, hp;
            sv.Process(in, lp, bp, hp);
            out[(size_t)i] = lp;
        }
        return Goertzel(out, freq, sr);
    };
    const double g100 = measure(100.0);
    const double g10k = measure(10000.0);
    printf("       100Hz %.3f / 10kHz %.5f\n", g100, g10k);
    CHECK_NEAR(20 * log10(g100), 0.0, 0.5, "100Hz 通带增益应 ≈0dB");
    CHECK(20 * log10(g10k) < -30.0, "10kHz 应衰减 >30dB，实测 %.1f", 20 * log10(g10k));
}

static void TestEnvelope() {
    printf("[env] ADSR 时序\n");
    Adsr e;
    e.Set(0.010f, 0.100f, 0.8f, 48000.0f, 0.050f);
    e.GateOn(true);
    for (int i = 0; i < 480; ++i) e.Process();        /* 10ms */
    for (int i = 0; i < 480; ++i) e.Process();        /* 再 10ms → 已进入 decay/sustain 附近 */
    CHECK(e.level > 0.5f, "起音后电平过低 %.3f", e.level);
    e.GateOff();
    for (int i = 0; i < 4800; ++i) e.Process();       /* 100ms >> release 50ms */
    CHECK(e.level < 0.01f, "释音后应接近 0，实测 %.4f", e.level);
}

static void TestMidiParser() {
    printf("[midi] 解析（running status / 实时字节 / SysEx）\n");
    MidiParser p;
    p.Reset();
    uint8_t st = 0, d1 = 0, d2 = 0;
    const uint8_t on[3] = {0x90, 60, 100};
    for (int i = 0; i < 3; ++i) p.Feed(on[i], &st, &d1, &d2);
    CHECK(st == 0x90 && d1 == 60 && d2 == 100, "NoteOn 解析错误 %02X %u %u", st, d1, d2);
    /* running status：只有数据字节 */
    p.Feed(0xF8, &st, &d1, &d2);                       /* 实时时钟，应被忽略 */
    p.Feed(62, &st, &d1, &d2);
    p.Feed(110, &st, &d1, &d2);
    CHECK(st == 0x90 && d1 == 62 && d2 == 110, "running status 解析错误 %02X %u %u", st, d1, d2);
    /* SysEx 丢弃 */
    const uint8_t sx[5] = {0xF0, 0x43, 0x12, 0x00, 0xF7};
    for (int i = 0; i < 5; ++i) p.Feed(sx[i], &st, &d1, &d2);
    CHECK(p.dropped_sysex == 1, "SysEx 应被计数丢弃，实测 %u", p.dropped_sysex);
    /* CC 两字节 + 单字节 Program Change */
    const uint8_t cc[3] = {0xB0, 74, 64};
    for (int i = 0; i < 3; ++i) p.Feed(cc[i], &st, &d1, &d2);
    CHECK(st == 0xB0 && d1 == 74 && d2 == 64, "CC 解析错误");
    p.Feed(0xC0, &st, &d1, &d2);
    p.Feed(5, &st, &d1, &d2);
    CHECK(st == 0xC0 && d1 == 5, "Program Change 解析错误 %02X %u", st, d1);
}

static void TestEngine() {
    printf("[engine] 复音 / 偷音 / 静音\n");
    synth_config_t cfg{};
    cfg.sample_rate = SYNTH_SR_DEFAULT;
    cfg.max_voices = SYNTH_MAX_VOICES;
    cfg.factory_bank = synth_factory_bank;
    cfg.factory_bank_size = synth_factory_bank_size;
    CHECK(synth_init(&cfg) == 0, "synth_init 失败");
    synth_set_master_volume(0.8f);

    std::vector<float> buf(SYNTH_BLOCK_FRAMES * 2u, 0.0f);
    /* 三音和弦 */
    synth_note_on(60, 100); synth_note_on(64, 100); synth_note_on(67, 100);
    double peak = 0.0;
    for (int b = 0; b < 375; ++b) {                     /* 1 秒 */
        synth_render_f32(buf.data(), SYNTH_BLOCK_FRAMES);
        for (float v : buf) { CHECK(v == v, "输出出现 NaN"); if (fabs(v) > peak) peak = fabs(v); }
    }
    synth_stats_t st{};
    synth_get_stats(&st);
    CHECK(st.peak_voices == 3, "应有 3 个声部，实测 %u", st.peak_voices);
    CHECK(peak > 0.05, "和弦应有输出，实测峰值 %.4f", peak);
    CHECK(peak <= 1.0, "输出不得削顶，实测 %.4f", peak);

    /* 释放后应衰减到接近静音 */
    synth_note_off(60); synth_note_off(64); synth_note_off(67);
    double tail = 0.0;
    for (int b = 0; b < 750; ++b) {
        synth_render_f32(buf.data(), SYNTH_BLOCK_FRAMES);
        if (b < 700) continue;                       /* 只统计最后 50 块（约 133ms） */
        for (float v : buf) if (fabs(v) > tail) tail = fabs(v);
    }
    CHECK(tail < 0.01, "释音 2 秒后应接近静音，实测 %.4f", tail);

    /* 偷音：20 个音 > 16 声部 */
    for (int n = 0; n < 20; ++n) synth_note_on((uint8_t)(40 + n), 100);
    for (int b = 0; b < 40; ++b) synth_render_f32(buf.data(), SYNTH_BLOCK_FRAMES);
    synth_get_stats(&st);
    CHECK(st.steals > 0, "应发生偷音，实测 %u", st.steals);
    CHECK(st.active_voices <= SYNTH_MAX_VOICES, "声部数超限 %u", st.active_voices);

    synth_panic();
    for (int b = 0; b < 20; ++b) synth_render_f32(buf.data(), SYNTH_BLOCK_FRAMES);
    synth_get_stats(&st);
    CHECK(st.active_voices == 0, "panic 后应无声部，实测 %u", st.active_voices);
}

static void TestFx() {
    printf("[fx] 延迟 / 合唱 / 过载\n");
    /* 延迟：脉冲应正好在设定延迟处出现 */
    StereoDelay dl;
    CHECK(dl.Init(48000u * 6u / 10u), "延迟缓冲分配失败");
    dl.Reset();
    dl.SetTimeSeconds(0.1f, 48000.0f);
    dl.SetFeedback(0.5f);
    dl.SetMix(1.0f);
    std::vector<float> out(20000, 0.0f);
    for (int i = 0; i < 20000; ++i) {
        float l = (i == 0) ? 1.0f : 0.0f;
        float r = l;
        dl.Process(l, r);
        out[(size_t)i] = l;
    }
    const int expect = 4800;                       /* 0.1s */
    int   peak_i = 0; float peak_v = 0.0f;
    for (int i = 4700; i < 4900; ++i) if (fabsf(out[(size_t)i]) > peak_v) { peak_v = fabsf(out[(size_t)i]); peak_i = i; }
    printf("       延迟峰值在 %d 样点（期望 %d，幅度 %.3f）\n", peak_i, expect, peak_v);
    CHECK(peak_v > 0.6f, "延迟幅度过低 %.3f", peak_v);
    CHECK(abs(peak_i - expect) <= 12, "延迟位置偏差过大 %d", peak_i);
    CHECK(fabsf(out[200]) < 0.01f, "延迟预响过大 %.4f", out[200]);

    /* 合唱：输出应有界且与输入不同 */
    Chorus ch;
    CHECK(ch.Init(48000u * 60u / 1000u), "合唱缓冲分配失败");
    ch.Reset();
    ch.SetParams(1.0f, 0.6f, 0.8f, 48000.0f);
    double diff = 0.0; float mx = 0.0f;
    for (int i = 0; i < 24000; ++i) {
        float l = 0.5f * sinf(2.0f * 3.14159265f * 440.0f * (float)i / 48000.0f);
        float r = l;
        const float dry = l;
        ch.Process(l, r);
        diff += fabs((double)(l - dry));
        if (fabsf(l) > mx) mx = fabsf(l);
    }
    printf("       合唱：输出峰值 %.3f，与干声累计差 %.2f\n", mx, diff);
    CHECK(mx < 1.5f, "合唱输出失控 %.3f", mx);
    CHECK(diff > 100.0, "合唱没有产生调制（差 %.2f）", diff);

    /* 过载：大输入必须被限幅 */
    const float d1 = Drive(8.0f, 4.0f, 0.4f);
    const float d2 = Drive(-8.0f, 4.0f, 0.4f);
    CHECK(fabsf(d1) <= 1.0f && fabsf(d2) <= 1.0f, "过载未限幅 %.3f %.3f", d1, d2);
}

/* 带延迟的音色在音符结束后应有回声尾巴 */
static void TestFxInEngine() {
    printf("[fx] 引擎级：延迟尾巴\n");
    synth_config_t cfg{};
    cfg.sample_rate = SYNTH_SR_DEFAULT;
    cfg.max_voices = SYNTH_MAX_VOICES;
    cfg.factory_bank = synth_factory_bank;
    cfg.factory_bank_size = synth_factory_bank_size;
    synth_init(&cfg);
    synth_set_master_volume(0.8f);

    std::vector<float> buf(SYNTH_BLOCK_FRAMES * 2u, 0.0f);
    auto tail_energy = [&](uint16_t patch_idx) {
        synth_panic();
        for (int b = 0; b < 40; ++b) synth_render_f32(buf.data(), SYNTH_BLOCK_FRAMES);
        synth_set_patch(patch_idx);
        synth_note_on(64, 110);
        for (int b = 0; b < 20; ++b) synth_render_f32(buf.data(), SYNTH_BLOCK_FRAMES);   /* ~53ms */
        synth_note_off(64);
        double e = 0.0;
        for (int b = 0; b < 300; ++b) {                     /* 0.8s：跳过释音，只看尾巴 */
            synth_render_f32(buf.data(), SYNTH_BLOCK_FRAMES);
            if (b < 40) continue;
            for (float v : buf) e += (double)v * (double)v;
        }
        return e;
    };
    const double dry = tail_energy(0);      /* Init Saw：无效果 */
    const double wet = tail_energy(6);      /* Super Saw：合唱+延迟 */
    printf("       干声尾能量 %.2e / 湿声尾能量 %.2e\n", dry, wet);
    CHECK(wet > dry * 3.0, "带延迟音色的尾巴能量应明显更大（干 %.2e 湿 %.2e）", dry, wet);
}

/* 琶音器：按住和弦应自动步进出多个音；松手后应停止 */
static void TestArp() {
    printf("[arp] 琶音器步进\n");
    synth_config_t cfg{};
    cfg.sample_rate = SYNTH_SR_DEFAULT;
    cfg.max_voices = SYNTH_MAX_VOICES;
    cfg.factory_bank = synth_factory_bank;
    cfg.factory_bank_size = synth_factory_bank_size;
    synth_init(&cfg);
    synth_set_master_volume(0.8f);
    synth_panic();
    std::vector<float> buf(SYNTH_BLOCK_FRAMES * 2u, 0.0f);
    for (int b = 0; b < 20; ++b) synth_render_f32(buf.data(), SYNTH_BLOCK_FRAMES);

    const uint8_t on[3] = {0xB0u, 20u, 1u};        /* CC20=1 → 上行琶音 */
    const uint8_t rate[3] = {0xB0u, 21u, 104u};    /* ~8 步/秒 */
    const uint8_t gate[3] = {0xB0u, 22u, 80u};
    synth_midi_in(on, 3); synth_midi_in(rate, 3); synth_midi_in(gate, 3);

    synth_note_on(60, 100); synth_note_on(64, 100); synth_note_on(67, 100);
    for (int b = 0; b < 375; ++b) synth_render_f32(buf.data(), SYNTH_BLOCK_FRAMES);   /* 1s */
    synth_stats_t st{};
    synth_get_stats(&st);
    printf("       1s 内触发 %u 次 note-on，声部峰值 %u\n", st.notes_on, st.peak_voices);
    CHECK(st.notes_on >= 6, "琶音器 1 秒应步进 ≥6 个音，实测 %u", st.notes_on);
    CHECK(st.peak_voices >= 1, "琶音器应有声部发声");

    synth_note_off(60); synth_note_off(64); synth_note_off(67);
    for (int b = 0; b < 200; ++b) synth_render_f32(buf.data(), SYNTH_BLOCK_FRAMES);   /* 0.53s */
    synth_get_stats(&st);
    CHECK(st.active_voices == 0, "松手后琶音器应停止，仍有 %u 声部", st.active_voices);
}

static void TestPatchText() {
    printf("[patch] .syn 文本解析/序列化\n");
    synth_patch_t p;
    patch_set_defaults(&p);
    const int applied = patch_from_text(
        "# 测试音色\n"
        "name = Test Pad\n"
        "osc1.model = supersaw\n"
        "osc2.model = 2\n"            /* 数字形式：pulse */
        "filter.type = lp12\n"
        "filter.cutoff = 200\n"
        "amp.a = 40\n"
        "lfo.dest = filter\n"
        "fx.chorus = 128\n"
        "osc2.semi = -12\n",
        &p);
    CHECK(applied == 9, "应应用 9 个字段，实测 %d", applied);
    CHECK(strcmp(p.name, "Test Pad") == 0, "音色名解析错误 '%s'", p.name);
    CHECK(p.osc1_model == SYNTH_OSC_SUPERSAW, "枚举名解析错误 %u", p.osc1_model);
    CHECK(p.osc2_model == SYNTH_OSC_PULSE, "数字枚举解析错误 %u", p.osc2_model);
    CHECK(p.filter_type == SYNTH_FILT_LP12, "滤波器枚举错误 %u", p.filter_type);
    CHECK(p.cutoff == 200 && p.amp_a == 40 && p.fx_chorus == 128, "数值字段错误");
    CHECK(p.osc2_semi == -12, "负值字段错误 %d", p.osc2_semi);

    /* 往返 */
    char buf[4096];
    const int len = patch_to_text(&p, buf, sizeof(buf));
    CHECK(len > 0, "序列化失败 %d", len);
    synth_patch_t q;
    patch_set_defaults(&q);
    const int applied2 = patch_from_text(buf, &q);
    CHECK(applied2 > 0, "回读失败 %d", applied2);
    CHECK(memcmp(&p, &q, sizeof(synth_patch_t)) == 0, "文本往返后音色不一致");

    /* 错误处理 */
    synth_patch_t bad;
    patch_set_defaults(&bad);
    CHECK(patch_from_text("no_equals_here\n", &bad) < 0, "缺少 = 应报错");
    CHECK(patch_from_text("unknown.key = 1\n", &bad) < 0, "未知键应报错");
    CHECK(patch_from_text("amp.a = 999\n", &bad) < 0, "越界值应报错");
    CHECK(patch_from_text("osc1.model = triangle\n", &bad) < 0, "未知枚举名应报错");
}

/* 阶段 2：DX7 FM 引擎（msfa，Apache-2.0） */
static void TestFmEngine() {
    printf("[fm] DX7 六运算器引擎\n");
    static ssynth::FmDx7Engine fm;                /* 6.4KB：放静态存储，避免栈开销 */
    fm.Init(48000.0f, 8);
    CHECK(fm.VoiceLoaded() == false, "初始应无音色");

    for (int v = 0; v < ssynth::kFmVoiceCount; ++v) {
        const uint8_t *voice = ssynth::FmVoice(v);
        fm.LoadVoice(voice);
        fm.Panic();
        CHECK(fm.VoiceLoaded(), "音色 %d 载入失败", v);

        fm.NoteOn(60, 100);                       /* C4 */
        std::vector<float> out(48000u * 2u, 0.0f);
        fm.Render(out.data(), 48000u);

        double peak = 0.0, sum = 0.0;
        bool nan = false;
        for (float x : out) { if (!(x == x)) nan = true; const double a = fabs(x); if (a > peak) peak = a; sum += (double)x * x; }
        const double rms = sqrt(sum / (double)out.size());
        /* 基频用峰值搜索（只看前 0.25s，衰减型音色也稳健） */
        std::vector<float> probe(out.begin(), out.begin() + 12000);
        double best_f = 0.0, best_m = 0.0;
        for (double f = 200.0; f <= 350.0; f += 1.0) {
            const double m = Goertzel(probe, f, 48000.0);
            if (m > best_m) { best_m = m; best_f = f; }
        }
        const double off = Goertzel(probe, 300.0, 48000.0);
        printf("       %-11s 峰值 %.4f RMS %.4f 基频峰值 %.1f Hz (%.4f) / 300Hz %.5f\n",
               ssynth::FmVoiceName(v), peak, rms, best_f, best_m, off);
        CHECK(!nan, "FM 输出出现 NaN");
        CHECK(peak > 0.01, "FM 输出过小（%s 峰值 %.4f）", ssynth::FmVoiceName(v), peak);
        CHECK(peak <= 1.0, "FM 输出削顶 %.4f", peak);
        CHECK(best_m > 0.0, "频谱应非空");

        uint8_t last_note = 60;
        /* 音高验证（只对持续型音色；打击型音色频谱峰值随八度漂移，属正常） */
        if (v < 2) {
        last_note = 72;
        fm.Panic();
        std::vector<float> oct(24000u * 2u, 0.0f);
        fm.NoteOn(60, 100);
        fm.Render(oct.data(), 24000u);
        double f_lo = 0.0, m_lo = 0.0;
        for (double f = 200.0; f <= 350.0; f += 1.0) { const double m = Goertzel(oct, f, 48000.0); if (m > m_lo) { m_lo = m; f_lo = f; } }
        fm.Panic();
        for (size_t i = 0; i < oct.size(); ++i) oct[i] = 0.0f;
        fm.NoteOn(72, 100);                      /* C5 */
        fm.Render(oct.data(), 24000u);
        double f_hi = 0.0, m_hi = 0.0;
        for (double f = 400.0; f <= 700.0; f += 1.0) { const double m = Goertzel(oct, f, 48000.0); if (m > m_hi) { m_hi = m; f_hi = f; } }
        const double ratio = (f_lo > 0.0) ? (f_hi / f_lo) : 0.0;
        printf("       音高缩放：C4 峰值 %.0f Hz → C5 峰值 %.0f Hz（比值 %.2f）\n", f_lo, f_hi, ratio);
        CHECK(ratio > 1.75 && ratio < 2.25, "八度应使频谱峰值翻倍（%.0f→%.0f）", f_lo, f_hi);
        }
        fm.NoteOff(last_note);
        std::vector<float> tail(48000u * 2u * 3u, 0.0f);
        fm.Render(tail.data(), 48000u * 3u);
        double tail_peak = 0.0;
        for (size_t i = tail.size() / 2u; i < tail.size(); ++i) { const double a = fabs(tail[i]); if (a > tail_peak) tail_peak = a; }
        printf("       释音 3s 后尾音峰值 %.6f，活跃声部 %u\n", tail_peak, fm.ActiveNotes());
        CHECK(tail_peak < 0.01, "FM 释音后应接近静音（%.6f）", tail_peak);
        CHECK(fm.ActiveNotes() == 0, "FM 声部应已释放（%u）", fm.ActiveNotes());
    }

    /* 复音与偷音 */
    fm.LoadVoice(ssynth::FmVoice(0));
    fm.Panic();
    for (int n = 0; n < 12; ++n) fm.NoteOn((uint8_t)(48 + n), 100);
    std::vector<float> poly(ssynth::kDx7Block * 2u * 2u, 0.0f);
    for (int b = 0; b < 20; ++b) fm.Render(poly.data(), ssynth::kDx7Block);
    printf("       12 音 → 活跃 %u，偷音 %u\n", fm.ActiveNotes(), fm.Steals());
    CHECK(fm.ActiveNotes() <= 8, "FM 声部数应 ≤8（实测 %u）", fm.ActiveNotes());
    CHECK(fm.Steals() > 0, "应有偷音");
    fm.Panic();
    CHECK(fm.ActiveNotes() == 0, "panic 后应无声部");

    /* 通过主引擎 API 播放 FM 音色（engine_type == 1 路由） */
    synth_config_t cfg{};
    cfg.sample_rate = SYNTH_SR_DEFAULT;
    cfg.max_voices = SYNTH_MAX_VOICES;
    cfg.factory_bank = synth_factory_bank;
    cfg.factory_bank_size = synth_factory_bank_size;
    synth_init(&cfg);
    synth_set_master_volume(0.8f);
    uint16_t fm_patch = 0;
    for (uint16_t i = 0; i < synth_factory_bank_size; ++i) {
        if (synth_factory_bank[i].engine_type == 1u) { fm_patch = i; break; }
    }
    printf("       FM 内置音色索引 = %u（%s）\n", fm_patch, synth_factory_bank[fm_patch].name);
    synth_set_patch(fm_patch);
    std::vector<float> ebuf(SYNTH_BLOCK_FRAMES * 2u, 0.0f);
    for (int b = 0; b < 10; ++b) synth_render_f32(ebuf.data(), SYNTH_BLOCK_FRAMES);   /* 应用音色 */
    synth_note_on(60, 100);
    double epeak = 0.0;
    for (int b = 0; b < 375; ++b) {                                                  /* 1 秒 */
        synth_render_f32(ebuf.data(), SYNTH_BLOCK_FRAMES);
        for (float x : ebuf) { CHECK(x == x, "引擎 FM 输出 NaN"); const double a = fabs(x); if (a > epeak) epeak = a; }
    }
    synth_note_off(60);
    for (int b = 0; b < 750; ++b) synth_render_f32(ebuf.data(), SYNTH_BLOCK_FRAMES);
    printf("       引擎 FM 峰值 %.4f\n", epeak);
    CHECK(epeak > 0.01, "引擎 FM 路由无输出（峰值 %.4f）", epeak);
    CHECK(epeak <= 1.0, "引擎 FM 输出削顶 %.4f", epeak);
    synth_panic();
}

/* 阶段 2：DX7 .syx（压缩 bank / 非压缩单音色） */
static void TestSysex() {
    printf("[sysex] DX7 .syx 解析\n");
    uint8_t vced[ssynth::kDx7VoiceBytes];
    memcpy(vced, ssynth::FmVoice(0), ssynth::kDx7VoiceBytes);

    /* 1) 压缩往返：VCED → 128B → VCED（前 155 字节应完全一致） */
    uint8_t packed[128];
    ssynth::PackVmemVoice(vced, packed);
    uint8_t back[ssynth::kDx7VoiceBytes];
    ssynth::UnpackVmemVoice(packed, back);
    int diff = 0;
    for (int i = 0; i < 155; ++i) if (vced[i] != back[i]) ++diff;
    printf("       压缩往返差异字节：%d / 155\n", diff);
    CHECK(diff == 0, "压缩往返不一致（%d 字节）", diff);

    /* 2) 构造 32 音色 bank dump（F0 43 00 09 20 00 + 32×128 + chk + F7） */
    static uint8_t bank[6 + 4096 + 2];
    bank[0] = 0xF0; bank[1] = 0x43; bank[2] = 0x00; bank[3] = 0x09; bank[4] = 0x20; bank[5] = 0x00;
    for (int v = 0; v < 32; ++v) memcpy(bank + 6 + v * 128, packed, 128);
    bank[6 + 4096] = 0x00; bank[7 + 4096] = 0xF7;

    static uint8_t voices[32][ssynth::kDx7VoiceBytes];
    const int n = ssynth::SysexToVoices(bank, sizeof(bank), &voices[0][0], 32);
    printf("       bank dump → 解析出 %d 个音色\n", n);
    CHECK(n == 32, "bank dump 应解析出 32 个音色（实测 %d）", n);
    int d2 = 0;
    for (int i = 0; i < 155; ++i) if (voices[0][i] != vced[i]) ++d2;
    CHECK(d2 == 0, "bank 解出的音色应与原音色一致（%d 字节不同）", d2);

    /* 3) 单音色非压缩 dump：F0 43 00 00 01 1B + 155 + chk + F7 */
    static uint8_t single[6 + 155 + 2];
    single[0] = 0xF0; single[1] = 0x43; single[2] = 0x00; single[3] = 0x00; single[4] = 0x01; single[5] = 0x1B;
    memcpy(single + 6, vced, 155);
    single[6 + 155] = 0x00; single[7 + 155] = 0xF7;
    const int n1 = ssynth::SysexToVoices(single, sizeof(single), &voices[0][0], 32);
    printf("       单音色 dump → 解析出 %d 个音色\n", n1);
    CHECK(n1 == 1, "单音色 dump 应解析出 1 个（实测 %d）", n1);
    int d3 = 0;
    for (int i = 0; i < 155; ++i) if (voices[0][i] != vced[i]) ++d3;
    CHECK(d3 == 0, "单音色解出应与原音色一致（%d 字节不同）", d3);

    /* 4) 引擎级：载入 bank 的第 3 个音色并播放 */
    synth_config_t cfg{};
    cfg.sample_rate = SYNTH_SR_DEFAULT;
    cfg.max_voices = SYNTH_MAX_VOICES;
    cfg.factory_bank = synth_factory_bank;
    cfg.factory_bank_size = synth_factory_bank_size;
    synth_init(&cfg);
    synth_set_master_volume(0.8f);
    const int loaded = synth_load_fm_sysex(bank, sizeof(bank), 3);
    CHECK(loaded == 32, "synth_load_fm_sysex 应返回 32（实测 %d）", loaded);

    std::vector<float> buf(SYNTH_BLOCK_FRAMES * 2u, 0.0f);
    for (int b = 0; b < 10; ++b) synth_render_f32(buf.data(), SYNTH_BLOCK_FRAMES);
    synth_note_on(60, 100);
    double peak = 0.0;
    for (int b = 0; b < 300; ++b) {
        synth_render_f32(buf.data(), SYNTH_BLOCK_FRAMES);
        for (float x : buf) { CHECK(x == x, "sysex 音色输出 NaN"); const double a = fabs(x); if (a > peak) peak = a; }
    }
    printf("       sysex 载入音色播放峰值 %.4f\n", peak);
    CHECK(peak > 0.01, "sysex 载入的音色应能发声（峰值 %.4f）", peak);
    synth_note_off(60);
    synth_panic();

    /* 5) 错误输入 */
    const uint8_t junk[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    CHECK(ssynth::SysexToVoices(junk, sizeof(junk), &voices[0][0], 32) < 0, "垃圾数据应报错");
}

/* 阶段 3：多样本 ROMpler */
static void TestRompler() {
    printf("[rom] 多样本 ROMpler（多音区 / 循环 / 重采样）\n");
    const double smp_sr = 24000.0;
    static int16_t smp1[14400];      /* 0.6s，220Hz 衰减正弦（root A3=57） */
    static int16_t smp2[14400];      /* 0.6s，330Hz 衰减正弦（root A4=69） */
    for (int i = 0; i < 14400; ++i) {
        const double t = (double)i / smp_sr;
        const double e = exp(-3.0 * t);
        smp1[i] = (int16_t)(sin(2.0 * M_PI * 220.0 * t) * e * 30000.0);
        smp2[i] = (int16_t)(sin(2.0 * M_PI * 330.0 * t) * e * 30000.0);
    }

    static synth_sample_instrument_t inst;
    memset(&inst, 0, sizeof(inst));
    inst.zone_count = 2;
    inst.volume = 0.8f;
    inst.zones[0].data = smp1; inst.zones[0].frames = 14400; inst.zones[0].rate = 24000;
    inst.zones[0].root = 57; inst.zones[0].low = 0;   inst.zones[0].high = 59;
    inst.zones[0].gain = 1.0f; inst.zones[0].loop = 1; inst.zones[0].loop_start = 2000; inst.zones[0].loop_end = 8000;
    inst.zones[1].data = smp2; inst.zones[1].frames = 14400; inst.zones[1].rate = 24000;
    inst.zones[1].root = 69; inst.zones[1].low = 60;  inst.zones[1].high = 127;
    inst.zones[1].gain = 1.0f;

    synth_config_t cfg{};
    cfg.sample_rate = SYNTH_SR_DEFAULT;
    cfg.max_voices = SYNTH_MAX_VOICES;
    cfg.factory_bank = synth_factory_bank;
    cfg.factory_bank_size = synth_factory_bank_size;
    synth_init(&cfg);
    synth_set_master_volume(0.8f);
    synth_set_sample_instrument(&inst);
    synth_set_engine_type(2);

    std::vector<float> buf(SYNTH_BLOCK_FRAMES * 2u, 0.0f);
    for (int b = 0; b < 10; ++b) synth_render_f32(buf.data(), SYNTH_BLOCK_FRAMES);

    auto pitch_of = [&](uint8_t note) {
        synth_note_on(note, 100);
        std::vector<float> out(188u * SYNTH_BLOCK_FRAMES, 0.0f);   /* 24064 帧，单声道连续存放 */
        for (int b = 0; b < 188; ++b) {                       /* 0.5s（步进 128 帧） */
            synth_render_f32(buf.data(), SYNTH_BLOCK_FRAMES);
            for (int i = 0; i < (int)SYNTH_BLOCK_FRAMES; ++i) {
                out[(size_t)b * SYNTH_BLOCK_FRAMES + i] = buf[(size_t)i * 2u];
            }
        }
        synth_note_off(note);
        for (int b = 0; b < 40; ++b) synth_render_f32(buf.data(), SYNTH_BLOCK_FRAMES);
        double bf = 0.0, bm = 0.0;
        for (double f = 80.0; f <= 900.0; f += 1.0) {
            const double m = Goertzel(out, f, 48000.0);
            if (m > bm) { bm = m; bf = f; }
        }
        return bf;
    };

    const double f57 = pitch_of(57);      /* 音区 1，根音 → 220Hz */
    const double f45 = pitch_of(45);      /* 音区 1，低八度 → 110Hz */
    const double f69 = pitch_of(69);      /* 音区 2，根音 → 330Hz */
    printf("       音高：zone1 根音 %.0fHz、下行八度 %.0fHz、zone2 根音 %.0fHz\n", f57, f45, f69);
    CHECK(fabs(f57 - 220.0) <= 5.0, "音区1 根音应为 220Hz（实测 %.0f）", f57);
    CHECK(fabs(f45 - 110.0) <= 5.0, "音区1 下行八度应为 110Hz（实测 %.0f）", f45);
    CHECK(fabs(f69 - 330.0) <= 5.0, "音区2 根音应为 330Hz（实测 %.0f）", f69);

    /* 循环：按住 3 秒后仍有声音（采样本身 0.6s 就衰减完了） */
    synth_note_on(57, 100);
    double tail_energy = 0.0;
    for (int b = 0; b < 1125; ++b) {                          /* 3s */
        synth_render_f32(buf.data(), SYNTH_BLOCK_FRAMES);
        if (b < 1050) continue;                               /* 只看最后 0.2s */
        for (float v : buf) { CHECK(v == v, "ROMpler 输出 NaN"); tail_energy += (double)v * v; }
    }
    printf("       循环后 0.2s 能量 %.6f\n", tail_energy);
    CHECK(tail_energy > 0.01, "循环应使声音持续（能量 %.6f）", tail_energy);

    /* 释音 */
    synth_note_off(57);
    for (int b = 0; b < 100; ++b) synth_render_f32(buf.data(), SYNTH_BLOCK_FRAMES);
    double rel = 0.0;
    for (float v : buf) { const double a = fabs(v); if (a > rel) rel = a; }
    printf("       释音 0.27s 后峰值 %.6f\n", rel);
    CHECK(rel < 0.01, "释音后应接近静音（%.6f）", rel);
    synth_panic();
}

/* 采样乐器清单（.ins）解析 + 多音区播放 */
static int16_t g_ins_smp_a[8000];
static int16_t g_ins_smp_b[8000];
static int FakeLoader(void *ctx, const char *path, const int16_t **out_data, uint32_t *out_frames, uint32_t *out_rate) {
    (void)ctx;
    for (int i = 0; i < 8000; ++i) {
        const double t = (double)i / 24000.0;
        g_ins_smp_a[i] = (int16_t)(sin(2.0 * M_PI * 220.0 * t) * exp(-2.0 * t) * 28000.0);
        g_ins_smp_b[i] = (int16_t)(sin(2.0 * M_PI * 330.0 * t) * exp(-2.0 * t) * 28000.0);
    }
    if (strstr(path, "low") != nullptr)      { *out_data = g_ins_smp_a; *out_frames = 8000; *out_rate = 24000; return 0; }
    if (strstr(path, "high") != nullptr)     { *out_data = g_ins_smp_b; *out_frames = 8000; *out_rate = 24000; return 0; }
    return -1;
}

static void TestInstrumentText() {
    printf("[ins] 采样乐器清单解析\n");
    const char *manifest =
        "# 双音区钢琴\n"
        "name = Two Zone Piano\n"
        "volume = 200\n"
        "zone0.sample = piano_low.wav\n"
        "zone0.root = 45\n"
        "zone0.low = 0\n"
        "zone0.high = 59\n"
        "zone0.loop = 1\n"
        "zone0.loop_start = 1000\n"
        "zone0.loop_end = 4000\n"
        "zone1.sample = piano_high.wav\n"
        "zone1.root = 69\n"
        "zone1.low = 60\n"
        "zone1.high = 127\n"
        "zone1.loop = 0\n";

    static synth_sample_instrument_t inst;
    const int zones = instrument_from_text(manifest, &inst, FakeLoader, nullptr);
    printf("       音区数 %d，名字 '%s'，音量 %.3f\n", zones, inst.name, inst.volume);
    CHECK(zones == 2, "应解析出 2 个音区（实测 %d）", zones);
    CHECK(strcmp(inst.name, "Two Zone Piano") == 0, "名字解析错误 '%s'", inst.name);
    CHECK(inst.zone_count == 2, "zone_count 错误 %u", inst.zone_count);
    CHECK(inst.zones[0].root == 45 && inst.zones[1].root == 69, "root 解析错误");
    CHECK(inst.zones[0].low == 0 && inst.zones[0].high == 59, "音区范围解析错误");
    CHECK(inst.zones[0].loop == 1 && inst.zones[0].loop_start == 1000 && inst.zones[0].loop_end == 4000, "循环点解析错误");
    CHECK(inst.zones[1].loop == 0, "zone1 不应循环");
    CHECK(inst.zones[0].data == g_ins_smp_a && inst.zones[1].data == g_ins_smp_b, "采样未按 zone 分别载入");
    CHECK(inst.zones[0].frames == 8000 && inst.zones[0].rate == 24000, "采样元数据错误");

    /* 往返 */
    char tbuf[1024];
    const char *paths[2] = {"piano_low.wav", "piano_high.wav"};
    const int len = instrument_to_text(&inst, paths, tbuf, sizeof(tbuf));
    CHECK(len > 0, "序列化失败 %d", len);
    static synth_sample_instrument_t inst2;
    const int zones2 = instrument_from_text(tbuf, &inst2, FakeLoader, nullptr);
    CHECK(zones2 == 2, "回读音区数错误 %d", zones2);
    CHECK(inst2.zones[1].root == 69 && inst2.zones[1].low == 60 && inst2.zones[1].high == 127, "往返后 zone1 参数不一致");

    /* 错误处理 */
    synth_sample_instrument_t bad;
    CHECK(instrument_from_text("zone0.foo = 1\n", &bad, nullptr, nullptr) < 0, "未知字段应报错");
    CHECK(instrument_from_text("zone9.root = 60\n", &bad, nullptr, nullptr) < 0, "音区越界应报错");
    CHECK(instrument_from_text("zone0.root = 999\n", &bad, nullptr, nullptr) < 0, "数值越界应报错");
    CHECK(instrument_from_text("bad line without equals\n", &bad, nullptr, nullptr) < 0, "缺 = 应报错");
    CHECK(instrument_from_text("zone0.sample = missing.wav\n", &bad, FakeLoader, nullptr) < 0, "采样载入失败应报错");

    /* 引擎播放：低音区用 zone0（220Hz 采样 root=A2=45） */
    synth_config_t cfg{};
    cfg.sample_rate = SYNTH_SR_DEFAULT;
    cfg.max_voices = SYNTH_MAX_VOICES;
    cfg.factory_bank = synth_factory_bank;
    cfg.factory_bank_size = synth_factory_bank_size;
    synth_init(&cfg);
    synth_set_master_volume(0.8f);
    synth_set_sample_instrument(&inst);
    synth_set_engine_type(2);
    std::vector<float> buf(SYNTH_BLOCK_FRAMES * 2u, 0.0f);
    for (int b = 0; b < 10; ++b) synth_render_f32(buf.data(), SYNTH_BLOCK_FRAMES);

    auto pitch = [&](uint8_t note) {
        synth_note_on(note, 100);
        std::vector<float> out(188u * SYNTH_BLOCK_FRAMES, 0.0f);
        for (int b = 0; b < 188; ++b) {
            synth_render_f32(buf.data(), SYNTH_BLOCK_FRAMES);
            for (int i = 0; i < (int)SYNTH_BLOCK_FRAMES; ++i) out[(size_t)b * SYNTH_BLOCK_FRAMES + i] = buf[(size_t)i * 2u];
        }
        synth_note_off(note);
        for (int b = 0; b < 40; ++b) synth_render_f32(buf.data(), SYNTH_BLOCK_FRAMES);
        double bf = 0.0, bm = 0.0;
        for (double f = 80.0; f <= 900.0; f += 1.0) { const double m = Goertzel(out, f, 48000.0); if (m > bm) { bm = m; bf = f; } }
        return bf;
    };
    const double f45 = pitch(45);      /* zone0 根音 → 220Hz */
    const double f69 = pitch(69);      /* zone1 根音 → 330Hz */
    printf("       zone0 根音 %.0fHz、zone1 根音 %.0fHz\n", f45, f69);
    CHECK(fabs(f45 - 220.0) <= 5.0, "zone0 应为 220Hz（实测 %.0f）", f45);
    CHECK(fabs(f69 - 330.0) <= 5.0, "zone1 应为 330Hz（实测 %.0f）", f69);
    synth_panic();
}

/* 循环接缝交叉淡化：接缝处故意做相位不连续，量测相邻样点跳变 */
static void TestLoopCrossfade() {
    printf("[rom] 循环接缝交叉淡化 / 三次插值\n");
    static int16_t smp[12000];
    for (int i = 0; i < 12000; ++i) smp[i] = 0;
    /* 全零采样，只在"循环末尾前一点"放一个 −0.49 尖峰：
       无交叉淡化 → 尖峰全幅出现并在回绕处骤然消失（跳变 ≈0.49）；
       有交叉淡化 → 该点落在淡化区，尖峰被淡出（跳变应远小于 0.05）。 */
    smp[8499] = (int16_t)-16000;
    static synth_sample_instrument_t inst;
    auto setup = [&](uint32_t crossfade, uint8_t interp) {
        memset(&inst, 0, sizeof(inst));
        inst.zone_count = 1;
        inst.volume = 0.8f;
        inst.zones[0].data = smp; inst.zones[0].frames = 12000; inst.zones[0].rate = 24000;
        inst.zones[0].root = 57; inst.zones[0].low = 0; inst.zones[0].high = 127;
        inst.zones[0].gain = 1.0f;
        inst.zones[0].loop = 1; inst.zones[0].loop_start = 3000; inst.zones[0].loop_end = 8500;
        inst.zones[0].crossfade = crossfade;
        inst.zones[0].interp = interp;
    };

    synth_config_t cfg{};
    cfg.sample_rate = SYNTH_SR_DEFAULT;
    cfg.max_voices = SYNTH_MAX_VOICES;
    cfg.factory_bank = synth_factory_bank;
    cfg.factory_bank_size = synth_factory_bank_size;
    synth_init(&cfg);
    synth_set_master_volume(0.8f);
    synth_set_engine_type(2);
    std::vector<float> buf(SYNTH_BLOCK_FRAMES * 2u, 0.0f);

    auto max_jump = [&](uint32_t crossfade, uint8_t interp) {
        synth_panic();
        setup(crossfade, interp);
        synth_set_sample_instrument(&inst);
        for (int b = 0; b < 20; ++b) synth_render_f32(buf.data(), SYNTH_BLOCK_FRAMES);
        synth_note_on(57, 100);        /* 原速：循环 5500 点 → 输出每 11000 点接一次缝 */
        double prev = 0.0, worst = 0.0;
        for (int b = 0; b < 700; ++b) {                  /* 约 1.9 秒 */
            synth_render_f32(buf.data(), SYNTH_BLOCK_FRAMES);
            for (int i = 0; i < (int)SYNTH_BLOCK_FRAMES; ++i) {
                const double x = buf[(size_t)i * 2u];
                if (b > 5) { const double d = fabs(x - prev); if (d > worst) worst = d; }
                prev = x;
            }
        }
        synth_note_off(57);
        for (int b = 0; b < 60; ++b) synth_render_f32(buf.data(), SYNTH_BLOCK_FRAMES);
        return worst;
    };

    const double no_xfade = max_jump(0u, 1u);
    const double with_xfade = max_jump(256u, 1u);
    const double cf2048 = max_jump(2048u, 1u);
    const double linear_only = max_jump(0u, 0u);
    printf("       最大相邻跳变：cf=0 %.5f / cf=256 %.5f / cf=2048 %.5f / 线性(cf=0) %.5f\n",
           no_xfade, with_xfade, cf2048, linear_only);
    printf("       [dbg] sizeof(zone)=%u sizeof(instr)=%u 尖峰位于采样 8499（循环 3000..8500）\n",
           (unsigned)sizeof(synth_sample_zone_t), (unsigned)sizeof(synth_sample_instrument_t));
    CHECK(with_xfade < no_xfade * 0.5, "引擎路径下交叉淡化应减小跳变（%.5f vs %.5f）", with_xfade, no_xfade);
    CHECK(no_xfade <= 1.0 && with_xfade <= 1.0 && cf2048 <= 1.0 && linear_only <= 1.0, "输出应落在 ±1 内");
    CHECK(no_xfade == no_xfade && with_xfade == with_xfade, "不应出现 NaN");
    synth_panic();
}

/* 直接驱动 Rompler（绕过引擎请求槽），确认交叉淡化分支是否命中 */
static void TestCrossfadeDirect() {
    printf("[rom] 直接驱动 Rompler 的交叉淡化诊断\n");
    static int16_t smp[12000];
    for (int i = 0; i < 12000; ++i) smp[i] = 0;
    smp[8499] = (int16_t)-16000;

    static synth_sample_instrument_t inst;
    static ssynth::Rompler rom;
    rom.Init(48000.0f, 8);

    auto run = [&](uint32_t cf) {
        memset(&inst, 0, sizeof(inst));
        inst.zone_count = 1; inst.volume = 0.8f;
        inst.zones[0].data = smp; inst.zones[0].frames = 12000; inst.zones[0].rate = 24000;
        inst.zones[0].root = 57; inst.zones[0].low = 0; inst.zones[0].high = 127;
        inst.zones[0].gain = 1.0f; inst.zones[0].interp = 1;
        inst.zones[0].loop = 1; inst.zones[0].loop_start = 3000; inst.zones[0].loop_end = 8500;
        inst.zones[0].crossfade = cf;
        rom.Panic();
        rom.SetInstrument(&inst);
        rom.NoteOn(57, 100);
        std::vector<float> b(SYNTH_BLOCK_FRAMES * 2u, 0.0f);
        double prev = 0.0, worst = 0.0, peak = 0.0;
        long worst_idx = -1;
        for (int blk = 0; blk < 700; ++blk) {
            for (size_t i = 0; i < b.size(); ++i) b[i] = 0.0f;
            rom.Render(b.data(), SYNTH_BLOCK_FRAMES);
            for (int i = 0; i < (int)SYNTH_BLOCK_FRAMES; ++i) {
                const double x = b[(size_t)i * 2u];
                const double ax = fabs(x);
                if (ax > peak) peak = ax;
                if (blk > 3) {
                    const double d = fabs(x - prev);
                    if (d > worst) { worst = d; worst_idx = (long)blk * 128 + i; }
                }
                prev = x;
            }
        }
        printf("       cf=%-5u 命中 %-6u 峰值 %.5f 最大跳变 %.5f (idx=%ld)\n",
               cf, rom.CrossfadeHits(), peak, worst, worst_idx);
        return worst;
    };
    const double a = run(0u);
    const double b2 = run(256u);
    CHECK(b2 < a * 0.3, "交叉淡化应把循环末尾尖峰淡出（%.5f vs %.5f）", b2, a);
    CHECK(a > 0.1, "无交叉淡化时尖峰应可见（%.5f，对照有效性）", a);
    CHECK(a <= 1.0 && b2 <= 1.0, "输出应落在 ±1 内");
    rom.Panic();
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);      /* 崩溃时也能看到已打印的输出 */
    printf("=== STM32 synth 宿主端单元测试 ===\n");
    TestOscillator();
    TestFilter();
    TestEnvelope();
    TestMidiParser();
    TestFx();
    TestEngine();
    TestFxInEngine();
    TestArp();
    TestPatchText();
    TestFmEngine();
    TestSysex();
    TestRompler();
    TestInstrumentText();
    TestLoopCrossfade();
    TestCrossfadeDirect();
    if (g_fail == 0) printf("\n全部通过 ✔\n");
    else printf("\n失败 %d 项 ✘\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
