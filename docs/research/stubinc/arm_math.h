#ifndef ARM_MATH_H_STUB
#define ARM_MATH_H_STUB
#include <stdint.h>
#include <math.h>
typedef float float32_t;
typedef int16_t q15_t;
typedef int32_t q31_t;
typedef struct { float32_t *pState; float32_t *pCoeffs; uint8_t numStages; } arm_biquad_casd_df1_inst_f32;
static inline void arm_fill_f32(float32_t v, float32_t *p, uint32_t n){ for(uint32_t i=0;i<n;i++) p[i]=v; }
static inline void arm_float_to_q15(const float32_t *s, q15_t *d, uint32_t n){ for(uint32_t i=0;i<n;i++){ float32_t f=s[i]*32768.0f; if(f>32767.f)f=32767.f; if(f<-32768.f)f=-32768.f; d[i]=(q15_t)f; } }
static inline void arm_sqrt_f32(float32_t v, float32_t *r){ *r = sqrtf(v); }
static inline void arm_scale_f32(const float32_t *s, float32_t k, float32_t *d, uint32_t n){ for(uint32_t i=0;i<n;i++) d[i]=s[i]*k; }
static inline void arm_mult_f32(const float32_t *a, const float32_t *b, float32_t *d, uint32_t n){ for(uint32_t i=0;i<n;i++) d[i]=a[i]*b[i]; }
static inline void arm_offset_f32(const float32_t *s, float32_t k, float32_t *d, uint32_t n){ for(uint32_t i=0;i<n;i++) d[i]=s[i]+k; }
static inline void arm_sub_f32(const float32_t *a, const float32_t *b, float32_t *d, uint32_t n){ for(uint32_t i=0;i<n;i++) d[i]=a[i]-b[i]; }
static inline void arm_biquad_cascade_df1_f32(const arm_biquad_casd_df1_inst_f32 *S, float32_t *pSrc, float32_t *pDst, uint32_t n){ (void)S; for(uint32_t i=0;i<n;i++) pDst[i]=pSrc[i]; }
static inline void arm_biquad_cascade_df1_init_f32(arm_biquad_casd_df1_inst_f32 *S, uint8_t n, float32_t *c, float32_t *st){ S->numStages=n; S->pCoeffs=c; S->pState=st; }
#define ARM_SQRT_FUNC arm_sqrt_f32
#endif
