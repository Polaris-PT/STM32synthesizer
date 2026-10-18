cmake_minimum_required(VERSION 3.20)
project(ssynth_host CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

file(GLOB_RECURSE SYNTH_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/../Synth/src/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../Synth/third_party/msfa/*.cpp")   # msfa：Apache-2.0
# 用 OBJECT 库：保证 hooks.cpp 里的弱符号默认实现一定参与链接
add_library(synthcore OBJECT ${SYNTH_SOURCES})
target_include_directories(synthcore PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/../Synth/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/../Synth/src"
  "${CMAKE_CURRENT_SOURCE_DIR}/../Synth/src/core"
  "${CMAKE_CURRENT_SOURCE_DIR}/../Synth/src/engines"
  "${CMAKE_CURRENT_SOURCE_DIR}/../Synth/third_party/msfa")
target_compile_options(synthcore PRIVATE -O2 -Wall)

add_executable(ssynth_render render_main.cpp wav_writer.cpp $<TARGET_OBJECTS:synthcore>)
target_link_libraries(ssynth_render PRIVATE synthcore)

add_executable(ssynth_tests "${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_dsp.cpp" $<TARGET_OBJECTS:synthcore>)
target_link_libraries(ssynth_tests PRIVATE synthcore)
