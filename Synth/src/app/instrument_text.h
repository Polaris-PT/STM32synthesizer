/* 采样乐器清单（.ins 文本）解析：把多音区定义读到 synth_sample_instrument_t。
   采样数据由调用方提供的 loader 回调载入（SD 卡或 QSPI 都可），因此本文件平台无关、可在宿主端单测。 */
#ifndef SSYNTH_INSTRUMENT_TEXT_H
#define SSYNTH_INSTRUMENT_TEXT_H

#include <stdint.h>
#include "synth/sample.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 载入一个采样文件：成功返回 0，并把 16bit 单声道数据/帧数/采样率写入出参（数据需长期有效）。 */
typedef int (*synth_sample_load_fn)(void *ctx, const char *path,
                                    const int16_t **out_data, uint32_t *out_frames, uint32_t *out_rate);

/* 解析 .ins 文本。loader 为 NULL 时只解析参数（不载入采样，用于测试/预检）。
   返回解析到的音区数（>=0），负数错误：-1 参数错 -2 行格式错 -3 未知键 -4 数值越界 -5 采样载入失败 */
int instrument_from_text(const char *text, synth_sample_instrument_t *out,
                         synth_sample_load_fn loader, void *ctx);

/* 序列化为 .ins 文本；返回写入字符数，负数为缓冲不足 */
int instrument_to_text(const synth_sample_instrument_t *inst, const char *const *sample_paths,
                       char *out, unsigned out_len);

#ifdef __cplusplus
}
#endif
#endif /* SSYNTH_INSTRUMENT_TEXT_H */
