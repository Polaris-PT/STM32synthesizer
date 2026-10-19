# 代码地图（读懂这个工程的顺序）

> 这份文档回答一个具体问题：**从哪读起、每层负责什么、数据从 MIDI 字节到喇叭经过了谁。**
> 配套文档：`docs/architecture.md`（数据流图、资源预算、各阶段完成度）、`docs/patch-format.md`（音色文本格式）、
> `docs/handoff.md`（当前状态与踩过的坑）、`docs/phase2-review.md`（生成工程的审查结论）。

## 1. 三层结构

```
┌──────────────────────────────────────────────────────────────────────┐
│ 硬件层  Core/  +  Drivers/  +  Middlewares/   ← CubeMX 生成，尽量不手改 │
│   main.c(USER CODE 区)  sai.c  quadspi.c  sdmmc.c  usart.c  it.c       │
└───────────────────────────┬──────────────────────────────────────────┘
                            │ 只通过 Synth/port/ 里的 C 胶水互相认识
┌───────────────────────────┴──────────────────────────────────────────┐
│ 端口层  Synth/port/   ← 唯一碰 HAL / TinyUSB / FatFs 的地方（C）        │
│   audio_out_sai.c     SAI3_B 双缓冲 + DMA 回调 + D-Cache clean         │
│   usb_midi_tinyusb.c  TinyUSB 描述符 + tud_midi_rx_cb → synth_midi_in  │
│   platform_stm32.c    USB 引脚/时钟/NVIC、PWR 电压检测、D-Cache、DWT   │
│   debug_uart.c        串口日志（8 KB 环形缓冲，非阻塞）                │
│   sample_storage_qspi.c  0x90000000 零拷贝采样                         │
│   patch_storage_sd.c / sample_storage_sd.c  FatFs（SYNTH_PORT_SD_ENABLE）│
└───────────────────────────┬──────────────────────────────────────────┘
                            │ 只有 C ABI：Synth/include/synth/*.h
┌───────────────────────────┴──────────────────────────────────────────┐
│ 引擎层  Synth/src/ + Synth/include/   ← C++17，不碰任何 HAL            │
│   core/     engine.cpp  voice  osc  filter  env  wavetable  rompler    │
│   engines/  fm_dx7  fm_voices  sysex        （DX7 六运算器）           │
│   fx/       fx_chain  chorus  delay  fx_memory（192 KB 静态池）        │
│   app/      patch_text  instrument_text  arp （文本音色 / 琶音器）      │
│   midi/     midi_parser.h （纯头文件状态机）                            │
└──────────────────────────────────────────────────────────────────────┘
```

**为什么这样分层**：引擎层可以在 PC 上编译跑单测（`host/`、`tests/`，用 mingw g++），
上板时只换端口层；CubeMX 重新生成代码不会碰到引擎。

## 2. 四个执行上下文（先建立时间模型，再看代码）

| 上下文 | 优先级 | 谁 | 干什么 | 文件 |
|---|---|---|---|---|
| SAI3_B DMA 半传输/传输完成中断 | **0（最高）** | `HAL_SAI_TxHalfCpltCallback` / `TxCpltCallback` | 渲染 128 帧填半个缓冲 + clean D-Cache | `Synth/port/audio_out_sai.c` |
| USB OTG_FS 中断 | 5 | `OTG_FS_IRQHandler` → `tud_int_handler(0)` | TinyUSB 收发；`tud_midi_rx_cb` 把 MIDI 字节塞进无锁环形缓冲 | `Core/Src/stm32h7xx_it.c`、`Synth/port/usb_midi_tinyusb.c` |
| SysTick | 15 | `SysTick_Handler` | `HAL_IncTick()` + `synth_tick_1ms()`（1 ms 时基） | `Core/Src/stm32h7xx_it.c` |
| 主循环 | 线程 | `main()` 的 `while(1)` | `synth_port_usb_task()`（枚举/类请求）、`synth_port_patch_service()`、`synth_service()`、日志、LED | `Core/Src/main.c` |

**关键推论**：声音只由 DMA 中断驱动。主循环再慢，只要是"慢"而不是"卡"，就不该有爆音；
出现"卡住没声音"要么是主循环卡死（有 `STALL` 日志）、要么是更高优先级中断里卡死
（`audio_isr_depth` / `usb_isr_depth` 探针），要么是音频缓冲被写坏（cache 一致性）。

## 3. 一次 MIDI 音符的完整路径

```
USB 4 字节事件包
  └─ tud_midi_rx_cb()                       Synth/port/usb_midi_tinyusb.c
       └─ synth_midi_in(bytes, n)           Synth/src/core/engine.cpp:463
            └─ 写入 ring[]（SPSC 无锁）      engine.cpp:71-74
主循环 / 音频中断里
  └─ DrainMidi()                            engine.cpp:285
       └─ MidiParser::Push()                Synth/src/midi/midi_parser.h（running status / SysEx 处理）
            └─ HandleMessage(status, d1, d2) engine.cpp:271
                 ├─ 0x8n note-off → VoiceNoteOff(e, ch, ...)  engine.cpp:203（查 note_voice[ch][note]）
                 ├─ 0x9n note-on  → VoiceNoteOn(e, ch, ...)   engine.cpp:172 → AllocateVoice(e, ch, ...):123（偷音策略）
                 │                  音色取自 e.chan_patch[ch]，抄进 Voice::patch_ 快照
                 ├─ 0xBn CC       → ControlChange(e, ch, ...) engine.cpp:240（CC5/72/73/76/77/79 改本通道音色并推给在响声部）
                 ├─ 0xCn PC       → ApplyPatchIndexToChannel(e, ch, n)（只切本通道，越界忽略）
                 └─ 0xF8 clock    → Arp::OnMidiClock()
渲染（DMA 中断里，每 128 帧）
  └─ RenderBlock(engine, out, frames)        engine.cpp:296
       ├─ Arp::AdvanceBlock()                Synth/src/app/arp.cpp
       ├─ 16 个 Voice::Render(out, frames, g) Synth/src/core/voice.cpp（用自己的 patch_ 快照 +
       │                                     OscModel + FilterBlock + 2×Adsr）
       ├─ FmDx7Engine::Render()   仅当 ActiveNotes()>0  Synth/src/engines/fm_dx7.cpp（一次只能载入一条 DX7 音色）
       ├─ Rompler::Render()       仅当 ActiveNotes()>0  Synth/src/core/rompler.cpp（乐器全局一个）
       └─ FxChain::Process()                 Synth/src/fx/fx_chain.cpp（全局一条，参数取"主音色" e.patch）
  └─ synth_render_i32()                      engine.cpp:451（float → int32，24 bit 左对齐）
       └─ g_audio_buf（AXI SRAM `.ram_section`，8 KB）
            └─ SAI3_B DMA → I2S → PCM5102A
```

## 4. 引擎内部的关键类型

- **`Engine`**（`engine.cpp:39-88`）：全局单例 `g_eng`。持有多音色映射 `chan_patch[16]`/`chan_idx[16]`
  （每 MIDI 通道一个音色；`patch` 是"主音色"，全局 FX 参数取自它）、`GlobalParams g`、
  `Voice voices[16]`、`note_voice[16][128]`/`note_held[16][128]`（通道 + 音符→声部映射、按住状态）、
  `FxChain fx`、`Arp arp`、`FmDx7Engine fm`、`Rompler rom`、MIDI 环形缓冲、控制线程请求标志
  （`req_patch_index`/`req_panic_flag`/…，主循环写、音频线程读）、`block_buf`。
- **`Voice`**（`core/voice.h:22`）：一个声部 = `OscModel`（双振荡器：wavetable/SuperSaw/噪声，可 detune）
  + `FilterBlock`（TPT SVF，12/24 dB 低通、高通、带通）+ 两个 `Adsr`（音量、滤波包络，可调制 cut-off）
  + 滑音/连奏/偷音淡出。`voice.cpp` 里 `Render()` 是单声部 DSP 的全部。
- **`OscModel` / `TableOsc` / `SuperSaw`**（`core/osc.h`）：波表带 mip 分级（抗混叠），
  `WaveTables`（`core/wavetable.cpp`）预计算 Saw/Tri 各级谐波表，共 33.5 KB，放 `.dtcm_bss`。
- **`TptSvf` / `FilterBlock`**（`core/filter.h`，纯头文件，全 inline）：零延迟反馈 SVF。
- **`Adsr`**（`core/env.h`，纯头文件）：五段状态机（Idle/Attack/Decay/Sustain/Release），时间→系数是解析式。
- **`MidiParser`**（`midi/midi_parser.h`，纯头文件）：字节流状态机，处理 running status、
  按状态字节推断数据字节数、SysEx 丢弃计数。
- **`FxChain`**（`fx/fx_chain.cpp`）：drive（软削波 + 补偿）→ chorus → delay；
  缓冲全部来自 `fx_memory.cpp` 的 192 KB 静态池（`.ram_section`，自写 4 字节对齐 bump 分配，不用 malloc）。
- **`Rompler`**（`core/rompler.cpp`）：多样本乐器，三次插值 + 循环接缝交叉淡化；采样可在 QSPI flash 里零拷贝。
- **`FmDx7Engine`**（`engines/fm_dx7.cpp`）：DX7 六运算器，`fm_voices.cpp` 提供 VCED 156 字节的构造器，
  `sysex.cpp` 解析 `.syx` 音色库。
- **`Arp`**（`app/arp.cpp`）：Up/Down/UpDown/Random/Played 五种模式，可按 MIDI 时钟同步。

## 5. 对外契约（C ABI）

- `Synth/include/synth/synth.h`：`synth_init/render_i32/midi_in/tick_1ms/service/...`
  + `synth_config_t`（采样率必须 48 kHz）+ `synth_stats_t`（声部数、偷音、丢字节、CPU 周期）
  + `synth_platform_hooks_t`（`cycles_now`/`cpu_hz`，用于 CPU 占用统计，由端口层安装）
  + `synth_set_channel_patch(ch, idx)` / `synth_get_channel_patch(ch)`（多音色：每通道一个音色，
    越界索引返回 0xFFFF；请求排进队列、音频线程下一块生效）。
- `Synth/include/synth/patch.h`：`synth_patch_t`（振荡器/滤波/包络/LFO/FX 参数 + 范围常量）、
  `synth_factory_bank[]`（内置音色表）。
- `Synth/include/synth/sample.h` / `pcm.h`：采样乐器/区/格式。

## 6. 内存布局（决定了哪些代码不能放在哪）

| 区域 | 地址 / 大小 | 放什么 | 为什么 |
|---|---|---|---|
| FLASH | 0x08000000 / 128 KB | 代码 + 常量 | — |
| DTCMRAM | 0x20000000 / 128 KB | 栈 8 KB + 堆 0.5 KB + `.dtcm_bss`：波表 33.5 KB、正弦表 4 KB | **CPU 零等待、但 DMA1/DMA2 访问不到** |
| AXI SRAM | 0x24000000 / 512 KB | `.bss`/`.data` + `.ram_section`：音频缓冲 8 KB、FX 池 192 KB、（可选）采样池 128 KB | DMA 可达；`.ram_section` 是 NOLOAD，**上电是随机值** |
| QSPI 映射 | 0x90000000 / 8 MB | W25Q64 内容（采样） | 只读、非缓存（MPU Region2） |

**两条硬规矩**：① DMA 缓冲绝不能放 DTCM；② `.ram_section` 里的变量必须自己初始化（NOLOAD 不跑 C 运行时清零）。

## 7. 推荐阅读顺序

1. **`Core/Src/main.c` 的 `main()`**（在 `USER CODE 4` 区，约 11 步）＋ `Synth/port/audio_out_sai.c`（69 行）。
   → 回答"谁在什么时候渲染音频、缓冲怎么切两半"。
2. **`Synth/include/synth/synth.h`**（73 行）＋ **`patch.h`**（88 行）。
   → 回答"引擎对外承诺什么、一个音色由哪些参数组成"。
3. **`Synth/src/core/engine.cpp`**：先看 `Engine` 结构体（39-88），
   再看 `DrainMidi`(285)/`HandleMessage`(271)/`VoiceNoteOn`(172)/`AllocateVoice`(123)/`RenderBlock`(296)。
   → 回答"MIDI 字节怎么变成声部、怎么混音"。
4. **`Synth/src/core/voice.cpp`**（169 行）＋ `osc.h`/`filter.h`/`env.h`。
   → 回答"一个音符的波形、滤波、包络具体怎么算"。
5. 按兴趣选修：`fx/fx_chain.cpp`（效果）、`core/rompler.cpp`（采样）、`engines/fm_dx7.cpp`（FM）、`app/arp.cpp`（琶音）。
6. 配合 `docs/architecture.md` 看资源预算与各阶段结论；`docs/patch-format.md` 看音色文本语法。

## 8. 调试抓手

- **串口日志**（USART1：PB14 = TX、PB15 = RX、115200 8N1）：
  开机横幅、各阶段结果、每秒一行 `stat`、`STALL` 行、每个 USB-MIDI 事件包、HardFault 现场。
  开关在 `Synth/port/synth_port_config.h`（`SYNTH_PORT_DEBUG_UART`、`SYNTH_PORT_MIDI_TRACE`）。
- **`g_app_status`**（32 个 u32，定义在 `Core/Src/main.c`）：时钟、QSPI ID、故障位、
  USB 寄存器快照、`loop_count`/`systick_ticks` 存活探针；用 `tools/status.ps1` 一条命令读回并解码。
- **`g_fault`**（20 个 u32）：HardFault 现场（CFSR/HFSR/BFAR/MMFAR/PC/LR 栈帧），软复位不丢。
- **PC 端单测**：`host/` + `tests/`，可脱离硬件验证 DSP 与解析器。
- **构建/烧写/读数**：`tools/build.ps1`、`tools/flash.ps1`、`tools/status.ps1`（都带 UTF-8 BOM，PS 5.1 才不会读乱）。

## 9. 诊断代码清单（可整体删除）

上板排查用的东西都集中在这里，**功能代码不依赖它们**（删掉后固件照常发声）。要减负时按需删：

| 位置 | 内容 | 删的时候注意 |
| --- | --- | --- |
| `Core/Src/main.c` PTD/PV | `g_app_status`（32 u32）、`g_fault`（20 u32，在 `.ram_section`） | 一并删 USER CODE 0 里喂它们的函数 |
| `Core/Src/main.c` USER CODE 0 | `App_RecordBoot` / `App_LedInit` / `App_LedTask` / `App_UsbKernelClockHz` / `App_SnapshotUsb` / `App_CheckSaiClock` / `App_Fault` / `App_HardFaultRecord*` / `App_FindFaultFrame` / `App_FaultBlinkForever` | `App_HardFaultRecordC` 被 `Core/Src/stm32h7xx_it.c` 的 `HardFault_Handler` 调用，要一并删 |
| `Core/Src/stm32h7xx_it.c` | SysTick 里 `++g_synth_systick_ticks`、OTG_FS 里 `++g_synth_usb_irq_count` | 同文件里的 `OTG_FS_IRQHandler → tud_int_handler(0)` **不是**诊断（USB 必需），别删 |
| `Synth/port/debug_uart.[ch]` | 串口日志（8 KB 环形缓冲 + 逐字节 IT + 接管 `_write`） | 把 `SYNTH_PORT_DEBUG_UART` 置 0 即可（全变空实现），不必删文件 |
| `Synth/port/platform_stm32.c` | `g_synth_systick_ticks` / `g_synth_usb_isr_depth` / `g_synth_usb_clk_rc` | 与 main.c 的探针一起删 |
| `Synth/port/audio_out_sai.c` | `g_synth_audio_isr_depth` / `g_synth_audio_last_half` / XRUN 日志 | 同上 |
| `Synth/port/usb_midi_tinyusb.c` | `g_synth_midi_rx_bytes` / `g_synth_usb_mount_count` / `g_synth_usb_umount_count` / `g_synth_usb_irq_count` | 同上 |
| `tools/status.ps1` | PC 侧读回并解码 | 纯脚本，不影响固件 |

三个开关都在 `Synth/port/synth_port_config.h`：

- `SYNTH_PORT_DEBUG_UART`：1 = 串口日志（默认），0 = 空实现（省 flash）
- `SYNTH_PORT_MIDI_TRACE`：每个 USB-MIDI 事件包打一行（默认 0，查"音停不下来"时开）
- `SYNTH_PORT_FAULT_RESET`：0 = HardFault 后停住闪灯（默认），1 = 记录完软复位、自动恢复发声

实测成本：诊断 + 串口日志约占 **8~11 KB flash**；`g_app_status` 128 B + `g_fault` 80 B 的 RAM（`.ram_section`）。
