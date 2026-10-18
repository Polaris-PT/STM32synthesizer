# STM32H750 + PCM5102A 合成器（应用层）

面向 **WeAct MiniSTM32H7xx（STM32H750VBT6，128KB flash / 1MB RAM）** + **PCM5102A**（I2S）的合成器应用层代码。
驱动由 CubeMX 生成，本仓库只包含**合成器核心 + USB MIDI + 音色/存储 + 琶音器**，通过 `Synth/port/` 与 HAL 对接。

## 目录
| 路径 | 说明 |
|---|---|
| `Synth/include/synth/` | 公开 API（`synth.h` / `patch.h` / `pcm.h`），不依赖 HAL |
| `Synth/src/core/` | 振荡器模型、滤波器、包络、声部、复音分配、引擎 |
| `Synth/src/midi/` | MIDI 1.0 字节流解析 |
| `Synth/src/fx/` | 合唱 / 延迟 / 软限幅（阶段 1 后续） |
| `Synth/src/app/` | 音色库、SD 音色、琶音器 |
| `Synth/port/` | **唯一接触 HAL / TinyUSB 的地方**（上板时用） |
| `host/` | PC 端离线渲染验证台（mingw g++ / gcc，无需硬件） |
| `tests/` | 宿主端单元测试 |
| `docs/cubemx-checklist.md` | **你需要在 CubeMX 里做的每一项 + 验收判据** |
| `docs/midi-cc-map.md` | 默认 CC 映射 |
| `docs/architecture.md` | 音频路径、引脚依据、预算 |

## 宿主端（PC）验证：不需要任何硬件
```powershell
cmake -G Ninja -S host -B host/build
cmake --build host/build
./host/build/ssynth_render.exe --demo --patch 0 --out demo.wav   # 离线渲染 WAV
./host/build/ssynth_tests.exe                                    # 单元测试
```

## 上板集成（等你用 CubeMX 生成驱动后）
1. 按 `docs/cubemx-checklist.md` 配置外设（**音频 = SAI3 Block B：PD8=SCK、PD9=SD、PD10=FS、PD14=MCLK**）。
2. 在 CubeMX 工程的根 `CMakeLists.txt` 中加入：
```cmake
file(GLOB_RECURSE SYNTH_SOURCES "Synth/src/*.c" "Synth/src/*.cpp")
file(GLOB_RECURSE SYNTH_PORT_SOURCES "Synth/port/*.c" "Synth/port/*.cpp")
target_sources(${CMAKE_PROJECT_NAME} PRIVATE ${SYNTH_SOURCES} ${SYNTH_PORT_SOURCES})
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE "Synth/include" "Synth/src" "Synth/src/core" "Synth/port")
set_source_files_properties(${SYNTH_SOURCES} PROPERTIES COMPILE_OPTIONS "-O2")
```
3. `main.c` 中调用 `synth_init()` → `synth_port_audio_start()` → 主循环里 `synth_service()` / `synth_tick_1ms()`。
4. 若 CubeMX 生成的 SAI 句柄名不同，只改 `Synth/port/synth_port_config.h`。

## 约束
- 采样率固定 48 kHz；16 音复音；无 RTOS；C++17 引擎 + C ABI 边界。
- 音频 DMA 缓冲必须放 AXI SRAM(0x24000000) 或 D2 SRAM(0x30000000)，不得放 DTCM。
