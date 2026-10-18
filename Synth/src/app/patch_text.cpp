#include "patch_text.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stddef.h>

namespace {

enum FieldType { kU8, kI8, kEnumOsc, kEnumFilter, kEnumLfoShape, kEnumLfoDest, kString };

struct Field {
    const char *key;
    uint8_t     type;
    uint16_t    offset;
};

const Field kFields[] = {
    {"osc1.model",   kEnumOsc,      offsetof(synth_patch_t, osc1_model)},
    {"osc2.model",   kEnumOsc,      offsetof(synth_patch_t, osc2_model)},
    {"osc1.pw",      kU8,           offsetof(synth_patch_t, osc1_pw)},
    {"osc2.pw",      kU8,           offsetof(synth_patch_t, osc2_pw)},
    {"osc2.semi",    kI8,           offsetof(synth_patch_t, osc2_semi)},
    {"osc2.detune",  kI8,           offsetof(synth_patch_t, osc2_detune)},
    {"osc1.level",   kU8,           offsetof(synth_patch_t, osc1_level)},
    {"osc2.level",   kU8,           offsetof(synth_patch_t, osc2_level)},
    {"sub.level",    kU8,           offsetof(synth_patch_t, sub_level)},
    {"noise.level",  kU8,           offsetof(synth_patch_t, noise_level)},
    {"filter.type",  kEnumFilter,   offsetof(synth_patch_t, filter_type)},
    {"filter.cutoff",kU8,           offsetof(synth_patch_t, cutoff)},
    {"filter.res",   kU8,           offsetof(synth_patch_t, resonance)},
    {"filter.keytrack", kU8,        offsetof(synth_patch_t, keytrack)},
    {"filter.env",   kI8,           offsetof(synth_patch_t, filter_env_amt)},
    {"amp.a",        kU8,           offsetof(synth_patch_t, amp_a)},
    {"amp.d",        kU8,           offsetof(synth_patch_t, amp_d)},
    {"amp.s",        kU8,           offsetof(synth_patch_t, amp_s)},
    {"amp.r",        kU8,           offsetof(synth_patch_t, amp_r)},
    {"flt.a",        kU8,           offsetof(synth_patch_t, flt_a)},
    {"flt.d",        kU8,           offsetof(synth_patch_t, flt_d)},
    {"flt.s",        kU8,           offsetof(synth_patch_t, flt_s)},
    {"flt.r",        kU8,           offsetof(synth_patch_t, flt_r)},
    {"lfo.shape",    kEnumLfoShape, offsetof(synth_patch_t, lfo_shape)},
    {"lfo.rate",     kU8,           offsetof(synth_patch_t, lfo_rate)},
    {"lfo.depth",    kU8,           offsetof(synth_patch_t, lfo_depth)},
    {"lfo.dest",     kEnumLfoDest,  offsetof(synth_patch_t, lfo_dest)},
    {"glide",        kU8,           offsetof(synth_patch_t, glide)},
    {"vel.amp",      kU8,           offsetof(synth_patch_t, vel_amp)},
    {"vel.cut",      kU8,           offsetof(synth_patch_t, vel_cut)},
    {"fx.chorus",    kU8,           offsetof(synth_patch_t, fx_chorus)},
    {"fx.delay",     kU8,           offsetof(synth_patch_t, fx_delay)},
    {"fx.delay_fb",  kU8,           offsetof(synth_patch_t, fx_delay_fb)},
    {"fx.drive",     kU8,           offsetof(synth_patch_t, fx_drive)},
    {"volume",       kU8,           offsetof(synth_patch_t, volume)},
    {"engine",       kU8,           offsetof(synth_patch_t, engine_type)},   /* 0=减法 1=DX7 FM */
    {"fm.voice",     kU8,           offsetof(synth_patch_t, fm_voice)},
};
const int kFieldCount = (int)(sizeof(kFields) / sizeof(kFields[0]));

const char *const kOscNames[]  = {"saw", "square", "pulse", "tri", "sine", "supersaw", "noise"};
const char *const kFilterNames[] = {"lp12", "lp24", "bp12", "hp12"};
const char *const kLfoShapeNames[] = {"sine", "tri", "square", "sh"};
const char *const kLfoDestNames[]  = {"pitch", "filter", "amp", "pw"};

int EnumFromName(const char *const *names, int count, const char *s) {
    for (int i = 0; i < count; ++i) {
        if (strcmp(names[i], s) == 0) return i;
    }
    return -1;
}

void Trim(char *s) {
    if (s == nullptr) return;
    char *p = s;
    while (*p && isspace((unsigned char)*p)) ++p;
    if (p != s) memmove(s, p, strlen(p) + 1u);
    size_t n = strlen(s);
    while (n > 0u && isspace((unsigned char)s[n - 1u])) s[--n] = '\0';
}

}  // namespace

extern "C" void patch_set_defaults(synth_patch_t *out) {
    if (out == nullptr) return;
    memset(out, 0, sizeof(*out));
    out->osc1_model = SYNTH_OSC_SAW;
    out->osc1_level = 220;
    out->osc1_pw    = 128;
    out->osc2_pw    = 128;
    out->filter_type = SYNTH_FILT_LP24;
    out->cutoff = 150;
    out->resonance = 40;
    out->amp_a = 4;  out->amp_d = 70; out->amp_s = 190; out->amp_r = 70;
    out->flt_a = 4;  out->flt_d = 90; out->flt_s = 120; out->flt_r = 80;
    out->lfo_shape = SYNTH_LFO_SINE;
    out->lfo_rate = 60; out->lfo_depth = 20; out->lfo_dest = SYNTH_LFO_DEST_FILTER;
    out->vel_amp = 200; out->vel_cut = 100;
    out->volume = 210;
    strncpy(out->name, "Init", SYNTH_PATCH_NAME_LEN - 1u);
}

extern "C" int patch_from_text(const char *text, synth_patch_t *out) {
    if (text == nullptr || out == nullptr) return -1;
    synth_patch_t tmp = *out;
    int applied = 0;

    char line[256];
    const char *p = text;
    while (*p) {
        size_t n = 0;
        while (*p && *p != '\n' && n + 1u < sizeof(line)) line[n++] = *p++;
        if (*p == '\n') ++p;
        line[n] = '\0';

        /* 去注释 */
        for (char *c = line; *c; ++c) {
            if (*c == '#' || *c == ';') { *c = '\0'; break; }
            if (c[0] == '/' && c[1] == '/') { c[0] = '\0'; break; }
        }
        Trim(line);
        if (line[0] == '\0') continue;

        char *eq = strchr(line, '=');
        if (eq == nullptr) return -2;
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        Trim(key);
        Trim(val);
        if (key[0] == '\0') return -3;

        if (strcmp(key, "name") == 0) {
            strncpy(tmp.name, val, SYNTH_PATCH_NAME_LEN - 1u);
            tmp.name[SYNTH_PATCH_NAME_LEN - 1u] = '\0';
            ++applied;
            continue;
        }

        int found = -1;
        for (int i = 0; i < kFieldCount; ++i) {
            if (strcmp(kFields[i].key, key) == 0) { found = i; break; }
        }
        if (found < 0) return -4;                       /* 未知键 */

        const Field &f = kFields[found];
        char *endp = nullptr;
        long num = strtol(val, &endp, 10);
        const bool numeric = (endp != nullptr && endp != val);

        switch (f.type) {
            case kEnumOsc: {
                int v = numeric ? (int)num : EnumFromName(kOscNames, SYNTH_OSC_COUNT, val);
                if (v < 0 || v >= SYNTH_OSC_COUNT) return -5;
                *((uint8_t *)((char *)&tmp + f.offset)) = (uint8_t)v;
                break;
            }
            case kEnumFilter: {
                int v = numeric ? (int)num : EnumFromName(kFilterNames, SYNTH_FILT_TYPE_COUNT, val);
                if (v < 0 || v >= SYNTH_FILT_TYPE_COUNT) return -5;
                *((uint8_t *)((char *)&tmp + f.offset)) = (uint8_t)v;
                break;
            }
            case kEnumLfoShape: {
                int v = numeric ? (int)num : EnumFromName(kLfoShapeNames, SYNTH_LFO_SHAPE_COUNT, val);
                if (v < 0 || v >= SYNTH_LFO_SHAPE_COUNT) return -5;
                *((uint8_t *)((char *)&tmp + f.offset)) = (uint8_t)v;
                break;
            }
            case kEnumLfoDest: {
                int v = numeric ? (int)num : EnumFromName(kLfoDestNames, SYNTH_LFO_DEST_COUNT, val);
                if (v < 0 || v >= SYNTH_LFO_DEST_COUNT) return -5;
                *((uint8_t *)((char *)&tmp + f.offset)) = (uint8_t)v;
                break;
            }
            case kU8:
                if (!numeric) return -6;
                if (num < 0 || num > 255) return -7;
                *((uint8_t *)((char *)&tmp + f.offset)) = (uint8_t)num;
                break;
            case kI8:
                if (!numeric) return -6;
                if (num < -128 || num > 127) return -7;
                *((int8_t *)((char *)&tmp + f.offset)) = (int8_t)num;
                break;
            default:
                return -8;
        }
        ++applied;
    }

    *out = tmp;
    return applied;
}

extern "C" int patch_to_text(const synth_patch_t *p, char *out, unsigned out_len) {
    if (p == nullptr || out == nullptr || out_len < 32u) return -1;
    unsigned used = 0;
    int n = snprintf(out, out_len, "# STM32 synth patch\nname = %s\n", p->name);
    if (n < 0 || (unsigned)n >= out_len) return -2;
    used = (unsigned)n;

    for (int i = 0; i < kFieldCount; ++i) {
        const Field &f = kFields[i];
        long v = 0;
        switch (f.type) {
            case kU8:  v = *((const uint8_t *)((const char *)p + f.offset)); break;
            case kI8:  v = *((const int8_t  *)((const char *)p + f.offset)); break;
            case kEnumOsc:      v = *((const uint8_t *)((const char *)p + f.offset)); break;
            case kEnumFilter:   v = *((const uint8_t *)((const char *)p + f.offset)); break;
            case kEnumLfoShape: v = *((const uint8_t *)((const char *)p + f.offset)); break;
            case kEnumLfoDest:  v = *((const uint8_t *)((const char *)p + f.offset)); break;
            default: continue;
        }
        const char *val = nullptr;
        char buf[16];
        if (f.type == kEnumOsc)      val = (v >= 0 && v < SYNTH_OSC_COUNT) ? kOscNames[v] : nullptr;
        else if (f.type == kEnumFilter)   val = (v >= 0 && v < SYNTH_FILT_TYPE_COUNT) ? kFilterNames[v] : nullptr;
        else if (f.type == kEnumLfoShape) val = (v >= 0 && v < SYNTH_LFO_SHAPE_COUNT) ? kLfoShapeNames[v] : nullptr;
        else if (f.type == kEnumLfoDest)  val = (v >= 0 && v < SYNTH_LFO_DEST_COUNT) ? kLfoDestNames[v] : nullptr;
        if (val == nullptr) {
            snprintf(buf, sizeof(buf), "%ld", v);
            val = buf;
        }
        n = snprintf(out + used, out_len - used, "%-16s = %s\n", f.key, val);
        if (n < 0 || (unsigned)n >= out_len - used) return -2;
        used += (unsigned)n;
    }
    return (int)used;
}
