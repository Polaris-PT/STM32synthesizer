/* 离线渲染：把一段内置演示（含和弦/旋律/换音色/弯音/延音）渲染成 WAV。
   用法：ssynth_render [--out file.wav] [--patch N] [--seconds S] [--note N] [--list] */
#include "wav_writer.h"
#include "synth/synth.h"
#include "synth/pcm.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

enum EvType { EV_NOTE_ON = 0, EV_NOTE_OFF, EV_CC, EV_PC, EV_BEND };
struct Ev { float t; uint8_t type; uint8_t a; uint8_t b; };

void AddNote(std::vector<Ev> &v, float t, float dur, uint8_t note, uint8_t vel) {
    v.push_back({t, EV_NOTE_ON, note, vel});
    v.push_back({t + dur, EV_NOTE_OFF, note, 0});
}

/* 8 小节 @120BPM 的演示：C - Am - F - G，右手旋律 + 换音色 + 延音 + 弯音 */
std::vector<Ev> BuildDemo(int patch) {
    std::vector<Ev> ev;
    const float bar = 2.0f;                       /* 120 BPM 4/4 */
    const uint8_t chords[4][4] = {
        {48, 60, 64, 67},   /* C  */
        {45, 57, 60, 64},   /* Am */
        {41, 53, 57, 60},   /* F  */
        {43, 55, 59, 62},   /* G  */
    };
    ev.push_back({0.0f, EV_PC, (uint8_t)patch, 0});

    for (int rep = 0; rep < 2; ++rep) {
        for (int c = 0; c < 4; ++c) {
            const float t0 = (float)(rep * 4 + c) * bar;
            for (int n = 0; n < 4; ++n) AddNote(ev, t0 + 0.02f * (float)n, bar * 0.92f, chords[c][n], 90);
            /* 旋律：每半小节两个音 */
            const uint8_t scale[8] = {72, 74, 76, 79, 77, 76, 74, 72};
            for (int m = 0; m < 4; ++m) {
                const uint8_t note = scale[(c * 3 + m) & 7];
                AddNote(ev, t0 + (float)m * (bar * 0.25f), bar * 0.22f, note, (uint8_t)(70 + m * 10));
            }
        }
        /* 第二遍换音色 + 延音踏板示范 */
        if (rep == 0) {
            ev.push_back({4.0f * bar - 0.05f, EV_CC, 64, 0});      /* 踏板抬起 */
            ev.push_back({4.0f * bar, EV_PC, (uint8_t)((patch + 6) % 12), 0});   /* Super Saw：带合唱+延迟 */
        }
    }
    /* 结尾：弯音 + 长和弦 */
    const float tend = 8.0f * bar;
    for (int n = 0; n < 4; ++n) AddNote(ev, tend, 2.0f, chords[0][n] + 12, 100);
    ev.push_back({tend + 0.4f, EV_BEND, 0x00, 0x50});               /* 上弯 */
    ev.push_back({tend + 0.9f, EV_BEND, 0x00, 0x40});               /* 回中 */
    ev.push_back({tend + 1.3f, EV_CC, 64, 127});                    /* 踏板踩下 */
    ev.push_back({tend + 1.6f, EV_CC, 64, 0});                      /* 释放 */
    return ev;
}

void Emit(Ev &e) {
    switch (e.type) {
        case EV_NOTE_ON:  synth_note_on(e.a, e.b); break;
        case EV_NOTE_OFF: synth_note_off(e.a); break;
        case EV_CC:       { const uint8_t m[3] = {0xB0u, e.a, e.b}; synth_midi_in(m, 3); } break;
        case EV_PC:       { const uint8_t m[2] = {0xC0u, e.a};      synth_midi_in(m, 2); } break;
        case EV_BEND:     { const uint8_t m[3] = {0xE0u, e.a, e.b};  synth_midi_in(m, 3); } break;
        default: break;
    }
}

}  // namespace

int main(int argc, char **argv) {
    const char *out = "demo.wav";
    int   patch = 0;
    float seconds = -1.0f;
    int   single_note = -1;
    bool  list = false;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--patch") && i + 1 < argc) patch = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--note") && i + 1 < argc) single_note = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--list")) list = true;
        else { printf("unknown arg: %s\n", argv[i]); return 2; }
    }

    if (list) {
        for (uint16_t i = 0; i < synth_factory_bank_size; ++i) {
            printf("%2u  %-14s osc1=%u osc2=%u filt=%u\n", i, synth_factory_bank[i].name,
                   synth_factory_bank[i].osc1_model, synth_factory_bank[i].osc2_model,
                   synth_factory_bank[i].filter_type);
        }
        return 0;
    }

    synth_config_t cfg{};
    cfg.sample_rate      = SYNTH_SR_DEFAULT;
    cfg.max_voices       = SYNTH_MAX_VOICES;
    cfg.factory_bank     = synth_factory_bank;
    cfg.factory_bank_size= synth_factory_bank_size;
    if (synth_init(&cfg) != 0) { printf("synth_init failed\n"); return 1; }
    synth_set_master_volume(0.8f);

    std::vector<Ev> events;
    float total = 0.0f;
    if (single_note >= 0) {
        AddNote(events, 0.05f, 2.0f, (uint8_t)single_note, 100);
        synth_set_patch((uint16_t)patch);
        total = seconds > 0.0f ? seconds : 3.0f;
    } else {
        events = BuildDemo(patch);
        total = seconds > 0.0f ? seconds : 20.0f;
    }

    WavWriter wav;
    if (!wav.Open(out, SYNTH_SR_DEFAULT)) { printf("cannot open %s\n", out); return 1; }

    const uint32_t block = SYNTH_BLOCK_FRAMES;
    std::vector<float> buf(block * 2u);
    const uint32_t total_frames = (uint32_t)(total * (float)SYNTH_SR_DEFAULT);
    uint32_t done = 0;
    size_t   next_ev = 0;

    const auto t_start = std::chrono::steady_clock::now();
    while (done < total_frames) {
        const uint32_t n = ((total_frames - done) > block) ? block : (total_frames - done);
        const float t_now = (float)done / (float)SYNTH_SR_DEFAULT;
        while (next_ev < events.size() && events[next_ev].t <= t_now) { Emit(events[next_ev]); ++next_ev; }
        synth_render_f32(buf.data(), n);
        wav.WriteFloat(buf.data(), n);
        done += n;
        if (((done / block) % 200u) == 0u) synth_tick_1ms();
    }
    const auto t_end = std::chrono::steady_clock::now();
    wav.Close();

    synth_stats_t st{};
    synth_get_stats(&st);
    const double secs = std::chrono::duration<double>(t_end - t_start).count();
    const double audio_secs = (double)done / (double)SYNTH_SR_DEFAULT;
    printf("== 离线渲染 %s ==\n", out);
    printf("  音频长度      : %.2f s (%u 帧, 48kHz 16bit 立体声)\n", audio_secs, done);
    printf("  峰值 / RMS    : %.4f / %.4f\n", wav.Peak(), sqrt(wav.SumSquares() / (double)(done * 2u)));
    printf("  声部峰值      : %u / %u\n", st.peak_voices, (unsigned)SYNTH_MAX_VOICES);
    printf("  偷音次数      : %u\n", st.steals);
    printf("  NoteOn 次数   : %u\n", st.notes_on);
    printf("  MIDI 字节/丢弃: %u / %u\n", st.midi_bytes, st.midi_dropped);
    printf("  未知 CC       : %u\n", st.unknown_cc);
    printf("  宿主渲染耗时  : %.3f s (实时率 %.1fx)\n", secs, audio_secs / (secs > 0.0 ? secs : 1.0));
    return 0;
}
