#include "instrument_text.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

namespace {
constexpr int kMaxZones = SYNTH_ROM_MAX_ZONES;
constexpr int kPathLen = 64;

struct ZoneTmp {
    int   used = 0;
    char  path[kPathLen] = {0};
    int   root = 60, low = 0, high = 127;
    int   loop = 0;
    long  loop_start = 0, loop_end = 0;
    int   gain = 128;          /* 0..255 → 0..2.0 */
    long  crossfade = 0;       /* 循环接缝交叉淡化样点数 */
    int   interp = 1;          /* 0=线性 1=三次 */
};

void Trim(char *s) {
    if (s == nullptr) return;
    char *p = s;
    while (*p && isspace((unsigned char)*p)) ++p;
    if (p != s) memmove(s, p, strlen(p) + 1u);
    size_t n = strlen(s);
    while (n > 0u && isspace((unsigned char)s[n - 1u])) s[--n] = '\0';
}
}  // namespace

extern "C" int instrument_from_text(const char *text, synth_sample_instrument_t *out,
                                    synth_sample_load_fn loader, void *ctx) {
    if (text == nullptr || out == nullptr) return -1;
    memset(out, 0, sizeof(*out));
    out->volume = 0.8f;
    strncpy(out->name, "Instrument", sizeof(out->name) - 1u);

    ZoneTmp tmp[kMaxZones];
    int zone_count = 0;
    int volume = 204;

    char line[192];
    const char *p = text;
    while (*p) {
        size_t n = 0;
        while (*p && *p != '\n' && n + 1u < sizeof(line)) line[n++] = *p++;
        if (*p == '\n') ++p;
        line[n] = '\0';

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

        if (strcmp(key, "name") == 0) {
            strncpy(out->name, val, sizeof(out->name) - 1u);
            out->name[sizeof(out->name) - 1u] = '\0';
            continue;
        }
        if (strcmp(key, "volume") == 0) {
            const long v = strtol(val, nullptr, 10);
            if (v < 0 || v > 255) return -4;
            volume = (int)v;
            continue;
        }

        /* zoneN.field */
        if (strncmp(key, "zone", 4) != 0) return -3;
        const char *q = key + 4;
        if (!isdigit((unsigned char)*q)) return -3;
        const int idx = (int)strtol(q, (char **)&q, 10);
        if (idx < 0 || idx >= kMaxZones) return -4;
        if (*q != '.') return -3;
        const char *field = q + 1;
        if (idx + 1 > zone_count) zone_count = idx + 1;
        ZoneTmp &z = tmp[idx];
        z.used = 1;

        if (strcmp(field, "sample") == 0 || strcmp(field, "path") == 0) {
            strncpy(z.path, val, kPathLen - 1u);
            z.path[kPathLen - 1u] = '\0';
        } else if (strcmp(field, "root") == 0) {
            const long v = strtol(val, nullptr, 10);
            if (v < 0 || v > 127) return -4;
            z.root = (int)v;
        } else if (strcmp(field, "low") == 0) {
            const long v = strtol(val, nullptr, 10);
            if (v < 0 || v > 127) return -4;
            z.low = (int)v;
        } else if (strcmp(field, "high") == 0) {
            const long v = strtol(val, nullptr, 10);
            if (v < 0 || v > 127) return -4;
            z.high = (int)v;
        } else if (strcmp(field, "loop") == 0) {
            z.loop = (val[0] == '1' || val[0] == 'y' || val[0] == 'Y') ? 1 : 0;
        } else if (strcmp(field, "loop_start") == 0) {
            z.loop_start = strtol(val, nullptr, 10);
            if (z.loop_start < 0) return -4;
        } else if (strcmp(field, "loop_end") == 0) {
            z.loop_end = strtol(val, nullptr, 10);
            if (z.loop_end < 0) return -4;
        } else if (strcmp(field, "gain") == 0) {
            const long v = strtol(val, nullptr, 10);
            if (v < 0 || v > 255) return -4;
            z.gain = (int)v;
        } else if (strcmp(field, "crossfade") == 0) {
            const long v = strtol(val, nullptr, 10);
            if (v < 0 || v > 65536) return -4;
            z.crossfade = v;
        } else if (strcmp(field, "interp") == 0) {
            if (val[0] == 'c' || val[0] == 'C' || val[0] == '1') z.interp = 1;
            else if (val[0] == 'l' || val[0] == 'L' || val[0] == '0') z.interp = 0;
            else return -4;
        } else {
            return -3;
        }
    }

    if (zone_count > kMaxZones) return -4;
    out->volume = (float)volume * (1.0f / 255.0f);

    for (int i = 0; i < zone_count; ++i) {
        ZoneTmp &z = tmp[i];
        synth_sample_zone_t &zz = out->zones[i];
        zz.data       = nullptr;
        zz.frames     = 0;
        zz.rate       = 44100;
        zz.root       = (uint8_t)z.root;
        zz.low        = (uint8_t)z.low;
        zz.high       = (uint8_t)z.high;
        zz.loop       = (uint8_t)z.loop;
        zz.loop_start = (uint32_t)z.loop_start;
        zz.loop_end   = (uint32_t)z.loop_end;
        zz.gain       = (float)z.gain * (2.0f / 255.0f);
        zz.crossfade  = (uint32_t)z.crossfade;
        zz.interp     = (uint8_t)z.interp;
        if (z.path[0] != '\0' && loader != nullptr) {
            const int16_t *data = nullptr;
            uint32_t frames = 0, rate = 0;
            if (loader(ctx, z.path, &data, &frames, &rate) != 0) return -5;
            zz.data   = data;
            zz.frames = frames;
            zz.rate   = (rate != 0u) ? rate : 44100u;
        }
    }
    out->zone_count = (uint8_t)zone_count;
    return zone_count;
}

extern "C" int instrument_to_text(const synth_sample_instrument_t *inst, const char *const *sample_paths,
                                  char *out, unsigned out_len) {
    if (inst == nullptr || out == nullptr || out_len < 64u) return -1;
    unsigned used = 0;
    int n = snprintf(out, out_len, "# STM32 synth instrument\nname = %s\nvolume = %u\n",
                     inst->name, (unsigned)(inst->volume * 255.0f + 0.5f));
    if (n < 0 || (unsigned)n >= out_len) return -2;
    used += (unsigned)n;

    for (unsigned i = 0; i < inst->zone_count && i < SYNTH_ROM_MAX_ZONES; ++i) {
        const synth_sample_zone_t &z = inst->zones[i];
        const char *path = (sample_paths != nullptr && sample_paths[i] != nullptr) ? sample_paths[i] : "";
        n = snprintf(out + used, out_len - used,
                     "zone%u.sample = %s\nzone%u.root = %u\nzone%u.low = %u\nzone%u.high = %u\n"
                     "zone%u.loop = %u\nzone%u.loop_start = %u\nzone%u.loop_end = %u\nzone%u.gain = %u\n"
                     "zone%u.crossfade = %u\nzone%u.interp = %u\n",
                     i, path, i, (unsigned)z.root, i, (unsigned)z.low, i, (unsigned)z.high,
                     i, (unsigned)z.loop, i, (unsigned)z.loop_start, i, (unsigned)z.loop_end,
                     i, (unsigned)(z.gain * 127.5f + 0.5f),
                     i, (unsigned)z.crossfade, i, (unsigned)z.interp);
        if (n < 0 || (unsigned)n >= out_len - used) return -2;
        used += (unsigned)n;
    }
    return (int)used;
}
