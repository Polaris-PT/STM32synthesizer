#ifndef TEENSY_SHIM_H
#define TEENSY_SHIM_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
uint32_t millis(void);
void delay(uint32_t);
#ifdef __cplusplus
}
#endif
static inline int32_t signed_saturate_rshift(int32_t val, int32_t bits, int32_t shift) {
  int32_t out = val >> shift;
  int32_t max = (1 << (bits - 1)) - 1, min = -(1 << (bits - 1));
  return out > max ? max : (out < min ? min : out);
}
#endif
