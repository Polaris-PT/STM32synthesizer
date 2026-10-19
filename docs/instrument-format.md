# 采样乐器清单格式（`.ins`）

放在 SD 卡 `/synth/NNN.ins`（NNN = Program Change 音色号）会被自动加载并切到 ROMpler；
纯文本 `key = value`，支持空行与注释（`#`、`;`、`//`）。一个乐器最多 **8 个音区**。

## 字段
| key | 含义 |
|---|---|
| `name` | 乐器名（≤15 字符） |
| `volume` | 0..255 → 0..1 总音量 |
| `zoneN.sample` | 采样文件路径（16bit PCM WAV；单声道或立体声，立体声下混为单声道） |
| `zoneN.root` | 采样的原始音高（MIDI 音号 0..127） |
| `zoneN.low` / `zoneN.high` | 该音区覆盖的键盘范围 |
| `zoneN.loop` | 1 = 循环，0 = 不循环 |
| `zoneN.loop_start` / `zoneN.loop_end` | 循环点（样点；`loop_end = 0` 表示到采样末尾） |
| `zoneN.gain` | 0..255 → 0..2.0 音区增益 |

音区匹配：`low ≤ 音符 ≤ high`，多个匹配时取下标最小的；都不匹配则用第一个可用音区。
重采样：`step = 2^((note − root)/12) × 采样率 ÷ 输出采样率`，线性插值。

## 示例
```ini
# 两音区钢琴
name = Two Zone Piano
volume = 200

zone0.sample = /samples/piano_low.wav
zone0.root = 45
zone0.low = 0
zone0.high = 59
zone0.loop = 1
zone0.loop_start = 12000
zone0.loop_end = 30000
zone0.gain = 128

zone1.sample = /samples/piano_high.wav
zone1.root = 69
zone1.low = 60
zone1.high = 127
zone1.loop = 0
```

## 错误码（`instrument_from_text` 负数返回）
| 值 | 含义 |
|---|---|
| -1 | 参数为空 |
| -2 | 行内缺少 `=` |
| -3 | 未知键/字段 |
| -4 | 数值或音区下标越界 |
| -5 | 采样载入失败 |

## 采样存放两条路
- **SD 卡**：放 WAV，由 `Synth/port/sample_storage_sd.c` 读入 **128KB RAM 采样池**（AXI SRAM，`.ram_section`）；
  池大小用 `Synth/port/synth_port_config.h` 的 `SYNTH_PORT_SAMPLE_POOL_BYTES` 调整（同时要 `SYNTH_PORT_SD_ENABLE=1`）。
- **QSPI flash**：裸 16bit 单声道 PCM，用 `synth_port_sample_add_qspi()` **零拷贝**挂载
  （`zone->data = 0x90000000 + offset`；需内存映射模式 + 放开 MPU 对该区的 NO_ACCESS）。适合大采样库。

## 相关 API
| 函数 | 位置 | 用途 |
|---|---|---|
| `instrument_from_text(text, out, loader, ctx)` | `Synth/src/app/instrument_text.cpp` | 解析清单（平台无关，宿主端可测） |
| `instrument_to_text(inst, paths, out, len)` | 同上 | 反向序列化 |
| `synth_port_instrument_load(path)` | `Synth/port/sample_storage_sd.c` | 从 SD 读 .ins 并交给引擎 |
| `synth_port_sample_load_wav(...)` | 同上 | 单个采样直接建音区 |
| `synth_port_sample_add_qspi(...)` | `Synth/port/sample_storage_qspi.c` | QSPI 零拷贝音区 |
| `synth_set_sample_instrument(inst)` + `synth_set_engine_type(2)` | 引擎 API | 挂载乐器并切到 ROMpler |
