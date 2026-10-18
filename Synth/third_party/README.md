# 第三方源码（vendored）

| 目录 | 来源 | 许可 | 说明 |
|---|---|---|---|
| `tinyusb/` | https://github.com/hathach/tinyusb @ `3c6524e`（2026-10-16） | **MIT**（见 `tinyusb/LICENSE`） | USB MIDI 设备栈（dwc2 驱动）。只保留 `src/` 与许可文件；更新方式：`git -c http.proxy= clone --depth 1 https://github.com/hathach/tinyusb` 后取其 `src/`。**不要修改 vendor 代码。** |
| `msfa/` | Google `music-synthesizer-for-android` 的 msfa 核心，取自 `Synth_Dexed`（codeberg.org/dcoredump/Synth_Dexed）`src/` | **Apache-2.0**（见 `msfa/LICENSE-APACHE2.txt`） | DX7 六运算器 FM 合成核心。只取 Apache-2.0 的 11 组文件：`EngineMsfa`、`fm_core`、`fm_op_kernel`、`dx7note`、`env`、`sin`、`exp2`、`freqlut`、`lfo`、`pitchenv`、`porta` + `synth.h`/`controllers.h`/`aligned_buf.h`。**未引入** Synth_Dexed 里 GPL-3.0 的 `dexed.cpp`（VoiceManager）、`EngineMkI`、`EngineOpl`、`PluginFx`。**不要修改 vendor 代码。** |

> DaisySP（MIT，Electrosmith）在阶段 3（Karplus-Strong、ModalVoice 等）按需引入，届时在此登记。
> 当前振荡器/滤波/效果均为自研（对比见 `docs/architecture.md`：自研 mip 带限波表混叠 −121 dB）。
