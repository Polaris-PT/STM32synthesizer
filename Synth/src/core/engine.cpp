/* 合成器引擎：复音分配、MIDI 分发、总线处理、渲染。 */
#include "synth/synth.h"
#include "synth/pcm.h"

#include <math.h>
#include <string.h>

#include "dsp_util.h"
#include "wavetable.h"
#include "voice.h"
#include "../fx/fx_chain.h"
#include "../fx/fx_memory.h"
#include "../app/arp.h"
#include "../engines/fm_dx7.h"
#include "../engines/fm_voices.h"
#include "../engines/sysex.h"
#include "rompler.h"
#include "../midi/midi_parser.h"

using namespace ssynth;

namespace {

const synth_platform_hooks_t *g_hooks = nullptr;

inline uint32_t CyclesNow() {
    return (g_hooks != nullptr && g_hooks->cycles_now != nullptr) ? g_hooks->cycles_now() : 0u;
}
inline uint32_t CpuHz() {
    return (g_hooks != nullptr && g_hooks->cpu_hz != nullptr) ? g_hooks->cpu_hz() : 0u;
}

constexpr uint32_t kMidiRingSize   = 256u;
constexpr uint32_t kMidiRingMask   = kMidiRingSize - 1u;
constexpr uint32_t kStealFadeFrame = 96u;    /* 2 ms @48k */

struct PendingNote { uint8_t valid; uint8_t note; uint8_t vel; };

struct Engine {
    bool     initialized = false;
    float    sr          = 48000.0f;
    uint8_t  max_voices  = SYNTH_MAX_VOICES;

    const synth_patch_t *bank = nullptr;
    uint16_t bank_size   = 0;
    uint16_t patch_index = 0;
    synth_patch_t patch{};

    GlobalParams g;
    FxChain      fx;
    Arp          arp;
    ssynth::FmDx7Engine fm;              /* 阶段 2：DX7 FM 声部（engine_type == 1 时使用） */
    ssynth::Rompler rom;                 /* 阶段 3：多样本 ROMpler（engine_type == 2 时使用） */
    const synth_sample_instrument_t * volatile req_instrument = nullptr;
    volatile uint32_t req_instrument_clear = 0u;
    int16_t      fm_loaded = -1;         /* 已载入的 FM 音色号（-1=无） */
    uint8_t      fm_ram_voice[156];      /* synth_load_fm_sysex() 载入的 RAM 音色 */
    bool         fm_ram_valid = false;
    Voice        voices[SYNTH_MAX_VOICES];
    PendingNote  pending[SYNTH_MAX_VOICES];
    int8_t       note_voice[128];
    bool         note_held[128];
    uint32_t     age_counter = 0;
    bool         sustain = false;
    bool         mono_mode = false;
    int8_t       chan_filter = -1;

    MidiParser   parser;
    synth_stats_t stats{};

    /* MIDI 环形缓冲：USB 回调写 head，音频块读 tail（SPSC） */
    volatile uint32_t ring_head = 0u;
    volatile uint32_t ring_tail = 0u;
    uint8_t ring[kMidiRingSize];

    /* 控制线程 → 音频线程请求 */
    volatile int32_t      req_patch_index = -1;
    const synth_patch_t * volatile req_patch_ptr = nullptr;
    volatile uint32_t     req_panic_flag = 0u;
    volatile uint32_t     req_all_off_flag = 0u;
    volatile float        req_master_vol = -1.0f;

    /* 总线 */
    float dc_x1[2] = {0.0f, 0.0f}, dc_y1[2] = {0.0f, 0.0f};
    float cpu_permille = 0.0f;

    float block_buf[SYNTH_BLOCK_FRAMES * 2u];
};

Engine g_eng;

void ClearNoteState(Engine &e) {
    for (int i = 0; i < 128; ++i) { e.note_voice[i] = -1; e.note_held[i] = false; }
}

void SilenceAll(Engine &e) {
    for (uint32_t v = 0; v < e.max_voices; ++v) { e.voices[v].Kill(); e.pending[v].valid = 0; }
    ClearNoteState(e);
    e.arp.AllNotesOff();
    e.fm.Panic();
    e.rom.Panic();
}

void ApplyPatchIndex(Engine &e, uint16_t idx) {
    if (e.bank != nullptr && idx < e.bank_size) {
        e.patch = e.bank[idx];
        e.patch_index = idx;
        e.stats.patch_index = idx;
    }
}

void StartVoice(Engine &e, int vi, uint8_t note, uint8_t vel, bool retrigger, bool glide) {
    Voice &v = e.voices[vi];
    const float prev = v.Hz();
    v.NoteOn(note, vel, e.patch, retrigger, prev, glide);
    v.SetAge(++e.age_counter);
    e.note_voice[note] = (int8_t)vi;
    e.note_held[note]  = true;
    e.stats.notes_on++;
}

/* 分配策略：同音重触发 → 空闲 → 释音中最安静 → 最安静 → 最旧 */
int AllocateVoice(Engine &e, uint8_t note) {
    if (e.note_voice[note] >= 0 && e.voices[e.note_voice[note]].Active()) return e.note_voice[note];

    for (uint32_t v = 0; v < e.max_voices; ++v) {
        if (!e.voices[v].Active()) return (int)v;
    }
    int   best = -1;
    float best_level = 2.0f;
    for (uint32_t v = 0; v < e.max_voices; ++v) {
        if (e.voices[v].Releasing() && e.voices[v].EnvLevel() < best_level) {
            best_level = e.voices[v].EnvLevel();
            best = (int)v;
        }
    }
    if (best >= 0) return best;

    best_level = 2.0f;
    for (uint32_t v = 0; v < e.max_voices; ++v) {
        if (e.voices[v].EnvLevel() < best_level) { best_level = e.voices[v].EnvLevel(); best = (int)v; }
    }
    return best;
}

/* ---- 直接驱动声部（琶音器也走这里） ---- */
void VoiceNoteOn(Engine &e, uint8_t note, uint8_t vel);
void VoiceNoteOff(Engine &e, uint8_t note);

void NoteOnInternal(Engine &e, uint8_t note, uint8_t vel) {
    if (e.arp.Enabled()) { e.arp.NoteOn(note, vel); return; }
    VoiceNoteOn(e, note, vel);
}

void NoteOffInternal(Engine &e, uint8_t note) {
    if (e.arp.Enabled()) { e.arp.NoteOff(note); return; }
    VoiceNoteOff(e, note);
}

void EnsureFmVoice(Engine &e) {
    if (e.patch.engine_type != 1u) return;
    const int16_t want = (int16_t)e.patch.fm_voice;
    if (e.fm_loaded == want) return;
    if (e.patch.fm_voice == 0xFFu) {
        if (e.fm_ram_valid) { e.fm.LoadVoice(e.fm_ram_voice); e.fm_loaded = want; }
        return;
    }
    e.fm.LoadVoice(ssynth::FmVoice((int)want));
    e.fm_loaded = want;
}

void VoiceNoteOn(Engine &e, uint8_t note, uint8_t vel) {
    if (note > 127u) return;
    if (e.patch.engine_type == 2u) { e.rom.NoteOn(note, vel); e.stats.notes_on++; return; }
    if (e.patch.engine_type == 1u) { EnsureFmVoice(e); e.fm.NoteOn(note, vel); e.stats.notes_on++; return; }
    if (e.mono_mode) {
        Voice &v = e.voices[0];
        const bool was_active = v.Active();
        v.NoteOn(note, vel, e.patch, !was_active ? true : false, v.Hz(), was_active);
        v.SetAge(++e.age_counter);
        e.note_voice[note] = 0;
        e.note_held[note]  = true;
        e.stats.notes_on++;
        return;
    }
    const int vi = AllocateVoice(e, note);
    if (vi < 0) return;

    Voice &v = e.voices[vi];
    if (!v.Active()) {
        StartVoice(e, vi, note, vel, true, false);
    } else if (v.Note() == note) {
        StartVoice(e, vi, note, vel, true, true);      /* 同音重触发 */
    } else {
        v.BeginSteal(kStealFadeFrame);                 /* 偷音：先淡出，再起新音 */
        e.pending[vi].valid = 1;
        e.pending[vi].note  = note;
        e.pending[vi].vel   = vel;
        e.stats.steals++;
    }
}

void VoiceNoteOff(Engine &e, uint8_t note) {
    if (e.patch.engine_type == 2u) { e.rom.NoteOff(note); return; }
    if (e.patch.engine_type == 1u) { e.fm.NoteOff(note); return; }
    e.note_held[note] = false;
    const int vi = e.note_voice[note];
    if (vi < 0) return;
    if (e.sustain) return;                             /* 延音踏板：留到踏板释放 */
    e.voices[vi].NoteOff();
    e.note_voice[note] = -1;
}

void ReleaseSustained(Engine &e) {
    for (int n = 0; n < 128; ++n) {
        if (!e.note_held[n] && e.note_voice[n] >= 0) {
            e.voices[e.note_voice[n]].NoteOff();
            e.note_voice[n] = -1;
        }
    }
}

void AllNotesOff(Engine &e) {
    for (uint32_t v = 0; v < e.max_voices; ++v) {
        if (e.voices[v].Active()) e.voices[v].NoteOff();
    }
    for (int n = 0; n < 128; ++n) { e.note_held[n] = false; e.note_voice[n] = -1; }
}

void ResetControllers(Engine &e) {
    e.g.cutoff_mod = 0.0f;
    e.g.res_mod = 0.0f;
    e.g.lfo_depth_mod = 0.0f;
    e.g.bend = 0.0f;
    e.sustain = false;
    e.stats.sustain = 0;
    ReleaseSustained(e);
}

void ControlChange(Engine &e, uint8_t cc, uint8_t val) {
    const float v01 = (float)val * (1.0f / 127.0f);
    switch (cc) {
        case 1:   e.g.lfo_depth_mod = v01; break;
        case 2:   e.g.cutoff_mod = v01 * 1.5f - 0.25f; break;
        case 5:   e.patch.glide = val; break;
        case 7:   e.g.master_vol = v01; break;
        case 64:
            e.sustain = (val >= 64);
            e.stats.sustain = e.sustain ? 1u : 0u;
            if (!e.sustain) ReleaseSustained(e);
            break;
        case 65:  e.mono_mode = (val >= 64); e.stats.mono_mode = e.mono_mode ? 1u : 0u; break;
        case 71:  e.g.res_mod = v01 * 1.5f; break;
        case 72:  e.patch.amp_a = val; break;
        case 73:  e.patch.amp_r = val; break;
        case 74:  e.g.cutoff_mod = v01 * 2.0f - 0.5f; break;
        case 76:  e.patch.lfo_rate = val; break;
        case 77:  e.patch.lfo_depth = val; break;
        case 79:  e.patch.volume = val; break;
        case 20:  e.arp.SetMode(val == 0 ? (uint8_t)Arp::kOff : (uint8_t)((val <= 5) ? val : 5)); break;
        case 21:  e.arp.SetRateHz(0.5f * powf(30.0f, v01)); break;
        case 22:  e.arp.SetGate(v01); break;
        case 23:  e.arp.SetOctaves((uint8_t)(1 + (val * 3) / 127)); break;
        case 120: SilenceAll(e); break;
        case 121: ResetControllers(e); break;
        case 123: AllNotesOff(e); break;
        default:  e.stats.unknown_cc++; break;
    }
}

void HandleMessage(Engine &e, uint8_t status, uint8_t d1, uint8_t d2) {
    const uint8_t type = status & 0xF0u;
    const uint8_t chan = status & 0x0Fu;
    if (e.chan_filter >= 0 && (int8_t)chan != e.chan_filter) return;
    switch (type) {
        case 0x90u: if (d2 == 0u) NoteOffInternal(e, d1); else NoteOnInternal(e, d1, d2); break;
        case 0x80u: NoteOffInternal(e, d1); break;
        case 0xB0u: ControlChange(e, d1, d2); break;
        case 0xC0u: e.req_patch_index = (int32_t)d1; break;
        case 0xE0u: e.g.bend = ((float)(((uint16_t)d2 << 7) | d1) - 8192.0f) * (1.0f / 8192.0f); break;
        default: break;
    }
}

void DrainMidi(Engine &e) {
    while (e.ring_tail != e.ring_head) {
        const uint8_t b = e.ring[e.ring_tail];
        e.ring_tail = (e.ring_tail + 1u) & kMidiRingMask;
        if (b == 0xF8u) { e.arp.OnMidiClock(); continue; }   /* MIDI 时钟：琶音器同步用 */
        uint8_t st = 0, d1 = 0, d2 = 0;
        if (e.parser.Feed(b, &st, &d1, &d2)) HandleMessage(e, st, d1, d2);
    }
    e.stats.sysex_dropped = e.parser.dropped_sysex;
}

void RenderBlock(Engine &e, float *out, uint32_t frames) {
    const uint32_t cyc0 = CyclesNow();

    /* 取消/复位请求 */
    if (e.req_panic_flag)      { e.req_panic_flag = 0u; SilenceAll(e); }
    if (e.req_all_off_flag)    { e.req_all_off_flag = 0u; AllNotesOff(e); }
    const int32_t rp = e.req_patch_index;
    if (rp >= 0) { e.req_patch_index = -1; ApplyPatchIndex(e, (uint16_t)rp); }
    const synth_patch_t *rpp = e.req_patch_ptr;
    if (rpp != nullptr) { e.req_patch_ptr = nullptr; e.patch = *rpp; e.patch_index = 0xFFFFu; }
    const float rmv = e.req_master_vol;
    if (rmv >= 0.0f) { e.req_master_vol = -1.0f; e.g.master_vol = clampf(rmv, 0.0f, 1.0f); }

    /* 阶段 3：乐器更换请求 */
    if (e.req_instrument_clear) { e.req_instrument_clear = 0u; e.rom.SetInstrument(nullptr); }
    const synth_sample_instrument_t *rin = e.req_instrument;
    if (rin != nullptr) { e.req_instrument = nullptr; e.rom.SetInstrument(rin); }

    DrainMidi(e);

    /* 琶音器步进（块级调度） */
    {
        Arp::Event aev[4];
        const int an = e.arp.AdvanceBlock(frames, aev, 4);
        for (int i = 0; i < an; ++i) {
            if (aev[i].type == 1) VoiceNoteOn(e, aev[i].note, aev[i].vel);
            else                  VoiceNoteOff(e, aev[i].note);
        }
    }

    /* 淡出完成的声部启动挂起的音 */
    for (uint32_t v = 0; v < e.max_voices; ++v) {
        if (e.pending[v].valid && !e.voices[v].Stealing() && !e.voices[v].Active()) {
            StartVoice(e, (int)v, e.pending[v].note, e.pending[v].vel, true, false);
            e.pending[v].valid = 0;
        }
    }

    memset(out, 0, (size_t)frames * 2u * sizeof(float));

    /* 阶段 2：DX7 FM 声部（与减法声部共存，切换音色时自然交叉） */
    if (e.patch.engine_type == 1u) {
        EnsureFmVoice(e);
        e.fm.Render(out, frames);
    }
    /* 阶段 3：ROMpler 采样声部 */
    if (e.patch.engine_type == 2u) {
        e.rom.Render(out, frames);
    }

    uint16_t active = 0;
    for (uint32_t v = 0; v < e.max_voices; ++v) {
        if (e.voices[v].Active()) {
            e.voices[v].Render(out, frames, e.patch, e.g);
            ++active;
        }
        e.voices[v].AdvanceAge();
    }

    /* 效果链：过载 → 合唱 → 延迟 */
    e.fx.UpdateParams(e.patch, e.sr);
    e.fx.Process(out, frames);

    /* 总线：主音量 → 软限幅 → 直流阻断 */
    const float vol = e.g.master_vol;
    for (uint32_t i = 0; i < frames; ++i) {
        for (int c = 0; c < 2; ++c) {
            float x = out[2u * i + (uint32_t)c] * vol;
            x = SoftClip(x);
            const float y = x - e.dc_x1[c] + 0.9975f * e.dc_y1[c];
            e.dc_x1[c] = x;
            e.dc_y1[c] = y;
            out[2u * i + (uint32_t)c] = y;
        }
    }

    /* 统计与 CPU */
    e.stats.active_voices = active;
    if (active > e.stats.peak_voices) e.stats.peak_voices = active;
    const uint32_t cyc1 = CyclesNow();
    if (cyc1 != 0u && cyc0 != 0u) {
        const uint32_t d = cyc1 - cyc0;
        e.stats.render_last_cycles = d;
        if (d > e.stats.render_peak_cycles) e.stats.render_peak_cycles = d;
        const uint32_t cpu_hz = CpuHz();
        if (cpu_hz != 0u) {
            const float budget = (float)frames * (float)cpu_hz / e.sr;
            const float load = 1000.0f * (float)d / budget;
            e.cpu_permille += 0.05f * (load - e.cpu_permille);
        }
    }
}

}  // namespace

/* ------------------------------ 公开 API ------------------------------ */

extern "C" void synth_set_platform_hooks(const synth_platform_hooks_t *hooks) { g_hooks = hooks; }

extern "C" int synth_init(const synth_config_t *cfg) {
    Engine &e = g_eng;
    if (cfg == nullptr) return -1;
    if (cfg->sample_rate != SYNTH_SR_DEFAULT) return -2;

    SineTable::Init();
    WaveTables::Init((float)cfg->sample_rate);

    e.sr         = (float)cfg->sample_rate;
    e.max_voices = (cfg->max_voices == 0u) ? SYNTH_MAX_VOICES
                  : (cfg->max_voices > SYNTH_MAX_VOICES ? SYNTH_MAX_VOICES : cfg->max_voices);
    e.bank       = (cfg->factory_bank != nullptr) ? cfg->factory_bank : synth_factory_bank;
    e.bank_size  = (cfg->factory_bank != nullptr) ? cfg->factory_bank_size : synth_factory_bank_size;

    for (uint32_t v = 0; v < SYNTH_MAX_VOICES; ++v) { e.voices[v].Init(e.sr); e.pending[v].valid = 0; }
    ClearNoteState(e);

    e.parser.Reset();
    e.ring_head = e.ring_tail = 0u;
    e.g = GlobalParams();
    e.sustain = false;
    e.mono_mode = false;
    e.dc_x1[0] = e.dc_x1[1] = e.dc_y1[0] = e.dc_y1[1] = 0.0f;
    e.cpu_permille = 0.0f;
    memset(&e.stats, 0, sizeof(e.stats));

    FxPoolInit();
    e.fx.Init(e.sr);
    e.arp.Init(e.sr);
    e.arp.SetEnabled(false);
    e.fm.Init(e.sr, 8);
    e.fm_loaded = -1;
    e.rom.Init(e.sr, SYNTH_ROM_MAX_VOICES);
    e.req_instrument = nullptr;
    e.req_instrument_clear = 0u;

    if (e.bank_size > 0u) ApplyPatchIndex(e, 0u);
    else { memset(&e.patch, 0, sizeof(e.patch)); e.patch.osc1_level = 200; e.patch.volume = 200; e.patch.filter_type = SYNTH_FILT_LP24; }

    e.initialized = true;
    return 0;
}

extern "C" void synth_deinit(void) { g_eng.initialized = false; }

extern "C" void synth_render_f32(float *interleaved_lr, uint32_t frames) {
    Engine &e = g_eng;
    if (!e.initialized || interleaved_lr == nullptr) return;
    uint32_t done = 0;
    while (done < frames) {
        const uint32_t n = ((frames - done) > SYNTH_BLOCK_FRAMES) ? SYNTH_BLOCK_FRAMES : (frames - done);
        RenderBlock(e, interleaved_lr + (size_t)done * 2u, n);
        done += n;
    }
}

extern "C" void synth_render_i32(int32_t *interleaved_lr, uint32_t frames) {
    Engine &e = g_eng;
    if (!e.initialized || interleaved_lr == nullptr) return;
    uint32_t done = 0;
    while (done < frames) {
        const uint32_t n = ((frames - done) > SYNTH_BLOCK_FRAMES) ? SYNTH_BLOCK_FRAMES : (frames - done);
        RenderBlock(e, e.block_buf, n);
        for (uint32_t i = 0; i < n * 2u; ++i) interleaved_lr[(size_t)done * 2u + i] = synth_f2i32(e.block_buf[i]);
        done += n;
    }
}

extern "C" void synth_midi_in(const uint8_t *bytes, uint32_t len) {
    Engine &e = g_eng;
    if (bytes == nullptr) return;
    for (uint32_t i = 0; i < len; ++i) {
        const uint32_t next = (e.ring_head + 1u) & kMidiRingMask;
        if (next == e.ring_tail) { e.stats.midi_dropped++; continue; }   /* 满：丢弃并计数 */
        e.ring[e.ring_head] = bytes[i];
        e.ring_head = next;
        e.stats.midi_bytes++;
    }
}

extern "C" void synth_tick_1ms(void) { /* 琶音器/统计上报将在此扩展 */ }

extern "C" void synth_service(void) { /* SD 音色加载等主循环任务将在此扩展 */ }

extern "C" void synth_all_notes_off(void) { g_eng.req_all_off_flag = 1u; }

extern "C" void synth_panic(void) { g_eng.req_panic_flag = 1u; }

extern "C" int synth_set_patch(uint16_t index) {
    if (index >= g_eng.bank_size) return -1;
    g_eng.req_patch_index = (int32_t)index;
    return 0;
}

extern "C" int synth_set_patch_ptr(const synth_patch_t *patch) {
    if (patch == nullptr) return -1;
    g_eng.req_patch_ptr = patch;
    return 0;
}

extern "C" uint16_t synth_get_patch_index(void) { return g_eng.patch_index; }

extern "C" void synth_note_on(uint8_t note, uint8_t velocity) {
    const uint8_t msg[3] = {0x90u, (uint8_t)(note & 0x7Fu), (uint8_t)(velocity & 0x7Fu)};
    synth_midi_in(msg, 3u);
}

extern "C" void synth_note_off(uint8_t note) {
    const uint8_t msg[3] = {0x80u, (uint8_t)(note & 0x7Fu), 0u};
    synth_midi_in(msg, 3u);
}

extern "C" void synth_set_fm_voice_bytes(const uint8_t *vced) {
    Engine &e = g_eng;
    if (vced == nullptr) return;
    memcpy(e.fm_ram_voice, vced, 155u);
    e.fm_ram_voice[155] = 0;
    e.fm_ram_valid = true;
    e.fm_loaded = -1;                       /* 强制下次渲染重新载入 */
}

extern "C" int synth_load_fm_sysex(const uint8_t *data, uint32_t len, uint8_t index) {
    Engine &e = g_eng;
    if (data == nullptr || len == 0u) return -1;

    /* 最多解析 32 个音色（一个 bank） */
    static uint8_t voices[32][156];
    const int n = ssynth::SysexToVoices(data, len, &voices[0][0], 32);
    if (n <= 0) return n;
    if (index >= (uint8_t)n) return -2;

    memcpy(e.fm_ram_voice, voices[index], 156u);
    e.fm_ram_valid = true;
    e.fm_loaded = -1;
    e.patch.engine_type = 1u;
    e.patch.fm_voice = 0xFFu;
    return n;
}

extern "C" void synth_set_sample_instrument(const synth_sample_instrument_t *inst) {
    Engine &e = g_eng;
    if (inst == nullptr) e.req_instrument_clear = 1u;
    else e.req_instrument = inst;
}

extern "C" void synth_set_engine_type(uint8_t engine_type) {
    g_eng.patch.engine_type = (engine_type > 2u) ? 2u : engine_type;
}

extern "C" void synth_set_channel_filter(int8_t channel) {
    g_eng.chan_filter = (channel < 0 || channel > 15) ? (int8_t)-1 : channel;
}

extern "C" void synth_set_master_volume(float vol01) {
    g_eng.req_master_vol = clampf(vol01, 0.0f, 1.0f);
}

extern "C" void synth_get_stats(synth_stats_t *out) {
    if (out == nullptr) return;
    *out = g_eng.stats;
    out->patch_index = g_eng.patch_index;
    out->sustain = g_eng.sustain ? 1u : 0u;
    out->mono_mode = g_eng.mono_mode ? 1u : 0u;
}

extern "C" uint32_t synth_cpu_load_permille(void) {
    return (uint32_t)(g_eng.cpu_permille + 0.5f);
}
