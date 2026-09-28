# Celadon 的 CMake 工程只用于 Linux 无头自动测试（ADR-003）。
# Mac 插件构建以 Celadon.jucer 为唯一真源，不走这里。
#
# 这里不用 JUCE 自带的 CMake（add_subdirectory(JUCE)）：它配置时要先编 juceaide，
# juceaide 依赖 juce_graphics → 服务器得装 fontconfig / freetype 等图形开发包。
# 测试只需要 4 个非界面模块，直接把模块的编译单元编成静态库，服务器零额外依赖。
#
# Source/dsp/ 的源文件列表必须与 Celadon.jucer 的 dsp 组保持一致；加文件时两边一起改。

cmake_minimum_required(VERSION 3.22)

project(Celadon VERSION 0.1.7.2 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 全工程的逐位契约（HalfBand / Saturator / TruePeak 的各路径与块长无关、Color / Limiter 的空闲跳过、GLAZE 整链与参照……）
# 都要求乘和加各自舍入（ADR-013，BUG-002）。GCC 对 C++ 默认 -ffp-contract=fast：目标有 FMA（-march=x86-64-v3 / native、aarch64）
# 时跨语句合并成融合乘加，且各循环不一样。GCC 一律关掉；宏给 HalfBand / Saturator / TruePeak 的编译期守卫看。
# clang 保持默认 on（只合并同一表达式），与 Xcode 的正式构建一致。
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options(-ffp-contract=off)
    add_compile_definitions(CELADON_FP_CONTRACT_OFF=1)
endif()

set(CELADON_JUCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../JUCE" CACHE PATH "JUCE 9.0.2 根目录（与 .jucer 的 ../JUCE/modules 同一份）")
set(JUCE_MODULES "${CELADON_JUCE_DIR}/modules")

# ---- JUCE 非界面模块（juce_dsp 依赖 juce_audio_formats → juce_audio_basics → juce_core）----
# Apple 平台上 JUCE 模块必须按 Objective-C++ 编（.mm 版本只是 #include 同名 .cpp）。
if(APPLE)
    enable_language(OBJCXX)
    set(JUCE_MODULE_EXT mm)
else()
    set(JUCE_MODULE_EXT cpp)
endif()

add_library(celadon_juce STATIC
    ${JUCE_MODULES}/juce_core/juce_core.${JUCE_MODULE_EXT}
    ${JUCE_MODULES}/juce_core/juce_core_CompilationTime.cpp
    ${JUCE_MODULES}/juce_core/juce_core_zlib.c
    ${JUCE_MODULES}/juce_audio_basics/juce_audio_basics.${JUCE_MODULE_EXT}
    ${JUCE_MODULES}/juce_audio_formats/juce_audio_formats.${JUCE_MODULE_EXT}
    ${JUCE_MODULES}/juce_audio_formats/juce_audio_formats_flac_1.c
    ${JUCE_MODULES}/juce_audio_formats/juce_audio_formats_flac_2.c
    ${JUCE_MODULES}/juce_dsp/juce_dsp.${JUCE_MODULE_EXT})

target_include_directories(celadon_juce SYSTEM PUBLIC ${JUCE_MODULES})

target_compile_definitions(celadon_juce
    PUBLIC
        JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1
        JUCE_STANDALONE_APPLICATION=1
        JUCE_USE_CURL=0
        JUCE_WEB_BROWSER=0
        JUCE_MODULE_AVAILABLE_juce_core=1
        JUCE_MODULE_AVAILABLE_juce_audio_basics=1
        JUCE_MODULE_AVAILABLE_juce_audio_formats=1
        JUCE_MODULE_AVAILABLE_juce_dsp=1
        $<$<CONFIG:Debug>:DEBUG=1 _DEBUG=1>
        $<$<NOT:$<CONFIG:Debug>>:NDEBUG=1>)

find_package(Threads REQUIRED)
target_link_libraries(celadon_juce PUBLIC Threads::Threads ${CMAKE_DL_LIBS})

if(APPLE)
    # 各模块头里 OSXFrameworks 列的系统框架。
    target_link_libraries(celadon_juce PUBLIC
        "-framework Accelerate" "-framework AudioToolbox" "-framework Cocoa" "-framework CoreAudio"
        "-framework CoreMIDI" "-framework Foundation" "-framework IOKit" "-framework QuartzCore"
        "-framework Security")
else()
    target_link_libraries(celadon_juce PUBLIC rt)
endif()

# ---- 被测代码 + 测试 ----
set(CELADON_DSP_SOURCES
    Source/dsp/BalanceModule.cpp
    Source/dsp/CeladonChain.cpp
    Source/dsp/ClayGuard.cpp
    Source/dsp/ClayModule.cpp
    Source/dsp/ColorModule.cpp
    Source/dsp/GlazeCalibrator.cpp
    Source/dsp/GlazeConstants.cpp
    Source/dsp/GlazeController.cpp
    Source/dsp/GlazeGuard.cpp
    Source/dsp/GlazePlanner.cpp
    Source/dsp/GlazeProfile.cpp
    Source/dsp/HalfBandOversampler.cpp
    Source/dsp/LinearPhaseLowpass.cpp
    Source/dsp/LimiterModule.cpp
    Source/dsp/LoudnessMeter.cpp
    Source/dsp/MusicAnalyzer.cpp
    Source/dsp/Saturator.cpp
    Source/dsp/TruePeak.cpp)

# 界面里唯一只依赖 juce_core 的文件：提示文案（测试检查覆盖所有参数）。
set(CELADON_HEADLESS_UI_SOURCES
    Source/ui/GlazeStatusText.cpp
    Source/ui/HelpText.cpp)

add_executable(celadon_tests
    ${CELADON_DSP_SOURCES}
    ${CELADON_HEADLESS_UI_SOURCES}
    tests/TestMain.cpp
    tests/LimiterTests.cpp
    tests/DspPartsTests.cpp
    tests/ColorTests.cpp
    tests/ChainTests.cpp
    tests/HelpTextTests.cpp
    tests/BalanceTests.cpp
    tests/BalancePerformanceTests.cpp
    tests/GlazeTests.cpp
    tests/GlazeBehaviourReport.cpp
    tests/MusicAnalyzerTests.cpp
    tests/GlazePlannerTests.cpp
    tests/GlazeCalibrationTests.cpp
    tests/GlazeControllerTests.cpp
    tests/GlazeStatusTextTests.cpp
    tests/ClayTests.cpp
    tests/ClayGuardTests.cpp
    tests/ClayGlazeTests.cpp
    tests/LegacyFixtureTests.cpp)

# 只对我们自己的代码开严格警告，JUCE 模块按 JUCE 自己的标准编。
target_compile_options(celadon_tests PRIVATE -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion)
target_link_libraries(celadon_tests PRIVATE celadon_juce)

# ---- 真歌离线对照工具（v0.1.7.0，设计规格-Clay-分析决策与验收 第 7 节；四个子项目共用）----
# 读一个文件夹的 wav，按设置网格离线渲染整条链、按同一整体响度对齐后写 32-bit float wav，另出指标表。
# 只链接 Source/dsp + juce_audio_formats（celadon_juce 已含），在测试服务器上跑：scripts/render-linux.sh。
# EXCLUDE_FROM_ALL：默认构建（测试脚本）不编它，工具坏了也不挡测试；用 --target celadon_render 单独编。
add_executable(celadon_render EXCLUDE_FROM_ALL
    ${CELADON_DSP_SOURCES}
    tools/render/RenderMain.cpp)
target_compile_options(celadon_render PRIVATE -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion)
target_link_libraries(celadon_render PRIVATE celadon_juce)
