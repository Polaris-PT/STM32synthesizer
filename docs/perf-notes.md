# 实时性能笔记：复音一多就「滋滋」/「卡死」

> 2026-10-19。板上实测数据来自 `tools\status.ps1`（`cpu_permille` / `render_peak_cycles` / `audio_underruns` 三个探针）。

## 1. 根因

旧实现把 `synth_render_i32()` 直接放在音频 DMA 中断里（`HAL_SAI_TxHalfCpltCallback` / `TxCpltCallback`，中断优先级 0）。
每半缓冲 128 帧只有 **2.667 ms**（480 MHz ≈ 1,280,000 周期）。复音一多，渲染超时，连锁出三种症状：

| 现象 | 机理 |
|---|---|
| 短暂「滋滋」 | 渲染超过半缓冲时间 → DMA 欠载（重复/断块） |
| 音停不下来 | 优先级 0 的音频中断挤掉 USB 中断（优先级 5）→ MIDI 包丢，note-off 没到引擎 |
| 特别复杂时整块板卡死 | 中断里一直渲染，主循环一圈都跑不动（探针会看到 `audio_isr_depth == 1`） |

## 2. 改造：渲染搬出中断

`Synth/port/audio_out_sai.c` 重写为 **主循环渲染 + 3 块预渲染环**：

- `synth_port_audio_pump()`：主循环每轮调用一次，环里空位就渲染 **1 块**（受控工作量）；
- 音频中断里只 `memcpy` 一块 + `SCB_CleanDCache_by_Addr`（几十微秒）；
- 环空时中断重复上一块并累加 `g_synth_audio_underruns` —— 宁可"卡一下"，也不拖死 USB 和主循环；
- `main()` 主循环第一步就是 `synth_port_audio_pump()`。

## 3. 实测（出厂音色 1，16 个音同时按下，`max_voices` 依次变化）

| 版本 | cpu_permille | render_peak_cycles | audio_underruns | FLASH |
|---|---|---|---|---|
| 中断里渲染（旧）+ `-O2` | 2121‰ = 212% | 2,879,758 (225%) | 590 | 103,280 B |
| 预渲染环 + 控制率 8→32 + 噪声/子振按需计算 | 1450‰ = 145% | 2,014,280 (157%) | 345 | 103,632 B |
| 再 + `-O3 -ffast-math` | 1311‰ = 131% | 1,767,464 (138%) | 264 | 115,280 B (88%) |
| 再 + `max_voices` 16→10 | **812‰ = 81%** | 1,162,280 (91%) | **0** | 115,280 B (88%) |

结论：

- 单声部约 **8.6%** 实时预算（出厂音色 1），实时上限约 **11 声部**；
- FM 音色（`engine_type == 1`）单声部成本是模拟音色的 **3 倍多**（PC 端基准：16 声部 338 µs vs 80~102 µs/块）；
- 因此 `Core/Src/main.c` 的 `cfg.max_voices` 定为 **10**（留 15~20% 余量给效果链和 FM 音色）。

## 4. 三处代码改动

1. `Synth/port/audio_out_sai.c`：3 块预渲染环（`.ram_section` / AXI、32 字节对齐）+ `synth_port_audio_pump()`；
   `Synth/port/synth_port.h` 增加声明与 `g_synth_audio_underruns`。
2. `Synth/src/core/voice.h`：`kCtrlRate` **8 → 32**（控制率 6 kHz → 1.5 kHz，`UpdateControl()` 里的
   `exp2f`/`tanf`/`sqrtf` 在 16 声部时是一成多预算）；
   `Synth/src/core/voice.cpp`：`sub_lvl_` / `noise_lvl_` 为 0 时跳过子振/噪声（原来每样点都算）。
3. `CMakeLists.txt`：引擎与端口层 `-O2` → **`-O3 -ffast-math -fno-math-errno`**。
   代价是 FLASH 103.6 → 115.3 KB（88%）。**若以后听到音高/包络异常，第一个要回退的就是 `-ffast-math`。**

## 5. 诊断字段（`g_app_status` 末尾，索引 32..34）

| 字段 | 含义 |
|---|---|
| `cpu_permille` | 引擎渲染负载千分比，1000 = 正好用满实时预算；> 600 就没余量 |
| `render_peak_cycles` | 单块渲染最大周期数；预算 1,280,000 周期 |
| `audio_underruns` | 主循环没喂上、音频中断重复上一块的次数（>0 = 有卡顿） |

`tools/status.ps1` 已解码并加了两条判据（`cpu_permille > 700`、`audio_underruns > 0`）。

## 6. 还能怎么提速（按性价比，尚未做）

1. **把引擎代码放进 ITCM**（64 KB 空着）：Flash 4 等待周期 + I-Cache 失效是当前最大的不确定项，预期 1.3~2×；
2. LTO（`-flto`）：跨 TU 内联，预期 5~15%，但 Flash 已 88%，可能装不下；
3. 振荡器 / `SineTable::Get` 的插值改成查表 + FMA，少一次边界判断；
4. 按负载动态降级（>90% 时对最老声部快速释音），比硬性少声部更"音乐化"。
