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

# 引擎必须 -O2（48kHz、128 帧/块，每块 ~2.6ms 的预算）；不要开 -ffast-math。
set_source_files_properties(${SYNTH_ENGINE_SOURCES} PROPERTIES COMPILE_OPTIONS "-O2")
# 端口层回调（SAI 半传输/完成中断里的渲染调用）也在实时路径上
set_source_files_properties(${SYNTH_PORT_SOURCES} PROPERTIES COMPILE_OPTIONS "-O2")

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

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)
