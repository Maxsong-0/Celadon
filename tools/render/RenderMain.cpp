/*
  ==============================================================================

    RenderMain.cpp
    celadon_render：真歌离线对照工具（v0.1.7.0 修坯 Clay 引入，四个子项目共用；设计规格-Clay-分析决策与验收 第 7 节）。

    合成信号证明不了「好听」。本工具把一个文件夹里的真歌按设置网格（GLAZE × 修坯 自动 / 全关 × 风格）过整条链离线渲染，
    全部按同一整体响度只乘增益对齐后写成 32-bit float wav 供盲听，另出指标表 metrics.md / metrics.csv。
    只链接 Source/dsp + juce_audio_formats（CMake 目标 celadon_render，EXCLUDE_FROM_ALL）：不依赖任何界面模块与
    juce_audio_processors，也就不经过 APVTS——各参数的出厂默认值取 Source/ParameterDefaults.h（插件参数表读的同一处真源，
    T-032 R10-01；此前在这里照 Parameters.cpp 手写一份，0.1.9.0 加的高级母带 8 项没跟上）。
    整个进程在 FTZ / DAZ 下跑（main 开头一次，T-032 R10-06）：与插件的音频线程同一数值环境（PluginProcessor::processBuses）。
    在测试服务器上编译运行（scripts/render-linux.sh；ADR-002 修订 4，服务器故障时以 Max 当场指令为准）。用法与输出说明见同目录 README.md。

    用法：celadon_render --in <歌曲目录> --out <输出目录> [--glaze 0,35,70] [--clay auto,off]
                         [--style auto[,warm,punchy,clean]] [--block 512] [--match-lufs -16]
                         [--metrics-only] [--trace] [--no-replay]
          celadon_render --selftest   （指标算法、文件名、延迟对齐、GLAZE 0 逐位直通、同决策重放的阳性对照；render-linux.sh 渲染前先跑）

    每首歌、每组设置分两遍（「先完整聆听再出成品」）：
      1. 聆听：新链 → setParams → prepare（与 PluginProcessor::prepareToPlay 同一顺序），按离线宿主口径（offline、在播、
         位置随块推进）把整首歌送进链，输出丢弃。GLAZE 在这一遍里学画像、走方案过渡、校准；
      2. 存档：createGlazeStateXml——即宿主存工程时 GLAZE 那一节（已提交版本 + 执行快照 EXECUTION，含 Clay 实际值与保护状态）；
      3. 成品：另起一条新链 → restoreGlazeStateXml → setParams → prepare（= 重开工程后离线导出），从头渲染整首歌，
         末尾补「链延迟 + 余振」个零把尾巴冲出来，丢掉开头「链延迟」个样本：成品与原曲、各组成品之间逐样本对齐。

    为什么第二遍用「存档 → 新链读回」，而不是在同一条链上跳回位置 0（handleDiscontinuity）：
      - 新链的各模块、两块表、分析器短期视图都是干净的。同一条链跳回 0 时，限幅器前瞻延迟线、Balance / Clay 滤波器、
        Glue 包络里还留着歌曲末尾，成品开头会先吐出一小段歌尾；要清就得整链 reset，而编排器的 reset 与「保存重开」本来就走
        同一个执行快照入口（GlazeController::onHostReset → adoptExecution）——GLAZE 这边两种做法得到的量一样，模块却只有
        新链是干净的；
      - 这正是用户真实的流程「听一遍 → 存工程 → 重开 → 离线导出」，也是 Codex rendercheck（tools/perf/bench.mm）验过的路径：
        实时 / 离线逐样本一致、读回后第一块就保持已执行的控制量。
    所以成品从第一个样本起就是学好之后（已提交画像、方案、校准、执行快照）的处理。

    输出（--out 目录）：
      - <歌名>__g<GLAZE>_clay-<auto|off|replay-off>_<风格>.wav：32-bit float，整体响度只乘增益对齐到 --match-lufs（默认 −16 LUFS），
        允许超过 0 dBFS，不再限幅；
      - metrics.md / metrics.csv：每首每组的对齐前整体响度、真峰值、PLR、限幅 GR 平均 / 最大、输出低频（< 125 Hz）左右相关度、
        真次声（< 16 Hz，8 阶）能量占比、Clay 四个实际值、保护介入比例、GLAZE 状态；每首另有一行「原始输入」作对照。
        每做完一首歌就重写一次：中途中断也留下已完成的部分。
      - GLAZE 0 时修坯「自动」与「全关」按规格逐位相同（设计规格-Clay 第 4 节「逐位直通」）：两组都在网格里时按对齐前成品的
        哈希核对，结果写进 metrics.md 的备注——真歌上的阳性对照。
      - 同决策重放（ADR-016，--no-replay 关掉）：每组 GLAZE > 0 的修坯「自动」之后，另起 GLAZE 0 的链逐块照搬它实际交给模块的
        参数、修坯换成「关」，多出一行 / 一个成品 clay-replay-off（修坯门槛拿它与「自动」比：两条链决策相同，差别只剩修坯）；
        同时做阳性对照（修坯「手动」照填自动链实际值重放，应逐位相同），结论在 metrics.csv 的 replay_check。
    --selftest：合成信号上的阳性对照（相关度 / 真次声 / 响度 / 真峰值的解析值、文件名、链延迟（282 + Clay 的 D + G）与逐样本对齐、
    GLAZE 0 逐位直通、同决策重放逐位复现），不读歌；这个工具不在 celadon_tests 里，render-linux.sh 渲染真歌前先跑它。
    进度、警告与错误都写 stderr；返回 0 = 全部成功，1 = 有文件读 / 写失败（或自检失败），2 = 参数错误。
    单线程：一次只跑一条链。DSP 代码没有按「多条链并发」验证过，测试服务器也是共用的，不宜一次占满多核。

  ==============================================================================
*/

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>

#include "../../Source/ParameterDefaults.h"
#include "../../Source/dsp/CeladonChain.h"
#include "../../Source/dsp/ChainParams.h"
#include "../../Source/dsp/GlazeTypes.h"
#include "../../Source/dsp/ClayGuard.h"
#include "../../Source/dsp/ClayModule.h"
#include "../../Source/dsp/LoudnessMeter.h"
#include "../../Source/dsp/TruePeak.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace
{
    using celadon::glaze::Phase;
    using celadon::glaze::Status;

    //==============================================================================
    // 常数

    // 成品在「链延迟」之外再冲出的余振（秒）：与插件报给宿主的尾巴同一处真源 CeladonChain::filterRingOutSeconds（0.35 s，
    // Clay 10 Hz 4 阶低切衰 60 dB 约 0.29 s 加余量；审核 V3-02：这里原来写死 0.3）。各组成品一样长，仍逐样本对齐。
    constexpr double ringOutSeconds = celadon::CeladonChain::filterRingOutSeconds;

    // 两个指标的分频点（设计规格-Clay-分析决策与验收 第 7 节「低频相关度、超低频能量」；次声口径按 ADR-012 改）。
    constexpr double lowCorrelationCutoffHz = 125.0;
    constexpr double infrasonicCutoffHz = 16.0;   // 真次声（ADR-012：与分析器 subShareDb 同一定义，8 阶 Butterworth 16 Hz）

    // 4 阶 Butterworth = 两级二阶：Q = 1 / (2·cos(π/8))、1 / (2·cos(3π/8))（与 ClayModule 4 阶低切同一组 Q）。
    constexpr double butterworthQ1 = 0.54119610014619698;
    constexpr double butterworthQ2 = 1.3065629648763766;
    // 8 阶 Butterworth = 四级二阶：Q = 1 / (2·cos((2k − 1)π / 16))，k = 1 ～ 4（与分析器真次声测量同一组 Q）。
    constexpr std::array<double, 4> butterworth8Q { { 0.50979557910415918, 0.60134488693504529, 0.89997622313641570, 2.5629154477415055 } };

    // 相关度 / 能量占比的静音门：低通后的均方低于它（约 −140 dBFS）就不给读数（没有这个频段的内容，比值没有意义）。
    constexpr double silentMeanSquare = 1.0e-14;

    // 读文件时每段的样本数（大文件分段读）。
    constexpr int readChunkSamples = 1 << 16;

    // 命令行与文件的合理范围。采样率超出 8 k ～ 768 kHz 多半是文件头坏了；块长上限只是防手误。
    constexpr double minSampleRate = 8000.0;
    constexpr double maxSampleRate = 768000.0;
    constexpr int maxBlockSize = 65536;
    constexpr double minMatchLufs = -60.0;
    constexpr double maxMatchLufs = 0.0;

    // 风格：下标 = glaze::Style（= 参数 glaze_style 的选项顺序）。命令行与文件名用英文，表里用界面上的中文。
    constexpr std::array<const char*, 4> styleIds   { { "auto", "warm", "punchy", "clean" } };
    constexpr std::array<const char*, 4> styleNames { { "自动", "温润", "冲击", "通透" } };

    //==============================================================================
    // 设置网格

    // 修坯的对照：自动 = 抽屉默认（三项都「自动」）；全关 = 三项都「关」（ADR-009 三态）；
    // lowcut / mono / width = 只有这一项「自动」、另两项「关」（拆分诊断：是哪一项改变了成品）。
    // replayOff 不是网格选项：每组 GLAZE > 0 的「自动」之后自动加一行「同决策重放 · 修坯关」（ADR-016，见 replayPass）。
    enum class ClayChoice { automatic, off, lowCutOnly, monoOnly, widthOnly, replayOff };

    struct Options
    {
        juce::File inputDir, outputDir;
        std::vector<float> glaze { 0.0f, 35.0f, 70.0f };
        std::vector<ClayChoice> clay { ClayChoice::automatic, ClayChoice::off };
        std::vector<int> styles { static_cast<int> (celadon::glaze::Style::automatic) };
        int blockSize = 512;
        double matchLufs = -16.0;
        bool metricsOnly = false;   // --metrics-only：照常渲染、量指标，但不写成品 wav（大批歌做门槛验收时用，省几 GB 磁盘与传输）
        bool replay = true;         // 每组 GLAZE > 0 的「自动」之后加「同决策重放 · 修坯关」一行与阳性对照（ADR-016）；--no-replay 关掉
        bool trace = false;         // --trace：每组设置另写 trace/<成品名>.csv（第二遍逐块：限幅 GR、修坯方案 / 保护 / 实时相关度、校准状态）
                                    // 与 trace/<成品名>__listen.csv（第一遍逐块的校准状态），诊断用
    };

    struct Combo
    {
        float glaze = 0.0f;                       // GLAZE 0 ～ 100 %
        ClayChoice clay = ClayChoice::automatic;
        int style = 0;                            // glaze::Style 下标
    };

    //==============================================================================
    // 文字

    // 固定小数位；「-0.00」这类负零写成「0.00」。
    std::string fixed (double value, int decimals)
    {
        char text[64] = {};

        if (std::snprintf (text, sizeof (text), "%.*f", decimals, value) <= 0)
            return "?";

        std::string result (text);

        if (result.size() > 1 && result[0] == '-' && result.find_first_not_of ("-0.") == std::string::npos)
            result.erase (0, 1);

        return result;
    }

    // 带符号（+0.80 / -1.20），零不带符号。
    std::string signedFixed (double value, int decimals)
    {
        auto result = fixed (value, decimals);

        if (result.find_first_not_of ("0.") != std::string::npos && result[0] != '-')
            result.insert (0, "+");

        return result;
    }

    // GLAZE 值：整数写整数（35），否则最多两位小数、去掉尾零（12.5）。
    std::string formatGlaze (float percent)
    {
        auto text = fixed (static_cast<double> (percent), 2);

        while (text.find ('.') != std::string::npos && (text.back() == '0' || text.back() == '.'))
        {
            const auto last = text.back();
            text.pop_back();

            if (last == '.')
                break;
        }

        return text;
    }

    // 时长 m:ss.s。
    std::string formatDuration (double seconds)
    {
        const auto tenths = std::llround (std::max (0.0, seconds) * 10.0);
        char text[64] = {};
        const auto written = std::snprintf (text, sizeof (text), "%lld:%02lld.%lld", tenths / 600, (tenths % 600) / 10, tenths % 10);
        return written > 0 ? std::string (text) : std::string ("?");
    }

    std::string trimmed (const std::string& text)
    {
        const auto first = text.find_first_not_of (" \t");

        if (first == std::string::npos)
            return {};

        const auto last = text.find_last_not_of (" \t");
        return text.substr (first, last - first + 1);
    }

    // 「0,35,70」→ {"0", "35", "70"}（各项去掉首尾空白；空项保留，由调用方报错）。
    std::vector<std::string> splitList (const std::string& text)
    {
        std::vector<std::string> items;
        std::string::size_type start = 0;

        while (true)
        {
            const auto comma = text.find (',', start);
            items.push_back (trimmed (text.substr (start, comma == std::string::npos ? std::string::npos : comma - start)));

            if (comma == std::string::npos)
                return items;

            start = comma + 1;
        }
    }

    // CSV 字段：含逗号、引号或换行时加引号（RFC 4180）。
    std::string csvField (const std::string& text)
    {
        if (text.find_first_of (",\"\n\r") == std::string::npos)
            return text;

        std::string quoted = "\"";

        for (const auto c : text)
        {
            if (c == '"')
                quoted += "\"\"";
            else
                quoted.push_back (c);
        }

        quoted += "\"";
        return quoted;
    }

    // Markdown 表格里的文字：竖线转义、换行变空格（歌名来自文件名，什么字符都可能有）。
    std::string mdText (const std::string& text)
    {
        std::string escaped;

        for (const auto c : text)
        {
            if (c == '|')
                escaped += "\\|";
            else if (c == '\n' || c == '\r')
                escaped.push_back (' ');
            else
                escaped.push_back (c);
        }

        return escaped;
    }

    const char* clayId (ClayChoice choice) noexcept
    {
        switch (choice)
        {
            case ClayChoice::automatic:  return "auto";
            case ClayChoice::off:        return "off";
            case ClayChoice::lowCutOnly: return "lowcut";
            case ClayChoice::monoOnly:   return "mono";
            case ClayChoice::widthOnly:  return "width";
            case ClayChoice::replayOff:  return "replay-off";
        }

        return "auto";
    }

    // 阶段：CSV 用英文标识（= glaze::Phase 的枚举名），表里用界面状态行的中文说法。
    const char* phaseId (Phase phase) noexcept
    {
        switch (phase)
        {
            case Phase::unlearned:      return "unlearned";
            case Phase::listening:      return "listening";
            case Phase::planTransition: return "planTransition";
            case Phase::calibrating:    return "calibrating";
            case Phase::locked:         return "locked";
        }

        return "unknown";
    }

    const char* phaseName (Phase phase) noexcept
    {
        switch (phase)
        {
            case Phase::unlearned:      return "未学习";
            case Phase::listening:      return "聆听中";
            case Phase::planTransition: return "方案过渡";
            case Phase::calibrating:    return "校准中";
            case Phase::locked:         return "已锁定";
        }

        return "未知";
    }

    std::string channelText (int channels)
    {
        if (channels == 1)
            return "单声道（复制成双声道）";

        if (channels == 2)
            return "立体声";

        return std::to_string (channels) + " 声道（只取前两个）";
    }

    std::string gridText (const Options& options)
    {
        std::string text = "GLAZE ";

        for (std::size_t i = 0; i < options.glaze.size(); ++i)
            text += (i > 0 ? " / " : "") + formatGlaze (options.glaze[i]);

        text += " × 修坯 ";

        for (std::size_t i = 0; i < options.clay.size(); ++i)
            text += (i > 0 ? " / " : "") + std::string (clayId (options.clay[i]));

        text += " × 风格 ";

        for (std::size_t i = 0; i < options.styles.size(); ++i)
            text += (i > 0 ? " / " : "") + std::string (styleIds[static_cast<std::size_t> (options.styles[i])]);

        return text;
    }

    // 「g35 clay-auto auto」（separator 分隔三段）。
    std::string comboLabel (const Combo& combo, const char* separator)
    {
        return "g" + formatGlaze (combo.glaze) + separator + "clay-" + clayId (combo.clay) + separator
             + styleIds[static_cast<std::size_t> (combo.style)];
    }

    // 成品文件名：<歌名>__g<GLAZE>_clay-<auto|off>_<风格>.wav。
    std::string outputFileName (const std::string& song, const Combo& combo)
    {
        return song + "__g" + formatGlaze (combo.glaze) + "_clay-" + clayId (combo.clay) + "_"
             + styleIds[static_cast<std::size_t> (combo.style)] + ".wav";
    }

    double nowSeconds()
    {
        return juce::Time::getMillisecondCounterHiRes() * 0.001;
    }

    //==============================================================================
    // 命令行

    // 整个字符串都得是一个有限的数（strtod 按 C 区域设置解析，小数点永远是「.」）。
    bool parseNumber (const std::string& text, double& value)
    {
        if (text.empty())
            return false;

        errno = 0;
        char* end = nullptr;
        const auto parsed = std::strtod (text.c_str(), &end);

        if (end == text.c_str() || *end != '\0' || errno == ERANGE || ! std::isfinite (parsed))
            return false;

        value = parsed;
        return true;
    }

    // juce::File 只收绝对路径；相对路径按当前工作目录展开（getChildFile 遇到绝对路径原样返回）。
    juce::File resolvePath (const std::string& path)
    {
        return juce::File::getCurrentWorkingDirectory().getChildFile (juce::String::fromUTF8 (path.c_str()));
    }

    void printUsage()
    {
        std::cerr << "用法：celadon_render --in <歌曲目录> --out <输出目录> [选项]\n"
                     "  --glaze 0,35,70     GLAZE 取值（0 ~ 100，逗号分隔），默认 0,35,70\n"
                     "  --clay auto,off     修坯：auto = 抽屉默认（三项都「自动」），off = 三项都「关」；\n"
                     "                      lowcut / mono / width = 只留这一项「自动」（拆分诊断）；默认 auto,off\n"
                     "  --style auto        GLAZE 风格 auto / warm / punchy / clean（可逗号分隔多个），默认 auto\n"
                     "  --block 512         宿主块长（样本，1 ~ 65536），默认 512\n"
                     "  --match-lufs -16    成品按整体响度只乘增益对齐到这个值（-60 ~ 0 LUFS），默认 -16\n"
                     "  --metrics-only      照常渲染并量指标，但不写成品 wav（大批歌做门槛验收时用）\n"
                     "  --trace             另写 trace/<成品名>.csv：第二遍逐块的限幅 GR、修坯方案 / 保护 / 实时低频相关度、校准状态；\n"
                     "                      trace/<成品名>__listen.csv：第一遍逐块的校准状态（诊断用）\n"
                     "  --no-replay         不做「同决策重放」（默认每组 GLAZE > 0 的修坯「自动」之后，另起一条 GLAZE 0 的链逐块照搬\n"
                     "                      它实际交给模块的参数、修坯换成「关」，出一行 clay-replay-off，并做逐位阳性对照；ADR-016）\n"
                     "  --selftest          只跑自检（指标算法、文件名、延迟对齐、GLAZE 0 逐位直通的阳性对照），不读歌\n"
                     "读 --in 目录第一层的 wav / aiff / flac，每首歌 × 每组设置写一个 32-bit float wav，另写 metrics.md 与\n"
                     "metrics.csv。进度与错误写 stderr；返回 0 成功、1 有文件读写失败（或自检失败）、2 参数错误。\n"
                     "详见 tools/render/README.md。\n";
    }

    enum class ParseResult { run, help, selfTest, invalid };

    // 解析失败时把原因写进 error（中文，直接给用户看）。
    ParseResult parseArguments (int argc, char* argv[], Options& options, std::string& error)
    {
        bool haveInput = false, haveOutput = false;

        for (int i = 1; i < argc; ++i)
        {
            const std::string name (argv[i]);

            if (name == "-h" || name == "--help")
                return ParseResult::help;

            if (name == "--selftest")
                return ParseResult::selfTest;

            if (name == "--trace")
            {
                options.trace = true;
                continue;
            }

            if (name == "--metrics-only")   // 不带取值的运行选项（另外两个是 --trace、--no-replay）
            {
                options.metricsOnly = true;
                continue;
            }

            if (name == "--no-replay")
            {
                options.replay = false;
                continue;
            }

            // 其余选项都带一个值。值可以以「-」开头（--match-lufs -16），所以直接取下一个参数，不判断它像不像选项。
            if (i + 1 >= argc)
            {
                error = "选项 " + name + " 缺少取值";
                return ParseResult::invalid;
            }

            const std::string value (argv[++i]);

            if (name == "--in")
            {
                options.inputDir = resolvePath (value);
                haveInput = true;
            }
            else if (name == "--out")
            {
                options.outputDir = resolvePath (value);
                haveOutput = true;
            }
            else if (name == "--glaze")
            {
                options.glaze.clear();

                for (const auto& item : splitList (value))
                {
                    double parsed = 0.0;

                    if (! parseNumber (item, parsed) || parsed < 0.0 || parsed > 100.0)
                    {
                        error = "--glaze 的「" + item + "」不是 0 ~ 100 之间的数";
                        return ParseResult::invalid;
                    }

                    const auto percent = static_cast<float> (parsed);

                    if (std::find (options.glaze.begin(), options.glaze.end(), percent) == options.glaze.end())
                        options.glaze.push_back (percent);
                }
            }
            else if (name == "--clay")
            {
                options.clay.clear();

                for (const auto& item : splitList (value))
                {
                    auto choice = ClayChoice::automatic;

                    if (item == "off")
                        choice = ClayChoice::off;
                    else if (item == "lowcut")
                        choice = ClayChoice::lowCutOnly;
                    else if (item == "mono")
                        choice = ClayChoice::monoOnly;
                    else if (item == "width")
                        choice = ClayChoice::widthOnly;
                    else if (item != "auto")
                    {
                        error = "--clay 只认 auto / off / lowcut / mono / width，不认「" + item + "」";
                        return ParseResult::invalid;
                    }

                    if (std::find (options.clay.begin(), options.clay.end(), choice) == options.clay.end())
                        options.clay.push_back (choice);
                }
            }
            else if (name == "--style")
            {
                options.styles.clear();

                for (const auto& item : splitList (value))
                {
                    const auto found = std::find_if (styleIds.begin(), styleIds.end(),
                                                     [&item] (const char* id) { return item == id; });

                    if (found == styleIds.end())
                    {
                        error = "--style 只认 auto / warm / punchy / clean，不认「" + item + "」";
                        return ParseResult::invalid;
                    }

                    const auto style = static_cast<int> (found - styleIds.begin());

                    if (std::find (options.styles.begin(), options.styles.end(), style) == options.styles.end())
                        options.styles.push_back (style);
                }
            }
            else if (name == "--block")
            {
                double parsed = 0.0;

                if (! parseNumber (value, parsed) || parsed < 1.0 || parsed > static_cast<double> (maxBlockSize)
                    || std::floor (parsed) != parsed)
                {
                    error = "--block 要 1 ~ 65536 之间的整数";
                    return ParseResult::invalid;
                }

                options.blockSize = static_cast<int> (parsed);
            }
            else if (name == "--match-lufs")
            {
                double parsed = 0.0;

                if (! parseNumber (value, parsed) || parsed < minMatchLufs || parsed > maxMatchLufs)
                {
                    error = "--match-lufs 要 -60 ~ 0 之间的数";
                    return ParseResult::invalid;
                }

                options.matchLufs = parsed;
            }
            else
            {
                error = "不认识的选项 " + name;
                return ParseResult::invalid;
            }
        }

        if (! haveInput || ! haveOutput)
        {
            error = "必须给出 --in 与 --out";
            return ParseResult::invalid;
        }

        if (options.inputDir == options.outputDir)
        {
            error = "--out 不能与 --in 是同一个目录（成品会在下次运行时被当成歌曲读进来）";
            return ParseResult::invalid;
        }

        return ParseResult::run;
    }

    // 网格顺序：GLAZE 在外、修坯居中、风格在内——同一 GLAZE 的「自动 / 全关」两组相邻，表里好对比。
    std::vector<Combo> makeCombos (const Options& options)
    {
        std::vector<Combo> combos;

        for (const auto percent : options.glaze)
            for (const auto choice : options.clay)
                for (const auto style : options.styles)
                    combos.push_back ({ percent, choice, style });

        return combos;
    }

    //==============================================================================
    // 读歌

    struct SongFile
    {
        juce::File file;
        std::string name;   // 成品文件名里的歌名（文件名去扩展名；同名不同扩展名时加上扩展名区分）
    };

    bool isAudioFile (const juce::File& file)
    {
        const auto extension = file.getFileExtension().toLowerCase();

        for (const auto* known : { ".wav", ".wave", ".bwf", ".aif", ".aiff", ".aifc", ".flac" })
            if (extension == known)
                return true;

        return false;
    }

    // --in 目录第一层的音频文件（不进子目录、跳过隐藏文件，如 Mac 的 ._xxx.wav），按路径排序：每次运行顺序相同。
    std::vector<SongFile> findSongs (const juce::File& directory)
    {
        auto files = directory.findChildFiles (juce::File::findFiles | juce::File::ignoreHiddenFiles, false);
        files.sort();

        std::vector<SongFile> songs;

        for (const auto& file : files)
            if (isAudioFile (file))
                songs.push_back ({ file, file.getFileNameWithoutExtension().toStdString() });

        // 同一个歌名有两个文件（song.wav 与 song.flac）：歌名后加扩展名，免得成品互相覆盖。
        // 不分大小写比：结果要 rsync 回 Mac（默认不分大小写的文件系统）。
        for (auto& song : songs)
        {
            const auto clashes = std::count_if (songs.begin(), songs.end(), [&song] (const SongFile& other)
            {
                return other.file.getFileNameWithoutExtension().equalsIgnoreCase (song.file.getFileNameWithoutExtension());
            });

            if (clashes > 1)
                song.name += "-" + song.file.getFileExtension().substring (1).toLowerCase().toStdString();
        }

        return songs;
    }

    struct Song
    {
        double sampleRate = 0.0;
        int sourceChannels = 0;
        int nonFiniteSamples = 0;
        juce::AudioBuffer<float> audio;   // 立体声（单声道复制成双声道，多声道取前两个）
    };

    bool loadSong (juce::AudioFormatManager& formats, const juce::File& file, Song& song, std::string& error)
    {
        // 按扩展名交给对应格式（.wav 只给 WAV 读取器试）。.wave / .aifc 这两个 JUCE 不按扩展名认，才改按内容逐个格式试
        // （WAV、AIFF 在注册表最前面）。不对所有文件都按内容试：注册的格式里有 MP3（JUCE_USE_MP3AUDIOFORMAT 默认开），
        // 坏掉的 wav 不该被别的格式硬读成一段杂音。
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

        if (reader == nullptr && file.hasFileExtension (".wave;.aifc"))
            reader.reset (formats.createReaderFor (file.createInputStream()));

        if (reader == nullptr)
        {
            error = "读不了（格式不支持或文件损坏）";
            return false;
        }

        const auto length = reader->lengthInSamples;
        song.sampleRate = reader->sampleRate;
        song.sourceChannels = static_cast<int> (reader->numChannels);

        if (! (song.sampleRate >= minSampleRate && song.sampleRate <= maxSampleRate))
        {
            error = "采样率 " + fixed (song.sampleRate, 0) + " Hz 不在 8 k ~ 768 kHz 之内";
            return false;
        }

        if (song.sourceChannels < 1 || length <= 0)
        {
            error = "文件里没有音频";
            return false;
        }

        // 第二遍的整段缓冲 = 原曲 + 链延迟 + 余振，块循环还会越过末尾最多一个块，都要放得进 int（juce::AudioBuffer 的长度类型）：
        // 留 1 s + 两个最大块的余量（链延迟 + 余振在任何采样率下都远小于 1 s）。
        const auto margin = static_cast<juce::int64> (std::ceil (song.sampleRate)) + 2 * static_cast<juce::int64> (maxBlockSize);

        if (length > static_cast<juce::int64> (std::numeric_limits<int>::max()) - margin)
        {
            error = "文件太长（超过 juce::AudioBuffer 能放的长度）";
            return false;
        }

        const auto numSamples = static_cast<int> (length);
        song.audio.setSize (2, numSamples);

        // 分段读进立体声缓冲：单声道由 JUCE 复制到右声道（AudioFormatReader::read 的「立体声目标 + 单声道源」），
        // 多于两个声道只取前两个。
        for (int start = 0; start < numSamples; start += readChunkSamples)
        {
            const auto count = std::min (readChunkSamples, numSamples - start);

            if (! reader->read (&song.audio, start, count, static_cast<juce::int64> (start), true, true))
            {
                error = "读到一半出错";
                return false;
            }
        }

        // 浮点 wav 里偶见的 NaN / Inf 按 0 处理（插件在处理器层把含非有限值的输入块整块清零；这里逐样本清，别的样本照原样）。
        song.nonFiniteSamples = 0;

        for (int ch = 0; ch < 2; ++ch)
        {
            auto* data = song.audio.getWritePointer (ch);

            for (int i = 0; i < numSamples; ++i)
            {
                if (! std::isfinite (data[i]))
                {
                    data[i] = 0.0f;
                    ++song.nonFiniteSamples;
                }
            }
        }

        return true;
    }

    //==============================================================================
    // 指标

    struct Measurement
    {
        bool hasLoudness = false;
        double integratedLufs = 0.0;       // 整体响度（LUFS）
        bool hasTruePeak = false;
        double truePeakDbtp = 0.0;         // 真峰值最大值（dBTP）
        bool hasLowCorrelation = false;
        double lowCorrelation = 0.0;       // < 125 Hz 左右相关度（−1 ～ 1）
        bool hasSubShare = false;
        double subShareDb = 0.0;           // 真次声（< 16 Hz，8 阶）能量占比（dB）
        double musicSeconds = 0.0;         // 有效音乐时长：K 加权 100 ms 子块在 −70 LUFS 绝对门以上的总时长（与校准器同一门限、同一子块，
                                           // 静音不算；门槛据此区分「时长不够锁定」，ADR-016）
        std::uint64_t hash = 0;            // 逐位核对用（FNV-1a）
    };

    struct Biquad
    {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
        double s1 = 0.0, s2 = 0.0;

        // 转置直接 II 型，double 精度（与 LoudnessMeter::kWeight 同一写法）。
        double process (double x) noexcept
        {
            const auto y = b0 * x + s1;
            s1 = b1 * x - a1 * y + s2;
            s2 = b2 * x - a2 * y;
            return y;
        }
    };

    // RBJ 二阶低通。1 − cos w0 写成 2·sin²(w0 / 2)：30 Hz / 192 kHz 时 w0 ≈ 1e-3，直接相减要丢掉约 6 位有效数字。
    Biquad makeLowpass (double cutoffHz, double q, double sampleRate) noexcept
    {
        const auto w0 = juce::MathConstants<double>::twoPi * cutoffHz / sampleRate;
        const auto sinHalf = std::sin (0.5 * w0);
        const auto oneMinusCos = 2.0 * sinHalf * sinHalf;
        const auto alpha = std::sin (w0) / (2.0 * q);
        const auto a0 = 1.0 + alpha;

        Biquad filter;
        filter.b0 = 0.5 * oneMinusCos / a0;
        filter.b1 = oneMinusCos / a0;
        filter.b2 = filter.b0;
        filter.a1 = -2.0 * std::cos (w0) / a0;
        filter.a2 = (1.0 - alpha) / a0;
        return filter;
    }

    // 4 阶 Butterworth 低通（两级二阶串联）。
    struct Lowpass4
    {
        Lowpass4 (double cutoffHz, double sampleRate) noexcept
            : first (makeLowpass (cutoffHz, butterworthQ1, sampleRate)),
              second (makeLowpass (cutoffHz, butterworthQ2, sampleRate))
        {
        }

        double process (double x) noexcept   { return second.process (first.process (x)); }

        Biquad first, second;
    };

    struct Lowpass8
    {
        Lowpass8 (double cutoffHz, double sampleRate) noexcept
            : stages { { makeLowpass (cutoffHz, butterworth8Q[0], sampleRate), makeLowpass (cutoffHz, butterworth8Q[1], sampleRate),
                         makeLowpass (cutoffHz, butterworth8Q[2], sampleRate), makeLowpass (cutoffHz, butterworth8Q[3], sampleRate) } }
        {
        }

        double process (double x) noexcept
        {
            for (auto& stage : stages)
                x = stage.process (x);

            return x;
        }

        std::array<Biquad, 4> stages;
    };

    // FNV-1a（64 位）逐样本按位累加（左声道整段，再右声道）。两段音频逐位相同则哈希相同；哈希不同则一定不逐位相同。
    std::uint64_t hashAudio (const juce::AudioBuffer<float>& audio) noexcept
    {
        constexpr std::uint64_t offsetBasis = 14695981039346656037ull;
        constexpr std::uint64_t prime = 1099511628211ull;
        auto hash = offsetBasis;

        for (int ch = 0; ch < audio.getNumChannels(); ++ch)
        {
            const auto* data = audio.getReadPointer (ch);

            for (int i = 0; i < audio.getNumSamples(); ++i)
            {
                std::uint32_t bits = 0;
                std::memcpy (&bits, data + i, sizeof (bits));

                for (int shift = 0; shift < 32; shift += 8)
                {
                    hash ^= static_cast<std::uint64_t> ((bits >> shift) & 0xffu);
                    hash *= prime;
                }
            }
        }

        return hash;
    }

    // 量一段立体声音频（原始输入或对齐前的成品）。
    Measurement measure (const juce::AudioBuffer<float>& audio, double sampleRate)
    {
        Measurement m;
        const auto numSamples = audio.getNumSamples();

        // 整体响度：插件自己的表（LoudnessMeter：BS.1770-4，−70 LUFS 绝对门 + −10 LU 相对门，与界面 OUT 表同一算法）。
        // process 不限块长，整段一次送完。
        {
            celadon::LoudnessMeter meter;
            meter.prepare ({ sampleRate, static_cast<juce::uint32> (juce::jmax (1, numSamples)), 2u });
            meter.process (audio);
            const auto lufs = meter.getIntegratedLufs();

            if (lufs > celadon::LoudnessMeter::noReading)
            {
                m.hasLoudness = true;
                m.integratedLufs = static_cast<double> (lufs);
            }
        }

        // 有效音乐时长：另一块表按 10 s 一段送（子块环形缓冲只存 12.8 s，整段一次送完会丢掉前面的子块），每段读出新完成的子块，
        // 数 K 加权均方在 −70 LUFS 绝对门以上的（LUFS = −0.691 + 10·log10(均方)，与 GlazeCalibrator 的块选择同口径）。
        {
            celadon::LoudnessMeter meter;
            const auto chunk = static_cast<int> (std::llround (10.0 * sampleRate));
            meter.prepare ({ sampleRate, static_cast<juce::uint32> (juce::jmax (1, chunk)), 2u });
            const auto gate = std::pow (10.0, (-70.0 + 0.691) / 10.0);
            const auto subBlockSeconds = static_cast<double> (meter.getSubBlockLength()) / sampleRate;
            std::array<celadon::LoudnessMeter::SubBlock, static_cast<std::size_t> (celadon::LoudnessMeter::subBlockRingCapacity)> blocks {};
            std::int64_t cursor = 0;
            std::int64_t counted = 0;

            for (int start = 0; start < numSamples; start += chunk)
            {
                const auto length = std::min (chunk, numSamples - start);
                const juce::AudioBuffer<float> part (const_cast<float* const*> (audio.getArrayOfReadPointers()), 2, start, length);
                meter.process (part);
                const auto read = std::abs (meter.readSubBlocksSince (cursor, blocks.data(), static_cast<int> (blocks.size())));

                for (int k = 0; k < read; ++k)
                    if (std::isfinite (blocks[static_cast<std::size_t> (k)].meanSquare) && blocks[static_cast<std::size_t> (k)].meanSquare >= gate)
                        ++counted;
            }

            m.musicSeconds = static_cast<double> (counted) * subBlockSeconds;
        }

        // 真峰值：插件的 8× 检测器（与限幅器、表同一算法）。检测器有 latencySamples 个样本的延迟，每个声道末尾补一段零
        // 把最后几个样本的插值点冲出来（同 tests/TestHelpers.h 的 truePeakOf）。
        {
            celadon::TruePeakDetector detector;
            detector.prepare (2);
            const std::array<float, static_cast<std::size_t> (celadon::TruePeakDetector::tapsPerPhase)> zeros {};
            auto peak = 0.0f;

            for (int ch = 0; ch < 2; ++ch)
            {
                peak = std::max (peak, detector.processBlockMax (ch, audio.getReadPointer (ch), numSamples));
                peak = std::max (peak, detector.processBlockMax (ch, zeros.data(), celadon::TruePeakDetector::tapsPerPhase));
            }

            if (peak > 0.0f)
            {
                m.hasTruePeak = true;
                m.truePeakDbtp = 20.0 * std::log10 (static_cast<double> (peak));
            }
        }

        // 低频相关度（4 阶 125 Hz）与真次声占比（8 阶 16 Hz）：double 精度的 Butterworth 低通，整段累计。
        //   ρ = 2ΣLR / (ΣL² + ΣR²) = (P_M − P_S) / (P_M + P_S)：与分析器 lowCorrelation（Σ X / Σ bandPower）同一口径，
        //   左右电平不等时与皮尔逊相关系数不同（它看的是侧声道能量占多少，正是收单要减的东西）；
        //   真次声 = M = (L + R) / 2 的 8 阶 16 Hz 低通能量 ÷ M 全频能量：与分析器 subShareDb 同一定义（ADR-012；这里整段计、
        //   不设有效音频门）。原来的 30 Hz 4 阶口径测到的主要是底鼓基音漏入（findings.md F-R1）。
        {
            Lowpass4 lowLeft (lowCorrelationCutoffHz, sampleRate), lowRight (lowCorrelationCutoffHz, sampleRate);
            Lowpass8 subMid (infrasonicCutoffHz, sampleRate);
            const auto* left = audio.getReadPointer (0);
            const auto* right = audio.getReadPointer (1);
            double cross = 0.0, powerLeft = 0.0, powerRight = 0.0, subEnergy = 0.0, midEnergy = 0.0;

            for (int i = 0; i < numSamples; ++i)
            {
                const auto l = static_cast<double> (left[i]);
                const auto r = static_cast<double> (right[i]);

                const auto lowL = lowLeft.process (l);
                const auto lowR = lowRight.process (r);
                cross += lowL * lowR;
                powerLeft += lowL * lowL;
                powerRight += lowR * lowR;

                const auto mid = 0.5 * (l + r);
                const auto sub = subMid.process (mid);
                subEnergy += sub * sub;
                midEnergy += mid * mid;
            }

            const auto count = static_cast<double> (juce::jmax (1, numSamples));

            if ((powerLeft + powerRight) / count > silentMeanSquare)
            {
                m.hasLowCorrelation = true;
                m.lowCorrelation = 2.0 * cross / (powerLeft + powerRight);
            }

            if (midEnergy / count > silentMeanSquare)
            {
                m.hasSubShare = true;
                m.subShareDb = 10.0 * std::log10 (std::max (subEnergy / midEnergy, 1.0e-15));   // 下限 −150 dB
            }
        }

        m.hash = hashAudio (audio);
        return m;
    }

    // stderr 进度里的一行摘要。
    std::string loudnessText (const Measurement& m)
    {
        std::string text = m.hasLoudness ? fixed (m.integratedLufs, 2) + " LUFS" : std::string ("整体响度无读数");

        if (m.hasTruePeak)
            text += "，真峰值 " + fixed (m.truePeakDbtp, 2) + " dBTP";

        if (m.hasLoudness && m.hasTruePeak)
            text += "，PLR " + fixed (m.truePeakDbtp - m.integratedLufs, 2) + " dB";

        if (m.hasLowCorrelation)
            text += "，低频相关度 " + fixed (m.lowCorrelation, 3);

        if (m.hasSubShare)
            text += "，次声 " + fixed (m.subShareDb, 1) + " dB";

        return text;
    }

    //==============================================================================
    // 渲染

    celadon::ChainParams makeFactoryParams()
    {
        // 各参数的新实例默认值：Source/ParameterDefaults.h 的 factoryChainParams()——插件参数表（Parameters.cpp）读的同一处真源，
        // 处理器 Debug 版构造时逐项核对两边一致（T-032 R10-01）。含 0.1.9.0 高级母带的新实例默认：推响不刺「自动」50 %、
        // 瞬态保留 50 %、目标频谱「自动」35 %（链里按 GLAZE 联动：GLAZE 0 时两项实际强度为 0），GLAZE 35、风格自动。
        // claySettings 是三态设置；p.clay（交给 ClayModule 的实际值）作为输入时编排器忽略（ChainParams.h 文件头），保持结构默认值。
        return celadon::param::defaults::factoryChainParams();
    }

    // 高级母带设置的一行说明（metrics.md 头部；T-032 R10-01：成品是哪条链，得在表里看得到）。
    std::string masteringText (const celadon::MasteringSettings& m)
    {
        const auto harsh = m.harshMode == celadon::HarshMode::off ? std::string ("关")
                         : (m.harshMode == celadon::HarshMode::automatic ? "自动 " : "手动 ") + fixed (m.harshAmount, 0) + " %";
        static constexpr std::array<const char*, 6> targetNames { { "关", "自动", "温润", "中性", "明亮", "参考" } };
        const auto targetIndex = static_cast<int> (m.targetMode);
        const auto target = targetIndex >= 0 && targetIndex < static_cast<int> (targetNames.size())
                              ? std::string (targetNames[static_cast<std::size_t> (targetIndex)]) : std::string ("?");
        const auto dither = m.dither == celadon::DitherMode::bits16 ? "16 bit" : (m.dither == celadon::DitherMode::bits24 ? "24 bit" : "关");
        return "推响不刺 " + harsh + "、瞬态保留 " + fixed (m.transientPreserve, 0) + " %、目标频谱 " + target
             + (m.targetMode == celadon::TargetMode::off ? std::string() : " " + fixed (m.targetAmount, 0) + " %")
             + "、编码余量 " + fixed (m.codecMarginDb, 1) + " dB、导出抖动 " + dither;
    }

    celadon::ChainParams makeComboParams (const Combo& combo)
    {
        auto p = makeFactoryParams();
        p.glazePercent = combo.glaze;
        p.glazeStyle = combo.style;

        // 「全关」= 抽屉里三项都选「关」（ADR-009：永远 0，GLAZE 多大都不处理）；单项对照 = 只留这一项「自动」。
        // 手动值与 Bypass 保持默认。
        const auto keep = [&combo] (ClayChoice only) noexcept
        {
            return combo.clay == ClayChoice::automatic || combo.clay == only;
        };

        if (! keep (ClayChoice::lowCutOnly)) p.claySettings.lowCutMode = celadon::ClayMode::off;
        if (! keep (ClayChoice::monoOnly))   p.claySettings.monoMode   = celadon::ClayMode::off;
        if (! keep (ClayChoice::widthOnly))  p.claySettings.widthMode  = celadon::ClayMode::off;

        return p;
    }

    juce::dsp::ProcessSpec makeSpec (double sampleRate, int blockSize) noexcept
    {
        return { sampleRate, static_cast<juce::uint32> (blockSize), 2u };
    }

    celadon::glaze::HostInfo makeHostInfo (std::int64_t position) noexcept
    {
        // 离线导出的宿主口径（设计规格-Clay-分析决策与验收 第 7 节）：离线、在播、位置从 0 起随块连续推进。
        // 拿到位置时处理器也给循环信息（PluginProcessor::readHostInfo：hasLoop = true），导出不循环。
        celadon::glaze::HostInfo info;
        info.offline = true;
        info.hasPlayState = true;
        info.isPlaying = true;
        info.hasPosition = true;
        info.positionSamples = position;
        info.hasLoop = true;
        info.looping = false;
        return info;
    }

    // --trace 的校准诊断列（两遍的追踪共用；T-015 / BUG-003「整首停在校准中」由它们查出，列的含义见 README「逐块追踪」）。
    const char* calibrationTraceHeader() noexcept
    {
        return "glaze_phase,cal_round_active,cal_state,cal_interrupt_mask,cal_gate_mask,cal_rounds_done,cal_subwindows,"
               "cal_subwindow_s,cal_guard_s,cal_last_error_db,cal_suggested_db,cal_target_db,cal_applicable,applied_bias_db,"
               "executed_bias_db,guard_db,plan_transition,plan_transitions,cal_begins,cal_finishes,feedback_steps,feedback_max_db";
    }

    std::string calibrationTraceColumns (const celadon::glaze::GlazeController& controller)
    {
        const auto p = controller.getProbeForTesting();
        return std::to_string (static_cast<int> (controller.getPhaseForTesting())) + "," + (p.roundActive ? "1" : "0") + ","
             + std::to_string (p.calibratorState) + "," + std::to_string (p.calibrationInterruptMask) + ","
             + std::to_string (p.calibrationGateMask) + "," + std::to_string (p.calibrationRoundsDone) + ","
             + std::to_string (p.calibrationSubWindows) + "," + fixed (p.calibrationSubWindowSeconds, 2) + ","
             + fixed (p.calibrationGuardSeconds, 2) + "," + fixed (p.calibrationLastErrorDb, 3) + ","
             + fixed (p.calibrationSuggestedDb, 3) + "," + fixed (p.targetPlan.effectiveGainDb, 3) + ","
             + (p.calibrationApplicable ? "1" : "0") + "," + fixed (p.appliedBiasDb, 4) + "," + fixed (p.executedBiasDb, 4) + ","
             + fixed (p.guardDb, 3) + "," + (p.transitionActive ? "1" : "0") + "," + std::to_string (p.planTransitions) + ","
             + std::to_string (p.calibrationBegins) + "," + std::to_string (p.calibrationFinishes) + ","
             + std::to_string (p.feedbackSteps) + "," + fixed (juce::jmin (p.feedbackMaxGainDb, 99.0f), 3);   // 不限时是很大的数：写 99
    }

    struct ListenResult
    {
        std::unique_ptr<juce::XmlElement> state;   // createGlazeStateXml：宿主存工程时 GLAZE 那一节
        Status status;                             // 第一遍结束时的状态
        int nonFiniteBlocks = 0;
        std::string trace;                         // --trace：第一遍逐块的校准列（不追踪时为空）
    };

    // 第一遍：整首歌送进一条新链，输出丢弃（GLAZE 学习、方案过渡、校准都在这一遍走完），最后把 GLAZE 状态存下来。
    ListenResult listenPass (const juce::AudioBuffer<float>& input, double sampleRate, int blockSize,
                             const celadon::ChainParams& params, bool trace = false)
    {
        ListenResult result;
        auto chain = std::make_unique<celadon::CeladonChain>();

        // 顺序照 PluginProcessor::prepareToPlay：先交参数再 prepare，各模块的平滑器直接落在这份参数上、不从默认值滑过来。
        chain->setParams (params, false);
        chain->prepare (makeSpec (sampleRate, blockSize));

        const auto numSamples = input.getNumSamples();
        juce::AudioBuffer<float> block (2, blockSize);

        for (int position = 0; position < numSamples; position += blockSize)
        {
            const auto length = std::min (blockSize, numSamples - position);
            block.setSize (2, length, false, false, true);

            for (int ch = 0; ch < 2; ++ch)
                block.copyFrom (ch, 0, input, ch, position, length);

            // 每块顺序照 PluginProcessor::processChain：宿主时间线 → 参数 → 处理。
            chain->setHostInfo (makeHostInfo (position));
            chain->setParams (params, false);

            if (chain->process (block))
                ++result.nonFiniteBlocks;

            if (trace)
            {
                // 一行 = 一块；t 按输入时间轴（块起点）。聆听、方案过渡、校准、约束反馈都在这一遍走完，校准卡住先看这份。
                if (result.trace.empty())
                    result.trace = std::string ("t_sec,limit_gain_db,") + calibrationTraceHeader() + "\n";

                result.trace += fixed (static_cast<double> (position) / sampleRate, 4) + ","
                              + fixed (chain->getEffectiveParams().limit.gainDb, 3) + ","
                              + calibrationTraceColumns (chain->getGlazeController()) + "\n";
            }
        }

        result.status = chain->getGlazeStatus();
        result.state = chain->createGlazeStateXml();
        return result;
    }

    struct RenderResult
    {
        juce::AudioBuffer<float> io;       // 第二遍整段：[0, 链延迟) 是延迟段，其后是成品
        int latencySamples = 0;
        int outputLength = 0;              // 成品长度 = 原曲 + 余振
        double grMeanDb = 0.0;             // 限幅 GR：各块读数按块长加权平均
        double grMaxDb = 0.0;
        double phaseGuardPercent = 0.0;    // 反相保护介入的时长占比（%）
        double widthGuardPercent = 0.0;    // 宽度保护介入的时长占比（%）
        double peakGuardPercent = 0.0;     // 修坯峰值守护 g < 1 的样本占比（%，ADR-014）
        double peakGuardMinGain = 1.0;     // 全曲最小的 g
        double blendIntendedEnergy = 0.0;  // 守护混合损失测量（T-043，ADR-027）：全曲「本该输出」与实际输出的 K 加权能量、量过的样本数
        double blendOutputEnergy = 0.0;
        std::int64_t blendSamples = 0;
        double slowInvertedPercent = 0.0;  // 分析器慢路低频相关度有效的块里 ρ < −0.2 的占比（%）：歌里有没有真反相段（门槛用）
        double limitInputPeakDb = -1000.0; // 限幅器入口峰值相对 Ceiling 的最大值（dB）= max(GR − 本块实际 Limit Gain)，只计 GR > 0 的块：
                                           // 扣掉两条链各自校准出的增益差，只剩入口峰值（与 ClayGlazeTests 的 readLimiter 同口径）
        int nonFiniteBlocks = 0;
        bool latencyChanged = false;
        Status status;                     // 成品末尾的状态
        std::string trace;                 // --trace：逐块 CSV（不追踪时为空）
        bool hasProfile = false;           // 成品末尾已提交的画像（先试后用的代价等；诊断列）
        celadon::glaze::MusicProfile profile;
        float clayPressureScale = 1.0f;    // 成品末尾修坯压力反馈的缩放与降档次数（ADR-014）
        int clayPressureSteps = 0;
        double clayPeakCostDb = 0.0;       // 成品末尾压力反馈第一触发量的代价（与先试同口径，ADR-028）与攒下的有效块数（每块 0.1 s）
        int clayPeakCostBlocks = 0;
    };

    // 成品 / 重放两种渲染逐块共用的统计：限幅 GR、保护介入、峰值守护、限幅入口峰值、慢路反相占比。
    struct PassStats
    {
        double grWeighted = 0.0, grMax = 0.0;
        std::int64_t phaseSamples = 0, widthSamples = 0, peakGuardSamples = 0, slowValidBlocks = 0, slowInvertedBlocks = 0;

        // 这一块读到的量（--trace 也要用）。
        double reduction = 0.0;
        Status status;
        celadon::ClayModule::PeakGuardStats peakGuard;

        void addBlock (celadon::CeladonChain& chain, int length, RenderResult& result)
        {
            // 限幅 GR 按块读（界面 GR 表同一接口：自上次读取以来的最大压缩量，读完清零）。
            reduction = static_cast<double> (chain.getAndResetLimitGainReductionDb());
            grWeighted += reduction * static_cast<double> (length);
            grMax = std::max (grMax, reduction);

            status = chain.getGlazeStatus();

            if (status.clayPhaseGuardActive)
                phaseSamples += length;

            if (status.clayWidthGuardActive)
                widthSamples += length;

            peakGuard = chain.getClayForTesting().getAndResetPeakGuardStats();
            peakGuardSamples += peakGuard.guardedSamples;
            result.peakGuardMinGain = std::min (result.peakGuardMinGain, static_cast<double> (peakGuard.minGain));
            result.blendIntendedEnergy += peakGuard.blendIntendedEnergy;
            result.blendOutputEnergy += peakGuard.blendOutputEnergy;
            result.blendSamples += peakGuard.blendSamples;

            if (reduction > 0.0)
                result.limitInputPeakDb = std::max (result.limitInputPeakDb,
                                                    reduction - static_cast<double> (chain.getEffectiveParams().limit.gainDb));

            const auto probe = chain.getGlazeController().getProbeForTesting();

            if (probe.liveLowCorrelation.valid)
            {
                ++slowValidBlocks;

                if (probe.liveLowCorrelation.value < -0.2f)   // = 出厂 clayGuardPhaseCorrelation
                    ++slowInvertedBlocks;
            }
        }

        void finish (int total, RenderResult& result) const
        {
            const auto renderedSamples = static_cast<double> (juce::jmax (1, total));
            result.grMeanDb = grWeighted / renderedSamples;
            result.grMaxDb = grMax;
            result.phaseGuardPercent = 100.0 * static_cast<double> (phaseSamples) / renderedSamples;
            result.widthGuardPercent = 100.0 * static_cast<double> (widthSamples) / renderedSamples;
            result.peakGuardPercent = 100.0 * static_cast<double> (peakGuardSamples) / renderedSamples;
            result.slowInvertedPercent = slowValidBlocks > 0
                                           ? 100.0 * static_cast<double> (slowInvertedBlocks) / static_cast<double> (slowValidBlocks) : 0.0;
        }
    };

    // 整段缓冲：输入 + 链延迟与余振的补零（成品 / 重放共用）。
    int prepareIo (const juce::AudioBuffer<float>& input, double sampleRate, int latency, RenderResult& result)
    {
        const auto numInput = input.getNumSamples();
        const auto ringOut = static_cast<int> (std::ceil (ringOutSeconds * sampleRate));
        const auto total = numInput + latency + ringOut;

        result.latencySamples = latency;
        result.outputLength = numInput + ringOut;
        result.io.setSize (2, total);
        result.io.clear();

        for (int ch = 0; ch < 2; ++ch)
            result.io.copyFrom (ch, 0, input, ch, 0, numInput);

        return total;
    }

    // 同决策重放的输入（ADR-016）：自动链第二遍实际交给模块的参数，按交的顺序——prepare 之前那次 setParams（读回存档后的第一次：
    // 执行快照里修坯的首块值在这里就被用掉，模块 prepare 时落在它上面）与之后逐块的。重放链照同一顺序喂：prepare 前用 prepared，
    // 之后逐块用 blocks。T-044 之前只记逐块、prepare 前拿第一块顶：第一遍末尾正在降档的歌（存档的压力缩放是目标档，快照里的修坯值
    // 还是降档前的）自动链开头有一段修坯 20 ms 的参数斜坡、重放链没有，阳性对照最大差 −40 ～ −80 dBFS、判 mismatch（改前 Jamendo
    // tech g70 一行，T-044 后降档变多的 emotapes / somggggg / tech 六行）。
    struct EffectiveLog
    {
        celadon::ChainParams prepared = makeFactoryParams();
        std::vector<celadon::ChainParams> blocks;
    };

    // 第二遍：新链读回第一遍的存档，从头渲染整首歌 + 延迟与余振的补零，作为成品。
    // effectiveLog ≠ nullptr 时记下链实际交给模块的参数（prepare 前一份 + 逐块，getEffectiveParams），给「同决策重放」用（ADR-016）。
    void renderPass (const juce::AudioBuffer<float>& input, double sampleRate, int blockSize,
                     const celadon::ChainParams& params, const juce::XmlElement* state, bool trace, RenderResult& result,
                     EffectiveLog* effectiveLog = nullptr)
    {
        auto chain = std::make_unique<celadon::CeladonChain>();

        // 顺序照宿主「重开工程 → 导出」：先读回存档（setStateInformation），再 prepareToPlay（先交参数再 prepare）。
        // rendercheck 验过「读回在 prepare 之前 / 之后」两种顺序结果逐样本相同。
        chain->restoreGlazeStateXml (state);
        chain->setParams (params, false);

        if (effectiveLog != nullptr)
            effectiveLog->prepared = chain->getEffectiveParams();   // prepare 时模块落在它上面（prepare 不再调 setParams）

        chain->prepare (makeSpec (sampleRate, blockSize));

        // 链延迟只由 Lookahead（固定 2 ms）、Color 的过采样滤波器与 Clay 的 D + G 决定，prepare 之后就是定值；渲染中仍逐块核对。
        const auto latency = chain->getLatencySamples();
        const auto total = prepareIo (input, sampleRate, latency, result);

        chain->getAndResetLimitGainReductionDb();                 // 清掉可能的残值（刚 prepare 完本应为 0）
        chain->getClayForTesting().getAndResetPeakGuardStats();   // 清掉 prepare 之后的残值

        if (effectiveLog != nullptr)
        {
            effectiveLog->blocks.clear();
            effectiveLog->blocks.reserve (static_cast<std::size_t> (total / juce::jmax (1, blockSize) + 1));
        }

        PassStats stats;

        // 原地处理整段缓冲（末尾 latency + ringOut 个样本是补的零），每块一个视图，不拷贝。
        for (int position = 0; position < total; position += blockSize)
        {
            const auto length = std::min (blockSize, total - position);
            juce::AudioBuffer<float> block (result.io.getArrayOfWritePointers(), 2, position, length);

            chain->setHostInfo (makeHostInfo (position));
            chain->setParams (params, false);

            if (effectiveLog != nullptr)
                effectiveLog->blocks.push_back (chain->getEffectiveParams());   // 这一块交给模块的就是它（setParams 里算好，process 用）

            if (chain->process (block))
                ++result.nonFiniteBlocks;

            if (chain->getLatencySamples() != latency)
                result.latencyChanged = true;

            stats.addBlock (*chain, length, result);

            if (trace)
            {
                const auto probe = chain->getGlazeController().getProbeForTesting();
                const auto& effective = chain->getEffectiveParams();
                const auto& blockStatus = stats.status;
                const auto& peakGuard = stats.peakGuard;

                // 一行 = 一块；t 按成品时间轴（块起点减链延迟）。相关度无效时留空。
                if (result.trace.empty())
                    result.trace = "t_sec,limit_gr_db,planned_lowcut_hz,planned_mono_amount,planned_mono_hz,planned_width_db,"
                                   "live_mono_amount,live_width_db,guard_phase_floor,guard_phase_hold_s,guard_phase_active,"
                                   "guard_seconds,rho_slow,rho_fast,clay_lowcut_hz,clay_mono_amount,clay_mono_hz,clay_width_db,"
                                   "peak_guard_samples,peak_guard_min_gain,blend_intended_energy,blend_output_energy,"
                                   "limit_gain_db,tone_db,glue,heat,clay_pressure_scale,clay_peak_cost_db,clay_peak_cost_blocks,"
                                   "clay_blend_loss_db,"
                                 + std::string (calibrationTraceHeader()) + "\n";

                const auto feature = [] (const celadon::glaze::Feature& f)
                {
                    return f.valid ? fixed (f.value, 4) : std::string();
                };

                // 守护混合损失的两份 K 加权能量（T-043）：量级跨很多个数量级，用科学计数法。
                const auto energy = [] (double value)
                {
                    char text[32];
                    std::snprintf (text, sizeof (text), "%.9e", value);
                    return std::string (text);
                };

                result.trace += fixed (static_cast<double> (position - latency) / sampleRate, 4) + ","
                              + fixed (stats.reduction, 3) + ","
                              + fixed (probe.clayPlanned.lowCutHz, 2) + "," + fixed (probe.clayPlanned.monoAmount, 4) + ","
                              + fixed (probe.clayPlanned.monoHz, 1) + "," + fixed (probe.clayPlanned.widthDb, 3) + ","
                              + fixed (probe.liveClay.monoAmount, 4) + "," + fixed (probe.liveClay.widthDb, 3) + ","
                              + fixed (probe.clayGuard.phaseFloor, 4) + "," + fixed (probe.clayGuard.phaseHoldSeconds, 3) + ","
                              + (probe.clayGuard.phaseActive ? "1" : "0") + "," + fixed (probe.clayGuardSeconds, 5) + ","
                              + feature (probe.liveLowCorrelation) + "," + feature (probe.liveLowCorrelationFast) + ","
                              + fixed (blockStatus.clayLowCutHz, 2) + "," + fixed (blockStatus.clayMonoAmount, 4) + ","
                              + fixed (blockStatus.clayMonoHz, 1) + "," + fixed (blockStatus.clayWidthDb, 3) + ","
                              + std::to_string (peakGuard.guardedSamples) + "," + fixed (peakGuard.minGain, 4) + ","
                              + energy (peakGuard.blendIntendedEnergy) + "," + energy (peakGuard.blendOutputEnergy) + ","
                              + fixed (effective.limit.gainDb, 3) + "," + fixed (effective.color.toneDb, 3) + ","
                              + fixed (effective.color.glue, 2) + "," + fixed (effective.color.heat, 2) + ","
                              + fixed (probe.clayPressureScale, 4) + ","
                              + (probe.clayPeakCostBlocks >= celadon::glaze::ClayPressureMonitor::minimumBlocks ? fixed (probe.clayPeakCostDb, 3) : std::string())
                              + "," + std::to_string (probe.clayPeakCostBlocks) + "," + fixed (probe.clayBlendLossSmoothedDb, 3) + ","
                              + calibrationTraceColumns (chain->getGlazeController()) + "\n";
            }
        }

        stats.finish (total, result);
        result.status = chain->getGlazeStatus();

        const auto committed = chain->getGlazeController().getCommittedStateForTesting();
        result.hasProfile = committed.hasProfile;
        result.profile = committed.profile;

        const auto finalProbe = chain->getGlazeController().getProbeForTesting();
        result.clayPressureScale = finalProbe.clayPressureScale;
        result.clayPressureSteps = finalProbe.clayPressureSteps;
        result.clayPeakCostDb = finalProbe.clayPeakCostDb;
        result.clayPeakCostBlocks = finalProbe.clayPeakCostBlocks;
    }

    //==============================================================================
    // 同决策重放（ADR-016）：修坯门槛要回答「同样的决策下，修坯开着有没有帮倒忙」。两条各自跑 GLAZE 的链会因为约束反馈、
    // 保护层发生的时刻不同而走到不同的方案（上限、Glue / Heat 份额、校准偏差都不同），跨链比较量到的多半是方案分岔（T-015 后
    // WTF g70 两条链上限 1.145 / 0.274 dB）。所以另起一条链：GLAZE 0（编排器不做任何决策，实际参数逐位等于喂进去的值，
    // 也不按保护层时隙切片），逐块喂「自动」链那一块实际交给模块的全部参数，只把修坯换掉：
    //   - off：三项都「关」→ 修坯中性（门槛拿它对比「自动」）；
    //   - copy：三项用「手动」填自动链那一块的实际修坯值 → 应与自动链逐位相同（阳性对照：证明重放本身不走样，
    //     包括 GLAZE 0 不切片这一点）。手动收单固定是「比例 1」，自动收单比例在 0、1 之间的块表达不了，对照记「不适用」。
    enum class ReplayClay { off, copy };

    // 同决策重放阳性对照的容差：最大样本差 ≤ 1e-9（−180 dBFS）算复现。不要求逐位相同：GLAZE > 0 的链按保护层 10 ms 时隙把宿主块
    // 切片处理、GLAZE 0 的重放链整块处理，整条链在歌尾余振衰减到 −600 dBFS 量级时会随切块方式差一点（2026-09-28 实测：l_kskdhjf、
    // ooaa 只在最后几百毫秒差 1e-31，重放链自己把每块切两段也差同样的量），与指标（0.001 dB / LU 分辨率）无关。重放真走样
    // （某块参数没照搬对）差值在 1e-6 以上，与这个容差差好几个数量级。
    constexpr double replayTolerance = 1.0e-9;

    double toDbfs (double linear)
    {
        return linear > 0.0 ? 20.0 * std::log10 (linear) : -std::numeric_limits<double>::infinity();
    }

    // 两段等长音频的最大逐样本差（长度或声道数不同返回 1 = 明显不同）。
    double maxAbsDifference (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
    {
        if (a.getNumChannels() != b.getNumChannels() || a.getNumSamples() != b.getNumSamples())
            return 1.0;

        double worst = 0.0;

        for (int ch = 0; ch < a.getNumChannels(); ++ch)
        {
            const auto* x = a.getReadPointer (ch);
            const auto* y = b.getReadPointer (ch);

            for (int i = 0; i < a.getNumSamples(); ++i)
                worst = std::max (worst, std::abs (static_cast<double> (x[i]) - static_cast<double> (y[i])));
        }

        return worst;
    }


    // 一块自动链的高级母带实际值能不能在重放链上原样复现（T-032 R10-01）：推响不刺 / 目标频谱的强度经 getEffectiveParams() 的
    // harshResolved / targetResolved 无损回喂；只有「参考」目标形状（参考曲线学好了才会出现）重放链复现不了——它没有侧链参考。
    bool masteringReplayable (const celadon::MasteringSettings& mastering) noexcept
    {
        return mastering.targetMode != celadon::TargetMode::reference;
    }

    // 一块自动链的实际修坯值能不能用「手动」态原样表达（ClayGuard.cpp resolveClayParams 的手动分支 + 钳制）。
    bool clayExpressibleAsManual (const celadon::ClayModule::Params& clay) noexcept
    {
        const auto lowCutOk = clay.lowCutHz == 0.0f
                           || (clay.lowCutHz >= celadon::ClayModule::minLowCutHz && clay.lowCutHz <= celadon::ClayModule::maxLowCutHz);
        const auto monoOk = clay.monoAmount == 0.0f || clay.monoAmount == 1.0f;
        const auto widthOk = clay.widthDb >= celadon::ClayModule::minWidthDb && clay.widthDb <= celadon::ClayModule::maxWidthDb;
        return lowCutOk && monoOk && widthOk;
    }

    celadon::ChainParams replayParams (const celadon::ChainParams& effective, ReplayClay mode) noexcept
    {
        // effective.mastering 是交给模块的实际值（T-032 R10-01）：强度写在 harshResolved / targetResolved、编码余量已扣进 Ceiling，
        // 原样照搬就逐位复现推响不刺 / 目标频谱（改前回写的是原始设置，GLAZE 0 的重放链把两项「自动」算成 0）。
        auto p = effective;
        p.glazePercent = 0.0f;         // 编排器不决策：实际参数 = 这份参数（逐位）
        p.limit.listen = false;
        p.clay = celadon::ClayModule::Params {};   // 作为输入时编排器忽略（ChainParams.h），只看 claySettings

        auto& c = p.claySettings;
        const auto& used = effective.clay;
        c.bypass = used.bypass;

        if (mode == ReplayClay::off)
        {
            c.lowCutMode = c.monoMode = c.widthMode = celadon::ClayMode::off;
            return p;
        }

        c.lowCutMode = used.lowCutHz > 0.0f ? celadon::ClayMode::manual : celadon::ClayMode::off;
        c.lowCutHz = used.lowCutHz > 0.0f ? used.lowCutHz : c.lowCutHz;
        c.monoMode = used.monoAmount > 0.0f ? celadon::ClayMode::manual : celadon::ClayMode::off;
        c.monoHz = used.monoAmount > 0.0f ? used.monoHz : c.monoHz;
        c.widthMode = used.widthDb != 0.0f ? celadon::ClayMode::manual : celadon::ClayMode::off;
        c.widthDb = used.widthDb;
        return p;
    }

    // 重放一遍：块长、宿主时间线与自动链的第二遍相同；prepare 前交 effectiveLog.prepared，之后每块取 blocks 里同一块的。
    // latency 应与自动链相同（Clay 的 D + G 不随参数变）。
    void replayPass (const juce::AudioBuffer<float>& input, double sampleRate, int blockSize,
                     const EffectiveLog& effectiveLog, ReplayClay mode, RenderResult& result)
    {
        auto chain = std::make_unique<celadon::CeladonChain>();
        const auto& blocks = effectiveLog.blocks;
        chain->setParams (replayParams (effectiveLog.prepared, mode), false);
        chain->prepare (makeSpec (sampleRate, blockSize));

        const auto latency = chain->getLatencySamples();
        const auto total = prepareIo (input, sampleRate, latency, result);

        chain->getAndResetLimitGainReductionDb();
        chain->getClayForTesting().getAndResetPeakGuardStats();

        PassStats stats;
        std::size_t index = 0;

        for (int position = 0; position < total; position += blockSize, ++index)
        {
            const auto length = std::min (blockSize, total - position);
            juce::AudioBuffer<float> block (result.io.getArrayOfWritePointers(), 2, position, length);

            chain->setHostInfo (makeHostInfo (position));
            chain->setParams (replayParams (blocks.empty() ? effectiveLog.prepared : blocks[std::min (index, blocks.size() - 1)], mode), false);

            if (chain->process (block))
                ++result.nonFiniteBlocks;

            if (chain->getLatencySamples() != latency)
                result.latencyChanged = true;

            stats.addBlock (*chain, length, result);
        }

        stats.finish (total, result);
        result.status = chain->getGlazeStatus();
    }

    //==============================================================================
    // 写文件

    bool writeFloatWav (const juce::File& file, const juce::AudioBuffer<float>& audio, double sampleRate, std::string& error)
    {
        // FileOutputStream 打开已有文件时是接在末尾写：先删掉旧文件（上一次运行的同名成品）。
        if (file.exists() && ! file.deleteFile())
        {
            error = "删不掉已有的同名文件";
            return false;
        }

        std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();

        if (stream == nullptr)
        {
            error = "建不了文件";
            return false;
        }

        // 32-bit IEEE float：对齐增益可能把峰值推过 0 dBFS，浮点格式原样保留，不削波、不再限幅。
        juce::WavAudioFormat wav;
        const auto writerOptions = juce::AudioFormatWriterOptions{}
                                       .withSampleRate (sampleRate)
                                       .withNumChannels (audio.getNumChannels())
                                       .withBitsPerSample (32)
                                       .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
        auto writer = wav.createWriterFor (stream, writerOptions);

        if (writer == nullptr)
        {
            stream.reset();
            file.deleteFile();
            error = "建不了 WAV 写入器";
            return false;
        }

        const auto ok = writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
        writer.reset();   // 析构时补写文件头里的长度，并关闭文件

        if (! ok)
        {
            file.deleteFile();
            error = "写到一半失败（磁盘满？）";
            return false;
        }

        return true;
    }

    // UTF-8 文本原样写入（replaceWithData 先写临时文件再改名，写一半断电也不会留下半截表）。
    bool writeText (const juce::File& file, const std::string& text)
    {
        return file.replaceWithData (text.data(), text.size());
    }

    //==============================================================================
    // 指标表

    struct Row
    {
        bool isInput = false;                  // true = 「原始输入」对照行（其余渲染相关的列为空）
        Combo combo;
        Measurement measurement;               // 对齐前
        bool aligned = false;
        double alignGainDb = 0.0;
        double grMeanDb = 0.0, grMaxDb = 0.0;
        double phaseGuardPercent = 0.0, widthGuardPercent = 0.0;
        double peakGuardPercent = 0.0, peakGuardMinGain = 1.0, slowInvertedPercent = 0.0, limitInputPeakDb = -1000.0;
        double blendIntendedEnergy = 0.0, blendOutputEnergy = 0.0;   // 守护混合损失（T-043）：全曲两份 K 加权能量
        std::int64_t blendSamples = 0;
        Status status;                         // 成品末尾（getGlazeStatus）
        Phase listenPhase = Phase::unlearned;  // 第一遍（聆听）结束时
        std::string fileName;                  // 写成功的成品文件名；失败为空
        bool hasProfile = false;               // 成品末尾已提交的画像（诊断列：先试后用的代价、真次声、低频相关度）
        celadon::glaze::MusicProfile profile;
        float clayPressureScale = 1.0f;
        int clayPressureSteps = 0;
        double clayPeakCostDb = 0.0;
        int clayPeakCostBlocks = 0;
        std::string replayCheck;               // 「自动」行（GLAZE > 0）：同决策重放阳性对照 ok / mismatch / n/a（ADR-016）；其余行为空
        double replayMaxDifference = -1.0;     // 阳性对照的最大样本差（线性；< 0 = 没做）
    };

    struct SongReport
    {
        std::string name;                      // 成品文件名里的歌名
        std::string sourceFile;                // 原文件名
        bool loaded = false;
        double sampleRate = 0.0;
        int sourceChannels = 0;
        double durationSeconds = 0.0;
        std::vector<Row> rows;                 // 第一行是原始输入
        std::vector<std::string> notes;        // 警告与核对结果
    };

    std::string orDash (bool has, const std::string& text)
    {
        return has ? text : std::string ("—");
    }

    std::string statusText (const Row& row)
    {
        if (row.combo.clay == ClayChoice::replayOff)
            return "同决策重放（GLAZE 0，逐块照搬同组「自动」实际交给模块的参数，修坯关）";

        std::string text = phaseName (row.status.phase);
        text += row.status.calibrationValid ? " · 校准有效" : " · 校准无效";

        if (row.combo.style == static_cast<int> (celadon::glaze::Style::automatic) && row.combo.glaze > 0.0f)
            text += std::string (" · 偏") + styleNames[static_cast<std::size_t> (row.status.resolvedStyle)];

        return text;
    }

    // 先试后用（ADR-012）：成品末尾哪几项因「试过会抬高峰值」被减量 / 否决（Status::clayCostLimitedMask）。空 = 没有。
    std::string costLimitedText (std::uint8_t mask)
    {
        std::string text;

        if ((mask & celadon::glaze::clayCostLimitedLowCut) != 0) text += "lowcut";
        if ((mask & celadon::glaze::clayCostLimitedMono) != 0)   text += text.empty() ? "mono" : "+mono";
        if ((mask & celadon::glaze::clayCostLimitedWidth) != 0)  text += text.empty() ? "width" : "+width";

        return text;
    }

    std::string markdownRow (const Row& row)
    {
        const auto& m = row.measurement;
        std::vector<std::string> cells;

        cells.push_back (row.isInput ? std::string ("原始输入") : comboLabel (row.combo, " · "));
        cells.push_back (orDash (m.hasLoudness, fixed (m.integratedLufs, 2)));
        cells.push_back (orDash (! row.isInput && row.aligned, signedFixed (row.alignGainDb, 2)));
        cells.push_back (orDash (m.hasTruePeak, fixed (m.truePeakDbtp, 2)));
        cells.push_back (orDash (m.hasLoudness && m.hasTruePeak, fixed (m.truePeakDbtp - m.integratedLufs, 2)));
        cells.push_back (orDash (! row.isInput, fixed (row.grMeanDb, 2) + " / " + fixed (row.grMaxDb, 2)));
        cells.push_back (orDash (m.hasLowCorrelation, fixed (m.lowCorrelation, 3)));
        cells.push_back (orDash (m.hasSubShare, fixed (m.subShareDb, 1)));

        if (row.isInput)
        {
            // 低切、收单 Hz、收单量、加宽、保护介入、GLAZE 状态、成品文件：原始输入没有这些。
            for (int i = 0; i < 7; ++i)
                cells.push_back ("—");
        }
        else
        {
            const auto& s = row.status;
            cells.push_back (s.clayLowCutHz > 0.0f ? fixed (s.clayLowCutHz, 1) : std::string ("关"));
            cells.push_back (fixed (s.clayMonoHz, 0));
            cells.push_back (fixed (s.clayMonoAmount, 2));
            cells.push_back (signedFixed (s.clayWidthDb, 2));
            cells.push_back (fixed (row.phaseGuardPercent, 1) + "% / " + fixed (row.widthGuardPercent, 1) + "%"
                             + (s.clayCostLimitedMask != 0 ? "，代价限：" + costLimitedText (s.clayCostLimitedMask) : std::string()));
            cells.push_back (statusText (row));
            cells.push_back (row.fileName.empty() ? std::string ("（没写成）") : row.fileName);
        }

        std::string line = "|";

        for (const auto& cell : cells)
            line += " " + mdText (cell) + " |";

        return line;
    }

    // 诊断列：成品末尾已提交画像的某个特征；没有画像、特征无效时为空。
    std::string profileFeature (const Row& row, const celadon::glaze::Feature& feature, int decimals)
    {
        return ! row.isInput && row.hasProfile && feature.valid ? fixed (feature.value, decimals) : std::string();
    }

    std::string csvRow (const SongReport& report, const Row& row)
    {
        const auto& m = row.measurement;
        const auto& s = row.status;
        const auto render = ! row.isInput;
        const auto replay = render && row.combo.clay == ClayChoice::replayOff;   // GLAZE 0 的重放链：GLAZE 状态列没有意义
        const auto empty = std::string();

        char hash[32] = {};
        const auto hashWritten = std::snprintf (hash, sizeof (hash), "%016llx", static_cast<unsigned long long> (m.hash));

        const std::vector<std::string> fields
        {
            render ? std::string ("render") : std::string ("input"),
            report.name,
            report.sourceFile,
            fixed (report.sampleRate, 0),
            fixed (report.durationSeconds, 3),
            render ? formatGlaze (row.combo.glaze) : empty,
            render ? std::string (clayId (row.combo.clay)) : empty,
            render ? std::string (styleIds[static_cast<std::size_t> (row.combo.style)]) : empty,
            m.hasLoudness ? fixed (m.integratedLufs, 3) : empty,
            render && row.aligned ? fixed (row.alignGainDb, 3) : empty,
            m.hasTruePeak ? fixed (m.truePeakDbtp, 3) : empty,
            m.hasLoudness && m.hasTruePeak ? fixed (m.truePeakDbtp - m.integratedLufs, 3) : empty,
            render ? fixed (row.grMeanDb, 3) : empty,
            render ? fixed (row.grMaxDb, 3) : empty,
            m.hasLowCorrelation ? fixed (m.lowCorrelation, 4) : empty,
            m.hasSubShare ? fixed (m.subShareDb, 2) : empty,
            render ? fixed (s.clayLowCutHz, 2) : empty,
            render ? fixed (s.clayMonoHz, 2) : empty,
            render ? fixed (s.clayMonoAmount, 4) : empty,
            render ? fixed (s.clayWidthDb, 3) : empty,
            render ? fixed (row.phaseGuardPercent, 2) : empty,
            render ? fixed (row.widthGuardPercent, 2) : empty,
            render ? costLimitedText (s.clayCostLimitedMask) : empty,
            render ? (replay ? std::string ("replay") : std::string (phaseId (s.phase))) : empty,
            render && ! replay ? std::string (s.calibrationValid ? "1" : "0") : empty,
            render && ! replay ? std::string (styleIds[static_cast<std::size_t> (s.resolvedStyle)]) : empty,
            render && ! replay ? std::string (phaseId (row.listenPhase)) : empty,
            hashWritten > 0 ? std::string (hash) : empty,
            render ? row.fileName : empty,
            profileFeature (row, row.profile.lowCutPeakCostDb, 3),
            profileFeature (row, row.profile.monoPeakCostDb[0], 3),
            profileFeature (row, row.profile.monoPeakCostDb[1], 3),
            profileFeature (row, row.profile.monoPeakCostDb[2], 3),
            profileFeature (row, row.profile.monoPeakCostDb[3], 3),
            profileFeature (row, row.profile.widthPeakCostDb, 3),
            profileFeature (row, row.profile.subShareDb, 2),
            profileFeature (row, row.profile.lowCorrelation, 4),
            row.hasProfile && row.profile.lowCutPeakCostDb.valid ? fixed (row.profile.lowCutPeakCostDb.effectiveSeconds, 1) : empty,
            render ? fixed (row.peakGuardPercent, 2) : empty,
            render ? fixed (row.peakGuardMinGain, 3) : empty,
            render ? fixed (row.slowInvertedPercent, 2) : empty,
            render && row.limitInputPeakDb > -999.0 ? fixed (row.limitInputPeakDb, 3) : empty,
            render && ! replay ? fixed (row.clayPressureScale, 2) : empty,
            render && ! replay ? std::to_string (row.clayPressureSteps) : empty,
            row.replayCheck,
            row.replayMaxDifference < 0.0 ? empty
                : (row.replayMaxDifference > 0.0 ? fixed (toDbfs (row.replayMaxDifference), 1) : std::string ("-inf")),
            fixed (m.musicSeconds, 1),
            profileFeature (row, row.profile.lowCutBlendLossDb[0], 3),
            profileFeature (row, row.profile.lowCutBlendLossDb[1], 3),
            render && row.blendSamples > 0 && row.blendIntendedEnergy > 0.0 && row.blendOutputEnergy > 0.0
                ? fixed (10.0 * std::log10 (row.blendIntendedEnergy / row.blendOutputEnergy), 3) : empty,
            render && ! replay && row.clayPeakCostBlocks >= celadon::glaze::ClayPressureMonitor::minimumBlocks
                ? fixed (row.clayPeakCostDb, 3) : empty,
            render && ! replay ? fixed (0.1 * (double) row.clayPeakCostBlocks, 1) : empty,
        };

        std::string line;

        for (std::size_t i = 0; i < fields.size(); ++i)
            line += (i > 0 ? "," : "") + csvField (fields[i]);

        return line;
    }

    std::string makeCsv (const std::vector<SongReport>& reports)
    {
        // 列名英文 snake_case（给脚本 / 表格软件用）；含义与 metrics.md 的「口径」一节相同。
        std::string csv = "kind,song,source_file,sample_rate,duration_s,glaze,clay,style,lufs,align_gain_db,true_peak_dbtp,"
                          "plr_db,limit_gr_mean_db,limit_gr_max_db,low_corr_lt125hz,infra_share_lt16hz_db,clay_lowcut_hz,"
                          "clay_mono_hz,clay_mono_amount,clay_width_db,phase_guard_pct,width_guard_pct,clay_cost_limited,glaze_phase,"
                          "calibration_valid,resolved_style,listen_phase,audio_hash,file,trial_lowcut_cost_db,trial_mono60_cost_db,"
                          "trial_mono80_cost_db,trial_mono120_cost_db,trial_mono160_cost_db,trial_width_cost_db,profile_sub_share_db,"
                          "profile_low_corr,trial_seconds,peak_guard_pct,peak_guard_min_gain,slow_inverted_pct,limit_input_peak_db,"
                          "clay_pressure_scale,clay_pressure_steps,replay_check,replay_max_diff_dbfs,music_seconds,trial_blend10_db,"
                          "trial_blend20_db,clay_blend_loss_db,clay_peak_cost_db,clay_peak_cost_s\n";

        for (const auto& report : reports)
            for (const auto& row : report.rows)
                csv += csvRow (report, row) + "\n";

        return csv;
    }

    std::string definitionsMarkdown()
    {
        return "## 口径\n\n"
               "- **原始输入**：读入后立体声化（单声道复制成双声道、多声道取前两个）的原曲，用同一套量法，作对照。\n"
               "- **LUFS（对齐前）**：插件自己的 BS.1770-4 表（`celadon::LoudnessMeter`，与界面 OUT 表同一算法）量整段成品"
               "（已去掉链延迟、含 0.3 s 余振）。\n"
               "- **对齐增益**：`--match-lufs` − 对齐前 LUFS；成品只乘这一个增益，不再限幅。\n"
               "- **真峰值**：插件的 8× 真峰值检测器（与限幅器、表同一算法），对齐前；默认 Ceiling 下应 ≤ -1.0 dBTP"
               "（验收口径容差 +0.01 dB）。\n"
               "- **PLR**：真峰值 − 整体响度（与对齐增益无关）。\n"
               "- **限幅 GR 平均 / 最大**：第二遍每个宿主块读一次 `getAndResetLimitGainReductionDb()`（块内最大压缩量）；"
               "平均 = 各块读数按块长加权（是「块峰值」的平均，比逐样本平均偏高；静音段计 0），最大 = 全曲最大。\n"
               "- **低频相关度 < 125 Hz**：左右各过 4 阶 Butterworth 125 Hz 低通后 ρ = 2ΣLR / (ΣL² + ΣR²) = (P_M − P_S) / (P_M + P_S)，"
               "与分析器 lowCorrelation 同一口径：1 = 单声道，0 = 不相关，−1 = 反相；左右电平不等时比皮尔逊相关系数低。\n"
               "- **真次声 < 16 Hz**：M = (L + R) / 2 过 8 阶 Butterworth 16 Hz 低通后的能量 ÷ M 全频能量（dB），"
               "与分析器 subShareDb 同一定义（ADR-012；这里整段计，不设有效音频门；静音段能量可忽略）。只看 M：侧声道里的次声不计入。\n"
               "- **低切 / 收单 Hz / 收单量 / 加宽**：成品末尾 `getGlazeStatus()` 的 clayLowCutHz / clayMonoHz / clayMonoAmount / "
               "clayWidthDb（上一块交给 ClayModule 的实际值）；低切「关」= 0；收单量为 0 时收单 Hz 无意义。\n"
               "- **保护介入**：第二遍里反相保护 / 宽度保护介入的时长占比（每块读一次状态）。\n"
               "- **GLAZE 状态**：成品末尾的阶段与「校准有效」；风格为「自动」且 GLAZE > 0 时附此刻偏向的风格。"
               "第一遍（聆听）结束时的阶段在 metrics.csv 的 listen_phase 列。\n"
               "- **成品文件**：`<歌名>__g<GLAZE>_clay-<auto|off|replay-off>_<风格>.wav`。metrics.csv 的 audio_hash 是对齐前成品"
               "（原始输入行是立体声化的输入）的 FNV-1a 哈希：同一首歌、同一组设置跨版本哈希相同 = 逐位不变。\n"
               "- **同决策重放（clay-replay-off，ADR-016）**：每组 GLAZE > 0 的修坯「自动」之后，另起一条 GLAZE 0 的链，逐块照搬"
               "「自动」链那一块实际交给模块的全部参数（含自动增量、校准偏差、保护退让），只把修坯换成「关」。两条链的决策一模一样，"
               "差别只剩修坯本身——修坯门槛比的是「自动 vs 重放全关」，不再比两条各自跑 GLAZE、方案会分岔的链。阳性对照：修坯用"
               "「手动」照填自动链的实际值再重放一遍，应与「自动」逐位相同（metrics.csv 的 replay_check：ok / mismatch / n/a）。"
               "重放行的 GLAZE 状态列没有意义（写 replay / 空）。\n"
               "- **有效音乐**：原曲 K 加权 100 ms 子块在 −70 LUFS 绝对门以上的总时长（与校准器同一门限；歌尾静音不算），"
               "metrics.csv 的 music_seconds。门槛只要求有效音乐 ≥ 30 s 的歌在成品末尾「已锁定 + 校准有效」。\n";
    }

    std::string makeMarkdown (const Options& options, const std::vector<Combo>& combos,
                              const std::vector<SongReport>& reports, const std::string& generatedAt)
    {
        std::string md = "# celadon_render 真歌对照指标\n\n";
        md += "- 生成时间：" + generatedAt + "（每做完一首歌重写一次本表）\n";
        md += "- 网格：" + gridText (options) + "，共 " + std::to_string (combos.size()) + " 组；宿主块长 "
            + std::to_string (options.blockSize) + " 样本；各文件按自己的采样率渲染\n";
        md += "- 对齐：成品按整体响度只乘增益对齐到 " + fixed (options.matchLufs, 1)
            + " LUFS；32-bit float，允许超过 0 dBFS，不再限幅\n";
        md += "- 流程：每组先完整聆听一遍（输出丢弃）→ 存 GLAZE 状态（createGlazeStateXml）→ 新链读回（restoreGlazeStateXml）"
              "后从头渲染第二遍作成品；丢掉开头的链延迟、末尾多冲 0.3 s 余振，成品与原曲逐样本对齐\n";
        md += "- 其余参数全是新实例默认（Ceiling -1.0 dBTP、Lookahead 2 ms、Limit Gain 0 等，见 Source/ParameterDefaults.h，"
              "插件参数表同一处真源）\n";
        md += "- 高级母带（新实例默认）：" + masteringText (makeFactoryParams().mastering)
            + "；两项「自动」的实际强度随 GLAZE 联动，GLAZE 0 时为 0\n\n";
        md += "先盲听、再看这张表（盲听做法见 tools/render/README.md）。\n\n";

        for (std::size_t i = 0; i < reports.size(); ++i)
        {
            const auto& report = reports[i];
            md += "## " + std::to_string (i + 1) + ". " + mdText (report.name) + "\n\n";

            if (report.loaded)
            {
                md += "原文件：" + mdText (report.sourceFile) + "，" + fixed (report.sampleRate, 0) + " Hz，"
                    + channelText (report.sourceChannels) + "，" + formatDuration (report.durationSeconds);

                if (! report.rows.empty() && report.rows.front().isInput)
                    md += "，有效音乐 " + fixed (report.rows.front().measurement.musicSeconds, 1) + " s";

                md += "\n\n";
                md += "| 组合 | LUFS（对齐前） | 对齐增益 dB | 真峰值 dBTP | PLR dB | 限幅 GR 平均 / 最大 dB | 低频相关度 < 125 Hz "
                      "| 真次声 < 16 Hz dB | 低切 Hz | 收单 Hz | 收单量 | 加宽 dB | 保护介入 反相 / 宽度 | GLAZE 状态 | 成品文件 |\n";
                md += "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---|\n";

                for (const auto& row : report.rows)
                    md += markdownRow (row) + "\n";

                md += "\n";
            }
            else
            {
                md += "原文件：" + mdText (report.sourceFile) + "\n\n";
            }

            if (! report.notes.empty())
            {
                md += "备注：\n\n";

                for (const auto& note : report.notes)
                    md += "- " + note + "\n";

                md += "\n";
            }
        }

        md += definitionsMarkdown();
        return md;
    }

    // 第一遍结束时还没到「已锁定」的说明（只对 GLAZE > 0 有意义：GLAZE 0 不校准，也不影响声音）。
    std::string listenNote (Phase phase)
    {
        switch (phase)
        {
            case Phase::unlearned:
            case Phase::listening:
                return "第一遍结束时 GLAZE 还没学完（有效音频不够长？）：成品里 GLAZE 按插件同样的规则边听边学";

            case Phase::planTransition:
                return "第一遍结束时方案过渡还没走完：成品开头用执行快照里的量，过渡与校准在成品里接着走";

            case Phase::calibrating:
                return "第一遍结束时校准还没完成：成品开头用执行快照里的量，校准在成品里接着做";

            case Phase::locked:
                return {};
        }

        return {};
    }

    // GLAZE 0 下修坯「自动」与「全关」两组成品应逐位相同（设计规格-Clay 第 4 节「逐位直通」：GLAZE 0 且抽屉默认 → 整链与
    // 改动前逐位相同；三项都「关」同样）。两组都在网格里时按对齐前成品的哈希核对（对齐增益由同一个响度算出，对齐前相同即
    // 对齐后相同），结果写进备注与 stderr——真歌上的阳性对照。
    void checkGlazeZeroPairs (SongReport& report)
    {
        for (const auto& automatic : report.rows)
        {
            if (automatic.isInput || automatic.combo.glaze > 0.0f || automatic.combo.clay != ClayChoice::automatic)
                continue;

            for (const auto& off : report.rows)
            {
                if (off.isInput || off.combo.glaze > 0.0f || off.combo.clay != ClayChoice::off
                    || off.combo.style != automatic.combo.style)
                    continue;

                const auto prefix = std::string ("GLAZE 0 · 风格 ") + styleIds[static_cast<std::size_t> (automatic.combo.style)]
                                  + "：修坯「自动」与「全关」两组成品";
                const auto note = automatic.measurement.hash == off.measurement.hash
                                    ? prefix + "逐位相同（阳性对照通过）"
                                    : prefix + "不同！违反设计规格-Clay 第 4 节「逐位直通」（GLAZE 0 时 Clay 自动值应为 0），请查";
                report.notes.push_back (note);
                std::cerr << "  " << note << "\n";
            }
        }
    }

    // 成品 = 丢掉开头链延迟之后的一段（视图，不拷贝）：先量（对齐前），再只乘增益对齐响度，最后写 32-bit float wav
    // （--metrics-only 时不写：指标照常，成品文件列为空；逐位对照仍靠 audio_hash）。返回成品文件名（不管写没写）。
    std::string measureAlignWrite (const Options& options, const Song& song, SongReport& report, RenderResult& render,
                                   Row& row, int& failures, int& written)
    {
        const auto label = comboLabel (row.combo, " ");
        const auto notePrefix = label + "：";
        juce::AudioBuffer<float> finished (render.io.getArrayOfWritePointers(), 2, render.latencySamples, render.outputLength);
        row.measurement = measure (finished, song.sampleRate);

        if (row.measurement.hasLoudness)
        {
            row.aligned = true;
            row.alignGainDb = options.matchLufs - row.measurement.integratedLufs;
            finished.applyGain (static_cast<float> (std::pow (10.0, row.alignGainDb / 20.0)));
        }
        else
        {
            report.notes.push_back (notePrefix + "整体响度没有读数（静音或低于 -70 LUFS），没有对齐");
        }

        std::cerr << loudnessText (row.measurement);

        if (row.aligned)
            std::cerr << "，对齐 " << signedFixed (row.alignGainDb, 2) << " dB";

        const auto fileName = outputFileName (report.name, row.combo);
        std::string error;

        if (options.metricsOnly)
        {
            std::cerr << "（只出指标，不写文件）\n";
        }
        else if (writeFloatWav (options.outputDir.getChildFile (juce::String::fromUTF8 (fileName.c_str())), finished,
                                song.sampleRate, error))
        {
            row.fileName = fileName;
            ++written;
            std::cerr << " → " << fileName << "\n";
        }
        else
        {
            report.notes.push_back (notePrefix + "写 " + fileName + " 失败：" + error);
            ++failures;
            std::cerr << " → 写文件失败：" << error << "\n";
        }

        return fileName;
    }

    // 同决策重放（ADR-016）：「自动」这一组的第二遍刚渲染完、逐块参数在 effectiveLog 里。
    //   ① 阳性对照（copy）：修坯用「手动」填自动链的实际值，成品应复现自动链（最大样本差 ≤ replayTolerance）
    //      → automatic.replayCheck = ok / mismatch / n/a，最大差记进 replayMaxDifference；
    //   ② 重放全关（off）：同样的参数、修坯关，出一行 clay-replay-off（门槛拿它与「自动」比，tools/render/gate_check.py）。
    void replayAutomatic (const Options& options, const Song& song, SongReport& report, Row& automatic,
                          const EffectiveLog& effectiveLog, const juce::AudioBuffer<float>& autoAudio,
                          int autoLatency, int& failures, int& written)
    {
        const auto notePrefix = comboLabel (automatic.combo, " ") + "：";
        const auto& blocks = effectiveLog.blocks;
        const auto clayExpressible = clayExpressibleAsManual (effectiveLog.prepared.clay)
                                  && std::all_of (blocks.begin(), blocks.end(),
                                                  [] (const celadon::ChainParams& p) { return clayExpressibleAsManual (p.clay); });
        const auto masteringExpressible = masteringReplayable (effectiveLog.prepared.mastering)
                                       && std::all_of (blocks.begin(), blocks.end(),
                                                       [] (const celadon::ChainParams& p) { return masteringReplayable (p.mastering); });

        if (clayExpressible && masteringExpressible)
        {
            RenderResult copy;
            replayPass (song.audio, song.sampleRate, options.blockSize, effectiveLog, ReplayClay::copy, copy);
            const juce::AudioBuffer<float> finished (copy.io.getArrayOfWritePointers(), 2, copy.latencySamples, copy.outputLength);
            const auto difference = copy.latencySamples == autoLatency ? maxAbsDifference (finished, autoAudio) : 1.0;
            automatic.replayMaxDifference = difference;
            automatic.replayCheck = difference <= replayTolerance ? "ok" : "mismatch";

            if (difference > replayTolerance)
            {
                report.notes.push_back (notePrefix + "同决策重放的阳性对照不通过：重放（修坯手动照填）与自动链成品最大差 "
                                        + fixed (toDbfs (difference), 1) + " dBFS（允许 ≤ " + fixed (toDbfs (replayTolerance), 0)
                                        + " dBFS），修坯门槛的「重放全关」对照因此不可信，请查（ADR-016）");
                ++failures;
            }
        }
        else
        {
            automatic.replayCheck = "n/a";
            report.notes.push_back (notePrefix + "同决策重放的阳性对照不适用："
                                    + std::string (! clayExpressible ? "自动收单比例在 0、1 之间，「手动」态表达不了（手动收单固定是 1）"
                                                                     : "目标频谱用了「参考」形状，重放链没有侧链参考"));
        }

        Row row;
        row.combo = automatic.combo;
        row.combo.clay = ClayChoice::replayOff;
        std::cerr << "      重放全关：" << std::flush;

        RenderResult off;
        replayPass (song.audio, song.sampleRate, options.blockSize, effectiveLog, ReplayClay::off, off);
        row.grMeanDb = off.grMeanDb;
        row.grMaxDb = off.grMaxDb;
        row.phaseGuardPercent = off.phaseGuardPercent;
        row.widthGuardPercent = off.widthGuardPercent;
        row.peakGuardPercent = off.peakGuardPercent;
        row.blendIntendedEnergy = off.blendIntendedEnergy;
        row.blendOutputEnergy = off.blendOutputEnergy;
        row.blendSamples = off.blendSamples;
        row.peakGuardMinGain = off.peakGuardMinGain;
        row.slowInvertedPercent = off.slowInvertedPercent;
        row.limitInputPeakDb = off.limitInputPeakDb;
        row.status = off.status;
        measureAlignWrite (options, song, report, off, row, failures, written);

        // 注意：automatic 引用的是 report.rows 的最后一个元素，下面 push_back 之后它可能失效——此后不再用它。
        if (off.latencySamples != autoLatency || off.latencyChanged)
            report.notes.push_back (notePrefix + "重放链的延迟与自动链不同（不该发生：Clay 的 D + G 不随参数变），对照不可信");

        if (off.nonFiniteBlocks > 0)
            report.notes.push_back (notePrefix + "重放全关链输出出现非有限值 " + std::to_string (off.nonFiniteBlocks) + " 块");

        report.rows.push_back (std::move (row));
    }

    // 一首歌：原始输入对照行 + 网格里的每组设置（聆听 → 读回 → 成品 → 量 → 对齐 → 写）。
    void processSong (const Options& options, const std::vector<Combo>& combos, const Song& song,
                      SongReport& report, int& failures, int& written)
    {
        Row inputRow;
        inputRow.isInput = true;
        inputRow.measurement = measure (song.audio, song.sampleRate);
        report.rows.push_back (inputRow);
        std::cerr << "  原始输入：" << loudnessText (inputRow.measurement) << "\n";

        for (std::size_t c = 0; c < combos.size(); ++c)
        {
            const auto& combo = combos[c];
            const auto label = comboLabel (combo, " ");
            const auto params = makeComboParams (combo);

            Row row;
            row.combo = combo;

            std::cerr << "  [" << (c + 1) << "/" << combos.size() << "] " << label << "：聆听 " << std::flush;

            // ① 第一遍：完整聆听，输出丢弃，最后存 GLAZE 状态。
            auto started = nowSeconds();
            const auto listen = listenPass (song.audio, song.sampleRate, options.blockSize, params, options.trace);
            row.listenPhase = listen.status.phase;
            std::cerr << fixed (nowSeconds() - started, 1) << " s（第一遍末 " << phaseName (listen.status.phase)
                      << (listen.status.calibrationValid ? " · 校准有效" : "") << "）→ 成品 " << std::flush;

            // ② 第二遍：新链读回存档，从头渲染成品。GLAZE > 0 的「自动」组逐块记下实际参数，留给同决策重放（ADR-016）。
            started = nowSeconds();
            RenderResult render;
            const auto replay = options.replay && combo.clay == ClayChoice::automatic && combo.glaze > 0.0f;
            EffectiveLog effectiveLog;
            renderPass (song.audio, song.sampleRate, options.blockSize, params, listen.state.get(), options.trace, render,
                        replay ? &effectiveLog : nullptr);
            row.grMeanDb = render.grMeanDb;
            row.grMaxDb = render.grMaxDb;
            row.phaseGuardPercent = render.phaseGuardPercent;
            row.widthGuardPercent = render.widthGuardPercent;
            row.peakGuardPercent = render.peakGuardPercent;
            row.blendIntendedEnergy = render.blendIntendedEnergy;
            row.blendOutputEnergy = render.blendOutputEnergy;
            row.blendSamples = render.blendSamples;
            row.peakGuardMinGain = render.peakGuardMinGain;
            row.slowInvertedPercent = render.slowInvertedPercent;
            row.limitInputPeakDb = render.limitInputPeakDb;
            row.status = render.status;
            row.hasProfile = render.hasProfile;
            row.profile = render.profile;
            row.clayPressureScale = render.clayPressureScale;
            row.clayPressureSteps = render.clayPressureSteps;
            row.clayPeakCostDb = render.clayPeakCostDb;
            row.clayPeakCostBlocks = render.clayPeakCostBlocks;

            // 对齐前的自动链成品留一份（measureAlignWrite 会原地乘对齐增益）：同决策重放的阳性对照逐样本比它。
            juce::AudioBuffer<float> autoAudio;

            if (replay)
                autoAudio.makeCopyOf (juce::AudioBuffer<float> (render.io.getArrayOfWritePointers(), 2, render.latencySamples, render.outputLength));

            // ③ 量、对齐、写（见 measureAlignWrite）。
            const auto notePrefix = label + "：";
            std::cerr << fixed (nowSeconds() - started, 1) << " s → ";
            const auto fileName = measureAlignWrite (options, song, report, render, row, failures, written);

            // --trace：逐块记录写到 trace/<成品名去掉 .wav>.csv（与成品同名，便于对照）。
            if (options.trace && ! render.trace.empty())
            {
                const auto traceDir = options.outputDir.getChildFile ("trace");
                const auto traceName = juce::String::fromUTF8 (fileName.c_str()).upToLastOccurrenceOf (".wav", false, false) + ".csv";

                if (! traceDir.createDirectory() || ! traceDir.getChildFile (traceName).replaceWithText (juce::String::fromUTF8 (render.trace.c_str())))
                {
                    report.notes.push_back (notePrefix + "写逐块追踪 " + traceName.toStdString() + " 失败");
                    ++failures;
                }

                // 第一遍（聆听）的校准追踪：trace/<成品名去掉 .wav>__listen.csv。
                const auto listenName = traceName.upToLastOccurrenceOf (".csv", false, false) + "__listen.csv";

                if (! listen.trace.empty()
                    && ! traceDir.getChildFile (listenName).replaceWithText (juce::String::fromUTF8 (listen.trace.c_str())))
                {
                    report.notes.push_back (notePrefix + "写逐块追踪 " + listenName.toStdString() + " 失败");
                    ++failures;
                }
            }

            // 备注：第一遍没走到「已锁定」、链输出出现非有限值、链延迟变化。
            if (combo.glaze > 0.0f && listen.status.phase != Phase::locked)
                report.notes.push_back (notePrefix + listenNote (listen.status.phase));

            if (listen.nonFiniteBlocks + render.nonFiniteBlocks > 0)
                report.notes.push_back (notePrefix + "链输出出现非有限值（内部溢出）"
                                        + std::to_string (listen.nonFiniteBlocks + render.nonFiniteBlocks)
                                        + " 块：插件按「清零 + 整链淡入重启」处理，成品对应位置有一段静音");

            if (render.latencyChanged)
                report.notes.push_back (notePrefix + "渲染中链延迟变了（不该发生：Lookahead 固定 2 ms），成品开头的延迟补偿可能不准");

            if (! replay)
            {
                report.rows.push_back (std::move (row));
                continue;
            }

            // ⑤ 同决策重放：「自动」行先入表，replayAutomatic 把阳性对照的结论写进它，再在它后面放「重放全关」一行。
            const auto autoLatency = render.latencySamples;
            render.io.setSize (0, 0);   // 成品已写完：先放掉这段缓冲（长歌几百 MB），重放各自再分配
            report.rows.push_back (std::move (row));
            auto& automatic = report.rows.back();
            replayAutomatic (options, song, report, automatic, effectiveLog, autoAudio, autoLatency, failures, written);
        }
    }

    //==============================================================================
    // 自检（--selftest）：指标算法、文件名、延迟对齐与 GLAZE 0 逐位直通的阳性对照。
    // 这个工具不在 celadon_tests 里（单独的 CMake 目标），本机又不许编译运行，所以把对照做成工具自己的一个模式：
    // 服务器上编好后 render-linux.sh 先跑它，通不过就不渲染真歌（指标算错了，拿去对照听感只会误导）。
    // 每项的期望值与容差都写明依据；全部通过返回 0，否则 1。

    constexpr double selfTestRate = 48000.0;

    float testSine (double frequency, int index, double amplitude)
    {
        return static_cast<float> (amplitude * std::sin (juce::MathConstants<double>::twoPi * frequency
                                                         * static_cast<double> (index) / selfTestRate));
    }

    // 两个声道各用一个函数按样本序号生成。
    template <typename LeftFunction, typename RightFunction>
    juce::AudioBuffer<float> testSignal (int numSamples, LeftFunction left, RightFunction right)
    {
        juce::AudioBuffer<float> buffer (2, numSamples);

        for (int i = 0; i < numSamples; ++i)
        {
            buffer.setSample (0, i, left (i));
            buffer.setSample (1, i, right (i));
        }

        return buffer;
    }

    // 两个声道互不相关的均匀白噪声（固定种子，结果可复现）。
    juce::AudioBuffer<float> testNoise (int numSamples, float amplitude)
    {
        juce::AudioBuffer<float> buffer (2, numSamples);
        juce::Random random (20260926);

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < numSamples; ++i)
                buffer.setSample (ch, i, amplitude * (2.0f * random.nextFloat() - 1.0f));

        return buffer;
    }

    // 左声道的归一化互相关：输出第 i + lag 个样本对输入第 i 个（i 两头各留 margin，保证不越界）。
    double crossCorrelation (const juce::AudioBuffer<float>& input, const juce::AudioBuffer<float>& output, int lag, int margin)
    {
        const auto* x = input.getReadPointer (0);
        const auto* y = output.getReadPointer (0);
        const auto end = std::min (input.getNumSamples(), output.getNumSamples()) - margin;
        double xy = 0.0, xx = 0.0, yy = 0.0;

        for (int i = margin; i < end; ++i)
        {
            const auto a = static_cast<double> (x[i]);
            const auto b = static_cast<double> (y[i + lag]);
            xy += a * b;
            xx += a * a;
            yy += b * b;
        }

        return xx > 0.0 && yy > 0.0 ? xy / std::sqrt (xx * yy) : 0.0;
    }

    struct SelfTest
    {
        int failed = 0;

        void check (bool ok, const std::string& name, const std::string& detail)
        {
            std::cerr << (ok ? "通过  " : "失败  ") << name << "：" << detail << "\n";

            if (! ok)
                ++failed;
        }
    };

    int runSelfTest()
    {
        SelfTest test;
        const auto tenSeconds = static_cast<int> (10.0 * selfTestRate);

        // ---- 低频相关度（口径 ρ = 2ΣLR / (ΣL² + ΣR²)）----
        // 左右相同 / 右 = −左 / 右 = 0.5 × 左：两路过的是同一组 double 滤波器，线性运算对取反、乘 2 的幂逐位对称，
        // 所以前两项是 ±1（留 1e-12 只防编译器改写运算顺序），第三项 = 2g / (1 + g²) = 0.8（g = 0.5；皮尔逊相关系数在这里
        // 会是 1——两种口径的区别就在左右电平不等时）。两路不相关噪声：125 Hz 低通后 10 s 约 2 × 125 × 10 = 2500 个独立样本，
        // ρ 的标准差约 0.02，容差 0.1 是 5 倍标准差（种子固定，结果每次相同）。
        {
            const auto sine = [] (int i) { return testSine (60.0, i, 0.5); };
            const auto same = measure (testSignal (tenSeconds, sine, sine), selfTestRate);
            const auto inverted = measure (testSignal (tenSeconds, sine, [] (int i) { return -testSine (60.0, i, 0.5); }),
                                           selfTestRate);
            const auto half = measure (testSignal (tenSeconds, sine, [] (int i) { return testSine (60.0, i, 0.25); }),
                                       selfTestRate);
            const auto uncorrelated = measure (testNoise (tenSeconds, 0.5f), selfTestRate);

            test.check (same.hasLowCorrelation && std::abs (same.lowCorrelation - 1.0) < 1.0e-12, "低频相关度 左右相同",
                        "ρ = " + fixed (same.lowCorrelation, 12) + "（期望 1）");
            test.check (inverted.hasLowCorrelation && std::abs (inverted.lowCorrelation + 1.0) < 1.0e-12, "低频相关度 左右反相",
                        "ρ = " + fixed (inverted.lowCorrelation, 12) + "（期望 -1）");
            test.check (half.hasLowCorrelation && std::abs (half.lowCorrelation - 0.8) < 1.0e-12, "低频相关度 右 = 0.5 × 左",
                        "ρ = " + fixed (half.lowCorrelation, 12) + "（期望 2g / (1 + g²) = 0.8）");
            test.check (uncorrelated.hasLowCorrelation && std::abs (uncorrelated.lowCorrelation) < 0.1, "低频相关度 不相关噪声",
                        "ρ = " + fixed (uncorrelated.lowCorrelation, 4) + "（期望 |ρ| < 0.1）");
        }

        // ---- 真次声占比（M 的 16 Hz 8 阶 Butterworth 低通能量 ÷ M 全频能量）----
        // 左右都是 0.5·sin(10 Hz) + 0.5·sin(1 kHz)：两个正弦功率相等，10 s 都是整数个周期（交叉项为 0）；10 Hz 过低通的
        // 功率增益 1 / (1 + (10/16)^16) ≈ 0.99947，1 kHz 漏过可忽略 → 期望 10·log10(0.5 × 0.99947) ≈ -3.013 dB。
        // 容差 0.05 dB：滤波器起振只占开头约 0.2 s 的一小部分（8 阶 16 Hz 余振比 4 阶 30 Hz 长），双线性变换弯折可忽略。
        {
            const auto mix = [] (int i) { return testSine (10.0, i, 0.5) + testSine (1000.0, i, 0.5); };
            const auto sub = measure (testSignal (tenSeconds, mix, mix), selfTestRate);
            const auto expected = 10.0 * std::log10 (0.5 / (1.0 + std::pow (10.0 / 16.0, 16.0)));

            test.check (sub.hasSubShare && std::abs (sub.subShareDb - expected) < 0.05, "真次声占比 10 Hz + 1 kHz",
                        fixed (sub.subShareDb, 3) + " dB（期望 " + fixed (expected, 3) + " ± 0.05 dB）");
        }

        // ---- 整体响度与真峰值（插件自己的表与检测器）----
        // 响度：左右都是 1 kHz、幅度 0.1 的正弦。BS.1770 规定 997 Hz / 0 dBFS 正弦放一个声道读 -3.01 LUFS；两个声道 +3.01，
        // 幅度 -20 dB → -20.0 LUFS（1 kHz 与 997 Hz 的 K 加权差 < 0.01 dB）；容差 0.1 LU（表的直方图一格 0.1 LU）。
        // 真峰值：幅度 0.5 → 20·log10(0.5) = -6.02 dBTP；48 kHz 采 1 kHz 正弦，8× 插值后离真实峰值 < 0.01 dB，容差 0.1 dB。
        {
            const auto tone = [] (int i) { return testSine (1000.0, i, 0.1); };
            const auto level = measure (testSignal (tenSeconds, tone, tone), selfTestRate);

            test.check (level.hasLoudness && std::abs (level.integratedLufs + 20.0) < 0.1, "整体响度 1 kHz 两声道 -20 dBFS",
                        fixed (level.integratedLufs, 3) + " LUFS（期望 -20.0 ± 0.1）");

            const auto loudTone = [] (int i) { return testSine (1000.0, i, 0.5); };
            const auto peak = measure (testSignal (static_cast<int> (selfTestRate), loudTone, loudTone), selfTestRate);
            const auto expectedPeak = 20.0 * std::log10 (0.5);

            test.check (peak.hasTruePeak && std::abs (peak.truePeakDbtp - expectedPeak) < 0.1, "真峰值 1 kHz 幅度 0.5",
                        fixed (peak.truePeakDbtp, 3) + " dBTP（期望 " + fixed (expectedPeak, 3) + " ± 0.1）");
        }

        // ---- 哈希：逐位相同 → 同哈希；改一个样本的最低位 → 哈希变 ----
        {
            const auto original = testNoise (4800, 0.5f);
            auto copy = original;   // AudioBuffer 拷贝是深拷贝
            const auto equal = hashAudio (original) == hashAudio (copy);
            copy.setSample (1, 1234, std::nextafter (copy.getSample (1, 1234), 1.0f));
            const auto differs = hashAudio (original) != hashAudio (copy);

            test.check (equal && differs, "成品哈希",
                        std::string (equal ? "相同音频哈希相同" : "相同音频哈希却不同")
                        + (differs ? "；改一个样本的最低位后哈希变了" : "；改了一个样本哈希却没变"));
        }

        // ---- 文件名与 GLAZE 写法（README、版本档里引用的约定）----
        {
            Combo combo;
            combo.glaze = 35.0f;
            const auto first = outputFileName ("Song A", combo);
            combo.glaze = 12.5f;
            combo.clay = ClayChoice::off;
            combo.style = static_cast<int> (celadon::glaze::Style::warm);
            const auto second = outputFileName ("Song A", combo);

            test.check (first == "Song A__g35_clay-auto_auto.wav" && second == "Song A__g12.5_clay-off_warm.wav"
                            && formatGlaze (0.0f) == "0" && formatGlaze (100.0f) == "100",
                        "成品文件名", first + "、" + second);
        }

        // ---- 整条链：延迟对齐与 GLAZE 0 逐位直通 ----
        // 两路不相关白噪声（幅度 0.05，峰值 -26 dBFS，远低于默认 Ceiling -1 dBTP，限幅器不动作），GLAZE 0、出厂参数，走与真歌
        // 完全相同的两遍流程（listenPass → renderPass），块长 512：
        //   - 链延迟 = 282 + Clay 的固定延迟 D + G（ADR-012 线性相位收单 D = 2(L_max − 1)、ADR-014 峰值守护前瞻 G：
        //     48 kHz 为 574 + 528 = 1102 → 1384 样本）；
        //   - 成品长度 = 输入 + filterRingOutSeconds（0.35 s）余振；
        //   - 成品对输入的互相关在滞后 -8 ～ +8 里最大值落在 0（白噪声只在滞后 0 相关）且 > 0.9（出厂参数、GLAZE 0 时整条链近乎
        //     透明：Balance / Color 手动值全 0，只剩隔直、过采样滤波这类带边处理）——证明丢掉开头「链延迟」个样本后逐样本对齐；
        //   - 修坯「自动」与「全关」两组成品逐位相同（设计规格-Clay 第 4 节「逐位直通」：GLAZE 0 时 Clay 自动值为 0）。
        {
            const auto noise = testNoise (static_cast<int> (4.0 * selfTestRate), 0.05f);
            const std::array<ClayChoice, 2> choices { { ClayChoice::automatic, ClayChoice::off } };
            std::array<std::uint64_t, 2> hashes {};
            constexpr int maxLag = 8;

            for (std::size_t k = 0; k < choices.size(); ++k)
            {
                Combo combo;
                combo.glaze = 0.0f;
                combo.clay = choices[k];
                const auto params = makeComboParams (combo);
                const auto listen = listenPass (noise, selfTestRate, 512, params);

                RenderResult render;
                renderPass (noise, selfTestRate, 512, params, listen.state.get(), false, render);
                const juce::AudioBuffer<float> finished (render.io.getArrayOfWritePointers(), 2, render.latencySamples,
                                                         render.outputLength);
                hashes[k] = hashAudio (finished);

                if (k > 0)
                    continue;

                const auto expectedLength = noise.getNumSamples() + static_cast<int> (std::ceil (ringOutSeconds * selfTestRate));
                const auto expectedLatency = 282 + celadon::ClayModule::latencyFor (selfTestRate);
                test.check (render.latencySamples == expectedLatency, "链延迟",
                            std::to_string (render.latencySamples) + " 样本（期望 " + std::to_string (expectedLatency)
                                + "：48 kHz / Lookahead 2 ms 的 282 + Clay 的 D + G）");
                test.check (render.outputLength == expectedLength, "成品长度",
                            std::to_string (render.outputLength) + " 样本（期望 " + std::to_string (expectedLength) + "）");

                auto bestLag = -maxLag - 1;
                auto best = -2.0;

                for (int lag = -maxLag; lag <= maxLag; ++lag)
                {
                    const auto correlation = crossCorrelation (noise, finished, lag, maxLag);

                    if (correlation > best)
                    {
                        best = correlation;
                        bestLag = lag;
                    }
                }

                test.check (bestLag == 0 && best > 0.9, "延迟对齐",
                            "互相关最大在滞后 " + std::to_string (bestLag) + "，值 " + fixed (best, 4) + "（期望滞后 0、值 > 0.9）");
            }

            test.check (hashes[0] == hashes[1], "GLAZE 0 修坯自动与全关逐位相同",
                        hashes[0] == hashes[1] ? std::string ("两组成品哈希相同") : std::string ("两组成品不同"));
        }

        // ---- 同决策重放（ADR-016）：GLAZE 35「自动」走真歌同一套两遍流程，第二遍逐块记下实际参数，再起 GLAZE 0 的链照搬：
        //   - 修坯用「手动」照填自动链的实际值 → 成品应复现自动链：最大样本差 ≤ replayTolerance（与真歌同一判据；这段信号上
        //     实测逐位相同，细节里写明是否逐位）；
        //   - 修坯「关」→ 延迟、成品长度与自动链相同。
        // 信号：两路不相关白噪声（峰值 −26 dBFS）加两声道同相的 8 Hz 正弦（0.1），让自动低切有东西可切；20 s，够聆听（15 s）
        // 学完、方案过渡走到校准，自动增量、校准偏差都在变。细节里报 GLAZE 是否真的动了、低切是否开过（只作说明，不判定）。
        {
            auto signal = testNoise (static_cast<int> (20.0 * selfTestRate), 0.05f);

            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < signal.getNumSamples(); ++i)
                    signal.setSample (ch, i, signal.getSample (ch, i) + testSine (8.0, i, 0.1));

            Combo combo;
            combo.glaze = 35.0f;
            combo.clay = ClayChoice::automatic;
            const auto params = makeComboParams (combo);
            const auto listen = listenPass (signal, selfTestRate, 512, params);

            RenderResult render;
            EffectiveLog effectiveLog;
            renderPass (signal, selfTestRate, 512, params, listen.state.get(), false, render, &effectiveLog);
            const auto& logged = effectiveLog.blocks;
            const juce::AudioBuffer<float> finished (render.io.getArrayOfWritePointers(), 2, render.latencySamples, render.outputLength);
            const auto autoHash = hashAudio (finished);

            const auto expressible = clayExpressibleAsManual (effectiveLog.prepared.clay) && masteringReplayable (effectiveLog.prepared.mastering)
                                  && std::all_of (logged.begin(), logged.end(),
                                                  [] (const celadon::ChainParams& p)
                                                  { return clayExpressibleAsManual (p.clay) && masteringReplayable (p.mastering); });
            const auto lowCutBlocks = std::count_if (logged.begin(), logged.end(),
                                                     [] (const celadon::ChainParams& p) { return p.clay.lowCutHz > 0.0f; });
            const auto automatedBlocks = std::count_if (logged.begin(), logged.end(),
                                                        [&params] (const celadon::ChainParams& p) { return p.limit.gainDb != params.limit.gainDb; });
            // 高级母带（新实例默认：推响不刺「自动」50、目标频谱「自动」35，T-032 R10-01）在 GLAZE 35 下真的交给了模块非零强度的块。
            const auto masteringBlocks = std::count_if (logged.begin(), logged.end(),
                                                        [] (const celadon::ChainParams& p)
                                                        { return p.balance.harshAmount > 0.0f && p.balance.targetAmount > 0.0f; });

            RenderResult copy;
            replayPass (signal, selfTestRate, 512, effectiveLog, ReplayClay::copy, copy);
            const juce::AudioBuffer<float> copyFinished (copy.io.getArrayOfWritePointers(), 2, copy.latencySamples, copy.outputLength);
            const auto difference = copy.latencySamples == render.latencySamples ? maxAbsDifference (copyFinished, finished) : 1.0;
            const auto identical = hashAudio (copyFinished) == autoHash;
            const auto same = expressible && difference <= replayTolerance;

            test.check (same, "同决策重放复现自动链",
                        std::string (! expressible ? "有块的修坯值「手动」表达不了"
                                     : (identical ? "修坯手动照填重放与自动链成品逐位相同"
                                                  : "最大样本差 " + fixed (toDbfs (difference), 1) + " dBFS（允许 ≤ −180 dBFS）"))
                            + "（" + std::to_string (logged.size()) + " 块；Limit Gain 被 GLAZE 改过的块 "
                            + std::to_string (automatedBlocks) + "，自动低切开着的块 " + std::to_string (lowCutBlocks)
                            + "，推响不刺与目标频谱都在起作用的块 " + std::to_string (masteringBlocks) + "）");

            // 阳性对照（T-032 R10-01）：出厂默认的高级母带两项「自动」真的在这条链里起作用——上面的「复现」覆盖了它们。
            test.check (masteringBlocks > 0, "重放对照覆盖高级母带",
                        std::to_string (masteringBlocks) + " 块推响不刺与目标频谱强度都 > 0（出厂默认 GLAZE 35 × 两项自动）");

            // 对照有牙：把每块的 mastering 换回原始设置（T-032 之前 getEffectiveParams() 回写的就是它）再重放——GLAZE 0 的重放链把
            // 两项「自动」算成 0，成品必须与自动链明显不同（最大样本差 > 1e-6）。证明上面的「复现」靠的是实际值回喂，而不是两项没起作用。
            auto rawLog = effectiveLog;
            rawLog.prepared.mastering = params.mastering;

            for (auto& p : rawLog.blocks)
                p.mastering = params.mastering;

            RenderResult raw;
            replayPass (signal, selfTestRate, 512, rawLog, ReplayClay::copy, raw);
            const juce::AudioBuffer<float> rawFinished (raw.io.getArrayOfWritePointers(), 2, raw.latencySamples, raw.outputLength);
            const auto rawDifference = raw.latencySamples == render.latencySamples ? maxAbsDifference (rawFinished, finished) : 1.0;
            test.check (expressible && rawDifference > 1.0e-6, "重放对照抓得住未解析的高级母带",
                        "回喂原始设置时最大样本差 " + fixed (toDbfs (rawDifference), 1) + " dBFS（期望 > −120 dBFS）");

            RenderResult off;
            replayPass (signal, selfTestRate, 512, effectiveLog, ReplayClay::off, off);
            test.check (off.latencySamples == render.latencySamples && off.outputLength == render.outputLength && ! off.latencyChanged,
                        "重放全关的延迟与长度",
                        std::to_string (off.latencySamples) + " / " + std::to_string (off.outputLength) + " 样本（自动链 "
                            + std::to_string (render.latencySamples) + " / " + std::to_string (render.outputLength) + "）");
        }

        if (test.failed == 0)
            std::cerr << "自检全部通过\n";
        else
            std::cerr << "自检有 " << test.failed << " 项失败\n";

        return test.failed == 0 ? 0 : 1;
    }
}

int main (int argc, char* argv[])
{
    // FTZ / DAZ（T-032 R10-06）：插件的音频线程在 ScopedNoDenormals 下跑整条链（PluginProcessor::processBuses），这里对整个进程
    // 设一次——聆听、成品、重放三遍，自检，以及用插件自己的表做的测量都在同一数值环境里（单线程，一次就够）。只影响 < 1.2e-38
    //（约 −758 dBFS）的非规格化数：成品只在歌尾那个量级可能与改动前不同，指标不变。
    juce::ScopedNoDenormals noDenormals;

    Options options;
    std::string error;

    switch (parseArguments (argc, argv, options, error))
    {
        case ParseResult::help:
            printUsage();
            return 0;

        case ParseResult::selfTest:
            return runSelfTest();

        case ParseResult::invalid:
            std::cerr << "celadon_render：" << error << "\n\n";
            printUsage();
            return 2;

        case ParseResult::run:
            break;
    }

    if (! options.inputDir.isDirectory())
    {
        std::cerr << "celadon_render：--in 不是目录：" << options.inputDir.getFullPathName().toStdString() << "\n";
        return 2;
    }

    if (const auto created = options.outputDir.createDirectory(); created.failed())
    {
        std::cerr << "celadon_render：建不了输出目录 " << options.outputDir.getFullPathName().toStdString() << "："
                  << created.getErrorMessage().toStdString() << "\n";
        return 1;
    }

    const auto songs = findSongs (options.inputDir);

    if (songs.empty())
    {
        std::cerr << "celadon_render：" << options.inputDir.getFullPathName().toStdString()
                  << " 里没有 wav / aiff / flac 文件（不进子目录）\n";
        return 1;
    }

    const auto combos = makeCombos (options);
    const auto generatedAt = juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H:%M:%S").toStdString();
    const auto metricsMd = options.outputDir.getChildFile ("metrics.md");
    const auto metricsCsv = options.outputDir.getChildFile ("metrics.csv");

    std::cerr << "celadon_render：" << songs.size() << " 首歌 × " << combos.size() << " 组（" << gridText (options)
              << "），宿主块长 " << options.blockSize << "，对齐到 " << fixed (options.matchLufs, 1) << " LUFS\n"
              << "输出目录：" << options.outputDir.getFullPathName().toStdString() << "\n";

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    std::vector<SongReport> reports;
    int failures = 0, written = 0;
    const auto started = nowSeconds();

    for (std::size_t index = 0; index < songs.size(); ++index)
    {
        const auto& songFile = songs[index];

        SongReport report;
        report.name = songFile.name;
        report.sourceFile = songFile.file.getFileName().toStdString();
        std::cerr << "[" << (index + 1) << "/" << songs.size() << "] " << report.sourceFile;

        Song song;

        if (loadSong (formats, songFile.file, song, error))
        {
            report.loaded = true;
            report.sampleRate = song.sampleRate;
            report.sourceChannels = song.sourceChannels;
            report.durationSeconds = static_cast<double> (song.audio.getNumSamples()) / song.sampleRate;
            std::cerr << "：" << fixed (song.sampleRate, 0) << " Hz，" << channelText (song.sourceChannels) << "，"
                      << formatDuration (report.durationSeconds) << "\n";

            if (song.nonFiniteSamples > 0)
            {
                // T-032 R10-08：链自己不设输入守卫（插件在处理器层整块清零），读文件时已逐样本清成 0；这里在 stderr 报一次。
                report.notes.push_back ("输入里有 " + std::to_string (song.nonFiniteSamples) + " 个 NaN / Inf 样本，已按 0 处理");
                std::cerr << "  注意：输入里有 " << song.nonFiniteSamples << " 个 NaN / Inf 样本，已按 0 处理（插件在处理器层把含非有限值的"
                             "整块清零，这里逐样本清）\n";
            }

            processSong (options, combos, song, report, failures, written);
            checkGlazeZeroPairs (report);
        }
        else
        {
            std::cerr << "：" << error << "，跳过\n";
            report.notes.push_back ("读不了：" + error);
            ++failures;
        }

        reports.push_back (std::move (report));

        // 每首歌做完就重写一次指标表：中途中断也留下已完成的部分。
        if (! writeText (metricsMd, makeMarkdown (options, combos, reports, generatedAt))
            || ! writeText (metricsCsv, makeCsv (reports)))
        {
            std::cerr << "celadon_render：写不了指标表 " << metricsMd.getFullPathName().toStdString() << "\n";
            return 1;
        }
    }

    std::cerr << "完成：" << written << " 个成品，" << failures << " 处失败，用时 " << fixed (nowSeconds() - started, 1)
              << " s；指标表 " << metricsMd.getFullPathName().toStdString() << "、metrics.csv\n";
    return failures > 0 ? 1 : 0;
}
