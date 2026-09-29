# 整插件性能基准（v0.5 起的性能验收工具；ADR-007）：把整个 CeladonAudioProcessor 按 Xcode 的实际编译参数编成无界面
# 基准程序 celadon_bench（processBlock 含两块表、参数读取、GLAZE 映射、整链；另有按模块 / 部件拆分与界面绘制计时），
# 可选再编一个 VST3 宿主 celadon_host，直接加载 Builds/MacOSX/build/<配置>/Celadon.vst3 实测（-DBUILD_HOST=ON）。
# 编译参数照抄 DerivedData 里 Xcode 的 *-common-args.resp（2026-09-25 v0.5 研究 P1 实读）：
#   Debug  : -O0 -g -D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_DEBUG -DDEBUG=1 -D_DEBUG=1
#   Release: -O3 -flto（LLVM_LTO=YES）-DNDEBUG=1 -D_NDEBUG=1，链接 -O3 -flto -dead_strip
# 用法见 scripts/perf-local.sh（加锁、低优先级、构建目录放系统临时目录）。
cmake_minimum_required(VERSION 3.22)
project(celadon_perf LANGUAGES C CXX OBJC OBJCXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OBJCXX_STANDARD 17)
set(CMAKE_OBJCXX_EXTENSIONS OFF)
# 架构：默认按本机硬件（Apple Silicon → arm64，Intel Mac → x86_64）；Apple Silicon 上也可以 -DCELADON_PERF_ARCH=x86_64
# 编出 x86_64 版、经 Rosetta 跑，粗看 Intel 那一半的开销（Rosetta 的数字不能当 Intel 真机的验收读数）。
set(CELADON_PERF_ARCH "" CACHE STRING "arm64 | x86_64（空 = 本机硬件架构）")
if(CELADON_PERF_ARCH STREQUAL "")
  execute_process(COMMAND sysctl -n hw.optional.arm64 OUTPUT_VARIABLE _celadon_has_arm64 OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(_celadon_has_arm64 STREQUAL "1")
    set(CMAKE_OSX_ARCHITECTURES arm64)
  else()
    set(CMAKE_OSX_ARCHITECTURES x86_64)
  endif()
else()
  set(CMAKE_OSX_ARCHITECTURES ${CELADON_PERF_ARCH})
endif()
message(STATUS "celadon_perf: ${CMAKE_OSX_ARCHITECTURES}")
set(CMAKE_OSX_DEPLOYMENT_TARGET 12.0)
get_filename_component(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
get_filename_component(JUCE_MODULES "${ROOT}/../JUCE/modules" ABSOLUTE)
# 原有的 ReleaseFastMath 一档已删：任何构建都不许 -ffast-math（ADR-007、ADR-013，三个逐位契约文件的守卫会报错）。
set(FLAVOUR "XcodeRelease" CACHE STRING "XcodeRelease | XcodeDebug | DebugO1 | DebugO0NoHardening | ReleaseO2NoLto")

include(units.cmake)
# CONFIGURE_DEPENDS：以后 Source 里加 / 删 .cpp，重新构建时自动重扫，不必手工重跑 cmake 配置。
file(GLOB PLUGIN_SOURCES CONFIGURE_DEPENDS ${ROOT}/Source/*.cpp ${ROOT}/Source/dsp/*.cpp ${ROOT}/Source/ui/*.cpp)

set(COMMON_FLAGS -fno-common -fpascal-strings -fvisibility=hidden $<$<COMPILE_LANGUAGE:CXX,OBJCXX>:-fvisibility-inlines-hidden>)
if(FLAVOUR STREQUAL "XcodeDebug")
  set(OPT_FLAGS -O0 -g $<$<COMPILE_LANGUAGE:CXX,OBJCXX>:-D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_DEBUG>)
  set(CFG_DEFS DEBUG=1 _DEBUG=1)
  set(LINK_FLAGS_FLAVOUR -O0)
elseif(FLAVOUR STREQUAL "DebugO1")
  set(OPT_FLAGS -O1 -g $<$<COMPILE_LANGUAGE:CXX,OBJCXX>:-D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_DEBUG>)
  set(CFG_DEFS DEBUG=1 _DEBUG=1)
  set(LINK_FLAGS_FLAVOUR -O1)
elseif(FLAVOUR STREQUAL "DebugO0NoHardening")
  set(OPT_FLAGS -O0 -g)
  set(CFG_DEFS DEBUG=1 _DEBUG=1)
  set(LINK_FLAGS_FLAVOUR -O0)
elseif(FLAVOUR STREQUAL "ReleaseO2NoLto")
  set(OPT_FLAGS -O2)
  set(CFG_DEFS NDEBUG=1 _NDEBUG=1)
  set(LINK_FLAGS_FLAVOUR -O2 -Wl,-dead_strip)
else() # XcodeRelease
  set(OPT_FLAGS -O3 -flto)
  set(CFG_DEFS NDEBUG=1 _NDEBUG=1)
  set(LINK_FLAGS_FLAVOUR -O3 -flto -Wl,-dead_strip)
endif()

set(MODS audio_basics audio_devices audio_formats audio_plugin_client audio_processors audio_processors_headless
         audio_utils core data_structures dsp events graphics gui_basics gui_extra)
set(ALL_DEFS ${CFG_DEFS} JUCE_PROJUCER_VERSION=0x90002 JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1
  JUCE_VST3_CAN_REPLACE_VST2=0 JUCE_STRICT_REFCOUNTEDPOINTER=1 JUCE_STANDALONE_APPLICATION=1
  JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_PLUGINHOST_VST3=1 JUCE_MODAL_LOOPS_PERMITTED=1)
foreach(m ${MODS})
  list(APPEND ALL_DEFS JUCE_MODULE_AVAILABLE_juce_${m}=1)
endforeach()

function(celadon_setup tgt)
  target_include_directories(${tgt} PRIVATE ${ROOT}/JuceLibraryCode ${ROOT}/Source)
  target_include_directories(${tgt} SYSTEM PRIVATE ${JUCE_MODULES}
      ${JUCE_MODULES}/juce_audio_processors_headless/format_types/VST3_SDK)
  target_compile_definitions(${tgt} PRIVATE ${ALL_DEFS})
  target_compile_options(${tgt} PRIVATE ${OPT_FLAGS} ${COMMON_FLAGS}
      $<$<COMPILE_LANGUAGE:CXX,OBJCXX,C,OBJC>:-include ${ROOT}/JuceLibraryCode/JucePluginDefines.h>)
endfunction()

add_library(juce_units STATIC ${JUCE_UNITS})
celadon_setup(juce_units)

set(FRAMEWORKS Accelerate AudioToolbox AudioUnit Cocoa CoreAudio CoreAudioKit CoreMIDI DiscRecording Foundation IOKit
               Metal MetalKit QuartzCore Security WebKit)

add_executable(celadon_bench bench.mm ${PLUGIN_SOURCES})
celadon_setup(celadon_bench)
target_link_libraries(celadon_bench PRIVATE juce_units)
target_link_options(celadon_bench PRIVATE ${LINK_FLAGS_FLAVOUR})
foreach(fw ${FRAMEWORKS})
  target_link_libraries(celadon_bench PRIVATE "-framework ${fw}")
endforeach()

if(BUILD_HOST)
  add_executable(celadon_host host.mm)
  celadon_setup(celadon_host)
  target_link_libraries(celadon_host PRIVATE juce_units)
  target_link_options(celadon_host PRIVATE ${LINK_FLAGS_FLAVOUR})
  foreach(fw ${FRAMEWORKS})
    target_link_libraries(celadon_host PRIVATE "-framework ${fw}")
  endforeach()
endif()
