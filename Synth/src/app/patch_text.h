/* 音色文本格式（.syn）：key = value，可读可手改，用于 SD 卡音色文件。 */
#ifndef SSYNTH_PATCH_TEXT_H
#define SSYNTH_PATCH_TEXT_H

#include "synth/patch.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 解析文本到音色；返回应用到字段数（>=0），负数表示格式错误。未出现的字段保持原值。 */
int patch_from_text(const char *text, synth_patch_t *out);

/* 序列化为文本；返回写入字符数（不含结尾 0），负数为缓冲不足。 */
int patch_to_text(const synth_patch_t *p, char *out, unsigned out_len);

/* 从内置表取默认音色（用于先 memset 再解析） */
void patch_set_defaults(synth_patch_t *out);

#ifdef __cplusplus
}
#endif
#endif /* SSYNTH_PATCH_TEXT_H */
