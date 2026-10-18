#include "wav_writer.h"
#include <math.h>
#include <string.h>
#include <vector>

static void put_u32(FILE *f, uint32_t v) { fputc(v & 0xFF, f); fputc((v >> 8) & 0xFF, f); fputc((v >> 16) & 0xFF, f); fputc((v >> 24) & 0xFF, f); }
static void put_u16(FILE *f, uint16_t v) { fputc(v & 0xFF, f); fputc((v >> 8) & 0xFF, f); }

bool WavWriter::Open(const char *path, uint32_t sample_rate) {
    fp_ = fopen(path, "wb");
    if (fp_ == nullptr) return false;
    rate_ = sample_rate;
    data_bytes_ = 0;
    frames_written_ = 0;
    peak_ = 0.0f;
    sum_sq_ = 0.0;
    WriteHeader(0);           /* 先写占位头，Close 时用真实长度重写 */
    return true;
}

void WavWriter::WriteHeader(uint32_t data_bytes) {
    fseek(fp_, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, fp_);
    put_u32(fp_, 36u + data_bytes);
    fwrite("WAVE", 1, 4, fp_);
    fwrite("fmt ", 1, 4, fp_);
    put_u32(fp_, 16u);
    put_u16(fp_, 1u);                 /* PCM */
    put_u16(fp_, 2u);                 /* 立体声 */
    put_u32(fp_, rate_);
    put_u32(fp_, rate_ * 2u * 2u);    /* byte rate */
    put_u16(fp_, 4u);                 /* block align */
    put_u16(fp_, 16u);                /* bits */
    fwrite("data", 1, 4, fp_);
    put_u32(fp_, data_bytes);
}

void WavWriter::WriteFloat(const float *interleaved_lr, uint32_t frames) {
    if (fp_ == nullptr) return;
    std::vector<int16_t> buf(frames * 2u);
    for (uint32_t i = 0; i < frames * 2u; ++i) {
        float x = interleaved_lr[i];
        if (!(x == x)) x = 0.0f;                    /* NaN 保护 */
        const float a = fabsf(x);
        if (a > peak_) peak_ = a;
        sum_sq_ += (double)x * (double)x;
        if (x > 0.99997f) x = 0.99997f;
        if (x < -1.0f) x = -1.0f;
        buf[i] = (int16_t)lrintf(x * 32767.0f);
    }
    fwrite(buf.data(), sizeof(int16_t), buf.size(), fp_);
    data_bytes_ += (uint32_t)(buf.size() * sizeof(int16_t));
    frames_written_ += frames;
}

void WavWriter::Close() {
    if (fp_ == nullptr) return;
    WriteHeader(data_bytes_);
    fclose(fp_);
    fp_ = nullptr;
}
