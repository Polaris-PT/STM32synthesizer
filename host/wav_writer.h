#ifndef SSYNTH_WAV_WRITER_H
#define SSYNTH_WAV_WRITER_H
#include <stdio.h>
#include <stdint.h>

/* 16bit 立体声 WAV 写入器 */
class WavWriter {
public:
    bool Open(const char *path, uint32_t sample_rate);
    void WriteFloat(const float *interleaved_lr, uint32_t frames);
    void Close();
    uint64_t FramesWritten() const { return frames_written_; }
    float Peak() const { return peak_; }
    double SumSquares() const { return sum_sq_; }
private:
    FILE    *fp_ = nullptr;
    uint32_t rate_ = 48000;
    uint32_t data_bytes_ = 0;
    uint64_t frames_written_ = 0;
    float    peak_ = 0.0f;
    double   sum_sq_ = 0.0;
    void WriteHeader(uint32_t data_bytes);
};
#endif
