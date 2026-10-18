/* 内置音色库。字段顺序必须与 synth_patch_t 完全一致。 */
#include "synth/patch.h"

#define PATCH_FIELDS(m1, m2, pw1, pw2, semi, det, l1, l2, sub, noise,                    \
                     ft, cut, res, kt, feamt,                                            \
                     aa, ad, as, ar, fa, fd, fs, fr,                                     \
                     lsh, lr, ld, ldest, gl, va, vc,                                     \
                     ch, dl, dfb, drv, vol, eng, fmv, nm)                                 \
    { m1, m2, pw1, pw2, semi, det, l1, l2, sub, noise,                                   \
      ft, cut, res, kt, feamt,                                                           \
      aa, ad, as, ar, fa, fd, fs, fr,                                                    \
      lsh, lr, ld, ldest, gl, va, vc,                                                    \
      ch, dl, dfb, drv, vol, eng, fmv, nm }

#define O1_SAW      SYNTH_OSC_SAW
#define O_SQUARE    SYNTH_OSC_SQUARE
#define O_PULSE     SYNTH_OSC_PULSE
#define O_TRI       SYNTH_OSC_TRI
#define O_SINE      SYNTH_OSC_SINE
#define O_SUPER     SYNTH_OSC_SUPERSAW
#define O_NOISE     SYNTH_OSC_NOISE
#define F_LP12      SYNTH_FILT_LP12
#define F_LP24      SYNTH_FILT_LP24
#define F_BP12      SYNTH_FILT_BP12
#define F_HP12      SYNTH_FILT_HP12
#define L_SINE      SYNTH_LFO_SINE
#define L_TRI       SYNTH_LFO_TRI
#define L_SQR       SYNTH_LFO_SQUARE
#define L_SH        SYNTH_LFO_SAMPLE_HOLD
#define D_PITCH     SYNTH_LFO_DEST_PITCH
#define D_FILTER    SYNTH_LFO_DEST_FILTER
#define D_AMP       SYNTH_LFO_DEST_AMP
#define D_PW        SYNTH_LFO_DEST_PW

const synth_patch_t synth_factory_bank[] = {
    /*           m1      m2      pw1 pw2 semi det  l1  l2 sub noi  ft    cut res  kt feamt  aa  ad  as  ar  fa  fd  fs  fr  lsh     lr  ld ldest     gl  va  vc  ch dl dfb drv vol name */
    PATCH_FIELDS(O1_SAW, O1_SAW, 128,128,   0, -8, 220,110,  0,  0, F_LP24,150, 40, 128,  60,   4, 70,190, 70,   4, 90,120, 80, L_SINE, 60, 20, D_FILTER, 20,200,100,  0, 0,  0,  0, 210, 0, 0, "Init Saw"),
    PATCH_FIELDS(O1_SAW, O1_SAW, 128,128,  12, -6, 180,150, 40,  0, F_LP24, 96, 30, 160,  40,  70,120,200,140,  60,140,150,150, L_SINE, 45, 25, D_FILTER, 30,120, 80, 90, 0,  0, 20, 200, 0, 0, "Warm Pad"),
    PATCH_FIELDS(O1_SAW, O_PULSE,100, 90,   0, -4, 200,120,  0,  0, F_LP24,170,110, 100,  90,   3, 55,160, 45,   5, 70,110, 70, L_TRI,  80, 35, D_FILTER,  8,150,120,  0, 0,  0,  0, 200, 0, 0, "Bright Lead"),
    PATCH_FIELDS(O1_SAW, O_SINE, 128,128,-12,  0, 200, 90, 90,  0, F_LP24, 78, 45,  80,  70,   2, 60,150, 40,   3, 80,100, 60, L_SINE, 40,  0, D_FILTER, 12,220,150,  0, 0,  0,  0, 210, 0, 0, "Sub Bass"),
    PATCH_FIELDS(O_TRI, O1_SAW, 128,128,   0,  0, 210, 60, 30, 20, F_LP24,140, 70, 120, 110,   2, 45,120, 30,   2, 50, 90, 45, L_SINE, 70,  0, D_FILTER,  0,230,140, 30,120, 90,  0, 210, 0, 0, "Pluck"),
    PATCH_FIELDS(O_SQUARE,O_SQUARE,140,140, 0,  0, 190, 90,  0,  0, F_LP12,120, 50,  90,  50,   6, 90,180, 60,  10,120,140, 80, L_SINE, 55, 15, D_FILTER, 25,170, 90,  0, 0,  0,  0, 200, 0, 0, "Square Keys"),
    PATCH_FIELDS(O_SUPER,O_SUPER, 128,128,  0, 14, 220,180,  0,  0, F_LP24,140, 40, 150,  30,  50,140,210,160,  40,150,170,160, L_SINE, 40, 20, D_FILTER, 35,100, 70,140, 90,110, 30, 190, 0, 0, "Super Saw"),
    PATCH_FIELDS(O_SINE, O_NOISE,128,128,   0,  0, 230, 14,  0, 26, F_LP12,120, 20, 100,  40,  90,170,220,170,  50,150,180,170, L_SINE, 50, 30, D_FILTER, 30, 90, 60,  0, 0,  0,  0, 200, 0, 0, "Sine Flute"),
    PATCH_FIELDS(O_PULSE,O_PULSE,128,150,   0,  6, 200,200,  0,  0, F_LP12,130, 30, 120,  20,  60,180,220,150,  60,170,190,160, L_TRI,  35, 60, D_PW,     30, 90, 70,110, 0,  0,  0, 190, 0, 0, "PWM Strings"),
    PATCH_FIELDS(O1_SAW, O_SQUARE,128,128,  0, -5, 210,140,  0,  0, F_LP24,110,120,  90, 110,   3, 90, 60, 60,   3, 60, 40, 70, L_SINE, 60,  0, D_FILTER, 10,200,130,  0, 0,  0,  0, 200, 0, 0, "Res Sweep"),
    PATCH_FIELDS(O_NOISE,O_NOISE,128,128,   0,  0, 200,  0,  0,180, F_LP24,110,140,  60, 120,  40,140, 30,160,  30,180,120,180, L_SINE, 40,  0, D_FILTER, 20,150, 90,  0, 0,  0,  0, 190, 0, 0, "Noise Sweep"),
    PATCH_FIELDS(O_SINE, O_SINE, 128,128,  12,  0, 200,160, 60,  0, F_LP12,170, 20,  60,   0,   6, 90,230, 50,   6,120,200, 90, L_SINE, 50, 10, D_FILTER, 15,190, 80,  0, 0,  0,  0, 210, 0, 0, "Organ"),
    /* 阶段 2：DX7 FM 音色（engine_type = 1，fm_voice 选内置 FM 音色表；减法字段不使用） */
    PATCH_FIELDS(O_SINE, O_SINE, 128,128,   0,  0,   0,  0,  0,  0, F_LP12,255,  0,   0,   0,   1, 10,255, 10,   1, 10,255, 10, L_SINE, 50,  0, D_FILTER,  0,  0,  0,  0, 0,  0,  0, 210, 1, 0, "FM E.Piano"),
    PATCH_FIELDS(O_SINE, O_SINE, 128,128,   0,  0,   0,  0,  0,  0, F_LP12,255,  0,   0,   0,   1, 10,255, 10,   1, 10,255, 10, L_SINE, 50,  0, D_FILTER,  0,  0,  0,  0, 0,  0,  0, 210, 1, 1, "FM Bass"),
    PATCH_FIELDS(O_SINE, O_SINE, 128,128,   0,  0,   0,  0,  0,  0, F_LP12,255,  0,   0,   0,   1, 10,255, 10,   1, 10,255, 10, L_SINE, 50,  0, D_FILTER,  0,  0,  0,  0, 0,  0,  0, 200, 1, 2, "FM Bell"),
};

const uint16_t synth_factory_bank_size =
    (uint16_t)(sizeof(synth_factory_bank) / sizeof(synth_factory_bank[0]));
