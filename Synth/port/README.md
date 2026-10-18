# Synth/port —— 与 CubeMX 工程的胶水层

这一层是**唯一**包含 HAL / TinyUSB / FATFS 头文件的地方；`Synth/src` 里的引擎完全平台无关。

## main.c 里需要调用的四件事

```c
MX_SAI3_Init();                  /* CubeMX 生成（SAI3 Block B） */
synth_port_init();               /* D-Cache / FPU FTZ / DWT / USB 时钟 / 平台钩子 */

synth_config_t cfg = { SYNTH_SR_DEFAULT, SYNTH_MAX_VOICES, synth_factory_bank, synth_factory_bank_size };
synth_init(&cfg);
synth_port_audio_start();        /* 填满两个半缓冲并启动 SAI3 循环 DMA */
synth_port_usb_init();
synth_port_sd_mount();           /* 可选：SD 音色 */

while (1) {
    synth_port_usb_task();       /* tud_task() */
    synth_port_patch_service();  /* Program Change → /synth/NNN.syn */
    synth_service();
}
```

可选：把 `synth_tick_1ms()` 放进 SysTick 或 TIM 中断（琶音器在渲染块内已经是采样精确调度，
这个 tick 只用于统计/后台任务）。

## HAL 回调

`audio_out_sai.c` 已经实现了 `HAL_SAI_TxHalfCpltCallback` / `HAL_SAI_TxCpltCallback` /
`HAL_SAI_ErrorCallback`，**不要**在别处重复实现。

## TinyUSB

本项目已经 vendor：`Synth/third_party/tinyusb`（MIT，commit `3c6524e`，2026-10-16）。
它自带的 `src/CMakeLists.txt` 提供 `tinyusb_target_add(TARGET)`，但**不含 DCD 驱动**，H7 需要自己补 dwc2：

```cmake
add_subdirectory(Synth/third_party/tinyusb/src ${CMAKE_BINARY_DIR}/tinyusb_src)
tinyusb_target_add(${CMAKE_PROJECT_NAME})
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
  Synth/third_party/tinyusb/src/portable/synopsys/dwc2/dcd_dwc2.c
  Synth/port/usb_midi_tinyusb.c)
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE Synth/port)   # 让 tusb_config.h 可见
```

`tusb_config.h` 放在 `Synth/port/`（`CFG_TUD_MIDI=1`、`CFG_TUSB_MCU=OPT_MCU_STM32H7`、`CFG_TUSB_OS=OPT_OS_NONE`）。

如果日后要重新拉取（例如升版本）：

```powershell
git -c http.proxy= -c https.proxy= clone --depth 1 https://github.com/hathach/tinyusb Synth/third_party/tinyusb
```

并把 `OTG_FS_IRQHandler` 改成调用 TinyUSB 的中断处理（见下发 TinyUSB 里
`src/portable/synopsys/dwc2/dwc2_stm32.h` 实际导出的符号，一般为 `tud_int_handler(0)`，
老版本是 `dcd_int_handler(0)`）。**不要**再启用 ST 的 USB Device 中间件。

## 引脚/句柄适配

只改 `synth_port_config.h`：
- `SYNTH_SAI_HANDLE`：CubeMX 生成的 SAI 句柄名（默认 `hsai3`）
- `SYNTH_PORT_BLOCK_FRAMES`：半缓冲帧数（默认 128）
- `SYNTH_PORT_AUDIO_RAM`：音频缓冲所在段（默认 `.ram_section` = AXI SRAM）
- `SYNTH_PORT_PATCH_DIR`：SD 音色目录
