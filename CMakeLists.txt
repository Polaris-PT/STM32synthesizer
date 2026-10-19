cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#
# ==== 应用层修改说明（CubeMX 不会覆盖本文件）====
#   1) 打开 C++17（引擎 Synth/src 是 C++）
#   2) 加入引擎源（Synth/src/**/*.cpp + Synth/third_party/msfa/*.cpp）与端口层（Synth/port/*.c）
#   3) 加入 vendored TinyUSB（只编 device + MIDI + dwc2，省 flash）
#   4) 默认构建类型改成 RelWithDebInfo（-O2 -g）：Debug(-O0) 下 HAL+TinyUSB+引擎
#      在 128KB flash 里会紧张；要单步调试时再 -DCMAKE_BUILD_TYPE=Debug。
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

# 引擎（Synth/src + msfa）用 C++17；-fno-exceptions/-fno-rtti 已在
# cmake/gcc-arm-none-eabi.cmake 的 CMAKE_CXX_FLAGS 里统一设置
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# gnu++17（不是严格 c++17）：vendored msfa 用 M_PI，而 newlib 只在 _GNU_SOURCE 下暴露它，
# 严格模式下 Synth/third_party/msfa/sin.cpp:38 会报 "'M_PI' was not declared in this scope"。
set(CMAKE_CXX_EXTENSIONS ON)

# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME STM32synthesizer)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings（C CXX ASM：CubeMX 生成的启动文件也要编译）
project(${CMAKE_PROJECT_NAME} C CXX ASM)
message("Build type: " ${CMAKE_BUILD_TYPE})

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# 〔LTO 已试过并放弃，这里留个记录，别再踩一遍〕
# -flto 在这套代码上能省 ~4 KB（116,172 B vs 120,072 B），但**与 HAL 的弱符号覆盖模式冲突**：
# HAL 里 `HAL_*_MspInit()` 是 `__weak` 空实现，我们的强定义在 Core/Src/*.c。整程序 LTO 时
# GCC 会让库里那个弱版本"胜出"，我们的 MspInit 被丢掉 —— ELF 里 `HAL_SAI_MspInit` 符号整个
# 消失，实机现象是 SAI/QSPI/SD 全没配（fault = 0x126、没声音、QSPI 映射失败）。
# 给 HAL 库加 -fno-lto 能修好弱符号，但省下来的就只剩 ~144 B（119,928 B），不值得；
# 给 Core/Src 加 -fno-lto 无效（目标级 -flto 在命令行上排得更靠后）。
# 结论：不提供 LTO 开关，发布版用 SYNTH_RELEASE + 冷路径 -Os（120,072 B / 91.61%）就够。

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# ---------------------------------------------------------------------------
# 合成器引擎 + STM32 端口层
# ---------------------------------------------------------------------------
# 引擎：Synth/src 下全部 .cpp（含 msfa：Apache-2.0，vendored 在 Synth/third_party/msfa）
file(GLOB_RECURSE SYNTH_ENGINE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Synth/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Synth/third_party/msfa/*.cpp")
# 端口层：CubeMX 工程 ↔ 引擎的适配（SAI DMA、TinyUSB、SD/QSPI、平台初始化）
file(GLOB SYNTH_PORT_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Synth/port/*.c")

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    ${SYNTH_ENGINE_SOURCES}
    ${SYNTH_PORT_SOURCES}
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/Synth/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/Synth/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/Synth/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/Synth/src/engines"
    "${CMAKE_CURRENT_SOURCE_DIR}/Synth/port"
    "${CMAKE_CURRENT_SOURCE_DIR}/Synth/third_party/msfa"
    "${CMAKE_CURRENT_SOURCE_DIR}/Synth/third_party/tinyusb/src"
)

# 引擎与端口层的实时预算：48 kHz、128 帧/块 = 2.667 ms（480 MHz 下 1.28M 周期）。
# 2026-10-19 实测（16 声部、出厂音色 1，板上 cpu_permille）：
#   -O2 默认                        → 2121‰（还带 tanf/exp2f 控制率浪费）
#   控制率 8→32、噪声/子振按需计算   → 1450‰
#   -O3 + -ffast-math（下面这条）    → 见 docs/phase2-review.md §9
# -ffast-math 对音频可以接受：省掉了 denormal 处理与 errno 分支，
# 引擎里没有需要 IEEE 边角语义（NaN/±0/inf）的算法。
set_source_files_properties(${SYNTH_ENGINE_SOURCES} ${SYNTH_PORT_SOURCES}
    PROPERTIES COMPILE_OPTIONS "-O3;-ffast-math;-fno-math-errno")

# ---------------------------------------------------------------------------
# FatFs / SD 驱动不在实时路径上 → 用 -Os 编译，把 flash 留给合成器。
# 背景：H750 只有 128 KB flash。打开 SD（SYNTH_PORT_SD_ENABLE=1）后 FatFs + SDMMC
# HAL 约 +18 KB，2026-10-19 曾把 .rodata 挤出 FLASH 2,616 B。本文件 CubeMX 只生成一次
# （重新生成不会覆盖），而 cmake/stm32cubemx/CMakeLists.txt 每次都会被重写，
# 所以与尺寸有关的编译开关放在这里。
# ---------------------------------------------------------------------------
target_compile_options(FatFs PRIVATE -Os)
target_compile_options(STM32_Drivers PRIVATE -Os)
set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/FATFS/App/fatfs.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/FATFS/Target/bsp_driver_sd.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/FATFS/Target/sd_diskio.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/FATFS/Target/fatfs_platform.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/sdmmc.c"
    PROPERTIES COMPILE_OPTIONS "-Os"
)

# ---------------------------------------------------------------------------
# 发布构建裁剪（2026-10-19）：H750 只有 128 KB flash，诊断代码（串口日志 + 现场打印）
# 是最大的一块可变开销。打开这个开关后：
#   - Synth/port/debug_uart.c 编译为空
#   - main.c 与端口层里的 dbg_log() 变成空宏 → 诊断字符串 + vsnprintf 都不再链进固件
#   - HardFault 的串口现场打印去掉（现场照旧写进 g_fault，tools\status.ps1 读得到）
# 保留：HardFault 记录、存活探针、PE3 心跳灯、g_app_status（合计几十字节，是排查的唯一手段）。
# 用法：cmake -DSYNTH_RELEASE=ON …（或 tools\build.ps1 传 -DSYNTH_RELEASE=ON，见脚本说明）
# ---------------------------------------------------------------------------
option(SYNTH_RELEASE "裁剪诊断代码（关调试串口日志）" OFF)
if(SYNTH_RELEASE)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE SYNTH_PORT_DEBUG_UART=0)
    message(STATUS "SYNTH_RELEASE=ON：调试串口日志已关闭（诊断字符串不再进 flash）")
endif()

# 非热路径用 -Os：CubeMX 应用源 + "冷"端口层文件（DSP 热路径与音频 ISR 仍是 -O3）。
# audio_out_sai.c 故意不在此列：它的 DMA 回调在优先级 0 的中断里跑。
set(SYNTH_CUBEMX_APP_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/main.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/stm32h7xx_it.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/stm32h7xx_hal_msp.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/gpio.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/dma.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/usart.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/quadspi.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/sai.c")
set_source_files_properties(${SYNTH_CUBEMX_APP_SOURCES} PROPERTIES COMPILE_OPTIONS "-Os")

set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/Synth/port/platform_stm32.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/Synth/port/usb_midi_tinyusb.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/Synth/port/patch_storage_sd.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/Synth/port/sample_storage_sd.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/Synth/port/sample_storage_qspi.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/Synth/port/debug_uart.c"
    PROPERTIES COMPILE_OPTIONS "-Os")

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # SD/FatFs 开关在 Synth/port/synth_port_config.h 里（SYNTH_PORT_SD_ENABLE），
    # 等 CubeMX 里加上 SDMMC1 + FATFS 后把它改成 1，不必动本文件。
)

# ---------------------------------------------------------------------------
# TinyUSB（MIT，vendored：Synth/third_party/tinyusb，commit 3c6524e）
# 官方 src/CMakeLists.txt 的 tinyusb_target_add() 定义在子目录作用域，根目录调用不到，
# 这里直接列需要的源文件：device 栈 + MIDI 类 + dwc2(OTG_FS) 驱动。
# CFG_TUD_* 在 Synth/port/tusb_config.h 里配置（只开 MIDI）。
# ---------------------------------------------------------------------------
set(TINYUSB_SRC "${CMAKE_CURRENT_SOURCE_DIR}/Synth/third_party/tinyusb/src")
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    "${TINYUSB_SRC}/tusb.c"
    "${TINYUSB_SRC}/common/tusb_fifo.c"
    "${TINYUSB_SRC}/device/usbd.c"
    "${TINYUSB_SRC}/class/midi/midi_device.c"
    "${TINYUSB_SRC}/portable/synopsys/dwc2/dcd_dwc2.c"
    "${TINYUSB_SRC}/portable/synopsys/dwc2/dwc2_common.c"
)

# TinyUSB 也不在实时路径上（重型工作在 tud_task()，主循环里）→ -Os 省 flash
set_source_files_properties(
    "${TINYUSB_SRC}/tusb.c"
    "${TINYUSB_SRC}/common/tusb_fifo.c"
    "${TINYUSB_SRC}/device/usbd.c"
    "${TINYUSB_SRC}/class/midi/midi_device.c"
    "${TINYUSB_SRC}/portable/synopsys/dwc2/dcd_dwc2.c"
    "${TINYUSB_SRC}/portable/synopsys/dwc2/dwc2_common.c"
    PROPERTIES COMPILE_OPTIONS "-Os"
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)
