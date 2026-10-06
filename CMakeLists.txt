# Celadon 的 CMake 工程只用于无头自动测试与几个命令行工具（ADR-003）：原本只在 Linux 上跑；v0.1.8.0 起同一套 celadon_tests
# 也能在 Windows（Visual Studio 2022 / MSVC，或 clang-cl）与 macOS（Apple Silicon、Intel）上配置、编译、运行，用来验证跨平台。
# 插件本身（VST3 / AU）仍以 Celadon.jucer 为唯一真源：Mac 走 Xcode 导出，Windows 走 Visual Studio 2022 导出，不走这里。
#
# 这里不用 JUCE 自带的 CMake（add_subdirectory(JUCE)）：它配置时要先编 juceaide，
# juceaide 依赖 juce_graphics → 服务器得装 fontconfig / freetype 等图形开发包。
# 测试只需要 4 个非界面模块，直接把模块的编译单元编成静态库，服务器零额外依赖。
#
# Source/dsp/ 的源文件列表必须与 Celadon.jucer 的 dsp 组保持一致；加文件时两边一起改——配置期会自动核对（见下「与 Celadon.jucer 的源文件清单自动核对」，T-032）。

cmake_minimum_required(VERSION 3.22)

project(Celadon VERSION 0.1.8.9 LANGUAGES C CXX)

# 目标是 Windows 时对象文件全路径最长 250 字符（BUG-007）。CMake 按「主机」取这个上限：Linux 主机 1000、Windows 主机 250。
# 在 109 上用 msvc-wine 交叉编（Linux 主机，scripts/windows-wine.sh）时就是 1000、从不缩短，而 Wine 里的 cl.exe 受 Windows 的
# 260 字符路径上限（MAX_PATH）限制。仓库外的 JUCE 模块，其对象文件路径里嵌着源文件的整条绝对路径（远端目录名因此出现两次），
# 工作树名稍长就越线：…/juce_audio_formats_flac_1.c.obj 262 字符 → C1083「Cannot open compiler generated file」。
# 按 Windows 主机的默认值设：超过时由 CMake 换成散列的短路径；真 Windows 上（test-windows.ps1）本来就是这个值，不变。
if(CMAKE_SYSTEM_NAME STREQUAL "Windows" AND NOT DEFINED CMAKE_OBJECT_PATH_MAX)
    set(CMAKE_OBJECT_PATH_MAX 250)
endif()

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

# MSVC 与 clang-cl（CMake 里都算 MSVC）：
#   /utf-8  源码是不带 BOM 的 UTF-8（中文字面量经 celadon::utf8 转换）；不给的话中文 Windows 按 GBK 读源码，字面量变乱码。
#   /bigobj JUCE 模块的合并编译单元与几个大测试文件的节数超过默认上限。
#   浮点保持默认的 /fp:precise：不合并乘加（ADR-013）；/fp:fast、/fp:contract 由 Saturator / TruePeak 等源码里的 #error 拦住。
#   clang-cl 沿用 clang 默认的 -ffp-contract=on（并预定义 _M_FP_CONTRACT，源码守卫会报错）：与 GCC 一样关掉。
#   /Zc:lambda（只给 cl.exe 的 C++）：C++17 下 MSVC 默认用旧的 lambda 处理器，lambda 不捕获就读外层的局部 constexpr
#           会报 C3493（标准允许，GCC / clang 照编）；打开合标准的处理器（C++20 起默认开）。与 Celadon.jucer 的 VS2022 导出一致。
if(MSVC)
    add_compile_options(/utf-8 /bigobj)

    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        add_compile_options(/clang:-ffp-contract=off)
        add_compile_definitions(CELADON_FP_CONTRACT_OFF=1)
    else()
        add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/Zc:lambda>)
    endif()
endif()

# 我们自己代码的警告等级（JUCE 模块按 JUCE 自己的标准编）。
if(MSVC)
    set(CELADON_WARNING_FLAGS /W4)
else()
    set(CELADON_WARNING_FLAGS -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion)
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
elseif(UNIX)
    target_link_libraries(celadon_juce PUBLIC rt)
endif()
# Windows：JUCE 模块在 MSVC / clang-cl 下用 #pragma comment (lib, …) 自己声明系统库（ws2_32、winmm、version……），这里不必再列。

# ---- 被测代码 + 测试 ----
set(CELADON_DSP_SOURCES
    Source/dsp/ReferenceSpectrum.cpp
    Source/dsp/ReleaseSafety.cpp
    Source/dsp/SegmentLedger.cpp
    Source/dsp/LiveProfile.cpp
    Source/dsp/ClayTrials.cpp
    Source/dsp/BalanceModule.cpp
    Source/dsp/CeladonChain.cpp
    Source/dsp/ClayGuard.cpp
    Source/dsp/ClayModule.cpp
    Source/dsp/ColorModule.cpp
    Source/dsp/ContrastGuard.cpp
    Source/dsp/OutputLevelTracker.cpp
    Source/dsp/PsrGuard.cpp
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

# 界面里只依赖 juce_core / juce_dsp 的文件：提示文案（测试检查覆盖所有参数）、GLAZE 状态行、界面语言表与频谱仪计算（v0.1.9.0）。
set(CELADON_HEADLESS_UI_SOURCES
    Source/ui/GlazeStatusText.cpp
    Source/ui/HelpText.cpp
    Source/ui/Localisation.cpp
    Source/ui/SpectrumAnalyser.cpp)

# ---- 与 Celadon.jucer 的源文件清单自动核对（T-032 R10-04）----
# 文件头承诺「Source/dsp/ 的源文件列表必须与 Celadon.jucer 的 dsp 组保持一致」此前全靠人：漏在 .jucer 的 .cpp 会让 Mac 插件链接失败或
# 旧实现留在插件里；漏在这里的文件测试跑不到。现在配置期读 .jucer，dsp 组的 .cpp 必须与 CELADON_DSP_SOURCES 完全相同，
# 无头界面文件必须是 .jucer ui 组的子集；不一致直接 FATAL_ERROR。.jucer 里 file="…" 与 compile="1" 可能不在同一行，按 file="…cpp" 匹配即可（.cpp 一律 compile=1）。
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/Celadon.jucer" CELADON_JUCER_TEXT)

function(celadon_jucer_sources out_var subdir)
    string(REGEX MATCHALL "file=\"Source/${subdir}/[^\"]+\.cpp\"" matches "${CELADON_JUCER_TEXT}")
    set(files "")
    foreach(m IN LISTS matches)
        string(REGEX REPLACE "^file=\"(.*)\"$" "\\1" f "${m}")
        list(APPEND files "${f}")
    endforeach()
    list(SORT files)
    list(REMOVE_DUPLICATES files)
    set(${out_var} "${files}" PARENT_SCOPE)
endfunction()

celadon_jucer_sources(CELADON_JUCER_DSP_SOURCES dsp)
celadon_jucer_sources(CELADON_JUCER_UI_SOURCES ui)

set(CELADON_DSP_SOURCES_SORTED ${CELADON_DSP_SOURCES})
list(SORT CELADON_DSP_SOURCES_SORTED)
if(NOT "${CELADON_JUCER_DSP_SOURCES}" STREQUAL "${CELADON_DSP_SOURCES_SORTED}")
    message(FATAL_ERROR "CMakeLists.txt 的 CELADON_DSP_SOURCES 与 Celadon.jucer 的 dsp 组不一致（两边一起改）。\n"
                        "  .jucer：${CELADON_JUCER_DSP_SOURCES}\n  CMake ：${CELADON_DSP_SOURCES_SORTED}")
endif()

foreach(f IN LISTS CELADON_HEADLESS_UI_SOURCES)
    if(NOT "${f}" IN_LIST CELADON_JUCER_UI_SOURCES)
        message(FATAL_ERROR "CMakeLists.txt 的无头界面文件 ${f} 不在 Celadon.jucer 的 ui 组里（两边一起改）。")
    endif()
endforeach()

add_executable(celadon_tests
    ${CELADON_DSP_SOURCES}
    ${CELADON_HEADLESS_UI_SOURCES}
    tests/AdvancedMasteringTests.cpp
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
    tests/Phase2OrchestratorTests.cpp
    tests/GlazeControllerTests.cpp
    tests/GlazeStatusTextTests.cpp
    tests/ClayTests.cpp
    tests/ClayGuardTests.cpp
    tests/ClayGlazeTests.cpp
    tests/LegacyFixtureTests.cpp
    tests/AnalyzerTapTests.cpp
    tests/LocalisationTests.cpp
    tests/SegmentLedgerTests.cpp
    tests/LiveProfileTests.cpp
    tests/ClayTrialsTests.cpp
    tests/SpectrumAnalyserTests.cpp)

# 只对我们自己的代码开严格警告，JUCE 模块按 JUCE 自己的标准编。
target_compile_options(celadon_tests PRIVATE ${CELADON_WARNING_FLAGS})
target_link_libraries(celadon_tests PRIVATE celadon_juce)

# 测试把整条链（CeladonChain 约 70 KB）、几块表当局部变量摆在栈上，单个测试函数的栈帧最大约 1 MB。Linux / macOS 主线程默认 8 MB，
# Windows 只有 1 MB，会栈溢出：测试程序在 Windows 上把主线程栈设为 16 MB。插件本身不受影响（链是处理器的成员，在堆上）。
if(MSVC)
    target_link_options(celadon_tests PRIVATE /STACK:16777216)
endif()

# ---- 真歌离线对照工具（v0.1.7.0，设计规格-Clay-分析决策与验收 第 7 节；四个子项目共用）----
# 读一个文件夹的 wav，按设置网格离线渲染整条链、按同一整体响度对齐后写 32-bit float wav，另出指标表。
# 只链接 Source/dsp + juce_audio_formats（celadon_juce 已含），在测试服务器上跑：scripts/render-linux.sh。
# EXCLUDE_FROM_ALL：默认构建（测试脚本）不编它，工具坏了也不挡测试；用 --target celadon_render 单独编。
add_executable(celadon_render EXCLUDE_FROM_ALL
    ${CELADON_DSP_SOURCES}
    tools/render/RenderMain.cpp)
target_compile_options(celadon_render PRIVATE ${CELADON_WARNING_FLAGS})
target_link_libraries(celadon_render PRIVATE celadon_juce)

if(MSVC)
    target_link_options(celadon_render PRIVATE /STACK:16777216)
endif()

# ---- 界面截图工具（v0.1.8.0 界面改版起；Linux 与 Windows，EXCLUDE_FROM_ALL）----
# 把整个插件（处理器 + 编辑器）连同 JUCE 的界面模块编进一个命令行程序：喂合成的音乐、跑消息循环、把编辑器按几种状态
#（中 / 英、A / B、警告、设置面板、不同缩放）离屏渲染成 PNG，用来对照 V4 设计稿。Mac 上照旧用 Xcode 看真插件。
# Linux 依赖 freetype2 / fontconfig / X11 的开发包（JUCE 界面模块在 Linux 上的要求）；DISPLAY 可以没有（xvfb-run 更稳）。
# Windows 不需要额外的包（JUCE 用系统的 DirectWrite / Direct2D）：也是验证界面代码在 Windows 上编得过、跑得起来的入口。
# 用法：cmake --build <dir> --target celadon_ui_snapshot && <dir>/celadon_ui_snapshot --out <目录>（见 tools/ui-snapshot）。
set(CELADON_CAN_BUILD_GUI OFF)

if(WIN32)
    set(CELADON_CAN_BUILD_GUI ON)
elseif(NOT APPLE)
    find_package(PkgConfig QUIET)

    if(PKG_CONFIG_FOUND)
        pkg_check_modules(CELADON_GUI_DEPS QUIET IMPORTED_TARGET freetype2 fontconfig x11)
    endif()

    if(CELADON_GUI_DEPS_FOUND)
        set(CELADON_CAN_BUILD_GUI ON)
    endif()
endif()

if(CELADON_CAN_BUILD_GUI)
    add_library(celadon_juce_gui STATIC EXCLUDE_FROM_ALL
        ${JUCE_MODULES}/juce_core/juce_core.cpp
        ${JUCE_MODULES}/juce_core/juce_core_CompilationTime.cpp
        ${JUCE_MODULES}/juce_core/juce_core_zlib.c
        ${JUCE_MODULES}/juce_events/juce_events.cpp
        ${JUCE_MODULES}/juce_data_structures/juce_data_structures.cpp
        ${JUCE_MODULES}/juce_graphics/juce_graphics.cpp
        ${JUCE_MODULES}/juce_graphics/juce_graphics_Harfbuzz.cpp
        ${JUCE_MODULES}/juce_graphics/juce_graphics_Sheenbidi.c
        ${JUCE_MODULES}/juce_graphics/juce_graphics_libjpg_1.c
        ${JUCE_MODULES}/juce_graphics/juce_graphics_libjpg_2.c
        ${JUCE_MODULES}/juce_graphics/juce_graphics_libjpg_3.c
        ${JUCE_MODULES}/juce_graphics/juce_graphics_libpng.c
        ${JUCE_MODULES}/juce_graphics/juce_graphics_lunasvg.c
        ${JUCE_MODULES}/juce_gui_basics/juce_gui_basics.cpp
        ${JUCE_MODULES}/juce_gui_basics/juce_gui_basics_2.cpp
        ${JUCE_MODULES}/juce_gui_basics/juce_gui_basics_3.cpp
        ${JUCE_MODULES}/juce_gui_basics/juce_gui_basics_4.cpp
        ${JUCE_MODULES}/juce_gui_basics/juce_gui_basics_5.cpp
        ${JUCE_MODULES}/juce_gui_extra/juce_gui_extra.cpp
        ${JUCE_MODULES}/juce_audio_basics/juce_audio_basics.cpp
        ${JUCE_MODULES}/juce_audio_formats/juce_audio_formats.cpp
        ${JUCE_MODULES}/juce_audio_formats/juce_audio_formats_flac_1.c
        ${JUCE_MODULES}/juce_audio_formats/juce_audio_formats_flac_2.c
        ${JUCE_MODULES}/juce_audio_processors_headless/juce_audio_processors_headless.cpp
        ${JUCE_MODULES}/juce_audio_processors/juce_audio_processors.cpp
        ${JUCE_MODULES}/juce_dsp/juce_dsp.cpp)

    target_include_directories(celadon_juce_gui SYSTEM PUBLIC ${JUCE_MODULES})
    target_compile_definitions(celadon_juce_gui
        PUBLIC
            JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1
            JUCE_STANDALONE_APPLICATION=1
            JUCE_USE_CURL=0
            JUCE_WEB_BROWSER=0
            JUCE_USE_XRANDR=0
            JUCE_USE_XINERAMA=0
            JUCE_MODAL_LOOPS_PERMITTED=1
            JUCE_MODULE_AVAILABLE_juce_core=1
            JUCE_MODULE_AVAILABLE_juce_events=1
            JUCE_MODULE_AVAILABLE_juce_data_structures=1
            JUCE_MODULE_AVAILABLE_juce_graphics=1
            JUCE_MODULE_AVAILABLE_juce_gui_basics=1
            JUCE_MODULE_AVAILABLE_juce_gui_extra=1
            JUCE_MODULE_AVAILABLE_juce_audio_basics=1
            JUCE_MODULE_AVAILABLE_juce_audio_formats=1
            JUCE_MODULE_AVAILABLE_juce_audio_processors_headless=1
            JUCE_MODULE_AVAILABLE_juce_audio_processors=1
            JUCE_MODULE_AVAILABLE_juce_dsp=1
            $<$<CONFIG:Debug>:DEBUG=1 _DEBUG=1>
            $<$<NOT:$<CONFIG:Debug>>:NDEBUG=1>)
    if(WIN32)
        target_link_libraries(celadon_juce_gui PUBLIC Threads::Threads)
    else()
        target_link_libraries(celadon_juce_gui PUBLIC PkgConfig::CELADON_GUI_DEPS Threads::Threads ${CMAKE_DL_LIBS} rt)
    endif()

    # 插件源码 #include <JuceHeader.h>：给截图工具一份只含上面这些模块的 JuceHeader（工程里的那份还包括插件外壳与设备模块）。
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/ui-snapshot-include/JuceHeader.h
"#pragma once
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_graphics/juce_graphics.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
namespace ProjectInfo { const char* const projectName = \"Celadon\"; const char* const versionString = \"0.1.8.9\"; const int versionNumber = 0x10809; }
")

    file(GLOB CELADON_UI_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/Source/ui/*.cpp)

    add_executable(celadon_ui_snapshot EXCLUDE_FROM_ALL
        ${CELADON_DSP_SOURCES}
        ${CELADON_UI_SOURCES}
        Source/Parameters.cpp
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
        tools/ui-snapshot/SnapshotMain.cpp)
    target_include_directories(celadon_ui_snapshot PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/ui-snapshot-include)

    if(MSVC)
        target_compile_options(celadon_ui_snapshot PRIVATE /FI${CMAKE_CURRENT_SOURCE_DIR}/JuceLibraryCode/JucePluginDefines.h ${CELADON_WARNING_FLAGS})
    else()
        target_compile_options(celadon_ui_snapshot PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/JuceLibraryCode/JucePluginDefines.h
                                                           ${CELADON_WARNING_FLAGS})
    endif()

    target_link_libraries(celadon_ui_snapshot PRIVATE celadon_juce_gui)

    if(MSVC)
        target_link_options(celadon_ui_snapshot PRIVATE /STACK:16777216)
    endif()
endif()
