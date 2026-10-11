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
                         [--genre general[,deep,tech,melodic]] [--delivery standard[,streaming,club]]
                         [--bpm 124 [--meter 4/4] [--listen-from-bar 1[,33]]] [--segments-csv]
                         [--live-trace [--live-only] [--live-constants 键=值,…] [--live-model fourOnFloor|loudness|passive] [--live-tag 名字]]
                         [--replay-override color.heat=0]   （T-096d：同决策重放另加一行「修坯照填 + 覆盖」，差信号写 heat_ab.csv）
          celadon_render --selftest   （指标算法、文件名、延迟对齐、GLAZE 0 逐位直通、同决策重放的阳性对照；render-linux.sh 渲染前先跑）
          celadon_render --selftest-house   （T-038：模拟走带、分段统计、曲风 / 交付 / 走带穿到链上的阳性对照）
          celadon_render --selftest-live    （T-048：实时画像追踪的阳性对照——段首尾相接、块长无关、与 GLAZE 无关、不改链的输出）

    T-038（曲风与交付，设计规格-曲风与交付、设计规格-曲风-House学习方式）：曲风 / 交付 / 聆听起点是网格的另三维；--bpm 模拟宿主走带
    （每块给速度、PPQ、上一小节起点、拍号）；--segments-csv 逐段（一小节或 2 秒）写输入端与成品的段响度、低频占比、限幅 GR（门控标定、
    House 门槛 tools/render/house_gate.py）。都不给时与改动前逐位相同：HostInfo、网格、文件名、metrics.csv 一字不差。

    T-048（实时 GLAZE 阶段 1，设计规格-实时GLAZE-主段与增益 第 7 节「因果重放」）：--live-trace 给每一遍新建的链接上实时画像 LiveProfile
    （分析器实时模式 → 实时段落账 → LiveProfile），逐段写追踪 live/<标签>/（见「T-048 实时画像追踪」一节与 README）；--live-only 只跑第一遍
    （最快的追踪跑法）。不给这些选项时一切输出与改动前逐字节相同；给了也只多写追踪文件与 metrics.md 的说明（链的输出逐位不变）。

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
        真次声（< 16 Hz，8 阶）能量占比、Clay 四个实际值、反相提示 / 宽度保护时长占比、GLAZE 状态；每首另有一行「原始输入」作对照。
        每做完一首歌就重写一次：中途中断也留下已完成的部分。
      - GLAZE 0 时修坯「自动」与「全关」按规格逐位相同（设计规格-Clay 第 4 节「逐位直通」）：两组都在网格里时按对齐前成品的
        哈希核对，结果写进 metrics.md 的备注——真歌上的阳性对照。
      - 同决策重放（ADR-016，--no-replay 关掉）：每组 GLAZE > 0 的修坯「自动」之后，另起 GLAZE 0 的链逐块照搬它实际交给模块的
        参数、修坯换成「关」，多出一行 / 一个成品 clay-replay-off（修坯门槛拿它与「自动」比：两条链决策相同，差别只剩修坯）；
        同时做阳性对照（修坯「手动」照填自动链实际值重放，应复现自动链：最大样本差 ≤ −180 dBFS，不要求逐位），结论在 metrics.csv 的 replay_check。
    --selftest：合成信号上的阳性对照（相关度 / 真次声 / 响度 / 真峰值的解析值、文件名、链延迟（282 + Clay 的 D + G）与逐样本对齐、
    GLAZE 0 逐位直通、同决策重放复现（最大样本差 ≤ −180 dBFS）），不读歌；这个工具不在 celadon_tests 里，render-linux.sh 渲染真歌前先跑它。
    进度、警告与错误都写 stderr；返回 0 = 全部成功，1 = 有文件读 / 写失败（或自检失败），2 = 参数错误。
    单线程：一次只跑一条链。DSP 代码没有按「多条链并发」验证过，测试服务器也是共用的，不宜一次占满多核。

  ==============================================================================
*/

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>

#include "../../Source/ParameterDefaults.h"
#include "../../Source/dsp/BalanceModule.h"
#include "../../Source/dsp/CeladonChain.h"
#include "../../Source/dsp/ChainParams.h"
#include "../../Source/dsp/GlazeConstants.h"
#include "../../Source/dsp/GlazeTypes.h"
#include "../../Source/dsp/ClayGuard.h"
#include "../../Source/dsp/ClayModule.h"
#include "../../Source/dsp/LiveProfile.h"
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
#include <type_traits>
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

    // T-085：仅命令行测量覆盖；未给选项时不接触链的出厂常数。
    double measurementStreamingTarget = std::numeric_limits<double>::quiet_NaN();

    std::unique_ptr<celadon::CeladonChain> makeRenderChain()
    {
        auto chain = std::make_unique<celadon::CeladonChain>();
        if (std::isfinite (measurementStreamingTarget))
        {
            auto constants = celadon::glaze::factoryConstants();
            auto& delivery = constants.delivery;
            const auto delta = static_cast<float> (measurementStreamingTarget + 10.0);
            delivery.streamingTargetLufs += delta;
            delivery.streamingMinLufs += delta;
            delivery.streamingMaxLufs += delta;
            chain->getGlazeController().setConstantsForTesting (constants);
        }
        return chain;
    }

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

    // FNV-1a（64 位）的两个常数：成品哈希（hashAudio）与实时画像追踪的位级摘要（BitDigest）共用。
    constexpr std::uint64_t fnvOffsetBasis = 14695981039346656037ull;
    constexpr std::uint64_t fnvPrime = 1099511628211ull;

    // 命令行与文件的合理范围。采样率超出 8 k ～ 768 kHz 多半是文件头坏了；块长上限只是防手误。
    constexpr double minSampleRate = 8000.0;
    constexpr double maxSampleRate = 768000.0;
    constexpr int maxBlockSize = 65536;
    constexpr double minMatchLufs = -60.0;
    constexpr double maxMatchLufs = 0.0;

    // 风格：下标 = glaze::Style（= 参数 glaze_style 的选项顺序）。命令行与文件名用英文，表里用界面上的中文。
    constexpr std::array<const char*, 4> styleIds   { { "auto", "warm", "punchy", "clean" } };
    constexpr std::array<const char*, 4> styleNames { { "自动", "温润", "冲击", "通透" } };

    // T-038 曲风与交付：下标 = glaze::Genre / glaze::Delivery（= 参数 glaze_genre / glaze_delivery 的选项顺序，设计规格-曲风与交付 第 2 节）。
    constexpr std::array<const char*, 4> genreIds      { { "general", "deep", "tech", "melodic" } };
    constexpr std::array<const char*, 4> genreNames    { { "通用", "Deep House", "Tech House", "Melodic-Progressive" } };
    constexpr std::array<const char*, 3> deliveryIds   { { "standard", "streaming", "club" } };
    constexpr std::array<const char*, 3> deliveryNames { { "标准", "流媒体", "俱乐部" } };
    static_assert (genreIds.size() == (std::size_t) celadon::glaze::numGenres && deliveryIds.size() == (std::size_t) celadon::glaze::numDeliveries,
                   "命令行的曲风 / 交付取值要与 GlazeTypes.h 的枚举一一对应");

    // 模拟宿主走带（--bpm）：速度与拍号的合理范围（宿主常见上限 999 BPM；拍号分母只认 2 的幂）。
    constexpr double minTransportBpm = 20.0;
    constexpr double maxTransportBpm = 999.0;
    constexpr int maxMeterNumerator = 32;

    // T-048 实时画像追踪：主段模型的命令行名（下标 = glaze::MainSectionModel，= 枚举名）、标签（输出分组 live/<标签>/）的默认值与长度上限。
    constexpr std::array<const char*, 3> liveModelIds { { "fourOnFloor", "loudness", "passive" } };
    static_assert ((int) celadon::glaze::MainSectionModel::fourOnFloor == 0 && (int) celadon::glaze::MainSectionModel::loudness == 1
                       && (int) celadon::glaze::MainSectionModel::passive == 2,
                   "liveModelIds 的下标要与 MainSectionModel 一一对应");
    constexpr const char* defaultLiveTag = "default";
    constexpr std::size_t maxLiveTagLength = 64;

    // T-096d 重放覆盖（--replay-override，见「T-096d 重放覆盖的 A/B 对照」一节）：同决策重放时能改写的参数白名单，集中在这一张表——
    // 别的线要覆盖别的键，就在这里加一行（键 + 短名 + 取值范围 + 读 / 写这个字段的两个函数），解析、行标签、逐字段核对都跟着走。
    //   - key  = ChainParams 里的成员路径（与 visitChainParamFields 报的字段名同一写法，逐字段核对靠它认出哪一项是覆盖的）；
    //   - tag  = 行标签 / 成品文件名里的短名（clay-replay-<tag><值>，color.heat=0 → clay-replay-heat0）；
    //   - 取值范围 = 模块 setParams 的钳制范围：越界直接报错，不让模块悄悄钳成别的值（ColorModule::setParams：Heat 0 ～ 100）。
    struct ReplayOverrideKey
    {
        const char* key;
        const char* tag;
        double minValue, maxValue;
        float (*get) (const celadon::ChainParams&) noexcept;
        void (*set) (celadon::ChainParams&, float) noexcept;
    };

    float getColorHeat (const celadon::ChainParams& p) noexcept          { return p.color.heat; }
    void setColorHeat (celadon::ChainParams& p, float value) noexcept    { p.color.heat = value; }

    constexpr std::array<ReplayOverrideKey, 1> replayOverrideKeys { { { "color.heat", "heat", 0.0, 100.0, getColorHeat, setColorHeat } } };

    // 解析后的一项覆盖：键在 replayOverrideKeys 里的下标 + 值。
    struct ReplayOverride
    {
        std::size_t key = 0;
        float value = 0.0f;
    };

    using ReplayOverrides = std::vector<ReplayOverride>;

    // 覆盖行的修坯标识（clay- 之后那段，如 replay-heat0）与覆盖原文（color.heat=0）：main 解析完设一次（见 main），之后只读。
    // 放成全局是因为 clayId / statusText 只拿得到 ClayChoice / Row；没给 --replay-override 时不会有覆盖行，这两个值不被读。
    struct ReplayOverrideNames
    {
        std::string id = "replay-override";
        std::string text;
    };

    ReplayOverrideNames replayOverrideNames;

    //==============================================================================
    // 设置网格

    // 修坯的对照：自动 = 三态默认（三项都「自动」）；全关 = 三项都「关」（ADR-009 三态）；
    // lowcut / mono / width = 只有这一项「自动」、另两项「关」（拆分诊断：是哪一项改变了成品）。
    // ADR-015 起自动收单恒为 0：mono 这一组的成品与全关逐位相同，只多记反相提示时长（phase_guard_pct 只在收单「自动」时计）。
    // replayOff 不是网格选项：每组 GLAZE > 0 的「自动」之后自动加一行「同决策重放 · 修坯关」（ADR-016，见 replayPass）。
    // replayOverride 也不是：给了 --replay-override 时再加一行「同决策重放 · 修坯照填 + 覆盖」（T-096d，标识见 replayOverrideNames）。
    enum class ClayChoice { automatic, off, lowCutOnly, monoOnly, widthOnly, replayOff, replayOverride };

    struct Options
    {
        juce::File inputDir, outputDir;
        std::vector<float> glaze { 0.0f, 35.0f, 70.0f };
        std::vector<ClayChoice> clay { ClayChoice::automatic, ClayChoice::off };
        std::vector<int> styles { static_cast<int> (celadon::glaze::Style::automatic) };
        int blockSize = 512;
        double matchLufs = -16.0;
        bool noAlign = false;
        double streamingTargetLufs = std::numeric_limits<double>::quiet_NaN();
        bool metricsOnly = false;   // --metrics-only：照常渲染、量指标，但不写成品 wav（大批歌做门槛验收时用，省几 GB 磁盘与传输）
        bool replay = true;         // 每组 GLAZE > 0 的「自动」之后加「同决策重放 · 修坯关」一行与阳性对照（ADR-016）；--no-replay 关掉
        bool trace = false;         // --trace：每组设置另写 trace/<成品名>.csv（第二遍逐块：限幅 GR、修坯方案值 / 宽度保护后 / 实际值、
                                    // 反相提示、实时相关度、校准状态）与 trace/<成品名>__listen.csv（第一遍逐块的校准状态），诊断用

        // ---- T-038 曲风与交付（设计规格-曲风与交付、设计规格-曲风-House学习方式）----
        std::vector<int> genres { static_cast<int> (celadon::glaze::Genre::general) };          // --genre：网格的一维（glaze::Genre 下标）
        std::vector<int> deliveries { static_cast<int> (celadon::glaze::Delivery::standard) };  // --delivery：网格的一维（glaze::Delivery 下标）
        double bpm = 0.0;           // --bpm：模拟宿主走带的速度；0 = 不给拍点（HostInfo 与改动前逐位相同）
        int meterNumerator = 4;     // --meter：拍号（只在给了 --bpm 时有意义）
        int meterDenominator = 4;
        std::vector<int> listenFromBars { 1 };   // --listen-from-bar：第一遍（聆听）从第几小节起播（1 = 从头；网格的一维，要 --bpm）
        bool segmentsCsv = false;   // --segments-csv：逐段（有 --bpm 时一段 = 一小节，否则每 2 秒）写输入端 / 成品的统计，见 writeSegments
        bool extended = false;      // 用了上面任一新选项：metrics.csv 末尾追加 T-038 的列、metrics.md 头部多写一行（默认运行的两份表与改动前相同）

        // ---- T-048 实时画像追踪（见「T-048 实时画像追踪」一节；不设 extended：metrics.csv 不因追踪多列）----
        bool liveTrace = false;                        // --live-trace：每一遍的链接上实时画像，逐段写 live/<标签>/
        bool liveOnly = false;                         // --live-only：只跑第一遍（聆听）出追踪，不出成品、不做重放（最快的追踪跑法）
        celadon::glaze::LiveConstants liveConstants;   // --live-constants 覆盖之后的常数（不给 = 出厂值，即 LiveConstants 的默认值）
        std::string liveConstantsText;                 // --live-constants 原文（记进汇总）
        bool liveModelOverride = false;                // --live-model：主段模型不按曲风取（mainSectionModelFor），一律用 liveModel
        celadon::glaze::MainSectionModel liveModel = celadon::glaze::MainSectionModel::fourOnFloor;
        std::string liveTag = defaultLiveTag;          // --live-tag：输出分组 live/<标签>/（扫参时一组常数一个标签）

        // ---- T-096d 重放覆盖（不给时一切输出与改动前逐字节相同：没有覆盖行、不写 heat_ab.csv、metrics.md 头部不多一行）----
        ReplayOverrides replayOverrides;   // --replay-override：每组重放之后另加 clay-replay-<覆盖> 一行，差信号写 heat_ab.csv
    };

    struct Combo
    {
        float glaze = 0.0f;                       // GLAZE 0 ～ 100 %
        ClayChoice clay = ClayChoice::automatic;
        int style = 0;                            // glaze::Style 下标
        int genre = 0;                            // glaze::Genre 下标（T-038）
        int delivery = 0;                         // glaze::Delivery 下标（T-038）
        int listenFromBar = 1;                    // 第一遍从第几小节起播（T-038 House 门槛 ①：从第 1 小节播 vs 从 drop 播）
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
            case ClayChoice::replayOverride: return replayOverrideNames.id.c_str();
        }

        return "auto";
    }

    // 同决策重放的行（全关 / 覆盖，T-096d）：GLAZE 0 的重放链，GLAZE 状态列没有意义。
    bool isReplayChoice (ClayChoice choice) noexcept
    {
        return choice == ClayChoice::replayOff || choice == ClayChoice::replayOverride;
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

        // T-038 的三维只在用了新选项时写（默认运行的表头、stderr 与改动前相同）。
        if (options.extended)
        {
            text += " × 曲风 ";

            for (std::size_t i = 0; i < options.genres.size(); ++i)
                text += (i > 0 ? " / " : "") + std::string (genreIds[static_cast<std::size_t> (options.genres[i])]);

            text += " × 交付 ";

            for (std::size_t i = 0; i < options.deliveries.size(); ++i)
                text += (i > 0 ? " / " : "") + std::string (deliveryIds[static_cast<std::size_t> (options.deliveries[i])]);

            if (options.listenFromBars.size() > 1 || options.listenFromBars.front() != 1)
            {
                text += " × 聆听起点 第 ";

                for (std::size_t i = 0; i < options.listenFromBars.size(); ++i)
                    text += (i > 0 ? " / " : "") + std::to_string (options.listenFromBars[i]);

                text += " 小节";
            }
        }

        return text;
    }

    // T-038：曲风 / 交付 / 聆听起点不是默认值时接在组合名后面（默认组合的标签与文件名与改动前逐字相同）。
    std::string comboSuffix (const Combo& combo, const char* separator)
    {
        std::string text;

        if (combo.genre != static_cast<int> (celadon::glaze::Genre::general))
            text += separator + std::string (genreIds[static_cast<std::size_t> (combo.genre)]);

        if (combo.delivery != static_cast<int> (celadon::glaze::Delivery::standard))
            text += separator + std::string (deliveryIds[static_cast<std::size_t> (combo.delivery)]);

        if (combo.listenFromBar != 1)
            text += separator + std::string ("from-b") + std::to_string (combo.listenFromBar);

        return text;
    }

    // 「g35 clay-auto auto」（separator 分隔三段；T-038 非默认的曲风 / 交付 / 聆听起点接在后面，如「g35 · clay-auto · auto · deep · club」）。
    std::string comboLabel (const Combo& combo, const char* separator)
    {
        return "g" + formatGlaze (combo.glaze) + separator + "clay-" + clayId (combo.clay) + separator
             + styleIds[static_cast<std::size_t> (combo.style)] + comboSuffix (combo, separator);
    }

    // 成品文件名：<歌名>__g<GLAZE>_clay-<auto|off>_<风格>[_<曲风>][_<交付>][_from-b<小节>].wav（后三段只在不是默认值时有）。
    std::string outputFileName (const std::string& song, const Combo& combo)
    {
        return song + "__g" + formatGlaze (combo.glaze) + "_clay-" + clayId (combo.clay) + "_"
             + styleIds[static_cast<std::size_t> (combo.style)] + comboSuffix (combo, "_") + ".wav";
    }

    // 成品名去掉 .wav（逐段统计 segments/<它>.csv 与成品同名，便于对照）。
    std::string withoutWavExtension (const std::string& fileName)
    {
        const std::string extension = ".wav";
        const auto hasExtension = fileName.size() > extension.size()
                               && fileName.compare (fileName.size() - extension.size(), extension.size(), extension) == 0;
        return hasExtension ? fileName.substr (0, fileName.size() - extension.size()) : fileName;
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
                     "                      （ADR-015 起自动收单恒为 0：mono 这一组的成品与 off 逐位相同，只多记反相提示时长）\n"
                     "  --style auto        GLAZE 风格 auto / warm / punchy / clean（可逗号分隔多个），默认 auto\n"
                     "  --block 512         宿主块长（样本，1 ~ 65536），默认 512\n"
                     "  --match-lufs -16    成品按整体响度只乘增益对齐到这个值（-60 ~ 0 LUFS），默认 -16\n"
                     "  --metrics-only      照常渲染并量指标，但不写成品 wav（大批歌做门槛验收时用）\n"
                     "  --trace             另写 trace/<成品名>.csv：第二遍逐块的限幅 GR、修坯方案值 / 宽度保护后 / 实际值、反相提示、\n"
                     "                      实时低频相关度、校准状态；trace/<成品名>__listen.csv：第一遍逐块的校准状态（诊断用）\n"
                     "  --no-replay         不做「同决策重放」（默认每组 GLAZE > 0 的修坯「自动」之后，另起一条 GLAZE 0 的链逐块照搬\n"
                     "                      它实际交给模块的参数、修坯换成「关」，出一行 clay-replay-off，并做阳性对照；ADR-016）\n"
                     "                      （阳性对照的判据：重放与自动链最大样本差 ≤ −180 dBFS，不要求逐位）\n"
                     "  --genre general     曲风 general / deep / tech / melodic（可逗号分隔多个，网格的一维），默认 general（T-038）\n"
                     "  --delivery standard 交付 standard / streaming / club（可逗号分隔多个，网格的一维），默认 standard（T-038）\n"
                     "  --bpm 124           模拟宿主走带：每块给速度、PPQ（从 0 起按样本推进）、上一小节起点与拍号；不给就不给拍点（同改动前）\n"
                     "  --meter 4/4         拍号（要与 --bpm 一起用），默认 4/4\n"
                     "  --listen-from-bar 1 第一遍（聆听）从第几小节起播（1 = 从头；可逗号分隔多个，网格的一维；要 --bpm）：\n"
                     "                      House 门槛「从第 1 小节播 vs 从 drop 播」用，第二遍（成品）照旧从头渲染\n"
                     "  --segments-csv      逐段写统计到 segments/：<歌名>__input.csv（输入端：段响度、低频占比）与 <成品名>.csv\n"
                     "                      （成品同一套 + 逐段限幅 GR）；有 --bpm 时一段 = 一小节，否则每 2 秒一段（T-038 门控标定与 House 门槛）\n"
                     "  --live-trace        T-048：每一遍的链接上实时画像（分析器实时模式），每发布一份画像写一行追踪到\n"
                     "                      live/<标签>/<歌名>__<曲风>.csv，另写 __final.csv 与 __judgements.csv（歌尾窗口里每段的最终判断；\n"
                     "                      同一首歌、曲风、聆听起点、常数只写一份；链的输出逐位不变）\n"
                     "  --live-only         只跑第一遍（聆听）出追踪：每首歌 × 每个曲风 × 每个聆听起点一遍，不出成品、不做重放（最快的跑法）\n"
                     "  --no-align         测量用：写未经整体等响的成品，默认仍等响\n"
                     "  --streaming-target-lufs n  测量用：覆盖流媒体基准目标（-20 ~ -8），上下界同移，Ceiling 不变\n"
                     "  --live-constants k=v,...  覆盖实时画像常数 LiveConstants 的任意字段（如 fourOnFloor.gateWidthLu=3.5,newProgramLu=8,\n"
                     "                      referenceMemorySeconds=240；键名同 GlazeConstants.h，主段模型的字段带 fourOnFloor. / loudness. /\n"
                     "                      passive. 前缀；取值范围是 LiveProfile.h 的公开表）；不认识的键、取值不合法都报错\n"
                     "  --live-model m      主段模型一律用 fourOnFloor / loudness / passive（默认按曲风：House 三类 4/4 型，通用响度型）\n"
                     "  --live-tag 名字     追踪的输出分组 live/<名字>/（字母、数字、. _ -），默认 default；改了常数或模型时必须给\n"
                     "  --replay-override color.heat=0  T-096d：同决策重放时覆盖参数（键=值，逗号分隔；白名单 color.heat，取值 0 ~ 100）：\n"
                     "                      每组 GLAZE > 0 的修坯「自动」之后另加一行 clay-replay-heat0（修坯照填、prepare 前与每一块都只改\n"
                     "                      覆盖的键），与阳性对照 replay-copy 逐样本相减，差信号指标写 heat_ab.csv；不能与 --no-replay、\n"
                     "                      --live-only 一起用（不给时一切输出与改动前相同）\n"
                     "  --selftest          只跑自检（指标算法、文件名、延迟对齐、GLAZE 0 逐位直通的阳性对照），不读歌\n"
                     "  --selftest-house    只跑 T-038 新选项的自检（模拟走带、分段统计、曲风 / 交付穿到链上的阳性对照），不读歌\n"
                     "  --selftest-live     只跑 T-048 实时画像追踪的自检（合成 House 曲上段首尾相接、块长与 GLAZE 无关、不改链的输出），不读歌\n"
                     "读 --in 目录第一层的 wav / aiff / flac，每首歌 × 每组设置写一个 32-bit float wav，另写 metrics.md 与\n"
                     "metrics.csv。进度与错误写 stderr；返回 0 成功、1 有文件读写失败（或自检失败）、2 参数错误。\n"
                     "详见 tools/render/README.md。\n";
    }

    // ---- T-048 --live-constants：LiveConstants 的键表与取值范围 ----
    // 键与合法取值用 LiveProfile.h 的公开表（visitLiveConstants / isInLiveConstantDomain；LiveProfile 的清洗 sanitiseLiveConstants 用的是
    // 同一张表）：那边把坏值悄悄换成出厂值，这里直接报错——扫参时给错的值不能变成「其实跑的是出厂值」。+∞ 只在表里注明「含 ∞」的键上
    // 合法（「不遗忘 / 永不触发」）。键名 = 字段名，主段模型的字段带「模型名.」前缀（共 3 × 9 + 10 = 37 个）。
    using LiveDomain = celadon::glaze::LiveConstantDomain;

    const char* liveDomainText (LiveDomain domain) noexcept
    {
        switch (domain)
        {
            case LiveDomain::finite:            return "有限的数";
            case LiveDomain::positiveFinite:    return "> 0 的有限数";
            case LiveDomain::percentile:        return "(0, 1] 之间的数";
            case LiveDomain::notNan:            return "数（可写 inf / -inf）";
            case LiveDomain::positive:          return "≥ 1 ms 的数（可写 inf）";
            case LiveDomain::unit:              return "[0, 1] 之间的数";
            case LiveDomain::nonNegative:       return "≥ 0 的数（可写 inf）";
            case LiveDomain::nonNegativeFinite: return "≥ 0 的有限数";
        }

        return "数";
    }

    // 解析、汇总（__final.csv）、自检共用：visit (键名, 字段的引用（float& 或 double&）, 取值范围)。
    template <typename Constants, typename Visitor>
    void visitLiveConstantKeys (Constants& c, Visitor&& visit)
    {
        celadon::glaze::visitLiveConstants (c, [&visit] (const char* model, const char* name, auto& field, LiveDomain domain)
        {
            visit (model == nullptr ? std::string (name) : std::string (model) + "." + name, field, domain);
        });
    }

    // 一个常数值：整个字符串得是一个数（strtod，C 区域设置）；收 inf / -inf（由取值范围决定合不合法），不收 NaN 与溢出。
    bool parseLiveNumber (const std::string& text, double& value)
    {
        if (text.empty())
            return false;

        errno = 0;
        char* end = nullptr;
        const auto parsed = std::strtod (text.c_str(), &end);

        if (end == text.c_str() || *end != '\0' || errno == ERANGE || std::isnan (parsed))
            return false;

        value = parsed;
        return true;
    }

    // 「fourOnFloor.gateWidthLu=3.5,newProgramLu=8」→ 改写 constants 里对应的字段。键不认识、少了「=」、值不是数或不在该键的取值范围、
    // 同一个键给了两次，都报错（error 写原因，constants 可能已改了一部分：调用方报错退出，不再用它）。
    bool parseLiveConstants (const std::string& text, celadon::glaze::LiveConstants& constants, std::string& error)
    {
        std::vector<std::string> seen;

        for (const auto& item : splitList (text))
        {
            const auto equals = item.find ('=');

            if (equals == std::string::npos)
            {
                error = "--live-constants 的「" + item + "」要写成 键=值（如 fourOnFloor.gateWidthLu=3.5）";
                return false;
            }

            const auto key = trimmed (item.substr (0, equals));
            const auto valueText = trimmed (item.substr (equals + 1));

            if (std::find (seen.begin(), seen.end(), key) != seen.end())
            {
                error = "--live-constants 里键「" + key + "」给了两次";
                return false;
            }

            seen.push_back (key);
            double parsed = 0.0;
            const auto isNumber = parseLiveNumber (valueText, parsed);
            auto found = false, accepted = false;
            auto domain = LiveDomain::finite;

            visitLiveConstantKeys (constants, [&] (const std::string& name, auto& field, LiveDomain fieldDomain)
            {
                if (name != key)
                    return;

                using Field = std::remove_reference_t<decltype (field)>;
                const auto value = static_cast<Field> (parsed);   // float 字段：先舍入成 float 再查范围（1e40 → inf）
                found = true;
                domain = fieldDomain;
                accepted = isNumber && celadon::glaze::isInLiveConstantDomain (static_cast<double> (value), fieldDomain);

                if (accepted)
                    field = value;
            });

            if (! found)
            {
                error = "--live-constants 不认识键「" + key + "」（键名同 GlazeConstants.h 的 LiveConstants，主段模型的字段带 fourOnFloor. / "
                        "loudness. / passive. 前缀；全部键见 tools/render/README.md）";
                return false;
            }

            if (! accepted)
            {
                error = "--live-constants 的 " + key + " 要 " + liveDomainText (domain) + "，不认「" + valueText + "」";
                return false;
            }
        }

        return true;
    }

    // ---- T-096d --replay-override：键表见文件开头的 replayOverrideKeys ----

    std::string knownReplayOverrideKeys()
    {
        std::string known;

        for (std::size_t k = 0; k < replayOverrideKeys.size(); ++k)
            known += (k > 0 ? " / " : "") + std::string (replayOverrideKeys[k].key);

        return known;
    }

    // 「color.heat=0」→ overrides。键不在白名单、少了「=」、值不是有限的数或不在该键的取值范围、同一个键给了两次，都报错
    //（写法照 --live-constants；error 写原因，调用方报错退出）。
    bool parseReplayOverrides (const std::string& text, ReplayOverrides& overrides, std::string& error)
    {
        overrides.clear();

        for (const auto& item : splitList (text))
        {
            const auto equals = item.find ('=');

            if (equals == std::string::npos)
            {
                error = "--replay-override 的「" + item + "」要写成 键=值（如 color.heat=0）";
                return false;
            }

            const auto key = trimmed (item.substr (0, equals));
            const auto valueText = trimmed (item.substr (equals + 1));
            const auto found = std::find_if (replayOverrideKeys.begin(), replayOverrideKeys.end(),
                                             [&key] (const ReplayOverrideKey& known) { return key == known.key; });

            if (found == replayOverrideKeys.end())
            {
                error = "--replay-override 不认识键「" + key + "」（白名单：" + knownReplayOverrideKeys() + "）";
                return false;
            }

            const auto index = static_cast<std::size_t> (found - replayOverrideKeys.begin());

            if (std::any_of (overrides.begin(), overrides.end(), [index] (const ReplayOverride& o) { return o.key == index; }))
            {
                error = "--replay-override 里键「" + key + "」给了两次";
                return false;
            }

            double parsed = 0.0;

            if (! parseNumber (valueText, parsed) || parsed < found->minValue || parsed > found->maxValue)
            {
                error = "--replay-override 的 " + key + " 要 " + fixed (found->minValue, 0) + " ~ " + fixed (found->maxValue, 0)
                      + " 之间的数，不认「" + valueText + "」";
                return false;
            }

            overrides.push_back ({ index, static_cast<float> (parsed) });
        }

        return true;
    }

    // 覆盖行的修坯标识：replay-<短名><值>，多个键用「-」连（color.heat=0 → replay-heat0）。值的写法同 GLAZE（整数写整数，最多两位小数）。
    std::string replayOverrideId (const ReplayOverrides& overrides)
    {
        std::string id = "replay";

        for (const auto& o : overrides)
            id += "-" + std::string (replayOverrideKeys[o.key].tag) + formatGlaze (o.value);

        return id;
    }

    // 覆盖原文的规范写法（metrics.md、heat_ab.csv 的 override 列）：键=值，逗号分隔。
    std::string replayOverrideText (const ReplayOverrides& overrides)
    {
        std::string text;

        for (std::size_t k = 0; k < overrides.size(); ++k)
            text += (k > 0 ? "," : "") + std::string (replayOverrideKeys[overrides[k].key].key) + "=" + formatGlaze (overrides[k].value);

        return text;
    }

    // 标签只用作目录名：字母、数字、. _ -，不以「.」开头（免得藏起来或跳出 live/），至多 64 个字符。
    bool validLiveTag (const std::string& tag) noexcept
    {
        if (tag.empty() || tag.size() > maxLiveTagLength || tag.front() == '.')
            return false;

        return std::all_of (tag.begin(), tag.end(), [] (char c)
        {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        });
    }

    enum class ParseResult { run, help, selfTest, selfTestHouse, selfTestLive, invalid };

    // 「曲风 / 交付」这类按名字选的一维：逗号分隔、去重，按 ids 的下标存。认不出时把原因写进 error。
    template <std::size_t N>
    bool parseNamedList (const std::string& option, const std::string& value, const std::array<const char*, N>& ids,
                         std::vector<int>& out, std::string& error)
    {
        out.clear();

        for (const auto& item : splitList (value))
        {
            const auto found = std::find_if (ids.begin(), ids.end(), [&item] (const char* id) { return item == id; });

            if (found == ids.end())
            {
                std::string known;

                for (std::size_t k = 0; k < N; ++k)
                    known += (k > 0 ? " / " : "") + std::string (ids[k]);

                error = option + " 只认 " + known + "，不认「" + item + "」";
                return false;
            }

            const auto index = static_cast<int> (found - ids.begin());

            if (std::find (out.begin(), out.end(), index) == out.end())
                out.push_back (index);
        }

        return true;
    }

    // 解析失败时把原因写进 error（中文，直接给用户看）。
    ParseResult parseArguments (int argc, char* argv[], Options& options, std::string& error)
    {
        bool haveInput = false, haveOutput = false, haveMeter = false, haveListenFrom = false;
        bool haveLiveConstants = false, haveLiveTag = false;   // T-048：这两个与 --live-model、--live-only 都要 --live-trace
        bool haveReplayOverride = false;                        // T-096d：只能给一次，要有同决策重放可覆盖

        for (int i = 1; i < argc; ++i)
        {
            const std::string name (argv[i]);

            if (name == "-h" || name == "--help")
                return ParseResult::help;

            if (name == "--selftest")
                return ParseResult::selfTest;

            if (name == "--selftest-house")
                return ParseResult::selfTestHouse;

            if (name == "--selftest-live")
                return ParseResult::selfTestLive;

            if (name == "--trace")
            {
                options.trace = true;
                continue;
            }

            if (name == "--live-trace")
            {
                options.liveTrace = true;
                continue;
            }

            if (name == "--live-only")
            {
                options.liveOnly = true;
                continue;
            }

            if (name == "--segments-csv")
            {
                options.segmentsCsv = true;
                options.extended = true;
                continue;
            }

            if (name == "--no-align")
            {
                options.noAlign = true;
                continue;
            }

            if (name == "--metrics-only")   // 不带取值的运行选项（另外三个是 --trace、--no-replay、--segments-csv）
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
            else if (name == "--streaming-target-lufs")
            {
                double parsed = 0.0;
                if (! parseNumber (value, parsed) || parsed < -20.0 || parsed > -8.0)
                {
                    error = "--streaming-target-lufs 要 -20 ~ -8 之间的数";
                    return ParseResult::invalid;
                }
                options.streamingTargetLufs = parsed;
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
            else if (name == "--genre")
            {
                if (! parseNamedList (name, value, genreIds, options.genres, error))
                    return ParseResult::invalid;

                options.extended = true;
            }
            else if (name == "--delivery")
            {
                if (! parseNamedList (name, value, deliveryIds, options.deliveries, error))
                    return ParseResult::invalid;

                options.extended = true;
            }
            else if (name == "--bpm")
            {
                double parsed = 0.0;

                if (! parseNumber (value, parsed) || parsed < minTransportBpm || parsed > maxTransportBpm)
                {
                    error = "--bpm 要 20 ~ 999 之间的数";
                    return ParseResult::invalid;
                }

                options.bpm = parsed;
                options.extended = true;
            }
            else if (name == "--meter")
            {
                // 「4/4」「3/4」「6/8」：分子 1 ～ 32，分母 1 / 2 / 4 / 8 / 16 / 32（宿主的拍号分母都是 2 的幂）。
                const auto slash = value.find ('/');
                double numerator = 0.0, denominator = 0.0;
                const auto ok = slash != std::string::npos
                             && parseNumber (trimmed (value.substr (0, slash)), numerator)
                             && parseNumber (trimmed (value.substr (slash + 1)), denominator)
                             && numerator >= 1.0 && numerator <= static_cast<double> (maxMeterNumerator) && std::floor (numerator) == numerator
                             && (denominator == 1.0 || denominator == 2.0 || denominator == 4.0 || denominator == 8.0
                                 || denominator == 16.0 || denominator == 32.0);

                if (! ok)
                {
                    error = "--meter 要写成「分子/分母」，分子 1 ~ 32 的整数、分母 1 / 2 / 4 / 8 / 16 / 32（如 4/4、6/8）";
                    return ParseResult::invalid;
                }

                options.meterNumerator = static_cast<int> (numerator);
                options.meterDenominator = static_cast<int> (denominator);
                options.extended = true;
                haveMeter = true;
            }
            else if (name == "--listen-from-bar")
            {
                options.listenFromBars.clear();

                for (const auto& item : splitList (value))
                {
                    double parsed = 0.0;

                    if (! parseNumber (item, parsed) || parsed < 1.0 || parsed > 100000.0 || std::floor (parsed) != parsed)
                    {
                        error = "--listen-from-bar 的「" + item + "」不是 ≥ 1 的整数（小节号从 1 起）";
                        return ParseResult::invalid;
                    }

                    const auto bar = static_cast<int> (parsed);

                    if (std::find (options.listenFromBars.begin(), options.listenFromBars.end(), bar) == options.listenFromBars.end())
                        options.listenFromBars.push_back (bar);
                }

                options.extended = true;
                haveListenFrom = true;
            }
            else if (name == "--live-constants")
            {
                if (haveLiveConstants)
                {
                    error = "--live-constants 只能给一次（多个键写在同一个值里，逗号分隔）";
                    return ParseResult::invalid;
                }

                if (! parseLiveConstants (value, options.liveConstants, error))
                    return ParseResult::invalid;

                options.liveConstantsText = value;
                haveLiveConstants = true;
            }
            else if (name == "--live-model")
            {
                const auto found = std::find_if (liveModelIds.begin(), liveModelIds.end(), [&value] (const char* id) { return value == id; });

                if (found == liveModelIds.end())
                {
                    error = "--live-model 只认 fourOnFloor / loudness / passive，不认「" + value + "」";
                    return ParseResult::invalid;
                }

                options.liveModel = static_cast<celadon::glaze::MainSectionModel> (found - liveModelIds.begin());
                options.liveModelOverride = true;
            }
            else if (name == "--live-tag")
            {
                if (! validLiveTag (value))
                {
                    error = "--live-tag 只用字母、数字、. _ -，不以「.」开头，至多 64 个字符，不认「" + value + "」";
                    return ParseResult::invalid;
                }

                options.liveTag = value;
                haveLiveTag = true;
            }
            else if (name == "--replay-override")
            {
                if (haveReplayOverride)
                {
                    error = "--replay-override 只能给一次（多个键写在同一个值里，逗号分隔）";
                    return ParseResult::invalid;
                }

                if (! parseReplayOverrides (value, options.replayOverrides, error))
                    return ParseResult::invalid;

                haveReplayOverride = true;
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

        // T-048：追踪的几个附属选项离开 --live-trace 没有意义；改了常数或模型却不给标签，追踪会写进 live/default/、与出厂常数的混在一起。
        if (! options.liveTrace && (options.liveOnly || haveLiveConstants || options.liveModelOverride || haveLiveTag))
        {
            error = std::string (options.liveOnly ? "--live-only" : haveLiveConstants ? "--live-constants"
                                 : options.liveModelOverride ? "--live-model" : "--live-tag")
                  + " 要与 --live-trace 一起用";
            return ParseResult::invalid;
        }

        if ((haveLiveConstants || options.liveModelOverride) && ! haveLiveTag)
        {
            error = "改了实时画像常数（--live-constants）或主段模型（--live-model）时要给 --live-tag，免得与出厂常数的追踪混在 live/default/ 里";
            return ParseResult::invalid;
        }

        // T-096d：覆盖只作用于同决策重放——关了重放（--no-replay）、只跑第一遍（--live-only）、网格里没有 GLAZE > 0 的修坯「自动」组时
        // 一行都不会多，报错免得白跑一批。
        if (haveReplayOverride)
        {
            const auto hasReplayCombo = std::any_of (options.glaze.begin(), options.glaze.end(), [] (float percent) { return percent > 0.0f; })
                                     && std::find (options.clay.begin(), options.clay.end(), ClayChoice::automatic) != options.clay.end();

            if (! options.replay || options.liveOnly || ! hasReplayCombo)
            {
                error = std::string ("--replay-override 只作用于同决策重放：")
                      + (! options.replay ? "不能与 --no-replay 一起用"
                         : options.liveOnly ? "不能与 --live-only 一起用（只跑第一遍，没有重放）"
                                            : "--glaze 里要有 > 0 的值、--clay 里要有 auto（只有 GLAZE > 0 的修坯「自动」组之后才有重放）");
                return ParseResult::invalid;
            }
        }

        // 拍号与「从第几小节起播」都按模拟走带的小节网格算：没有速度就没有小节。
        if ((haveMeter || haveListenFrom) && ! (options.bpm > 0.0))
        {
            error = std::string (haveMeter ? "--meter" : "--listen-from-bar") + " 要与 --bpm 一起用（小节按模拟走带的速度与拍号算）";
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
    // T-038 的曲风、交付、聆听起点排在风格之后（默认各只有一个取值：网格与改动前相同）。
    std::vector<Combo> makeCombos (const Options& options)
    {
        std::vector<Combo> combos;

        for (const auto percent : options.glaze)
            for (const auto choice : options.clay)
                for (const auto style : options.styles)
                    for (const auto genre : options.genres)
                        for (const auto delivery : options.deliveries)
                            for (const auto fromBar : options.listenFromBars)
                                combos.push_back ({ percent, choice, style, genre, delivery, fromBar });

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
        auto hash = fnvOffsetBasis;

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
                    hash *= fnvPrime;
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
        p.glazeGenre = combo.genre;         // T-038（默认 0 / 0 = 通用 / 标准 = 出厂值，与改动前相同）
        p.glazeDelivery = combo.delivery;

        // 「全关」= 三项都选「关」（ADR-009：永远 0，GLAZE 多大都不处理）；单项对照 = 只留这一项「自动」。
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

    // T-038 模拟宿主走带（--bpm；设计规格-曲风-House学习方式 第 2.1、8 节要用的拍点）：恒定速度、样本 0 = PPQ 0（离线导出从工程开头起播），
    // PPQ 按样本位置算（与宿主分块无关：同一位置在任何块长下逐位相同）。enabled = false 时 makeHostInfo 与改动前逐位相同。
    struct Transport
    {
        bool enabled = false;
        double bpm = 0.0;
        int numerator = 4, denominator = 4;
        double sampleRate = 0.0;

        // 每小节多少个四分音符（PPQ 按四分音符计，宿主口径）：4/4 → 4、3/4 → 3、6/8 → 3。
        double quartersPerBar() const noexcept   { return static_cast<double> (numerator) * 4.0 / static_cast<double> (denominator); }
        double samplesPerBar() const noexcept    { return quartersPerBar() * 60.0 / bpm * sampleRate; }
        double ppqAt (std::int64_t position) const noexcept   { return static_cast<double> (position) * bpm / (60.0 * sampleRate); }

        // 第 bar 小节（从 1 起）的起点样本：锚点 0 + round((bar − 1) × 每小节样本数)（与 SegmentGrid 同一取整）。
        std::int64_t barStartSample (int bar) const noexcept
        {
            return std::llround (static_cast<double> (bar - 1) * samplesPerBar());
        }
    };

    Transport makeTransport (const Options& options, double sampleRate) noexcept
    {
        Transport transport;
        transport.enabled = options.bpm > 0.0;
        transport.bpm = options.bpm;
        transport.numerator = options.meterNumerator;
        transport.denominator = options.meterDenominator;
        transport.sampleRate = sampleRate;
        return transport;
    }

    // 分段网格（T-038 --segments-csv 的逐段统计与 T-048 实时画像追踪的小节号共用）：有 --bpm 时一段 = 一小节，第 k 段（从 0 起）的起点 =
    // round(k × 每小节样本数)（锚点 0、恒定速度：规格第 2.1 节「锚点样本 + round(n × 每小节样本数)」）；没给 --bpm 时每
    // HouseConstants::fallbackSegmentSeconds（出厂 2 s）一段，按样本墙钟算（插件分 2 秒段时按有效音频样本推进，离线标定只需要大致的段，
    // 这一点与插件不同）。
    struct SegmentGrid
    {
        bool bars = false;
        double samplesPerSegment = 1.0;

        std::int64_t startOf (std::int64_t k) const noexcept   { return std::llround (static_cast<double> (k) * samplesPerSegment); }

        // 样本所在的段（从 0 起）。先按除法估，再按 startOf 的取整修正一步：边界样本永远归后一段。
        std::int64_t indexOf (std::int64_t sample) const noexcept
        {
            auto k = static_cast<std::int64_t> (std::floor (static_cast<double> (std::max<std::int64_t> (0, sample)) / samplesPerSegment));

            while (k > 0 && startOf (k) > sample)
                --k;

            while (startOf (k + 1) <= sample)
                ++k;

            return k;
        }
    };

    SegmentGrid makeSegmentGrid (const Transport& transport, double sampleRate)
    {
        SegmentGrid grid;
        grid.bars = transport.enabled;
        grid.samplesPerSegment = std::max (1.0, transport.enabled ? transport.samplesPerBar()
                                                                  : celadon::glaze::factoryConstants().house.fallbackSegmentSeconds * sampleRate);
        return grid;
    }

    celadon::glaze::HostInfo makeHostInfo (std::int64_t position, const Transport& transport = Transport {}) noexcept
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

        // T-038：给了 --bpm 才填拍点（同 PluginProcessor::readHostInfo 的各项：速度、PPQ、上一小节起点、拍号，都是本块起点的值）。
        if (transport.enabled)
        {
            const auto quartersPerBar = transport.quartersPerBar();
            info.hasTempo = true;
            info.bpm = transport.bpm;
            info.hasPpq = true;
            info.ppqPosition = transport.ppqAt (position);
            info.hasBarStart = true;
            info.ppqLastBarStart = std::floor (info.ppqPosition / quartersPerBar) * quartersPerBar;
            info.hasTimeSignature = true;
            info.timeSignatureNumerator = transport.numerator;
            info.timeSignatureDenominator = transport.denominator;
        }

        return info;
    }

    //==============================================================================
    // T-048 实时画像追踪（--live-trace / --live-only / --live-constants / --live-model / --live-tag；设计规格-实时GLAZE-主段与增益
    // 第 7 节「因果重放验收」，T-048 阶段 1 BRIEF 第 3 节）。不给这些选项时下面的东西一样都不跑：链、表、文件名与改动前逐字节相同。
    //
    // 每一遍新建的链（聆听 / 成品 / 重放）prepare 之后接一个 LiveTraceSink：分析器打开实时模式，实时段落账每定稿一段就交给它，它入队交给
    // 实时画像 LiveProfile（T-048 阶段 1 修订「回看重判」：每来一段按此刻的参考把窗口里的段全部重判一遍）。分析器每块报实时段落账交齐的
    // 位置（advanceTo，MusicAnalyzer.h「时间轴与 advanceTo」），接收者转给 LiveProfile；追踪器自己不报时间。
    // LiveProfile 在「段尾 + 发布延迟」这一刻发布那一段的画像，并回调接收者（PublicationListener）：接收者把这一份画像与诊断整份拷进定长
    // 环形缓冲（音频线程上被调：只做定长拷贝，不分配、不加锁）。主线程每处理完一块用 readCommits 读新提交，按序号与快照配对，写成 CSV
    // 一行（LiveTracer）。一次 advanceTo 可能发布好几份（大块、补完欠的片），每一份都有自己的快照，所以追踪与宿主块长无关。一遍结束时关实时
    // 模式（链把最后一块还在 Balance 里的帧补交给实时段落账，再交出歌尾，见 LiveTracer::finish），最后 flush（还没到发布时刻的段按序发布完，
    // 各行仍记各自的发布时刻）。
    //
    // 实时画像只看输入（与 GLAZE、风格、修坯、交付无关），所以同一首歌、同一曲风、同一聆听起点、同一组常数只写一份：第一次跑到的那一遍
    // 聆听（第一遍）写成 live/<标签>/<歌名>__<曲风>[_from-b<N>].csv 与 __final.csv；之后的各遍照样追踪、只与写下的那份逐行比较，结论写进
    // live/<标签>/<歌名>__passes.csv、metrics.md 的备注与 stderr。第二遍与重放从不当写下的那份：读档之后分析器不学习，实时段落账交来的段
    // 可能从第一段起就与第一遍不同（差在哪些列见 __passes.csv 的 first_difference_columns），输入尾部还多了延迟与余振的补零。

    const char* segmentModeId (celadon::glaze::SegmentMode mode) noexcept
    {
        switch (mode)
        {
            case celadon::glaze::SegmentMode::none:    return "none";
            case celadon::glaze::SegmentMode::bars:    return "bars";
            case celadon::glaze::SegmentMode::seconds: return "seconds";
        }

        return "none";
    }

    // 这一遍用哪个主段模型：给了 --live-model 就一律用它，否则按曲风（mainSectionModelFor：House 三类 4/4 型、通用响度型）。
    celadon::glaze::MainSectionModel liveModelFor (const Options& options, int genre) noexcept
    {
        return options.liveModelOverride ? options.liveModel
                                         : celadon::glaze::mainSectionModelFor (celadon::glaze::genreFromIndex (genre));
    }

    // 分析器实时段落账的接收者。segmentFinalized / advanceTo 在音频线程上被调：段入队交给实时画像；每发布一份画像（livePublished 回调），
    // 把这一份的画像与诊断整份拷进定长环形缓冲——只有定长拷贝，不分配、不加锁（Snapshot 是可平凡拷贝的定长结构，见 static_assert）。
    // 主线程按提交记录的序号来取（snapshotFor）。段里有、提交记录里没有的三项（epoch、已听、实时试算块数）按到达序号另记一份：新实例里
    // 提交记录的序号就是段的到达序号（每段发布一份、按到达的先后）。
    class LiveTraceSink final : public celadon::glaze::SegmentSink, private celadon::glaze::LiveProfile::PublicationListener
    {
    public:
        static constexpr int capacity = celadon::glaze::LiveProfile::commitCapacity;   // 与提交记录的环形缓冲一样长：一块里最多发布这么多份
        static constexpr int arrivalCapacity = capacity + celadon::glaze::LiveProfile::queueCapacity;   // 交来还没发布的段不超过队列容量

        // 一份发布的读数：画像整份、诊断，加上段里有、提交记录里没有的三项。
        struct Snapshot
        {
            std::uint64_t sequence = 0;     // 第几份发布（从 0 起，= LiveCommitRecord::sequence = 段的到达序号）
            std::uint32_t epoch = 0;        // 段所属的连续时间段（跳播 / 回绕 / 宿主 reset 之后 + 1）
            bool heard = false;             // 段的「已听」（有效音频 ≥ 半段）
            int liveTrialBlocks = 0;        // 段里实时试算的有效块数（开始学习后的预热空档在这里看得出来）
            celadon::glaze::MusicProfile profile;
            celadon::glaze::LiveDiagnostics diagnostics;
        };

        static_assert (std::is_trivially_copyable<Snapshot>::value, "快照在音频线程上整份拷贝：必须是定长、可平凡拷贝的结构（不分配）");

        LiveTraceSink (double sampleRate, const celadon::glaze::LiveConstants& constants, celadon::glaze::MainSectionModel model)
        {
            live.prepare (sampleRate);
            live.setConstants (constants, model);
            live.setPublicationListener (this);
        }

        ~LiveTraceSink() override
        {
            live.setPublicationListener (nullptr);
        }

        void segmentFinalized (const celadon::glaze::SegmentLedger::Segment& segment) noexcept override
        {
            auto& a = arrivals[static_cast<std::size_t> (arrived % static_cast<std::uint64_t> (arrivalCapacity))];
            a.sequence = arrived;
            a.epoch = segment.epoch;
            a.heard = segment.heard;
            a.liveTrialBlocks = segment.liveTrialBlocks;
            ++arrived;
            live.segmentFinalized (segment);
        }

        void advanceTo (std::int64_t analyzerTime) noexcept override
        {
            live.advanceTo (analyzerTime);
        }

        // 流结束：还没到发布时刻的段按序发布完（LiveProfile::flush）。
        void flush() noexcept
        {
            live.flush();
        }

        const celadon::glaze::LiveProfile& getLiveProfile() const noexcept   { return live; }

        // 第 sequence 份发布的快照；还没发布、或已被环形缓冲覆盖（一块里发布的超过容量，不该发生）返回 nullptr。
        const Snapshot* snapshotFor (std::uint64_t sequence) const noexcept
        {
            if (sequence >= published || published - sequence > static_cast<std::uint64_t> (capacity))
                return nullptr;

            const auto& s = snapshots[static_cast<std::size_t> (sequence % static_cast<std::uint64_t> (capacity))];
            return s.sequence == sequence ? &s : nullptr;
        }

    private:
        struct Arrival
        {
            std::uint64_t sequence = std::numeric_limits<std::uint64_t>::max();
            std::uint32_t epoch = 0;
            bool heard = false;
            int liveTrialBlocks = 0;
        };

        void livePublished (const celadon::glaze::LiveCommitRecord& record) noexcept override
        {
            auto& s = snapshots[static_cast<std::size_t> (record.sequence % static_cast<std::uint64_t> (capacity))];
            const auto& a = arrivals[static_cast<std::size_t> (record.sequence % static_cast<std::uint64_t> (arrivalCapacity))];
            const auto known = a.sequence == record.sequence;
            s.sequence = record.sequence;
            s.epoch = known ? a.epoch : 0;
            s.heard = known && a.heard;
            s.liveTrialBlocks = known ? a.liveTrialBlocks : -1;   // −1：段的到达信息配不上（不该发生；追踪记成异常）
            s.profile = live.getProfile();
            s.diagnostics = live.getDiagnostics();
            published = record.sequence + 1;
        }

        celadon::glaze::LiveProfile live;
        std::array<Snapshot, static_cast<std::size_t> (capacity)> snapshots {};
        std::array<Arrival, static_cast<std::size_t> (arrivalCapacity)> arrivals {};
        std::uint64_t arrived = 0, published = 0;
    };

    // 位级摘要（FNV-1a 64 位，同 hashAudio）：追踪 CSV 里的数按固定小数位写（读着方便、逐字节可比），摘要按写出的每个量的二进制位累加——
    // 两份追踪的摘要相同 = 写出的量逐位相同（固定小数位看不出的末位差，摘要看得出）。无效的量只记「无效」，不记它的值（同 CSV 里的空格）。
    struct BitDigest
    {
        std::uint64_t value = fnvOffsetBasis;

        template <typename T>
        void add (T x) noexcept
        {
            static_assert (std::is_arithmetic<T>::value, "只累加数");
            std::array<unsigned char, sizeof (T)> bytes {};
            std::memcpy (bytes.data(), &x, sizeof (T));

            for (const auto byte : bytes)
            {
                value ^= static_cast<std::uint64_t> (byte);
                value *= fnvPrime;
            }
        }

        void add (bool x) noexcept   { add (static_cast<std::uint8_t> (x ? 1 : 0)); }

        template <typename T>
        void addOptional (bool valid, T x) noexcept
        {
            add (valid);

            if (valid)
                add (x);
        }

        void addFeature (const celadon::glaze::Feature& f) noexcept   { addOptional (f.valid, f.value); }
    };

    std::string hex64 (std::uint64_t value)
    {
        char text[32] = {};
        const auto written = std::snprintf (text, sizeof (text), "%016llx", static_cast<unsigned long long> (value));
        return written > 0 ? std::string (text) : std::string ("?");
    }

    // 追踪 CSV 的表头（列的含义见 README「T-048 实时画像追踪」）。空格 = 不知道（valid = false），不用 0 冒充。
    std::string liveTraceHeader()
    {
        std::string header = "sequence,bar,bar_offset,start_sample,end_sample,start_s,end_s,nominal_s,mode,epoch,heard,effective_s,"
                             "loudness_lufs,low_share,loudness_factor,low_factor,mainness,weight_s,reference_lufs,low_reference,"
                             "main_level_lufs,main_mass_s,confidence,time_decay,new_program,live_trial_blocks,"
                             "reference_after_lufs,low_reference_after,active_s,seconds_since_main,new_program_count,"
                             "lra_lu,plr_db,true_peak_dbtp,slope_db_per_oct,profile_low_share,profile_high_share,representativeness,"
                             "cost_lowcut_db";

        for (const auto hz : celadon::glaze::clayMonoCandidateHz)
            header += ",cost_mono" + std::to_string (static_cast<int> (hz)) + "_db";

        header += ",cost_width_db";

        for (const auto hz : celadon::glaze::clayBlendCandidateHz)
            header += ",blend" + std::to_string (static_cast<int> (hz)) + "_db";

        // T-048 阶段 1 修订（回看重判）加的列：发布时刻、进账判断（按上一份画像的参考；长时间无主段与换歌只看它）、换歌的票与起伏、
        // 窗口段数与最老一段在 M 里的份额。
        header += ",publish_s,admission_loudness_factor,admission_low_factor,admission_mainness,np_vote,np_range_lu,window_segments,oldest_main_share";
        return header;
    }

    // 一次提交一行：LiveCommitRecord 的全部字段 + 小节号（有 --bpm 时）+ 这一段并入之后画像的主要字段与诊断。
    // start / end 是分析器时间轴（这一遍送进链的第一个样本 = 0，同 LiveCommitRecord）；小节号按歌曲时间轴（= 分析器时间 + passStart，
    // 从第 N 小节起播时 passStart = 起播样本）算段的中点落在哪一小节（工具的小节网格，同 --segments-csv）：段起点与网格差一两个样本的取整
    // 也归对小节；bar_offset = 段起点 − 该小节起点（歌曲时间轴的样本，对齐时为 0）。
    std::string liveTraceRow (const celadon::glaze::LiveCommitRecord& r, const LiveTraceSink::Snapshot& s, const SegmentGrid& grid,
                              double sampleRate, std::int64_t passStart)
    {
        const auto optional = [] (bool valid, double value, int decimals) { return valid ? fixed (value, decimals) : std::string(); };
        const auto feature = [] (const celadon::glaze::Feature& f, int decimals) { return f.valid ? fixed (f.value, decimals) : std::string(); };
        const auto flag = [] (bool on) { return std::string (on ? "1" : "0"); };
        std::string bar, offset;

        if (grid.bars)
        {
            const auto songStart = r.start + passStart;
            const auto index = grid.indexOf (songStart + (r.end - r.start) / 2);
            bar = std::to_string (index + 1);
            offset = std::to_string (songStart - grid.startOf (index));
        }

        const auto& d = s.diagnostics;
        const auto& p = s.profile;
        auto row = std::to_string (r.sequence) + "," + bar + "," + offset + "," + std::to_string (r.start) + "," + std::to_string (r.end) + ","
                 + fixed (static_cast<double> (r.start) / sampleRate, 4) + "," + fixed (static_cast<double> (r.end) / sampleRate, 4) + ","
                 + fixed (r.nominalSeconds, 4) + "," + segmentModeId (r.mode) + "," + std::to_string (s.epoch) + "," + flag (s.heard) + ","
                 + fixed (r.effectiveSeconds, 4) + "," + optional (r.loudnessValid, r.loudnessLufs, 4) + ","
                 + optional (r.lowShareValid, r.lowShare, 5) + "," + fixed (r.loudnessFactor, 5) + "," + fixed (r.lowFactor, 5) + ","
                 + fixed (r.mainness, 5) + "," + fixed (r.weightSeconds, 4) + "," + optional (r.referenceValid, r.referenceLufs, 4) + ","
                 + optional (r.lowReferenceValid, r.lowReference, 5) + "," + optional (r.mainLevelValid, r.mainLevelLufs, 4) + ","
                 + fixed (r.mainMassSeconds, 4) + "," + fixed (r.confidence, 5) + "," + flag (r.timeDecay) + "," + flag (r.newProgram) + ","
                 + std::to_string (s.liveTrialBlocks) + "," + optional (d.referenceValid, d.referenceLufs, 4) + ","
                 + optional (d.lowReferenceValid, d.lowReference, 5) + "," + fixed (d.activeSeconds, 4) + "," + fixed (d.secondsSinceMain, 4) + ","
                 + std::to_string (d.newProgramCount) + "," + feature (p.loudnessRangeLu, 4) + "," + feature (p.plrDb, 4) + ","
                 + feature (p.truePeakDbtp, 4) + "," + feature (p.spectralSlopeDbPerOct, 4) + "," + feature (p.lowShare, 5) + ","
                 + feature (p.highShare, 5) + "," + fixed (p.representativeness, 5) + "," + feature (p.lowCutPeakCostDb, 4);

        for (const auto& cost : p.monoPeakCostDb)
            row += "," + feature (cost, 4);

        row += "," + feature (p.widthPeakCostDb, 4);

        for (const auto& loss : p.lowCutBlendLossDb)
            row += "," + feature (loss, 4);

        row += "," + fixed (static_cast<double> (r.publishTime) / sampleRate, 4) + "," + fixed (r.admissionLoudnessFactor, 5) + ","
             + fixed (r.admissionLowFactor, 5) + "," + fixed (r.admissionMainness, 5) + "," + std::to_string (static_cast<int> (r.newProgramVote)) + ","
             + optional (r.newProgramRangeValid, r.newProgramRangeLu, 4) + "," + std::to_string (d.windowSegments) + ","
             + fixed (d.oldestMainShare, 6);
        return row;
    }

    // 与 liveTraceRow 同一批量、同一顺序累加进摘要（改了一边要改另一边）。
    void addToDigest (BitDigest& digest, const celadon::glaze::LiveCommitRecord& r, const LiveTraceSink::Snapshot& s) noexcept
    {
        digest.add (r.sequence);
        digest.add (r.start);
        digest.add (r.end);
        digest.add (r.nominalSeconds);
        digest.add (static_cast<std::uint8_t> (r.mode));
        digest.add (s.epoch);
        digest.add (s.heard);
        digest.add (r.effectiveSeconds);
        digest.addOptional (r.loudnessValid, r.loudnessLufs);
        digest.addOptional (r.lowShareValid, r.lowShare);
        digest.add (r.loudnessFactor);
        digest.add (r.lowFactor);
        digest.add (r.mainness);
        digest.add (r.weightSeconds);
        digest.addOptional (r.referenceValid, r.referenceLufs);
        digest.addOptional (r.lowReferenceValid, r.lowReference);
        digest.addOptional (r.mainLevelValid, r.mainLevelLufs);
        digest.add (r.mainMassSeconds);
        digest.add (r.confidence);
        digest.add (r.timeDecay);
        digest.add (r.newProgram);
        digest.add (s.liveTrialBlocks);

        const auto& d = s.diagnostics;
        digest.addOptional (d.referenceValid, d.referenceLufs);
        digest.addOptional (d.lowReferenceValid, d.lowReference);
        digest.add (d.activeSeconds);
        digest.add (d.secondsSinceMain);
        digest.add (d.newProgramCount);

        const auto& p = s.profile;
        digest.addFeature (p.loudnessRangeLu);
        digest.addFeature (p.plrDb);
        digest.addFeature (p.truePeakDbtp);
        digest.addFeature (p.spectralSlopeDbPerOct);
        digest.addFeature (p.lowShare);
        digest.addFeature (p.highShare);
        digest.add (p.representativeness);
        digest.addFeature (p.lowCutPeakCostDb);

        for (const auto& cost : p.monoPeakCostDb)
            digest.addFeature (cost);

        digest.addFeature (p.widthPeakCostDb);

        for (const auto& loss : p.lowCutBlendLossDb)
            digest.addFeature (loss);

        digest.add (r.publishTime);
        digest.add (r.admissionLoudnessFactor);
        digest.add (r.admissionLowFactor);
        digest.add (r.admissionMainness);
        digest.add (r.newProgramVote);
        digest.addOptional (r.newProgramRangeValid, r.newProgramRangeLu);
        digest.add (d.windowSegments);
        digest.add (d.oldestMainShare);
    }

    // 一遍的追踪结果（主线程）。
    struct LiveTraceResult
    {
        std::vector<std::string> rows;                            // 数据行（表头见 liveTraceHeader），一次提交一行，按提交先后
        std::vector<celadon::glaze::LiveCommitRecord> records;    // 同样的提交（自检与逐遍比较用）
        BitDigest digest;                                         // 全部行的位级摘要
        celadon::glaze::MusicProfile finalProfile;                // finish 之后（开着的段已交出、全部发布）的画像与诊断
        celadon::glaze::LiveDiagnostics finalDiagnostics;
        std::vector<celadon::glaze::LiveSegmentJudgement> judgements;   // finish 之后窗口里每一段的最终判断（回看重判到歌尾，从旧到新）
        bool overrun = false;    // 读提交时落后超过环形容量（一块里交来的段超过 64 段，不该发生）：少了几行，追踪不完整
        bool mismatch = false;   // 提交记录与快照配不上（序号、I 或段的到达信息不一致，不该发生）：追踪不可信
    };

    // 一遍的追踪：prepare 之后 attach，每处理完一块 afterBlock（读新提交、写行），最后 finish（关实时模式交出最后一块的帧与开着的段、
    // 发布完、再读一次、记下最终画像与判断）。时间轴只由分析器报（advanceTo = 实时段落账交齐的位置，MusicAnalyzer.h「时间轴与 advanceTo」），
    // 追踪器自己不报：报「送进链的样本数」会走在交齐位置前面，段就可能晚于自己的发布时刻才到。
    class LiveTracer
    {
    public:
        LiveTracer (double sampleRateToUse, const celadon::glaze::LiveConstants& constants, celadon::glaze::MainSectionModel model,
                    const Transport& transportToUse)
            : sink (std::make_unique<LiveTraceSink> (sampleRateToUse, constants, model)),
              sampleRate (sampleRateToUse),
              grid (makeSegmentGrid (transportToUse, sampleRateToUse)),
              buffer (static_cast<std::size_t> (LiveTraceSink::capacity))
        {
        }

        // 消息线程，链 prepare 之后、处理之前（分析器打开实时模式）。passStartSample = 这一遍送进链的第一个样本在歌曲里的位置
        // （从第 N 小节起播时 > 0），追踪的小节号按它换到歌曲的小节网格上（liveTraceRow）。
        // T-048 阶段 2（Q2）：画像只许链里那一份，--live-trace 改读它的 readCommits / getDiagnostics。
        // CeladonChain 还没有公开这份画像（编排器代理未合入），这里仍自建 LiveProfile 再 setLiveSegmentSink。
        // 合入后删掉自建的 sink，attach 改为读链内那一份；对象仍须是链持有的那一个。
        void attach (celadon::CeladonChain& chain, std::int64_t passStartSample = 0)
        {
            passStart = passStartSample;
            chain.setLiveSegmentSink (sink.get());
        }

        // 每处理完一块：这一块里分析器报过的交齐位置可能已经让实时画像发布了几份，读出来写行。
        void afterBlock()
        {
            collect();
        }

        // 收尾（这一遍要读的状态都读完之后调：存档、成品、指标与不追踪时取自同一时刻）。关实时模式：链把最后一块还在 Balance 里的帧
        // 只交给实时段落账（CeladonChain::setLiveSegmentSink 带上自己的 balance；链在「下一块」开头才交上一块的帧，离线流没有下一块），
        // 再把开着的段（歌尾那一截）就地定稿交给接收者、报到此刻——之后链不再引用接收者。T-048 阶段 1 修订 B2 之前这里要先补一个
        // 0 样本的块，否则歌尾一两段少了最后一块的帧、随块长变（MusicAnalyzerLive 用例 5d 对照两种做法逐字段相同）。
        void finish (celadon::CeladonChain& chain)
        {
            chain.setLiveSegmentSink (nullptr);
            sink->flush();
            collect();
            result.finalProfile = sink->getLiveProfile().getProfile();
            result.finalDiagnostics = sink->getLiveProfile().getDiagnostics();
            result.judgements.resize (static_cast<std::size_t> (celadon::glaze::LiveProfile::windowCapacity));
            result.judgements.resize (static_cast<std::size_t> (sink->getLiveProfile().readJudgements (result.judgements.data(),
                                                                                                      celadon::glaze::LiveProfile::windowCapacity)));
        }

        const LiveTraceResult& getResult() const noexcept   { return result; }
        const SegmentGrid& getGrid() const noexcept          { return grid; }

    private:
        void collect()
        {
            while (true)
            {
                const auto read = sink->getLiveProfile().readCommits (cursor, buffer.data(), static_cast<int> (buffer.size()));
                result.overrun = result.overrun || read < 0;
                const auto count = std::abs (read);

                for (int i = 0; i < count; ++i)
                    addRow (buffer[static_cast<std::size_t> (i)]);

                if (count < static_cast<int> (buffer.size()))
                    return;
            }
        }

        void addRow (const celadon::glaze::LiveCommitRecord& record)
        {
            // 记录与快照是同一次并入的两半：序号对得上、I 逐位相同（record 的 mainLevel 就是并入之后画像的 integratedLufs）。
            const auto* snapshot = sink->snapshotFor (record.sequence);

            if (snapshot == nullptr)
            {
                result.mismatch = true;
                return;
            }

            const auto& level = snapshot->profile.integratedLufs;
            result.mismatch = result.mismatch || level.valid != record.mainLevelValid || snapshot->liveTrialBlocks < 0
                           || (level.valid && std::memcmp (&level.value, &record.mainLevelLufs, sizeof (float)) != 0);

            result.rows.push_back (liveTraceRow (record, *snapshot, grid, sampleRate, passStart));
            result.records.push_back (record);
            addToDigest (result.digest, record, *snapshot);
        }

        std::unique_ptr<LiveTraceSink> sink;
        double sampleRate = 48000.0;
        SegmentGrid grid;
        std::vector<celadon::glaze::LiveCommitRecord> buffer;
        std::uint64_t cursor = 0;
        LiveTraceResult result;
        std::int64_t passStart = 0;
    };

    // 没有 --live-trace 时为空（各遍照旧不接追踪）。
    std::unique_ptr<LiveTracer> makeLiveTracer (const Options& options, int genre, double sampleRate, const Transport& transport)
    {
        if (! options.liveTrace)
            return nullptr;

        return std::make_unique<LiveTracer> (sampleRate, options.liveConstants, liveModelFor (options, genre), transport);
    }

    // --trace 的阶段 1 校准诊断列（两遍的追踪共用；T-015 / BUG-003「整首停在校准中」由它们查出）。
    // 阶段 2 没有校准轮。这些列跟着 GlazeController 的探针走，探针删掉之后改成 confidence,g_trim_db,protection_db,guard_cause。
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
        celadon::glaze::CommittedState committed;  // T-038：第一遍结束时已提交的版本（方案增益、目标、权重、画像；House 门槛 ① / ⑤ 用）
    };

    // 第一遍：整首歌送进一条新链，输出丢弃（GLAZE 学习、方案过渡、校准都在这一遍走完），最后把 GLAZE 状态存下来。
    // T-038：transport 给模拟走带的拍点；startSample > 0 = 从这个位置起播到歌尾（House 门槛「从 drop 播」；宿主位置照实际样本给）。
    // T-048：live ≠ nullptr 时这一遍接实时画像追踪（见「T-048 实时画像追踪」）。
    ListenResult listenPass (const juce::AudioBuffer<float>& input, double sampleRate, int blockSize,
                             const celadon::ChainParams& params, bool trace = false, const Transport& transport = Transport {},
                             int startSample = 0, LiveTracer* live = nullptr)
    {
        ListenResult result;
        auto chain = makeRenderChain();

        // 顺序照 PluginProcessor::prepareToPlay：先交参数再 prepare，各模块的平滑器直接落在这份参数上、不从默认值滑过来。
        chain->setParams (params, false);
        chain->prepare (makeSpec (sampleRate, blockSize));

        if (live != nullptr)
            live->attach (*chain, std::max (0, startSample));

        const auto numSamples = input.getNumSamples();
        juce::AudioBuffer<float> block (2, blockSize);

        for (int position = std::max (0, startSample); position < numSamples; position += blockSize)
        {
            const auto length = std::min (blockSize, numSamples - position);
            block.setSize (2, length, false, false, true);

            for (int ch = 0; ch < 2; ++ch)
                block.copyFrom (ch, 0, input, ch, position, length);

            // 每块顺序照 PluginProcessor::processChain：宿主时间线 → 参数 → 处理。
            chain->setHostInfo (makeHostInfo (position, transport));
            chain->setParams (params, false);

            if (chain->process (block))
                ++result.nonFiniteBlocks;

            if (live != nullptr)
                live->afterBlock();

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
        result.committed = chain->getGlazeController().getCommittedStateForTesting();

        // 追踪最后收尾（补交最后一块的帧、交出歌尾那一截段）：放在读完状态之后，存下的状态与不追踪时一样取自同一时刻。
        if (live != nullptr)
            live->finish (*chain);

        return result;
    }

    // T-038：第二遍 / 重放逐块的限幅 GR（成品时间轴上的块起点；--segments-csv 按段汇总，见 writeSegments）。
    struct BlockReduction
    {
        std::int64_t outputStart = 0;   // 块起点减链延迟（成品时间轴；开头延迟段为负）
        int length = 0;
        double reductionDb = 0.0;       // getAndResetLimitGainReductionDb()：块内最大压缩量
    };

    struct RenderResult
    {
        juce::AudioBuffer<float> io;       // 第二遍整段：[0, 链延迟) 是延迟段，其后是成品
        int latencySamples = 0;
        int outputLength = 0;              // 成品长度 = 原曲 + 余振
        double grMeanDb = 0.0;             // 限幅 GR：各块读数按块长加权平均
        double grMaxDb = 0.0;
        double phaseGuardPercent = 0.0;    // 反相提示亮着的时长占比（%；ADR-015 起只提示、不改声音；列名 phase_guard_pct 沿用旧名）
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

        // ---- T-038（metrics.csv 的追加列，见 csvExtendedFields）----
        float effectiveCeilingDb = 0.0f;   // 最后一块交给限幅器的有效 Ceiling（编码余量、流媒体 −2 dBTP 上限都已算进去）
        float beatReleaseCapMaxMs = 0.0f;  // 各块交给限幅器的慢段释放上限的最大值（0 = 一直没封顶；House 限幅跟拍）
        bool hasGuardLimits = false;       // 成品末尾保护层生效的约束上限（= 风格预算：blendConstants(方案源常数, 方案权重)）
        celadon::glaze::StyleConstants guardLimits;
        std::vector<BlockReduction> reductions;   // --segments-csv：逐块限幅 GR（不分段时为空）
    };

    // 成品 / 重放两种渲染逐块共用的统计：限幅 GR、反相提示 / 宽度保护时长、峰值守护、限幅入口峰值、慢路反相占比。
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
    // T-038：transport 给模拟走带的拍点（与第一遍同一条时间线）；recordReductions = 逐块记限幅 GR（--segments-csv）。
    // T-048：live ≠ nullptr 时这一遍接实时画像追踪。
    void renderPass (const juce::AudioBuffer<float>& input, double sampleRate, int blockSize,
                     const celadon::ChainParams& params, const juce::XmlElement* state, bool trace, RenderResult& result,
                     EffectiveLog* effectiveLog = nullptr, const Transport& transport = Transport {},
                     bool recordReductions = false, LiveTracer* live = nullptr)
    {
        auto chain = makeRenderChain();

        // 顺序照宿主「重开工程 → 导出」：先读回存档（setStateInformation），再 prepareToPlay（先交参数再 prepare）。
        // rendercheck 验过「读回在 prepare 之前 / 之后」两种顺序结果逐样本相同。
        chain->restoreGlazeStateXml (state);
        chain->setParams (params, false);

        if (effectiveLog != nullptr)
            effectiveLog->prepared = chain->getEffectiveParams();   // prepare 时模块落在它上面（prepare 不再调 setParams）

        chain->prepare (makeSpec (sampleRate, blockSize));

        if (live != nullptr)
            live->attach (*chain);

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

        if (recordReductions)
        {
            result.reductions.clear();
            result.reductions.reserve (static_cast<std::size_t> (total / juce::jmax (1, blockSize) + 1));
        }

        // 原地处理整段缓冲（末尾 latency + ringOut 个样本是补的零），每块一个视图，不拷贝。
        for (int position = 0; position < total; position += blockSize)
        {
            const auto length = std::min (blockSize, total - position);
            juce::AudioBuffer<float> block (result.io.getArrayOfWritePointers(), 2, position, length);

            chain->setHostInfo (makeHostInfo (position, transport));
            chain->setParams (params, false);

            if (effectiveLog != nullptr)
                effectiveLog->blocks.push_back (chain->getEffectiveParams());   // 这一块交给模块的就是它（setParams 里算好，process 用）

            // T-038：这一块交给限幅器的有效 Ceiling 与慢段释放上限（只读，不改链的状态）。
            result.effectiveCeilingDb = chain->getEffectiveParams().limit.ceilingDb;
            result.beatReleaseCapMaxMs = std::max (result.beatReleaseCapMaxMs, chain->getEffectiveParams().limit.beatReleaseCapMs);

            if (chain->process (block))
                ++result.nonFiniteBlocks;

            if (live != nullptr)
                live->afterBlock();

            if (chain->getLatencySamples() != latency)
                result.latencyChanged = true;

            stats.addBlock (*chain, length, result);

            if (recordReductions)
                result.reductions.push_back ({ static_cast<std::int64_t> (position) - latency, length, stats.reduction });

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
        result.hasGuardLimits = finalProbe.guardLimitsSet;   // T-038 House 门槛 ②：风格预算（保护层此刻生效的约束上限）
        result.guardLimits = finalProbe.guardLimits;

        if (live != nullptr)
            live->finish (*chain);   // 读完上面的状态之后再收尾（同 listenPass）
    }

    //==============================================================================
    // 同决策重放（ADR-016）：修坯门槛要回答「同样的决策下，修坯开着有没有帮倒忙」。两条各自跑 GLAZE 的链会因为约束反馈、
    // 保护层发生的时刻不同而走到不同的方案（上限、Glue / Heat 份额、校准偏差都不同），跨链比较量到的多半是方案分岔（T-015 后
    // WTF g70 两条链上限 1.145 / 0.274 dB）。所以另起一条链：GLAZE 0（编排器不做任何决策，实际参数逐位等于喂进去的值，
    // 也不按保护层时隙切片），逐块喂「自动」链那一块实际交给模块的全部参数，只把修坯换掉：
    //   - off：三项都「关」→ 修坯中性（门槛拿它对比「自动」）；
    //   - copy：三项用「手动」填自动链那一块的实际修坯值 → 应复现自动链（最大样本差 ≤ −180 dBFS，见下；阳性对照：证明重放本身不走样，
    //     包括 GLAZE 0 不切片这一点）。手动收单固定是「比例 1」，收单比例在 0、1 之间的块「手动」态表达不了（ADR-015 起自动收单恒为 0，
    //     这一种已不出现）；另一种原因是目标频谱用了「参考」形状、重放链没有侧链参考（本工具的目标频谱固定是出厂「自动」，也走不到）。
    //     两种都记「不适用」。
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

    // 一种重放：修坯怎么换（off / copy），外加 T-096d 的参数覆盖（nullptr = 不覆盖，即原来的两种重放）。
    // 构造函数不是 explicit：原来传 ReplayClay 的调用点原样可用。
    struct ReplayVariant
    {
        ReplayVariant (ReplayClay clayMode, const ReplayOverrides* overrideList = nullptr) noexcept
            : clay (clayMode), overrides (overrideList)
        {
        }

        ReplayClay clay;
        const ReplayOverrides* overrides;
    };

    // 重放链交给链的一份参数：先按修坯换法照搬（replayParams），再把覆盖的键改成给定值。prepare 前那份与每一块都走这里
    //（BUG-012：只改逐块、漏了 prepare 前那份，模块 prepare 时就落在旧值上，开头 20 ms 的平滑会让对照走样）。
    celadon::ChainParams replayParams (const celadon::ChainParams& effective, const ReplayVariant& variant) noexcept
    {
        auto p = replayParams (effective, variant.clay);

        if (variant.overrides != nullptr)
            for (const auto& o : *variant.overrides)
                replayOverrideKeys[o.key].set (p, o.value);

        return p;
    }

    // 重放一遍：块长、宿主时间线与自动链的第二遍相同；prepare 前交 effectiveLog.prepared，之后每块取 blocks 里同一块的。
    // latency 应与自动链相同（Clay 的 D + G 不随参数变）。
    // T-038：transport 与自动链第二遍相同（限幅跟拍的上限随实际参数回喂，见 CeladonChain::setParams）；recordReductions 同 renderPass。
    // T-048：live ≠ nullptr 时这一遍接实时画像追踪。
    // T-096d：variant 可带参数覆盖（prepare 前那份与每一块都覆盖）；used ≠ nullptr 时记下这条重放链实际交给模块的参数（同 renderPass
    // 的 effectiveLog：prepare 前一份 + 逐块），给「除覆盖的键外逐位相同」的核对用。
    void replayPass (const juce::AudioBuffer<float>& input, double sampleRate, int blockSize,
                     const EffectiveLog& effectiveLog, const ReplayVariant& variant, RenderResult& result,
                     const Transport& transport = Transport {}, bool recordReductions = false, LiveTracer* live = nullptr,
                     EffectiveLog* used = nullptr)
    {
        auto chain = makeRenderChain();
        const auto& blocks = effectiveLog.blocks;
        chain->setParams (replayParams (effectiveLog.prepared, variant), false);

        if (used != nullptr)
            used->prepared = chain->getEffectiveParams();

        chain->prepare (makeSpec (sampleRate, blockSize));

        if (live != nullptr)
            live->attach (*chain);

        const auto latency = chain->getLatencySamples();
        const auto total = prepareIo (input, sampleRate, latency, result);

        chain->getAndResetLimitGainReductionDb();
        chain->getClayForTesting().getAndResetPeakGuardStats();

        PassStats stats;
        std::size_t index = 0;

        if (recordReductions)
        {
            result.reductions.clear();
            result.reductions.reserve (static_cast<std::size_t> (total / juce::jmax (1, blockSize) + 1));
        }

        if (used != nullptr)
        {
            used->blocks.clear();
            used->blocks.reserve (static_cast<std::size_t> (total / juce::jmax (1, blockSize) + 1));
        }

        for (int position = 0; position < total; position += blockSize, ++index)
        {
            const auto length = std::min (blockSize, total - position);
            juce::AudioBuffer<float> block (result.io.getArrayOfWritePointers(), 2, position, length);

            chain->setHostInfo (makeHostInfo (position, transport));
            chain->setParams (replayParams (blocks.empty() ? effectiveLog.prepared : blocks[std::min (index, blocks.size() - 1)], variant), false);

            if (used != nullptr)
                used->blocks.push_back (chain->getEffectiveParams());

            result.effectiveCeilingDb = chain->getEffectiveParams().limit.ceilingDb;
            result.beatReleaseCapMaxMs = std::max (result.beatReleaseCapMaxMs, chain->getEffectiveParams().limit.beatReleaseCapMs);

            if (chain->process (block))
                ++result.nonFiniteBlocks;

            if (live != nullptr)
                live->afterBlock();

            if (chain->getLatencySamples() != latency)
                result.latencyChanged = true;

            stats.addBlock (*chain, length, result);

            if (recordReductions)
                result.reductions.push_back ({ static_cast<std::int64_t> (position) - latency, length, stats.reduction });
        }

        stats.finish (total, result);
        result.status = chain->getGlazeStatus();

        // 读完上面的状态之后再收尾（同 listenPass）。
        if (live != nullptr)
            live->finish (*chain);
    }

    //==============================================================================
    // 逐段统计（T-038 --segments-csv）：门控阈值的标定（设计规格-曲风-House学习方式 第 6 节：逐小节导出段响度与低频占比，人工标出
    // 前奏 / breakdown / drop 后选 ΔG 与低频门系数）与 House 门槛（设计规格-曲风与交付 第 7 节第 4 条）。只读音频、不碰链：
    // 不依赖编排器的 SegmentLedger，口径按规格第 2.3 节在这里独立算一份（README「逐段统计」写明与插件的差别）。

    // 与 MusicAnalyzer.cpp 同值的几个常数（那边在匿名命名空间里，这里照抄并注明出处；改那边要同步改这里）：
    constexpr int lowShareLastBand = 4;                  // lowShareLastBand：低频 = 100 Hz 段及以下（段 0 ～ 4，上边界 112 Hz）
    constexpr double frameAbsoluteGatePower = 1.0e-7;    // frameAbsoluteGatePower：帧总功率 −70 dBFS 绝对门
    constexpr double frameRelativeGateRatio = 0.01;      // frameRelativeGateRatio：帧总功率低于均值 20 dB 的帧不计
    constexpr double effectiveAbsoluteGateLufs = -70.0;  // 「有效音频」门：400 ms 门块 ≥ −70 LUFS（absoluteGateLufs）……
    constexpr double effectiveRelativeGateLu = -20.0;    // ……且不低于参考 I 以下 20 LU（effectiveRelativeGateLu）
    constexpr int shortTermSubBlocks = 30;               // shortTermSubBlocks：短期响度 S 的 3 s 窗（EBU Tech 3341）

    struct SegmentStat
    {
        std::int64_t start = 0, end = 0;           // [start, end)（样本；最后一段截到音频末尾）
        double energy = 0.0;                        // 过了有效音频门的 100 ms 子块的 K 加权均方之和（子块按起点归段）
        int validSubBlocks = 0;
        bool hasShortTerm = false;                  // EBU 短期响度 S（3 s 窗、不门限）在本段最后一个子块结尾处的读数（参考：门控用的是上面的段响度）
        double shortTermLufs = 0.0;
        double lowPower = 0.0, totalPower = 0.0;    // 过了帧门的 Balance 帧：段 0 ～ 4 / 全部 27 段的功率之和（帧按分析窗起点归段）
        int frames = 0;
        double grWeighted = 0.0, grMax = 0.0;       // 只有成品：逐块限幅 GR（块按成品时间轴的起点归段，按块长加权）
        std::int64_t grSamples = 0;
        int grBlocks = 0, grOverP95Cap = 0;         // 块数；块 GR 高于风格预算 limitGrP95CapDb 的块数（门槛 ②：占比 ≤ 5 % 即 P95 ≤ 上限）
    };

    struct SegmentReport
    {
        SegmentGrid grid;
        double subBlockSeconds = 0.1;
        double referenceLufs = 0.0;                 // 有效音频门的参考 I：整段音频的 BS.1770 整体响度（插件用学习区间的 I，这里用整首）
        bool hasReference = false;
        bool droppedFrames = false;                 // 读 Balance 帧时丢过帧（不该发生：每段只送 4096 个样本）
        std::vector<SegmentStat> segments;
    };

    // 量一段立体声音频（输入，或对齐前的成品截到原曲长度）的逐段统计：
    //   - 段响度（规格第 2.3 节）：段内过了「有效音频」门的 100 ms 子块的 K 加权能量平均，换算成 LUFS。门 = 以本子块结尾的 400 ms 门块
    //     （前 3 个子块用已有的）≥ −70 LUFS，且不低于参考 I − 20 LU——与 MusicAnalyzer 的 effective 同一套判据，参考 I 换成整首的；
    //   - 低频占比（与画像 lowShare 同一口径）：一个独立的 BalanceModule（参数全 0，只用它的分析）逐帧给 27 段功率，过帧门
    //     （总功率 ≥ −70 dBFS，且不低于全曲过绝对门各帧的平均 20 dB）的帧上 Σ段 0～4 ÷ Σ全部 27 段；插件的相对门按学习区间的均值。
    SegmentReport measureSegments (const juce::AudioBuffer<float>& audio, int numSamples, double sampleRate, const SegmentGrid& grid)
    {
        SegmentReport report;
        report.grid = grid;
        numSamples = juce::jlimit (0, audio.getNumSamples(), numSamples);

        if (numSamples <= 0)
            return report;

        const auto count = grid.indexOf (numSamples - 1) + 1;
        report.segments.resize (static_cast<std::size_t> (count));

        for (std::int64_t k = 0; k < count; ++k)
        {
            auto& s = report.segments[static_cast<std::size_t> (k)];
            s.start = grid.startOf (k);
            s.end = std::min<std::int64_t> (grid.startOf (k + 1), numSamples);
        }

        const auto segmentOf = [&report, &grid] (std::int64_t sample) -> SegmentStat*
        {
            const auto k = grid.indexOf (sample);
            return k >= 0 && k < static_cast<std::int64_t> (report.segments.size()) ? &report.segments[static_cast<std::size_t> (k)] : nullptr;
        };

        // ---- 段响度：插件自己的表出 100 ms 子块（与 IN 表、分析器同一测点同一算法）；每 4 s 读一次（子块环形缓冲存 12.8 s）----
        {
            celadon::LoudnessMeter meter;
            const auto chunk = static_cast<int> (std::llround (4.0 * sampleRate));
            meter.prepare ({ sampleRate, static_cast<juce::uint32> (juce::jmax (1, chunk)), 2u });
            report.subBlockSeconds = static_cast<double> (meter.getSubBlockLength()) / sampleRate;
            std::array<celadon::LoudnessMeter::SubBlock, static_cast<std::size_t> (celadon::LoudnessMeter::subBlockRingCapacity)> blocks {};
            std::vector<celadon::LoudnessMeter::SubBlock> all;
            std::int64_t cursor = 0;

            for (int start = 0; start < numSamples; start += chunk)
            {
                const auto length = std::min (chunk, numSamples - start);
                const juce::AudioBuffer<float> part (const_cast<float* const*> (audio.getArrayOfReadPointers()), 2, start, length);
                meter.process (part);
                const auto read = std::abs (meter.readSubBlocksSince (cursor, blocks.data(), static_cast<int> (blocks.size())));
                all.insert (all.end(), blocks.begin(), blocks.begin() + read);
            }

            const auto integrated = meter.getIntegratedLufs();
            report.hasReference = integrated > celadon::LoudnessMeter::noReading;
            report.referenceLufs = static_cast<double> (integrated);
            const auto subBlockLength = static_cast<std::int64_t> (meter.getSubBlockLength());

            for (std::size_t i = 0; i < all.size(); ++i)
            {
                // 短期响度 S：以本子块结尾的 30 个子块（3 s）均方平均，不门限、不满 3 s 不给（同 MusicAnalyzer 的 shortTermLufs）。
                // 每段记它最后一个子块处的读数（子块按起点归段，后面的覆盖前面的）。
                if (i + 1 >= static_cast<std::size_t> (shortTermSubBlocks))
                {
                    double shortSum = 0.0;
                    auto finite = true;

                    for (std::size_t back = 0; back < static_cast<std::size_t> (shortTermSubBlocks); ++back)
                    {
                        const auto ms = all[i - back].meanSquare;
                        finite = finite && std::isfinite (ms) && ms >= 0.0;
                        shortSum += finite ? ms : 0.0;
                    }

                    if (auto* s = finite ? segmentOf (all[i].endSample - subBlockLength) : nullptr)
                    {
                        s->hasShortTerm = true;
                        s->shortTermLufs = -0.691 + 10.0 * std::log10 (std::max (shortSum / shortTermSubBlocks, 1.0e-30));
                    }
                }

                // 400 ms 门块：以本子块结尾的至多 4 个子块的均方平均（同 MusicAnalyzer::consumeSubBlock 的 gatingPower）。
                double windowSum = 0.0;
                int windowCount = 0;

                for (std::size_t back = 0; back < 4 && back <= i; ++back)
                {
                    const auto ms = all[i - back].meanSquare;

                    if (std::isfinite (ms) && ms >= 0.0)
                    {
                        windowSum += ms;
                        ++windowCount;
                    }
                }

                const auto ms = all[i].meanSquare;

                if (windowCount == 0 || ! std::isfinite (ms) || ms < 0.0)
                    continue;

                const auto gatingLufs = -0.691 + 10.0 * std::log10 (std::max (windowSum / windowCount, 1.0e-30));
                const auto effective = gatingLufs >= effectiveAbsoluteGateLufs
                                    && (! report.hasReference || gatingLufs >= report.referenceLufs + effectiveRelativeGateLu);

                if (! effective)
                    continue;

                if (auto* s = segmentOf (all[i].endSample - subBlockLength))
                {
                    s->energy += ms;
                    ++s->validSubBlocks;
                }
            }
        }

        // ---- 低频占比：独立的 Balance 只做分析（参数全 0，输出逐位直通；处理的是一份拷贝）----
        {
            celadon::BalanceModule balance;
            constexpr int chunk = 4096;   // 每段至多 4096 / hop 帧（48 k 下 8 帧，8 kHz 也不超过 32），远小于帧环形缓冲的 64
            balance.prepare ({ sampleRate, static_cast<juce::uint32> (chunk), 2u });
            balance.setParams (celadon::BalanceModule::Params {});
            const auto window = static_cast<std::int64_t> (balance.getAnalysisWindowSamples());

            struct FramePower { std::int64_t start; double low, total; };
            std::vector<FramePower> frames;
            std::array<celadon::BalanceModule::FrameSnapshot, static_cast<std::size_t> (celadon::BalanceModule::frameRingCapacity)> snapshots {};
            juce::AudioBuffer<float> scratch (2, chunk);
            std::int64_t cursor = 0;

            for (int start = 0; start < numSamples; start += chunk)
            {
                const auto length = std::min (chunk, numSamples - start);
                scratch.setSize (2, length, false, false, true);

                for (int ch = 0; ch < 2; ++ch)
                    scratch.copyFrom (ch, 0, audio, ch, start, length);

                balance.process (scratch);
                const auto read = balance.readFramesSince (cursor, snapshots.data(), static_cast<int> (snapshots.size()));
                report.droppedFrames = report.droppedFrames || read < 0;

                for (int f = 0; f < std::abs (read); ++f)
                {
                    const auto& snapshot = snapshots[static_cast<std::size_t> (f)];
                    const auto windowStart = snapshot.endSample - window;

                    if (windowStart < 0)
                        continue;   // 分析窗还没装满（同分析器：窗起点早于开播的帧不用）

                    double low = 0.0, total = 0.0;

                    for (int b = 0; b < celadon::BalanceModule::numBands; ++b)
                    {
                        const auto p = std::max (0.0, static_cast<double> (snapshot.bandPower[static_cast<std::size_t> (b)]));
                        total += p;
                        low += b <= lowShareLastBand ? p : 0.0;
                    }

                    if (std::isfinite (total))
                        frames.push_back ({ windowStart, low, total });
                }
            }

            double gatedSum = 0.0;
            int gatedCount = 0;

            for (const auto& frame : frames)
            {
                if (frame.total >= frameAbsoluteGatePower)
                {
                    gatedSum += frame.total;
                    ++gatedCount;
                }
            }

            const auto relativeGate = gatedCount > 0 ? frameRelativeGateRatio * gatedSum / gatedCount : 0.0;

            for (const auto& frame : frames)
            {
                if (frame.total < frameAbsoluteGatePower || frame.total < relativeGate)
                    continue;

                if (auto* s = segmentOf (frame.start))
                {
                    s->lowPower += frame.low;
                    s->totalPower += frame.total;
                    ++s->frames;
                }
            }
        }

        return report;
    }

    // 成品的逐块限幅 GR 按段汇总（块按成品时间轴的起点归段；开头的延迟段归第 0 段，原曲之后的余振不计）。
    // p95CapDb > 0 时另数「块 GR 高于它」的块：风格预算的 P95 一项按块计（GlazeGuard 按 10 ms 时隙的均值比，这里的块值是块内最大，偏严）。
    void addReductions (SegmentReport& report, const std::vector<BlockReduction>& reductions, double p95CapDb)
    {
        for (const auto& r : reductions)
        {
            const auto k = report.grid.indexOf (std::max<std::int64_t> (0, r.outputStart));

            if (k < 0 || k >= static_cast<std::int64_t> (report.segments.size()))
                continue;

            auto& s = report.segments[static_cast<std::size_t> (k)];
            s.grWeighted += r.reductionDb * static_cast<double> (r.length);
            s.grSamples += r.length;
            s.grMax = std::max (s.grMax, r.reductionDb);
            ++s.grBlocks;

            if (p95CapDb > 0.0 && r.reductionDb > p95CapDb)
                ++s.grOverP95Cap;
        }
    }

    // 逐段统计表（CSV）。segment 从 1 起（按小节分段时 = 小节号）；没有有效子块的段 lufs 留空，没有过门帧的段 low_share 留空，
    // 歌开头不满 3 s 的段 short_term_lufs 留空。
    std::string segmentsCsvText (const SegmentReport& report, double sampleRate, bool withReductions)
    {
        std::string csv = "segment,mode,start_sample,end_sample,start_s,end_s,lufs,valid_s,short_term_lufs,low_share,frames";

        if (withReductions)
            csv += ",gr_mean_db,gr_max_db,gr_blocks,gr_over_p95_cap_blocks";

        csv += "\n";
        const auto* mode = report.grid.bars ? "bars" : "seconds";

        for (std::size_t k = 0; k < report.segments.size(); ++k)
        {
            const auto& s = report.segments[k];
            const auto hasLoudness = s.validSubBlocks > 0 && s.energy > 0.0;
            const auto hasLowShare = s.frames > 0 && s.totalPower > 0.0;

            csv += std::to_string (k + 1) + "," + mode + "," + std::to_string (s.start) + "," + std::to_string (s.end) + ","
                 + fixed (static_cast<double> (s.start) / sampleRate, 4) + "," + fixed (static_cast<double> (s.end) / sampleRate, 4) + ","
                 + (hasLoudness ? fixed (-0.691 + 10.0 * std::log10 (s.energy / s.validSubBlocks), 3) : std::string()) + ","
                 + fixed (s.validSubBlocks * report.subBlockSeconds, 1) + ","
                 + (s.hasShortTerm ? fixed (s.shortTermLufs, 3) : std::string()) + ","
                 + (hasLowShare ? fixed (s.lowPower / s.totalPower, 4) : std::string()) + "," + std::to_string (s.frames);

            if (withReductions)
                csv += "," + (s.grSamples > 0 ? fixed (s.grWeighted / static_cast<double> (s.grSamples), 3) : std::string()) + ","
                     + (s.grBlocks > 0 ? fixed (s.grMax, 3) : std::string()) + "," + std::to_string (s.grBlocks) + ","
                     + std::to_string (s.grOverP95Cap);

            csv += "\n";
        }

        return csv;
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

        // ---- T-038 追加列（用了新选项时写在 metrics.csv 末尾，见 csvExtendedFields）----
        celadon::glaze::CommittedState listenCommitted;   // 第一遍（聆听）结束时已提交的版本：方案增益、目标、权重、画像、House 门控结果
        Status listenStatus;                               // 第一遍结束时的状态（House 锁定进度）
        int listenStartSample = 0;                         // 第一遍从哪个样本起播（--listen-from-bar）
        float effectiveCeilingDb = 0.0f;                   // 成品最后一块交给限幅器的有效 Ceiling
        float beatReleaseCapMaxMs = 0.0f;                  // 成品各块慢段释放上限的最大值（0 = 没封顶）
        bool hasGuardLimits = false;                       // 成品末尾的风格预算（保护层生效的约束上限）
        celadon::glaze::StyleConstants guardLimits;
        std::string segmentsFile;                          // --segments-csv：这一行的逐段统计文件（segments/ 下；写失败为空）
    };

    // ---- T-096d 重放覆盖的差信号（heat_ab.csv 一行的指标部分；算法见 measureDifference）----
    // 「母带」= 阳性对照 replay-copy 的对齐前成品（已证与自动链最大样本差 ≤ −180 dBFS），差信号 = 覆盖重放 − 母带，逐样本。
    struct DiffMetrics
    {
        double maxAbsDiff = 0.0;                    // 差信号最大样本绝对值（线性）
        bool hasRms = false;
        double rmsReMasterDb = 0.0;                 // 10·log10(Σ差² / Σ母带²)：两声道合计、不加权（差为 0 时 −inf）
        bool hasMasterLufs = false;
        double masterLufs = 0.0;                    // 母带整体响度（LoudnessMeter，与 metrics.csv 的 lufs 同一张表）
        bool hasLoudness = false;
        double gatedMasterLufs = 0.0;               // 门控时自己按 BS.1770-4 算出的母带整体响度（与 masterLufs 应差 < 0.1 LU：那张表按 0.1 LU 直方图）
        double kReMasterLu = 0.0;                   // K 加权响度差（LU）：母带门控选中的 400 ms 门块上 10·log10(Σ差 / Σ母带)
        bool hasWorstWindow = false;
        double worstWindowLu = 0.0;                 // 最坏 3 s 窗：过门的 3 s 窗里 10·log10(差 / 母带)（K 加权）的最大值
        double worstWindowStartSeconds = 0.0;       // 那个窗的起点（成品时间轴）
        std::array<bool, (std::size_t) celadon::BalanceModule::numBands> hasBand {};
        std::array<double, (std::size_t) celadon::BalanceModule::numBands> bandReMasterDb {};   // 27 段：差信号段能量 ÷ 母带同一段能量（dB）
        bool hasHighBand = false;
        double highBandRmsDbfs = 0.0;               // 17.8 kHz 以上（8 阶 Butterworth 高通）差信号的 RMS（dBFS，两声道合计）
        double highBandReMasterDb = 0.0;            // 同一能量 ÷ 母带全频能量（dB；MP3 解码的母带在 17.8 kHz 以上常常是空的，不拿同一段比）
        std::string naReason;                       // 算不出的项与原因（素材原因：母带静音、不够 3 s、采样率太低……；空 = 全都算出了）
    };

    // heat_ab.csv 的一行：一首歌 × 一组「自动」（GLAZE > 0）的覆盖对照。
    struct HeatAbRecord
    {
        Combo combo;                                // 同组「自动」的组合（GLAZE、风格、曲风、交付、聆听起点）
        std::string variantFile;                    // clay-replay-<覆盖> 的成品文件名（--metrics-only 或写失败时为空）
        std::string replayCheck;                    // 阳性对照 ok / mismatch / n/a（= metrics.csv 自动行的 replay_check）
        double replayMaxDifference = -1.0;          // 阳性对照的最大样本差（线性；< 0 = 没做）
        std::string trust;                          // ok / untrusted（阳性对照没过或不适用、参数核对没过、延迟对不上）/ noop（每块记录值都已等于覆盖值）
        bool fedParamsEqual = false;                // 交给两条重放链的参数：除覆盖的键外逐字段逐位相同，覆盖的键逐位等于覆盖值
        bool chainParamsEqual = false;              // 两条重放链实际交给模块的参数（getEffectiveParams）：同上
        std::string firstMismatch;                  // 不相同时第一处（块号与字段名）
        int blocks = 0;                             // 自动链第二遍的块数
        int overriddenBlocks = 0;                   // 记录值与覆盖值不同的块数（含 prepare 前那份；0 = 覆盖什么都没改）
        double heatMean = 0.0, heatMax = 0.0;       // 自动链逐块 effective.color.heat 的均值 / 最大（这首被移除的 Heat；手动 Heat 出厂 0，全是自动量）
        double heatZeroPercent = 0.0;               // 其中为 0 的块占比（%）：底量被撤回 / 守护削到 0 的那部分
        bool hasMetrics = false;                    // false：差信号各列留空（原因在 naReason）
        DiffMetrics metrics;
        std::string naReason;
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
        std::vector<HeatAbRecord> heatAb;      // T-096d：给了 --replay-override 时每组重放一行（写进 heat_ab.csv）
    };

    std::string orDash (bool has, const std::string& text)
    {
        return has ? text : std::string ("—");
    }

    std::string statusText (const Row& row)
    {
        if (row.combo.clay == ClayChoice::replayOverride)
            return "同决策重放（GLAZE 0，逐块照搬同组「自动」实际交给模块的参数，修坯照填，另覆盖 " + replayOverrideNames.text + "；T-096d）";

        if (row.combo.clay == ClayChoice::replayOff)
            return "同决策重放（GLAZE 0，逐块照搬同组「自动」实际交给模块的参数，修坯关）";

        std::string text = phaseName (row.status.phase);
        text += row.status.calibrationValid ? " · 校准有效" : " · 校准无效";

        if (row.combo.style == static_cast<int> (celadon::glaze::Style::automatic) && row.combo.glaze > 0.0f)
            text += std::string (" · 偏") + styleNames[static_cast<std::size_t> (row.status.resolvedStyle)];

        return text;
    }

    // 成品末尾哪几项「自动」修坯因代价被减量 / 否决（Status::clayCostLimitedMask）。空 = 没有。三处会置位：先试后用试出会抬高峰值
    // （ADR-012）、低切按守护混合损失封顶（ADR-027）、压力反馈降过档（ADR-014 / ADR-028）。
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
            // 低切、收单 Hz、收单量、加宽、反相提示 / 宽度保护、GLAZE 状态、成品文件：原始输入没有这些。
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

    // T-038 追加列的列名（用了新选项时接在 metrics.csv 表头末尾；与 csvExtendedFields 一一对应，--selftest-house 核对个数）。
    const char* csvExtendedHeader() noexcept
    {
        return "genre,delivery,bpm,meter,listen_from_bar,listen_start_s,listen_has_profile,plan_gain_db,plan_requested_gain_db,"
               "plan_requested_target_lufs,plan_effective_target_lufs,listen_calibration_valid,calibration_bias_db,weight_warm,"
               "weight_punchy,weight_clean,profile_lufs,profile_lra_lu,profile_low_share,profile_slope_db_per_oct,house_gate_valid,"
               "house_segment_mode,"
               "house_heard_segments,house_full_segments,house_heard_s,house_full_s,house_reopens,house_lock_pending,"
               "effective_ceiling_dbtp,beat_release_cap_ms,budget_limit_gr_mean_db,budget_limit_gr_p95_db,budget_limit_gr_peak_db,"
               "segments_file";
    }

    // T-038 追加列的值（含义见 README「T-038 追加列」）。方案 / 画像 / House 门控各列取第一遍（聆听）结束时已提交的版本——
    // 门槛 ①「锁定后的方案增益」比的就是它；有效 Ceiling、跟拍上限、风格预算取成品那一遍。重放行（GLAZE 0 的链）不填方案各列。
    std::vector<std::string> csvExtendedFields (const SongReport& report, const Row& row, const Options& options)
    {
        const auto empty = std::string();
        const auto render = ! row.isInput;
        const auto replay = render && isReplayChoice (row.combo.clay);
        const auto listened = render && ! replay;
        const auto& c = row.listenCommitted;
        const auto planned = listened && c.hasProfile;
        const auto& g = c.profile.houseGate;
        const auto gated = planned && g.valid;
        const auto transport = options.bpm > 0.0;

        const auto feature = [planned] (const celadon::glaze::Feature& f, int decimals)
        {
            return planned && f.valid ? fixed (f.value, decimals) : std::string();
        };

        return {
            render ? std::string (genreIds[static_cast<std::size_t> (row.combo.genre)]) : empty,
            render ? std::string (deliveryIds[static_cast<std::size_t> (row.combo.delivery)]) : empty,
            transport ? fixed (options.bpm, 3) : empty,
            transport ? std::to_string (options.meterNumerator) + "/" + std::to_string (options.meterDenominator) : empty,
            render ? std::to_string (row.combo.listenFromBar) : empty,
            listened && report.sampleRate > 0.0 ? fixed (static_cast<double> (row.listenStartSample) / report.sampleRate, 3) : empty,
            listened ? std::string (c.hasProfile ? "1" : "0") : empty,
            planned ? fixed (c.plan.effectiveGainDb, 3) : empty,
            planned ? fixed (c.plan.requestedGainDb, 3) : empty,
            planned ? fixed (c.plan.requestedTargetLufs, 3) : empty,
            planned ? fixed (c.plan.effectiveTargetLufs, 3) : empty,
            planned ? std::string (c.calibration.valid ? "1" : "0") : empty,
            planned ? fixed (c.calibration.gainBiasDb, 3) : empty,
            planned ? fixed (c.plan.weights.warm, 4) : empty,
            planned ? fixed (c.plan.weights.punchy, 4) : empty,
            planned ? fixed (c.plan.weights.clean, 4) : empty,
            feature (c.profile.integratedLufs, 3),
            feature (c.profile.loudnessRangeLu, 3),
            feature (c.profile.lowShare, 4),
            feature (c.profile.spectralSlopeDbPerOct, 3),
            planned ? std::string (g.valid ? "1" : "0") : empty,
            gated ? std::string (segmentModeId (g.mode)) : empty,
            gated ? std::to_string (g.heardSegments) : empty,
            gated ? std::to_string (g.fullSegments) : empty,
            gated ? fixed (g.heardSeconds, 1) : empty,
            gated ? fixed (g.fullSeconds, 1) : empty,
            gated ? std::to_string (g.reopenCount) : empty,
            listened ? std::string (row.listenStatus.houseLockPending ? "1" : "0") : empty,
            render ? fixed (row.effectiveCeilingDb, 3) : empty,
            render ? fixed (row.beatReleaseCapMaxMs, 2) : empty,
            render && row.hasGuardLimits ? fixed (row.guardLimits.limitGrMeanCapDb, 3) : empty,
            render && row.hasGuardLimits ? fixed (row.guardLimits.limitGrP95CapDb, 3) : empty,
            render && row.hasGuardLimits ? fixed (row.guardLimits.limitGrPeakCapDb, 3) : empty,
            row.segmentsFile,
        };
    }

    std::string csvRow (const SongReport& report, const Row& row, const Options& options)
    {
        const auto& m = row.measurement;
        const auto& s = row.status;
        const auto render = ! row.isInput;
        const auto replay = render && isReplayChoice (row.combo.clay);   // GLAZE 0 的重放链：GLAZE 状态列没有意义
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
            // T-048 阶段 2。confidence / current_mainness / cold_export / guard_cause / status_line 在 Status 有这些字段之前留空
            // （空 confidence：gate_check.py / house_gate.py 仍按 glaze_phase + calibration_valid，旧表输出不变）。
            // protection_db 先写现有的 g_保护。编排器合入后把前五列填上，门槛改看 c ≥ 0.5。
            empty,
            empty,
            empty,
            empty,
            render && ! replay ? fixed (s.protectionDb, 3) : empty,
            empty,
        };

        std::string line;

        for (std::size_t i = 0; i < fields.size(); ++i)
            line += (i > 0 ? "," : "") + csvField (fields[i]);

        // T-038：用了新选项才追加（默认运行的 metrics.csv 与改动前逐字节相同）。
        if (options.extended)
            for (const auto& field : csvExtendedFields (report, row, options))
                line += "," + csvField (field);

        return line;
    }

    std::string makeCsv (const std::vector<SongReport>& reports, const Options& options)
    {
        // 列名英文 snake_case（给脚本 / 表格软件用）；含义与 metrics.md 的「口径」一节相同。
        std::string csv = "kind,song,source_file,sample_rate,duration_s,glaze,clay,style,lufs,align_gain_db,true_peak_dbtp,"
                          "plr_db,limit_gr_mean_db,limit_gr_max_db,low_corr_lt125hz,infra_share_lt16hz_db,clay_lowcut_hz,"
                          "clay_mono_hz,clay_mono_amount,clay_width_db,phase_guard_pct,width_guard_pct,clay_cost_limited,glaze_phase,"
                          "calibration_valid,resolved_style,listen_phase,audio_hash,file,trial_lowcut_cost_db,trial_mono60_cost_db,"
                          "trial_mono80_cost_db,trial_mono120_cost_db,trial_mono160_cost_db,trial_width_cost_db,profile_sub_share_db,"
                          "profile_low_corr,trial_seconds,peak_guard_pct,peak_guard_min_gain,slow_inverted_pct,limit_input_peak_db,"
                          "clay_pressure_scale,clay_pressure_steps,replay_check,replay_max_diff_dbfs,music_seconds,trial_blend10_db,"
                          "trial_blend20_db,clay_blend_loss_db,clay_peak_cost_db,clay_peak_cost_s,"
                          "confidence,current_mainness,cold_export,guard_cause,protection_db,status_line";

        if (options.extended)
            csv += std::string (",") + csvExtendedHeader();

        csv += "\n";

        for (const auto& report : reports)
            for (const auto& row : report.rows)
                csv += csvRow (report, row, options) + "\n";

        return csv;
    }

    std::string definitionsMarkdown()
    {
        return "## 口径\n\n"
               "- **原始输入**：读入后立体声化（单声道复制成双声道、多声道取前两个）的原曲，用同一套量法，作对照。\n"
               "- **LUFS（对齐前）**：插件自己的 BS.1770-4 表（`celadon::LoudnessMeter`，与界面 OUT 表同一算法）量整段成品"
               "（已去掉链延迟、含 " + fixed (ringOutSeconds, 2) + " s 余振）。\n"
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
               "clayWidthDb（上一块交给 ClayModule 的实际值）；低切「关」= 0；收单量为 0 时收单 Hz 无意义（本工具不用手动收单，ADR-015 起收单 Hz 恒为 80、收单量恒为 0）。\n"
               "- **反相提示 / 宽度保护**：第二遍里反相提示亮着（收单灯琥珀；ADR-015 起只提示、不改声音；只在收单为「自动」的组计：auto、mono 组，其余组与重放行恒为 0）/ "
               "宽度保护介入的时长占比（只在加宽为「自动」的组计；每块读一次状态）。"
               "格末若有「，代价限：…」，是成品末尾因代价被减量 / 否决的「自动」修坯项（lowcut、width 或 lowcut+width；metrics.csv 的 clay_cost_limited）：先试后用试出会抬高峰值、低切按守护混合损失封顶、压力反馈降过档，三处任一都会置位。\n"
               "- **GLAZE 状态**：成品末尾的阶段与「校准有效」；风格为「自动」且 GLAZE > 0 时附此刻偏向的风格。"
               "第一遍（聆听）结束时的阶段在 metrics.csv 的 listen_phase 列。\n"
               "- **成品文件**：`<歌名>__g<GLAZE>_clay-<auto|off|replay-off>_<风格>.wav`。metrics.csv 的 audio_hash 是对齐前成品"
               "（原始输入行是立体声化的输入）的 FNV-1a 哈希：同一首歌、同一组设置跨版本哈希相同 = 逐位不变。\n"
               "- **同决策重放（clay-replay-off，ADR-016）**：每组 GLAZE > 0 的修坯「自动」之后，另起一条 GLAZE 0 的链，逐块照搬"
               "「自动」链那一块实际交给模块的全部参数（含自动增量、校准偏差、保护退让），只把修坯换成「关」。两条链的决策一模一样，"
               "差别只剩修坯本身——修坯门槛比的是「自动 vs 重放全关」，不再比两条各自跑 GLAZE、方案会分岔的链。阳性对照：修坯用"
               "「手动」照填自动链的实际值再重放一遍，应复现「自动」（最大样本差 ≤ −" + fixed (-toDbfs (replayTolerance), 0) + " dBFS，不要求逐位；metrics.csv 的 replay_check：ok / mismatch / n/a）。"
               "重放行的 GLAZE 状态列没有意义（写 replay / 空）。\n"
               "- **有效音乐**：原曲 K 加权 100 ms 子块在 −70 LUFS 绝对门以上的总时长（与校准器同一门限；歌尾静音不算），"
               "metrics.csv 的 music_seconds。confidence 有数时，门槛要求有效音乐 ≥ 30 s 的歌在成品末尾 c ≥ 0.5；"
               "格子还空着时仍要求「已锁定 + 校准有效」（阶段 1 编排器）。\n";
    }

    std::string makeMarkdown (const Options& options, const std::vector<Combo>& combos,
                              const std::vector<SongReport>& reports, const std::string& generatedAt)
    {
        std::string md = "# celadon_render 真歌对照指标\n\n";
        md += "- 生成时间：" + generatedAt + "（每做完一首歌重写一次本表）\n";
        md += "- 网格：" + gridText (options) + "，共 " + std::to_string (combos.size()) + " 组；宿主块长 "
            + std::to_string (options.blockSize) + " 样本；各文件按自己的采样率渲染\n";
        if (options.noAlign)
            md += "- 对齐：关闭（--no-align），写未经整体等响的 32-bit float 成品\n";
        else
            md += "- 对齐：成品按整体响度只乘增益对齐到 " + fixed (options.matchLufs, 1)
            + " LUFS；32-bit float，允许超过 0 dBFS，不再限幅\n";
        md += "- 流程：每组先完整聆听一遍（输出丢弃）→ 存 GLAZE 状态（createGlazeStateXml）→ 新链读回（restoreGlazeStateXml）"
              "后从头渲染第二遍作成品；丢掉开头的链延迟、末尾多冲 " + fixed (ringOutSeconds, 2) + " s 余振，成品与原曲逐样本对齐\n";
        md += "- 其余参数全是新实例默认（Ceiling -1.0 dBTP、Lookahead 2 ms、Limit Gain 0 等，见 Source/ParameterDefaults.h，"
              "插件参数表同一处真源）\n";
        md += "- 高级母带（新实例默认）：" + masteringText (makeFactoryParams().mastering)
            + "；两项「自动」的实际强度随 GLAZE 联动，GLAZE 0 时为 0\n";

        if (std::isfinite (options.streamingTargetLufs))
            md += "- 测量覆盖：流媒体基准目标 " + fixed (options.streamingTargetLufs, 1)
                + " LUFS，上下界同移；未改出厂值\n";

        // T-038：用了新选项才写这一行（默认运行的表头与改动前相同）。
        if (options.extended)
        {
            std::string genres, deliveries;

            for (std::size_t i = 0; i < options.genres.size(); ++i)
                genres += (i > 0 ? " / " : "") + std::string (genreNames[static_cast<std::size_t> (options.genres[i])]);

            for (std::size_t i = 0; i < options.deliveries.size(); ++i)
                deliveries += (i > 0 ? " / " : "") + std::string (deliveryNames[static_cast<std::size_t> (options.deliveries[i])]);

            md += "- T-038：曲风 " + genres + "、交付 " + deliveries + "（组合名里只写出非默认的）；宿主走带："
                + (options.bpm > 0.0 ? "模拟 " + fixed (options.bpm, 2) + " BPM、" + std::to_string (options.meterNumerator) + "/"
                                         + std::to_string (options.meterDenominator) + "（PPQ 从 0 起按样本推进；组合名带 from-bN 的组第一遍从第 N 小节起播）"
                                     : std::string ("不给拍点（同改动前）"))
                + (options.segmentsCsv ? std::string ("；逐段统计在 segments/（") + (options.bpm > 0.0 ? "一段 = 一小节" : "每 2 秒一段") + "）"
                                       : std::string())
                + "；metrics.csv 末尾另有 T-038 的列（README「T-038 追加列」）\n";
        }

        // T-048：给了 --live-trace 才写这一行（不给时表头与改动前相同）。
        if (options.liveTrace)
        {
            md += "- T-048 实时画像追踪：live/" + mdText (options.liveTag) + "/（每首歌 × 曲风 × 聆听起点一份，取第一次跑到的那一遍第一遍；主段模型 "
                + (options.liveModelOverride ? std::string (liveModelIds[static_cast<std::size_t> (options.liveModel)]) + "（--live-model）"
                                             : std::string ("按曲风：House 三类 4/4 型、通用响度型"))
                + "；常数 " + (options.liveConstantsText.empty() ? std::string ("出厂值") : "出厂值改 " + mdText (options.liveConstantsText))
                + (options.liveOnly ? std::string ("；--live-only：只跑第一遍，没有成品行") : std::string ("；追踪不改链的输出"))
                + "；README「T-048 实时画像追踪」）\n";
        }

        // T-096d：给了 --replay-override 才写这一行（不给时表头与改动前相同）。
        if (! options.replayOverrides.empty())
            md += "- T-096d 重放覆盖：每组 GLAZE > 0 的修坯「自动」之后另加一行 clay-" + mdText (replayOverrideNames.id)
                + "（同决策重放、修坯照填，prepare 前与每一块都另覆盖 " + mdText (replayOverrideNames.text)
                + "）；它与阳性对照 replay-copy 逐样本相减的差信号指标在 heat_ab.csv（docs/validation/2026-10-11-T096d-heat-ab/README.md）\n";

        md += "\n";
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
                      "| 真次声 < 16 Hz dB | 低切 Hz | 收单 Hz | 收单量 | 加宽 dB | 反相提示 / 宽度保护 | GLAZE 状态 | 成品文件 |\n";
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
                // T-038：同一曲风 / 交付 / 聆听起点的两组才成对（默认各只有一个取值，配对与改动前相同）。
                if (off.isInput || off.combo.glaze > 0.0f || off.combo.clay != ClayChoice::off
                    || off.combo.style != automatic.combo.style || off.combo.genre != automatic.combo.genre
                    || off.combo.delivery != automatic.combo.delivery || off.combo.listenFromBar != automatic.combo.listenFromBar)
                    continue;

                const auto prefix = std::string ("GLAZE 0 · 风格 ") + styleIds[static_cast<std::size_t> (automatic.combo.style)]
                                  + comboSuffix (automatic.combo, " · ") + "：修坯「自动」与「全关」两组成品";
                const auto note = automatic.measurement.hash == off.measurement.hash
                                    ? prefix + "逐位相同（阳性对照通过）"
                                    : prefix + "不同！违反设计规格-Clay 第 4 节「逐位直通」（GLAZE 0 时 Clay 自动值应为 0），请查";
                report.notes.push_back (note);
                std::cerr << "  " << note << "\n";
            }
        }
    }

    // T-038 --segments-csv：把一份逐段统计写到 <out>/segments/<baseName>.csv，返回相对 --out 的路径（写失败返回空、记备注、计失败）。
    std::string writeSegments (const Options& options, SongReport& report, const std::string& baseName, const SegmentReport& segments,
                               double sampleRate, bool withReductions, int& failures)
    {
        const auto dir = options.outputDir.getChildFile ("segments");
        const auto fileName = baseName + ".csv";

        if (! dir.createDirectory()
            || ! writeText (dir.getChildFile (juce::String::fromUTF8 (fileName.c_str())), segmentsCsvText (segments, sampleRate, withReductions)))
        {
            report.notes.push_back ("写逐段统计 segments/" + fileName + " 失败");
            ++failures;
            return {};
        }

        if (segments.droppedFrames)
            report.notes.push_back ("segments/" + fileName + "：读 Balance 分析帧时丢过帧（不该发生），低频占比少算了几帧");

        return "segments/" + fileName;
    }

    //==============================================================================
    // T-048 实时画像追踪的落盘与逐遍比较（每首歌一份 LiveTraceBook；口径见「T-048 实时画像追踪」一节）。

    // 一遍追踪落到哪一份：曲风 + 本遍起点小节（第二遍、重放都从第 1 小节起）。文件名基 = <歌名>__<曲风>[_from-b<N>]。
    std::string liveTraceBase (const std::string& song, int genre, int startBar)
    {
        return song + "__" + genreIds[static_cast<std::size_t> (genre)] + (startBar > 1 ? "_from-b" + std::to_string (startBar) : std::string());
    }

    juce::File liveTraceDirectory (const Options& options)
    {
        return options.outputDir.getChildFile ("live").getChildFile (juce::String::fromUTF8 (options.liveTag.c_str()));
    }

    // 常数原样写出：float 9 位、double 17 位有效数字（读回逐位相同；±∞ 写 inf / -inf）。
    std::string exactNumber (float value)
    {
        char text[48] = {};
        return std::snprintf (text, sizeof (text), "%.9g", static_cast<double> (value)) > 0 ? std::string (text) : std::string ("?");
    }

    std::string exactNumber (double value)
    {
        char text[48] = {};
        return std::snprintf (text, sizeof (text), "%.17g", value) > 0 ? std::string (text) : std::string ("?");
    }

    // <基>__final.csv（键,值）：这一遍的设定（歌、曲风、主段模型、标签、常数全部 37 项的实际值）、追踪的段数与位级摘要、finish 之后的
    // 诊断与画像（每个特征的值与 confidence；空 = 不知道）。只写决定追踪的量：取自网格里哪一组（GLAZE、风格……）、宿主块长都不写——
    // 追踪与它们无关，这样最快跑法与完整跑法的两份文件能逐字节比（取自哪一组写在 metrics.md 备注与 __passes.csv 里）。
    std::string liveFinalCsv (const Options& options, const SongReport& report, const LiveTraceResult& result, const Combo& combo,
                              int startBar, std::int64_t startSample)
    {
        std::string csv = "key,value\n";
        const auto add = [&csv] (const std::string& key, const std::string& value) { csv += csvField (key) + "," + csvField (value) + "\n"; };
        const auto flag = [] (bool on) { return std::string (on ? "1" : "0"); };
        const auto optional = [] (bool valid, double value, int decimals) { return valid ? fixed (value, decimals) : std::string(); };
        const auto feature = [&add] (const std::string& key, const celadon::glaze::Feature& f, int decimals)
        {
            add ("profile." + key, f.valid ? fixed (f.value, decimals) : std::string());
            add ("profile." + key + ".confidence", f.valid ? fixed (f.confidence, 4) : std::string());
        };

        const auto model = liveModelFor (options, combo.genre);
        add ("format", "2");   // 2：T-048 阶段 1 修订（回看重判）——多了 diag.window_segments 等与常数 newProgramRangeLu、publishDelaySeconds
        add ("song", report.name);
        add ("source_file", report.sourceFile);
        add ("sample_rate", fixed (report.sampleRate, 0));
        add ("duration_s", fixed (report.durationSeconds, 3));
        add ("genre", genreIds[static_cast<std::size_t> (combo.genre)]);
        add ("model", liveModelIds[static_cast<std::size_t> (model)]);
        add ("model_source", options.liveModelOverride ? "live-model" : "genre");
        add ("tag", options.liveTag);
        add ("constants_override", options.liveConstantsText);
        add ("bpm", options.bpm > 0.0 ? fixed (options.bpm, 3) : std::string());
        add ("meter", options.bpm > 0.0 ? std::to_string (options.meterNumerator) + "/" + std::to_string (options.meterDenominator) : std::string());
        add ("listen_from_bar", std::to_string (startBar));
        add ("pass_start_sample", std::to_string (startSample));
        add ("commits", std::to_string (result.rows.size()));
        add ("trace_digest", hex64 (result.digest.value));
        add ("anomaly_overrun", flag (result.overrun));
        add ("anomaly_mismatch", flag (result.mismatch));

        const auto& d = result.finalDiagnostics;
        add ("diag.commits", std::to_string (d.commits));
        add ("diag.reference_lufs", optional (d.referenceValid, d.referenceLufs, 4));
        add ("diag.low_reference", optional (d.lowReferenceValid, d.lowReference, 5));
        add ("diag.main_mass_s", fixed (d.mainMassSeconds, 4));
        add ("diag.confidence", fixed (d.confidence, 5));
        add ("diag.active_s", fixed (d.activeSeconds, 4));
        add ("diag.seconds_since_main", fixed (d.secondsSinceMain, 4));
        add ("diag.time_decay_active", flag (d.timeDecayActive));
        add ("diag.new_program_count", std::to_string (d.newProgramCount));
        add ("diag.window_segments", std::to_string (d.windowSegments));
        add ("diag.dropped_segments", std::to_string (d.droppedSegments));
        add ("diag.oldest_main_share", fixed (d.oldestMainShare, 6));
        add ("diag.oldest_reference_share", fixed (d.oldestReferenceShare, 6));
        add ("diag.publish_s", fixed (static_cast<double> (d.publishTime) / report.sampleRate, 4));

        const auto& p = result.finalProfile;
        feature ("integrated_lufs", p.integratedLufs, 4);
        feature ("loudness_range_lu", p.loudnessRangeLu, 4);
        feature ("plr_db", p.plrDb, 4);
        feature ("true_peak_dbtp", p.truePeakDbtp, 4);
        feature ("slope_db_per_oct", p.spectralSlopeDbPerOct, 4);
        feature ("low_share", p.lowShare, 5);
        feature ("high_share", p.highShare, 5);
        feature ("low_band_range_db", p.lowBandRangeDb, 4);
        feature ("onset_density_per_s", p.onsetDensityPerSecond, 4);
        feature ("onset_strength_db", p.onsetStrengthDb, 4);
        feature ("sub_share_db", p.subShareDb, 4);
        feature ("low_side_to_mid_db", p.lowSideToMidDb, 4);
        feature ("low_correlation", p.lowCorrelation, 4);
        feature ("high_side_to_mid_db", p.highSideToMidDb, 4);
        feature ("high_correlation", p.highCorrelation, 4);
        feature ("cost_lowcut_db", p.lowCutPeakCostDb, 4);

        for (std::size_t i = 0; i < p.monoPeakCostDb.size(); ++i)
            feature ("cost_mono" + std::to_string (static_cast<int> (celadon::glaze::clayMonoCandidateHz[i])) + "_db", p.monoPeakCostDb[i], 4);

        feature ("cost_width_db", p.widthPeakCostDb, 4);

        for (std::size_t i = 0; i < p.lowCutBlendLossDb.size(); ++i)
            feature ("blend" + std::to_string (static_cast<int> (celadon::glaze::clayBlendCandidateHz[i])) + "_db", p.lowCutBlendLossDb[i], 4);

        add ("profile.effective_s", fixed (p.effectiveSeconds, 4));
        add ("profile.representativeness", fixed (p.representativeness, 5));
        add ("profile.format_version", std::to_string (p.formatVersion));

        visitLiveConstantKeys (options.liveConstants, [&add] (const std::string& name, const auto& value, LiveDomain) { add ("const." + name, exactNumber (value)); });
        return csv;
    }

    // <基>__judgements.csv：finish 之后（回看重判到歌尾）窗口里每一段的最终判断——段落的「最终 m′」对照 T-038 离线门控（追踪里的 mainness
    // 是那一段第一次发布时的判断）。bar 同追踪（按歌曲的小节网格算段的中点，没有 --bpm 时空着）。
    std::string liveJudgementsCsv (const LiveTraceResult& result, const SegmentGrid& grid, std::int64_t passStart, double sampleRate)
    {
        std::string csv = "sequence,bar,start_sample,end_sample,start_s,end_s,mainness_final,weight_final_s\n";

        for (const auto& j : result.judgements)
        {
            std::string bar;

            if (grid.bars)
                bar = std::to_string (grid.indexOf (j.start + passStart + (j.end - j.start) / 2) + 1);

            csv += std::to_string (j.sequence) + "," + bar + "," + std::to_string (j.start) + "," + std::to_string (j.end) + ","
                 + fixed (static_cast<double> (j.start) / sampleRate, 4) + "," + fixed (static_cast<double> (j.end) / sampleRate, 4) + ","
                 + fixed (j.mainness, 5) + "," + fixed (j.weightSeconds, 5) + "\n";
        }

        return csv;
    }

    // 一遍追踪在 <歌名>__passes.csv 里的一行。
    struct LivePass
    {
        std::string pass;                    // listen（第一遍）/ render（第二遍）/ replay-copy、replay-off（同决策重放）
        std::string combo;                   // 组合标签
        int genre = 0;
        int startBar = 1;                    // 本遍从第几小节起送进链（第二遍、重放都是 1）
        std::size_t rows = 0;
        std::uint64_t digest = 0;
        bool written = false;                // 这一遍就是写下的那份
        bool compared = false;               // 有写下的那份可比
        std::size_t matching = 0;            // 与写下的那份开头逐字相同的行数
        std::size_t referenceRows = 0;       // 写下的那份的行数
        std::int64_t firstDifference = -1;   // 第一处不同的那一段的起点（样本；完全相同 = −1）
        std::string differingColumns;        // 第一处不同的那一行里不同的列（「;」连起来；段数不同而前缀全同时写 rows）
        bool digestDiffers = false;          // 各行逐字相同、位级摘要却不同（固定小数位看不出的末位差）
        bool overrun = false, mismatch = false;

        bool identical() const noexcept   { return firstDifference < 0 && ! digestDiffers; }
    };

    // 两行追踪（同一张表头 liveTraceHeader）里不同的列名，用「;」连起来：逐遍比较时一眼看出差在段的哪一项（有效时长？试算块数？）。
    std::string differingColumns (const std::string& a, const std::string& b)
    {
        const auto names = splitList (liveTraceHeader());
        const auto left = splitList (a), right = splitList (b);
        std::string out;

        for (std::size_t i = 0; i < std::max (left.size(), right.size()); ++i)
        {
            if (i < left.size() && i < right.size() && left[i] == right[i])
                continue;

            out += (out.empty() ? "" : ";") + (i < names.size() ? names[i] : "column" + std::to_string (i + 1));
        }

        return out;
    }

    struct LiveTraceBook
    {
        struct Written
        {
            int genre = 0, startBar = 1;
            std::string base, combo;
            std::vector<std::string> rows;
            std::vector<std::int64_t> starts;   // 各行段起点（比较时报第一处不同在哪）
            std::uint64_t digest = 0;           // 写下的那份的位级摘要
        };

        std::vector<Written> written;
        std::vector<LivePass> passes;
    };

    // 一遍追踪交账：第一遍（pass = listen）且这个（曲风、起点）还没写过 → 写成 <基>.csv 与 <基>__final.csv；否则与写下的那份逐行比较。
    // 第二遍与重放从不当「写下的那份」：输入尾部多了延迟与余振的补零，读档之后分析器也不学习（交来的段可能从第一段起就不同）。
    void offerLiveTrace (const Options& options, SongReport& report, LiveTraceBook& book, const LiveTracer& tracer, const std::string& pass,
                         const Combo& combo, int startBar, std::int64_t startSample, int& failures)
    {
        const auto& result = tracer.getResult();
        LivePass entry;
        entry.pass = pass;
        entry.combo = comboLabel (combo, " · ");
        entry.genre = combo.genre;
        entry.startBar = startBar;
        entry.rows = result.rows.size();
        entry.digest = result.digest.value;
        entry.overrun = result.overrun;
        entry.mismatch = result.mismatch;

        const auto found = std::find_if (book.written.begin(), book.written.end(), [&combo, startBar] (const LiveTraceBook::Written& w)
        {
            return w.genre == combo.genre && w.startBar == startBar;
        });

        if (found == book.written.end() && pass == "listen")
        {
            const auto dir = liveTraceDirectory (options);
            const auto base = liveTraceBase (report.name, combo.genre, startBar);
            const auto relative = "live/" + options.liveTag + "/" + base;
            auto text = liveTraceHeader() + "\n";

            for (const auto& row : result.rows)
                text += row + "\n";

            if (! dir.createDirectory()
                || ! writeText (dir.getChildFile (juce::String::fromUTF8 ((base + ".csv").c_str())), text)
                || ! writeText (dir.getChildFile (juce::String::fromUTF8 ((base + "__final.csv").c_str())),
                                liveFinalCsv (options, report, result, combo, startBar, startSample))
                || ! writeText (dir.getChildFile (juce::String::fromUTF8 ((base + "__judgements.csv").c_str())),
                                liveJudgementsCsv (result, tracer.getGrid(), startSample, report.sampleRate)))
            {
                report.notes.push_back ("写实时画像追踪 " + relative + ".csv / __final.csv / __judgements.csv 失败");
                ++failures;
            }
            else
            {
                report.notes.push_back ("实时画像追踪 " + relative + ".csv（" + std::to_string (result.rows.size()) + " 段，取自「"
                                        + entry.combo + "」的第一遍）");
            }

            LiveTraceBook::Written w;
            w.genre = combo.genre;
            w.startBar = startBar;
            w.base = base;
            w.combo = entry.combo;
            w.rows = result.rows;
            w.digest = result.digest.value;

            for (const auto& r : result.records)
                w.starts.push_back (r.start);

            book.written.push_back (std::move (w));
            entry.written = true;
        }
        else if (found != book.written.end())
        {
            entry.compared = true;
            entry.referenceRows = found->rows.size();
            const auto common = std::min (found->rows.size(), result.rows.size());
            std::size_t k = 0;

            while (k < common && found->rows[k] == result.rows[k])
                ++k;

            entry.matching = k;

            if (k < result.records.size())
                entry.firstDifference = result.records[k].start;
            else if (k < found->starts.size())
                entry.firstDifference = found->starts[k];

            if (k < common)
                entry.differingColumns = differingColumns (found->rows[k], result.rows[k]);
            else if (found->rows.size() != result.rows.size())
                entry.differingColumns = "rows";

            entry.digestDiffers = entry.firstDifference < 0 && found->digest != result.digest.value;
        }

        // 读提交落后、记录与快照配不上：工具自己的错，追踪不可信（不该发生）。
        if (result.overrun || result.mismatch)
        {
            const auto passName = pass == "listen" ? "第一遍" : pass == "render" ? "第二遍" : pass == "replay-copy" ? "重放（修坯手动照填）" : "重放（修坯关）";
            report.notes.push_back ("「" + entry.combo + "」" + passName + "的实时画像追踪"
                                    + (result.overrun ? "不完整（读提交落后超过环形容量）" : "配不上（提交记录与快照的序号或 I 不一致）") + "，请查");
            ++failures;
        }

        book.passes.push_back (std::move (entry));
    }

    // 一首歌做完：有过逐遍比较时写 live/<标签>/<歌名>__passes.csv，并把结论写进备注与 stderr。第一遍之间（只差 GLAZE / 风格 / 修坯 /
    // 交付）应逐行相同——不同就是「实时画像只看输入」不成立，单独记一条注意；第二遍与重放只报前缀。
    void writeLivePasses (const Options& options, SongReport& report, const LiveTraceBook& book, int& failures)
    {
        if (std::none_of (book.passes.begin(), book.passes.end(), [] (const LivePass& p) { return ! p.written; }))
            return;

        std::string csv = "pass,combo,genre,start_bar,commits,digest,written,compared,matching_rows,reference_rows,first_difference_sample,"
                          "first_difference_columns,digest_differs,overrun,mismatch\n";
        int sameListen = 0, differentListen = 0, sameOther = 0, prefixOther = 0, uncompared = 0;
        std::size_t shortestPrefix = std::numeric_limits<std::size_t>::max(), prefixOf = 0;

        for (const auto& p : book.passes)
        {
            csv += csvField (p.pass) + "," + csvField (p.combo) + "," + genreIds[static_cast<std::size_t> (p.genre)] + "," + std::to_string (p.startBar)
                 + "," + std::to_string (p.rows) + "," + hex64 (p.digest) + "," + (p.written ? "1" : "0") + "," + (p.compared ? "1" : "0") + ","
                 + (p.compared ? std::to_string (p.matching) : std::string()) + "," + (p.compared ? std::to_string (p.referenceRows) : std::string())
                 + "," + (p.compared && p.firstDifference >= 0 ? std::to_string (p.firstDifference) : std::string()) + ","
                 + csvField (p.differingColumns) + "," + (p.digestDiffers ? "1" : "0") + ","
                 + (p.overrun ? "1" : "0") + "," + (p.mismatch ? "1" : "0") + "\n";

            if (p.written)
                continue;

            if (! p.compared)
            {
                ++uncompared;
                continue;
            }

            const auto identical = p.identical();

            if (p.pass == "listen")
            {
                if (identical)
                    ++sameListen;
                else
                    ++differentListen;

                if (! identical)
                {
                    const auto note = "注意：「" + p.combo + "」第一遍的实时画像追踪与写下的那份不同（"
                                    + (p.digestDiffers ? std::string ("各行逐字相同、位级摘要不同")
                                                       : "前 " + std::to_string (p.matching) + " / " + std::to_string (p.referenceRows)
                                                             + " 段相同，第一处不同在第 " + std::to_string (p.firstDifference) + " 样本起的段："
                                                             + p.differingColumns)
                                    + "）——实时画像本应只看输入";
                    report.notes.push_back (note);
                    std::cerr << "  " << note << "\n";
                }
            }
            else
            {
                if (identical)
                    ++sameOther;
                else
                    ++prefixOther;

                if (! identical && p.matching < shortestPrefix)
                {
                    shortestPrefix = p.matching;
                    prefixOf = p.referenceRows;
                }
            }
        }

        const auto relative = "live/" + options.liveTag + "/" + report.name + "__passes.csv";
        const auto dir = liveTraceDirectory (options);

        if (! dir.createDirectory() || ! writeText (dir.getChildFile (juce::String::fromUTF8 ((report.name + "__passes.csv").c_str())), csv))
        {
            report.notes.push_back ("写 " + relative + " 失败");
            ++failures;
            return;
        }

        auto summary = "实时画像逐遍比较（" + relative + "）：其余第一遍 " + std::to_string (sameListen) + " 遍逐行相同、"
                     + std::to_string (differentListen) + " 遍不同；第二遍与重放 " + std::to_string (sameOther) + " 遍逐行相同、"
                     + std::to_string (prefixOther) + " 遍只有前缀相同";

        if (prefixOther > 0)
            summary += "（最短前缀 " + std::to_string (shortestPrefix) + " / " + std::to_string (prefixOf) + " 段）";

        if (uncompared > 0)
            summary += "；" + std::to_string (uncompared) + " 遍没有可比的第一遍";

        report.notes.push_back (summary);
        std::cerr << "  " << summary << "\n";
    }

    // 成品的逐段统计：对齐前的成品截到原曲长度（与输入同一条时间轴、同一张段表），再并上逐块限幅 GR。必须在 measureAlignWrite
    // 之前调（它原地乘对齐增益）。p95CapDb = 这一组的风格预算 limitGrP95CapDb（没有为 0：不数超出的块）。
    SegmentReport outputSegments (RenderResult& render, int numInput, double sampleRate, const SegmentGrid& grid, double p95CapDb)
    {
        const auto length = std::min (numInput, render.outputLength);
        const juce::AudioBuffer<float> finished (render.io.getArrayOfWritePointers(), 2, render.latencySamples, length);
        auto segments = measureSegments (finished, length, sampleRate, grid);
        addReductions (segments, render.reductions, p95CapDb);
        return segments;
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

        if (row.measurement.hasLoudness && ! options.noAlign)
        {
            row.aligned = true;
            row.alignGainDb = options.matchLufs - row.measurement.integratedLufs;
            finished.applyGain (static_cast<float> (std::pow (10.0, row.alignGainDb / 20.0)));
        }
        else if (! row.measurement.hasLoudness)
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

    //==============================================================================
    // T-096d 重放覆盖的 A/B 对照（--replay-override；T-093 REPORT §2.8 / R6-03：自动 Heat 底量到底听不听得见）。每组「自动」的同决策
    // 重放多跑一遍、多比一次：
    //   - 基准 replay-copy：修坯照填自动链的实际值——就是下面的阳性对照那一遍（已证与自动链最大样本差 ≤ −180 dBFS）；
    //   - 覆盖 replay-<覆盖>：同一份逐块参数、同样修坯照填，prepare 前那份与每一块都再把覆盖的键改成给定值（ReplayVariant）。
    // 两条都是 GLAZE 0 的链（编排器不决策，实际参数逐位等于喂进去的值），只差覆盖的键——逐块核对两边交给链的参数与链实际交给模块的
    // 参数（sameExceptOverrides）。覆盖 − 基准 = 这个参数这一份在成品里的贡献（差信号），在进程内量（measureDifference），不写差信号 wav。
    // 插件代码一行不改：color.heat = 0 走 ColorModule 现有的空闲跳过。

    constexpr double heatAbHighBandHz = 17800.0;          // 「17.8 kHz 以上」：Balance 27 段最高段（16 kHz）的 IEC 标称上沿（精确上沿
                                                          // 16 kHz × 2^(1/6) ≈ 17.96 kHz，落在 8 阶高通的过渡带里，两边不留缝）
    constexpr int integratedGatingSubBlocks = 4;          // BS.1770-4 门块 400 ms = 4 个 100 ms 子块（75 % 重叠：每个子块结尾起一块）
    constexpr double integratedAbsoluteGateLufs = -70.0;  // BS.1770-4 绝对门
    constexpr double integratedRelativeGateLu = -10.0;    // BS.1770-4 相对门

    // 一个标量字段逐位相同（float 的 +0 / −0、不同的 NaN 都算不同：核对要的是「交给模块的位」一模一样）。
    template <typename T>
    bool sameBits (const T& x, const T& y) noexcept
    {
        static_assert (std::is_scalar_v<T>, "只比标量字段：标量没有内部填充，memcmp 比的就是值的位");
        return std::memcmp (&x, &y, sizeof (T)) == 0;
    }

    // ChainParams 的全部叶子字段（数组逐元素）：visit (字段名, 数组下标或 −1, a 的这个字段, b 的这个字段)。字段名 = 成员路径（用宏从
    // 路径生成，名字与路径写不岔），与 replayOverrideKeys 的键同一写法。ChainParams 或它的子结构加了字段要在这里补一行：
    // --selftest 按各字段的偏移核对有没有漏（相邻两字段之间的空隙只能是对齐填充）。
    template <typename Visitor>
    void visitChainParamFields (const celadon::ChainParams& a, const celadon::ChainParams& b, Visitor&& visit)
    {
       #define CELADON_T096D_FIELD(path) visit (#path, -1, a.path, b.path)
        CELADON_T096D_FIELD (balance.recover);
        CELADON_T096D_FIELD (balance.tame);
        CELADON_T096D_FIELD (balance.brighten);
        CELADON_T096D_FIELD (balance.speed);
        CELADON_T096D_FIELD (balance.bypass);
        CELADON_T096D_FIELD (balance.automaticRecover);
        CELADON_T096D_FIELD (balance.automaticTame);
        CELADON_T096D_FIELD (balance.harshAmount);
        CELADON_T096D_FIELD (balance.transientPreserve);
        CELADON_T096D_FIELD (balance.targetAmount);
        CELADON_T096D_FIELD (balance.targetSlopeDbPerOct);
        CELADON_T096D_FIELD (balance.hasTargetReference);

        for (std::size_t k = 0; k < a.balance.targetReferenceDb.size(); ++k)
            visit ("balance.targetReferenceDb", static_cast<int> (k), a.balance.targetReferenceDb[k], b.balance.targetReferenceDb[k]);

        CELADON_T096D_FIELD (color.toneDb);
        CELADON_T096D_FIELD (color.glue);
        CELADON_T096D_FIELD (color.heat);
        CELADON_T096D_FIELD (color.mixPercent);
        CELADON_T096D_FIELD (color.outputDb);
        CELADON_T096D_FIELD (color.bypass);

        CELADON_T096D_FIELD (limit.gainDb);
        CELADON_T096D_FIELD (limit.ceilingDb);
        CELADON_T096D_FIELD (limit.releaseMs);
        CELADON_T096D_FIELD (limit.autoRelease);
        CELADON_T096D_FIELD (limit.lookaheadMs);
        CELADON_T096D_FIELD (limit.clip);
        CELADON_T096D_FIELD (limit.listen);
        CELADON_T096D_FIELD (limit.bypass);
        CELADON_T096D_FIELD (limit.beatReleaseCapMs);

        CELADON_T096D_FIELD (glazePercent);
        CELADON_T096D_FIELD (glazeStyle);
        CELADON_T096D_FIELD (glazeGenre);
        CELADON_T096D_FIELD (glazeDelivery);

        CELADON_T096D_FIELD (mastering.harshMode);
        CELADON_T096D_FIELD (mastering.harshAmount);
        CELADON_T096D_FIELD (mastering.transientPreserve);
        CELADON_T096D_FIELD (mastering.targetMode);
        CELADON_T096D_FIELD (mastering.targetAmount);
        CELADON_T096D_FIELD (mastering.codecMarginDb);
        CELADON_T096D_FIELD (mastering.dither);
        CELADON_T096D_FIELD (mastering.previewTargetLufs);
        CELADON_T096D_FIELD (mastering.harshResolved);
        CELADON_T096D_FIELD (mastering.targetResolved);
        CELADON_T096D_FIELD (mastering.targetSlopeResolvedSet);
        CELADON_T096D_FIELD (mastering.targetSlopeResolvedDbPerOct);

        CELADON_T096D_FIELD (claySettings.lowCutMode);
        CELADON_T096D_FIELD (claySettings.monoMode);
        CELADON_T096D_FIELD (claySettings.widthMode);
        CELADON_T096D_FIELD (claySettings.lowCutHz);
        CELADON_T096D_FIELD (claySettings.monoHz);
        CELADON_T096D_FIELD (claySettings.widthDb);
        CELADON_T096D_FIELD (claySettings.bypass);

        CELADON_T096D_FIELD (clay.lowCutHz);
        CELADON_T096D_FIELD (clay.monoHz);
        CELADON_T096D_FIELD (clay.monoAmount);
        CELADON_T096D_FIELD (clay.widthDb);
        CELADON_T096D_FIELD (clay.bypass);
       #undef CELADON_T096D_FIELD
    }

    std::string fieldName (const char* name, int index)
    {
        return index < 0 ? std::string (name) : std::string (name) + "[" + std::to_string (index) + "]";
    }

    // b 是不是「a 只改了覆盖的键」：覆盖的键 b 逐位等于覆盖值，其余每个字段 a、b 逐位相同。不是时把第一处写进 where（字段名）。
    bool sameExceptOverrides (const celadon::ChainParams& a, const celadon::ChainParams& b, const ReplayOverrides& overrides,
                              std::string& where)
    {
        auto same = true;

        visitChainParamFields (a, b, [&] (const char* name, int index, const auto& x, const auto& y)
        {
            if (! same)
                return;

            using Field = std::decay_t<decltype (x)>;
            const ReplayOverride* overridden = nullptr;

            for (const auto& o : overrides)
                if (index < 0 && std::strcmp (name, replayOverrideKeys[o.key].key) == 0)
                    overridden = &o;

            auto ok = false;

            if constexpr (std::is_same_v<Field, float>)
                ok = overridden != nullptr ? sameBits (y, overridden->value) : sameBits (x, y);
            else
                ok = overridden == nullptr && sameBits (x, y);   // 白名单的键都是 float 字段：非 float 字段不会被覆盖

            if (! ok)
            {
                same = false;
                where = fieldName (name, index) + (overridden != nullptr ? "（不等于覆盖值）" : "");
            }
        });

        return same;
    }

    // RBJ 二阶高通（与 makeLowpass 同一写法；1 + cos w0 写成 2·cos²(w0 / 2)）。
    Biquad makeHighpass (double cutoffHz, double q, double sampleRate) noexcept
    {
        const auto w0 = juce::MathConstants<double>::twoPi * cutoffHz / sampleRate;
        const auto cosHalf = std::cos (0.5 * w0);
        const auto onePlusCos = 2.0 * cosHalf * cosHalf;
        const auto alpha = std::sin (w0) / (2.0 * q);
        const auto a0 = 1.0 + alpha;

        Biquad filter;
        filter.b0 = 0.5 * onePlusCos / a0;
        filter.b1 = -onePlusCos / a0;
        filter.b2 = filter.b0;
        filter.a1 = -2.0 * std::cos (w0) / a0;
        filter.a2 = (1.0 - alpha) / a0;
        return filter;
    }

    // 8 阶 Butterworth 高通（四级二阶，Q 同 Lowpass8 的 butterworth8Q）。
    struct Highpass8
    {
        Highpass8 (double cutoffHz, double sampleRate) noexcept
            : stages { { makeHighpass (cutoffHz, butterworth8Q[0], sampleRate), makeHighpass (cutoffHz, butterworth8Q[1], sampleRate),
                         makeHighpass (cutoffHz, butterworth8Q[2], sampleRate), makeHighpass (cutoffHz, butterworth8Q[3], sampleRate) } }
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

    // K 加权均方 → LUFS（BS.1770：−0.691 + 10·log10）；≤ 0 时 −inf。
    double lufsOfMeanSquare (double meanSquare) noexcept
    {
        return meanSquare > 0.0 ? -0.691 + 10.0 * std::log10 (meanSquare) : -std::numeric_limits<double>::infinity();
    }

    // 能量比（dB）；分子为 0（差信号恰好为 0）时 −inf。调用方保证分母 > 0。
    double energyRatioDb (double numerator, double denominator) noexcept
    {
        return numerator > 0.0 ? 10.0 * std::log10 (numerator / denominator) : -std::numeric_limits<double>::infinity();
    }

    // dB 读数的文字：−inf 写「-inf」（同 metrics.csv 的 replay_max_diff_dbfs），否则固定小数位。
    std::string decibelText (double value, int decimals)
    {
        return std::isinf (value) && value < 0.0 ? std::string ("-inf") : fixed (value, decimals);
    }

    // 一段立体声音频的 K 加权 100 ms 子块：插件自己的表 LoudnessMeter（与 IN / OUT 表同一 K 加权，各声道之和），外加它的整体响度读数。
    // 按 4 s 一段送、每段读出新完成的子块（子块环只存 12.8 s，整段一次送完会丢前面的）；末尾不满 100 ms 的半截不计。
    struct KWeightedBlocks
    {
        std::vector<double> meanSquares;            // 非有限的子块按 0 计（两份成品都是有限的，不该出现）
        double subBlockSeconds = 0.1;
        float integratedLufs = celadon::LoudnessMeter::noReading;
        bool dropped = false;                       // 读丢过子块（不该发生：每段 4 s，远小于环的 12.8 s）
    };

    KWeightedBlocks kWeightedBlocks (const juce::AudioBuffer<float>& audio, double sampleRate)
    {
        KWeightedBlocks result;
        const auto numSamples = audio.getNumSamples();
        const auto chunk = static_cast<int> (std::llround (4.0 * sampleRate));
        celadon::LoudnessMeter meter;
        meter.prepare ({ sampleRate, static_cast<juce::uint32> (juce::jmax (1, chunk)), 2u });
        result.subBlockSeconds = static_cast<double> (meter.getSubBlockLength()) / sampleRate;
        std::array<celadon::LoudnessMeter::SubBlock, static_cast<std::size_t> (celadon::LoudnessMeter::subBlockRingCapacity)> blocks {};
        std::int64_t cursor = 0;

        for (int start = 0; start < numSamples; start += chunk)
        {
            const auto length = std::min (chunk, numSamples - start);
            const juce::AudioBuffer<float> part (const_cast<float* const*> (audio.getArrayOfReadPointers()), 2, start, length);
            meter.process (part);
            const auto read = meter.readSubBlocksSince (cursor, blocks.data(), static_cast<int> (blocks.size()));
            result.dropped = result.dropped || read < 0;

            for (int k = 0; k < std::abs (read); ++k)
            {
                const auto meanSquare = blocks[static_cast<std::size_t> (k)].meanSquare;
                result.meanSquares.push_back (std::isfinite (meanSquare) && meanSquare > 0.0 ? meanSquare : 0.0);
            }
        }

        result.integratedLufs = meter.getIntegratedLufs();
        return result;
    }

    // 以第 last 个子块结尾的 400 ms 门块的 K 加权均方（4 个子块的平均，同 LoudnessMeter）。调用方保证 last ≥ 3。
    double gatingBlockPower (const std::vector<double>& subBlocks, std::size_t last) noexcept
    {
        double sum = 0.0;

        for (std::size_t back = 0; back < static_cast<std::size_t> (integratedGatingSubBlocks); ++back)
            sum += subBlocks[last - back];

        return sum / static_cast<double> (integratedGatingSubBlocks);
    }

    // BS.1770-4 整体响度的门控，只按母带判：返回选中的门块（记门块最后一个子块的下标）与这些门块上母带的整体响度。
    // 差信号常在 −70 LUFS 以下，自己过不了绝对门；用母带选出的门块，差信号与母带在同一组时刻上比。
    struct IntegratedGating
    {
        std::vector<std::size_t> blocks;
        double lufs = 0.0;
    };

    IntegratedGating gateLikeIntegrated (const std::vector<double>& subBlocks)
    {
        IntegratedGating gating;
        std::vector<std::size_t> aboveAbsolute;
        double absoluteSum = 0.0;

        for (auto last = static_cast<std::size_t> (integratedGatingSubBlocks - 1); last < subBlocks.size(); ++last)
        {
            const auto power = gatingBlockPower (subBlocks, last);

            if (lufsOfMeanSquare (power) > integratedAbsoluteGateLufs)
            {
                aboveAbsolute.push_back (last);
                absoluteSum += power;
            }
        }

        if (aboveAbsolute.empty())
            return gating;

        const auto relativeGate = lufsOfMeanSquare (absoluteSum / static_cast<double> (aboveAbsolute.size())) + integratedRelativeGateLu;
        double keptSum = 0.0;

        for (const auto last : aboveAbsolute)
        {
            const auto power = gatingBlockPower (subBlocks, last);

            if (lufsOfMeanSquare (power) > relativeGate)
            {
                gating.blocks.push_back (last);
                keptSum += power;
            }
        }

        if (! gating.blocks.empty())
            gating.lufs = lufsOfMeanSquare (keptSum / static_cast<double> (gating.blocks.size()));

        return gating;
    }

    // 一段立体声音频在 Balance 27 段上的总能量：独立的 BalanceModule（参数全 0，只用它的分析；处理的是一份拷贝）逐帧给段功率，
    // 分析窗装满的帧（窗起点 ≥ 0，同 measureSegments）逐段求和。两段音频用同一套分析，段能量之比与 Balance 的归一化无关。
    // （measureSegments 的低频占比里有同样的读帧循环；本线不动它，免得与别的线在那一段撞车。）
    struct BandEnergy
    {
        std::array<double, (std::size_t) celadon::BalanceModule::numBands> energy {};
        bool dropped = false;                       // 读丢过帧（不该发生：每段只送 4096 个样本）
    };

    BandEnergy balanceBandEnergy (const juce::AudioBuffer<float>& audio, double sampleRate)
    {
        BandEnergy result;
        const auto numSamples = audio.getNumSamples();
        celadon::BalanceModule balance;
        constexpr int chunk = 4096;   // 同 measureSegments：每段至多 4096 / hop 帧，远小于帧环形缓冲的 64
        balance.prepare ({ sampleRate, static_cast<juce::uint32> (chunk), 2u });
        balance.setParams (celadon::BalanceModule::Params {});
        const auto window = static_cast<std::int64_t> (balance.getAnalysisWindowSamples());
        std::array<celadon::BalanceModule::FrameSnapshot, static_cast<std::size_t> (celadon::BalanceModule::frameRingCapacity)> snapshots {};
        juce::AudioBuffer<float> scratch (2, chunk);
        std::int64_t cursor = 0;

        for (int start = 0; start < numSamples; start += chunk)
        {
            const auto length = std::min (chunk, numSamples - start);
            scratch.setSize (2, length, false, false, true);

            for (int ch = 0; ch < 2; ++ch)
                scratch.copyFrom (ch, 0, audio, ch, start, length);

            balance.process (scratch);
            const auto read = balance.readFramesSince (cursor, snapshots.data(), static_cast<int> (snapshots.size()));
            result.dropped = result.dropped || read < 0;

            for (int f = 0; f < std::abs (read); ++f)
            {
                const auto& snapshot = snapshots[static_cast<std::size_t> (f)];

                if (snapshot.endSample - window < 0)
                    continue;   // 分析窗还没装满（同分析器与 measureSegments）

                for (std::size_t b = 0; b < result.energy.size(); ++b)
                    result.energy[b] += std::max (0.0, static_cast<double> (snapshot.bandPower[b]));
            }
        }

        return result;
    }

    // 差信号指标（各项定义见 DiffMetrics）。master / variant 是两份等长、逐样本对齐的对齐前成品（调用方核对过延迟与长度）。
    // 算不出的项（素材原因）不填，原因并进 naReason，绝不填 0。
    DiffMetrics measureDifference (const juce::AudioBuffer<float>& master, const juce::AudioBuffer<float>& variant, double sampleRate)
    {
        DiffMetrics m;
        std::vector<std::string> missing;
        const auto numSamples = std::min (master.getNumSamples(), variant.getNumSamples());

        if (numSamples <= 0)
        {
            m.naReason = "成品是空的";
            return m;
        }

        // 逐样本：差信号（double 相减后存 float——两份成品同量级，存 float 的相对误差约 6e-8，比要分辨的 −50 dB 级小几个数量级）、
        // 两份能量、17.8 kHz 以上的能量（高通吃 double 的差）。
        juce::AudioBuffer<float> difference (2, numSamples);
        const auto withHighBand = sampleRate > 2.0 * heatAbHighBandHz;
        double differenceEnergy = 0.0, masterEnergy = 0.0, highEnergy = 0.0;

        for (int ch = 0; ch < 2; ++ch)
        {
            const auto* x = master.getReadPointer (ch);
            const auto* y = variant.getReadPointer (ch);
            auto* d = difference.getWritePointer (ch);
            Highpass8 highpass (withHighBand ? heatAbHighBandHz : 0.25 * sampleRate, sampleRate);   // 没有这个频段时不用（截止频率只为构造合法）

            for (int i = 0; i < numSamples; ++i)
            {
                const auto delta = static_cast<double> (y[i]) - static_cast<double> (x[i]);
                d[i] = static_cast<float> (delta);
                m.maxAbsDiff = std::max (m.maxAbsDiff, std::abs (delta));
                differenceEnergy += delta * delta;
                masterEnergy += static_cast<double> (x[i]) * static_cast<double> (x[i]);

                if (withHighBand)
                {
                    const auto high = highpass.process (delta);
                    highEnergy += high * high;
                }
            }
        }

        if (! (masterEnergy > 0.0))
        {
            m.naReason = "母带静音：差信号的相对量都算不出";
            return m;
        }

        m.hasRms = true;
        m.rmsReMasterDb = energyRatioDb (differenceEnergy, masterEnergy);

        if (withHighBand)
        {
            m.hasHighBand = true;
            m.highBandRmsDbfs = energyRatioDb (highEnergy, 2.0 * static_cast<double> (numSamples));   // 两声道合计的均方 → dBFS
            m.highBandReMasterDb = energyRatioDb (highEnergy, masterEnergy);
        }
        else
        {
            missing.push_back ("采样率 ≤ 35.6 kHz，没有 17.8 kHz 以上的频段");
        }

        // K 加权：两份子块在同一子块网格上；整体按母带的门块比，最坏窗按母带的短期响度过门。
        const auto masterBlocks = kWeightedBlocks (master, sampleRate);
        const auto differenceBlocks = kWeightedBlocks (difference, sampleRate);

        if (masterBlocks.integratedLufs > celadon::LoudnessMeter::noReading)
        {
            m.hasMasterLufs = true;
            m.masterLufs = static_cast<double> (masterBlocks.integratedLufs);
        }

        if (masterBlocks.dropped || differenceBlocks.dropped || masterBlocks.meanSquares.size() != differenceBlocks.meanSquares.size())
        {
            missing.push_back ("K 加权子块读丢了（不该发生），K 加权两项不算");
        }
        else
        {
            const auto& masterSub = masterBlocks.meanSquares;
            const auto& differenceSub = differenceBlocks.meanSquares;
            const auto gating = gateLikeIntegrated (masterSub);

            if (gating.blocks.empty())
            {
                missing.push_back ("母带整体响度没有读数（全曲低于 −70 LUFS 或不到 400 ms），K 加权响度差不算");
            }
            else
            {
                double differenceSum = 0.0, masterSum = 0.0;

                for (const auto last : gating.blocks)
                {
                    differenceSum += gatingBlockPower (differenceSub, last);
                    masterSum += gatingBlockPower (masterSub, last);
                }

                m.hasLoudness = true;
                m.gatedMasterLufs = gating.lufs;
                m.kReMasterLu = energyRatioDb (differenceSum, masterSum);
            }

            // 最坏 3 s 窗：以每个子块结尾的 30 个子块（同短期响度 S）。母带这一窗要过「有效音频」门（≥ −70 LUFS，且不低于整体 − 20 LU，
            // 同 measureSegments 的门）：歌头歌尾的静音、淡出不算——那里两份都近乎 0，比值没有意义。
            const auto window = static_cast<std::size_t> (shortTermSubBlocks);

            if (masterSub.size() < window)
            {
                missing.push_back ("不够 3 s：没有 3 s 窗");
            }
            else
            {
                for (auto last = window - 1; last < masterSub.size(); ++last)
                {
                    double masterWindow = 0.0, differenceWindow = 0.0;

                    for (std::size_t back = 0; back < window; ++back)
                    {
                        masterWindow += masterSub[last - back];
                        differenceWindow += differenceSub[last - back];
                    }

                    const auto windowLufs = lufsOfMeanSquare (masterWindow / static_cast<double> (window));

                    if (! (windowLufs >= effectiveAbsoluteGateLufs) || (m.hasLoudness && windowLufs < m.gatedMasterLufs + effectiveRelativeGateLu))
                        continue;

                    const auto ratio = energyRatioDb (differenceWindow, masterWindow);

                    if (! m.hasWorstWindow || ratio > m.worstWindowLu)
                    {
                        m.hasWorstWindow = true;
                        m.worstWindowLu = ratio;
                        m.worstWindowStartSeconds = static_cast<double> (last + 1 - window) * masterBlocks.subBlockSeconds;
                    }
                }

                if (! m.hasWorstWindow)
                    missing.push_back ("没有过门的 3 s 窗（母带的 3 s 短期响度都低于 −70 LUFS 或整体响度 − 20 LU）");
            }
        }

        // 27 段：差信号段能量 ÷ 母带同一段能量。母带某段没有能量时那一段留空。
        const auto masterBands = balanceBandEnergy (master, sampleRate);
        const auto differenceBands = balanceBandEnergy (difference, sampleRate);

        if (masterBands.dropped || differenceBands.dropped)
        {
            missing.push_back ("读丢了 Balance 帧（不该发生），27 段不算");
        }
        else
        {
            auto emptyBands = 0;

            for (std::size_t b = 0; b < m.bandReMasterDb.size(); ++b)
            {
                m.hasBand[b] = masterBands.energy[b] > 0.0;

                if (m.hasBand[b])
                    m.bandReMasterDb[b] = energyRatioDb (differenceBands.energy[b], masterBands.energy[b]);
                else
                    ++emptyBands;
            }

            if (emptyBands > 0)
                missing.push_back (std::to_string (emptyBands) + " 个 1/3 倍频程段母带没有能量，留空");
        }

        for (std::size_t k = 0; k < missing.size(); ++k)
            m.naReason += (k > 0 ? "；" : "") + missing[k];

        return m;
    }

    // 重放行（全关 / 覆盖）从重放那一遍拿的统计：两种重放行同一组字段，表里的列口径一致。
    void takeReplayStats (const RenderResult& replay, Row& row)
    {
        row.grMeanDb = replay.grMeanDb;
        row.grMaxDb = replay.grMaxDb;
        row.phaseGuardPercent = replay.phaseGuardPercent;
        row.widthGuardPercent = replay.widthGuardPercent;
        row.peakGuardPercent = replay.peakGuardPercent;
        row.blendIntendedEnergy = replay.blendIntendedEnergy;
        row.blendOutputEnergy = replay.blendOutputEnergy;
        row.blendSamples = replay.blendSamples;
        row.peakGuardMinGain = replay.peakGuardMinGain;
        row.slowInvertedPercent = replay.slowInvertedPercent;
        row.limitInputPeakDb = replay.limitInputPeakDb;
        row.status = replay.status;
        row.effectiveCeilingDb = replay.effectiveCeilingDb;
        row.beatReleaseCapMaxMs = replay.beatReleaseCapMaxMs;
    }

    // 覆盖重放的基准：阳性对照那一遍 replay-copy（只在给了 --replay-override 时留下；不给时那一遍用完就放掉，同改动前）。
    struct ReplayBaseline
    {
        bool rendered = false;
        RenderResult result;
        EffectiveLog used;   // 那条重放链实际交给模块的参数（prepare 前一份 + 逐块）
    };

    // 自动链记录里覆盖值与记录值不同的份数（prepare 前那份 + 逐块）：0 = 覆盖什么都没改（color.heat=0 而这首这一组没有自动 Heat）。
    int countOverridden (const EffectiveLog& log, const ReplayOverrides& overrides)
    {
        const auto differs = [&overrides] (const celadon::ChainParams& p)
        {
            return std::any_of (overrides.begin(), overrides.end(), [&p] (const ReplayOverride& o)
            {
                return ! sameBits (replayOverrideKeys[o.key].get (p), o.value);
            });
        };

        return (differs (log.prepared) ? 1 : 0) + static_cast<int> (std::count_if (log.blocks.begin(), log.blocks.end(), differs));
    }

    // 自动链逐块 effective.color.heat 的均值 / 最大 / 为 0 的占比（%）：这首这一组实际被移除的 Heat（渲染器的手动 Heat 出厂 0，全是自动量）。
    void heatStats (const EffectiveLog& log, HeatAbRecord& record)
    {
        record.blocks = static_cast<int> (log.blocks.size());
        double sum = 0.0, peak = 0.0;
        int zero = 0;

        for (const auto& p : log.blocks)
        {
            const auto heat = static_cast<double> (p.color.heat);
            sum += heat;
            peak = std::max (peak, heat);
            zero += heat == 0.0 ? 1 : 0;
        }

        const auto count = static_cast<double> (juce::jmax<std::size_t> (1, log.blocks.size()));
        record.heatMean = sum / count;
        record.heatMax = peak;
        record.heatZeroPercent = log.blocks.empty() ? 0.0 : 100.0 * static_cast<double> (zero) / count;
    }

    // 两条重放链（基准 / 覆盖）的参数核对，两层都要「除覆盖的键外逐位相同、覆盖的键逐位等于覆盖值」：
    //   - 交给链的：每份记录（prepare 前 + 逐块）按两种变体算出的 replayParams；
    //   - 链实际交给模块的：两遍各自记下的 getEffectiveParams（GLAZE 0 的链应原样照搬；不照搬说明别的决策也跟着变了）。
    // 第一处不同写进 record.firstMismatch。
    void checkReplayParams (const EffectiveLog& automatic, const EffectiveLog& baselineUsed, const EffectiveLog& variantUsed,
                            const ReplayVariant& variant, HeatAbRecord& record)
    {
        const auto& overrides = *variant.overrides;
        std::string where;

        // 第 k 份的名字：0 = prepare 前那份，k ≥ 1 = 第 k − 1 块（块号从 0 起）。只在出错时拼字符串。
        const auto partName = [] (std::size_t k) { return k == 0 ? std::string ("prepare 前") : "第 " + std::to_string (k - 1) + " 块"; };
        const auto partOf = [] (const EffectiveLog& log, std::size_t k) -> const celadon::ChainParams& { return k == 0 ? log.prepared : log.blocks[k - 1]; };

        record.fedParamsEqual = true;

        for (std::size_t k = 0; k <= automatic.blocks.size(); ++k)
        {
            const auto& recorded = partOf (automatic, k);

            if (! sameExceptOverrides (replayParams (recorded, ReplayClay::copy), replayParams (recorded, variant), overrides, where))
            {
                record.fedParamsEqual = false;
                record.firstMismatch = "交给链的参数 " + partName (k) + " " + where;
                break;
            }
        }

        record.chainParamsEqual = baselineUsed.blocks.size() == variantUsed.blocks.size();

        if (! record.chainParamsEqual && record.firstMismatch.empty())
            record.firstMismatch = "两条重放链的块数不同（" + std::to_string (baselineUsed.blocks.size()) + " / "
                                 + std::to_string (variantUsed.blocks.size()) + "）";

        for (std::size_t k = 0; record.chainParamsEqual && k <= baselineUsed.blocks.size(); ++k)
        {
            if (! sameExceptOverrides (partOf (baselineUsed, k), partOf (variantUsed, k), overrides, where))
            {
                record.chainParamsEqual = false;

                if (record.firstMismatch.empty())
                    record.firstMismatch = "链实际交给模块的参数 " + partName (k) + " " + where;
            }
        }
    }

    // 一组「自动」之后的覆盖重放与差信号（只在给了 --replay-override 时调）：多一行 clay-replay-<覆盖>（成品照常量、对齐、写），
    // heat_ab.csv 多一行。replayCheck / replayMaxDifference = 这一组阳性对照的结论；baseline = 阳性对照那一遍（不适用时这里补跑）。
    // 出错规则（T-096 Q12）：阳性对照没过或不适用 → 这一行标 untrusted、照样量完；参数核对没过、延迟对不上 → untrusted 并计入
    // failures（返回非 0）；每块记录值都已等于覆盖值 → noop，差信号各列留空（不填 0）。都不中止整批。
    void replayWithOverrides (const Options& options, const Song& song, SongReport& report, const Combo& automaticCombo,
                              const std::string& replayCheck, double replayMaxDifference, const EffectiveLog& effectiveLog,
                              ReplayBaseline& baseline, double p95CapDb, int& failures, int& written, const Transport& transport,
                              const SegmentGrid& grid)
    {
        const auto notePrefix = comboLabel (automaticCombo, " ") + "：";
        const ReplayVariant variant (ReplayClay::copy, &options.replayOverrides);

        if (! baseline.rendered)
        {
            replayPass (song.audio, song.sampleRate, options.blockSize, effectiveLog, ReplayClay::copy, baseline.result, transport, false,
                        nullptr, &baseline.used);
            baseline.rendered = true;
        }

        Row row;
        row.combo = automaticCombo;
        row.combo.clay = ClayChoice::replayOverride;
        std::cerr << "      重放覆盖 " << replayOverrideNames.text << "：" << std::flush;

        RenderResult overridden;
        EffectiveLog overriddenUsed;
        replayPass (song.audio, song.sampleRate, options.blockSize, effectiveLog, variant, overridden, transport, options.segmentsCsv,
                    nullptr, &overriddenUsed);
        takeReplayStats (overridden, row);

        HeatAbRecord record;
        record.combo = automaticCombo;
        record.replayCheck = replayCheck;
        record.replayMaxDifference = replayMaxDifference;
        record.overriddenBlocks = countOverridden (effectiveLog, options.replayOverrides);
        heatStats (effectiveLog, record);
        checkReplayParams (effectiveLog, baseline.used, overriddenUsed, variant, record);

        const auto& base = baseline.result;
        const auto aligned = overridden.latencySamples == base.latencySamples && overridden.outputLength == base.outputLength
                          && ! overridden.latencyChanged && ! base.latencyChanged;
        std::vector<std::string> untrusted;

        if (replayCheck != "ok")
            untrusted.push_back ("阳性对照 " + (replayCheck.empty() ? std::string ("缺失") : replayCheck));

        if (! record.fedParamsEqual || ! record.chainParamsEqual)
        {
            untrusted.push_back ("参数核对没过（" + record.firstMismatch + "）");
            ++failures;
        }

        if (! aligned)
        {
            untrusted.push_back ("覆盖重放与基准的延迟 / 长度不同（不该发生：延迟不随参数变）");
            ++failures;
        }

        if (record.overriddenBlocks == 0)
        {
            record.naReason = "每块的记录值都已等于覆盖值（" + replayOverrideNames.text + "；color.heat=0 时即这首这一组没有自动 Heat），"
                              "差信号恒为 0，各列留空";
        }
        else if (aligned)
        {
            // 必须在 measureAlignWrite 之前量：它原地乘对齐增益。
            const juce::AudioBuffer<float> master (const_cast<float* const*> (base.io.getArrayOfReadPointers()), 2, base.latencySamples, base.outputLength);
            const juce::AudioBuffer<float> variantAudio (overridden.io.getArrayOfWritePointers(), 2, overridden.latencySamples, overridden.outputLength);
            record.metrics = measureDifference (master, variantAudio, song.sampleRate);
            record.hasMetrics = true;
            record.naReason = record.metrics.naReason;
        }

        record.trust = ! untrusted.empty() ? "untrusted" : (record.overriddenBlocks == 0 ? "noop" : "ok");

        if (! untrusted.empty())
        {
            std::string reasons = "不可信：";

            for (std::size_t k = 0; k < untrusted.size(); ++k)
                reasons += (k > 0 ? "、" : "") + untrusted[k];

            record.naReason = reasons + (record.naReason.empty() ? std::string() : "；" + record.naReason);
        }

        if (options.segmentsCsv)
            row.segmentsFile = writeSegments (options, report, withoutWavExtension (outputFileName (report.name, row.combo)),
                                              outputSegments (overridden, song.audio.getNumSamples(), song.sampleRate, grid, p95CapDb),
                                              song.sampleRate, true, failures);

        measureAlignWrite (options, song, report, overridden, row, failures, written);
        record.variantFile = row.fileName;

        if (overridden.nonFiniteBlocks > 0)
            report.notes.push_back (notePrefix + "重放覆盖链输出出现非有限值 " + std::to_string (overridden.nonFiniteBlocks) + " 块");

        report.rows.push_back (std::move (row));

        // 备注与 stderr：一句话的结论（数在 heat_ab.csv）。
        const auto& m = record.metrics;
        auto summary = notePrefix + "重放覆盖 " + replayOverrideNames.text + "（heat_ab.csv，" + record.trust + "）：";

        if (record.hasMetrics && m.hasLoudness)
            summary += "差信号 K 加权比母带 " + decibelText (m.kReMasterLu, 1) + " LU";

        if (record.hasMetrics && m.hasWorstWindow)
            summary += "（最坏 3 s 窗 " + decibelText (m.worstWindowLu, 1) + " LU，起点 " + fixed (m.worstWindowStartSeconds, 1) + " s）";

        summary += "；自动 Heat 均值 " + fixed (record.heatMean, 2) + " / 最大 " + fixed (record.heatMax, 2) + "，为 0 的块 "
                 + fixed (record.heatZeroPercent, 1) + " %";

        if (! record.naReason.empty())
            summary += "；" + record.naReason;

        report.notes.push_back (summary);
        std::cerr << "        " << summary << "\n";
        report.heatAb.push_back (std::move (record));
    }

    // heat_ab.csv（只在给了 --replay-override 时写；一行 = 一首歌 × 一组「自动」；列的含义见 docs/validation/2026-10-11-T096d-heat-ab/README.md）。
    std::string heatAbHeader()
    {
        std::string header = "song,source_file,sample_rate,glaze,style,genre,delivery,listen_from_bar,override,variant_file,trust,"
                             "replay_check,replay_max_diff_dbfs,fed_params_equal,chain_params_equal,first_mismatch,blocks,overridden_blocks,"
                             "heat_mean,heat_max,heat_zero_pct,master_lufs,diff_max_abs_dbfs,diff_rms_re_master_db,diff_k_re_master_lu,"
                             "diff_k_worst3s_lu,worst3s_start_s,diff_above_17k8_rms_dbfs,diff_above_17k8_re_master_db";

        for (int b = 0; b < celadon::BalanceModule::numBands; ++b)
            header += ",band_" + fixed (static_cast<double> (celadon::BalanceModule::getBandCentreHz (b)), 0) + "hz_re_master_db";

        return header + ",na_reason";
    }

    std::string heatAbRow (const SongReport& report, const HeatAbRecord& r)
    {
        const auto empty = std::string();
        const auto& m = r.metrics;
        const auto has = [&r] (bool item) { return r.hasMetrics && item; };
        const auto boolText = [] (bool value) { return std::string (value ? "true" : "false"); };

        std::vector<std::string> fields
        {
            report.name,
            report.sourceFile,
            fixed (report.sampleRate, 0),
            formatGlaze (r.combo.glaze),
            styleIds[static_cast<std::size_t> (r.combo.style)],
            genreIds[static_cast<std::size_t> (r.combo.genre)],
            deliveryIds[static_cast<std::size_t> (r.combo.delivery)],
            std::to_string (r.combo.listenFromBar),
            replayOverrideNames.text,
            r.variantFile,
            r.trust,
            r.replayCheck,
            r.replayMaxDifference < 0.0 ? empty : decibelText (toDbfs (r.replayMaxDifference), 1),
            boolText (r.fedParamsEqual),
            boolText (r.chainParamsEqual),
            r.firstMismatch,
            std::to_string (r.blocks),
            std::to_string (r.overriddenBlocks),
            fixed (r.heatMean, 3),
            fixed (r.heatMax, 3),
            fixed (r.heatZeroPercent, 2),
            has (m.hasMasterLufs) ? fixed (m.masterLufs, 3) : empty,
            r.hasMetrics ? decibelText (toDbfs (m.maxAbsDiff), 1) : empty,
            has (m.hasRms) ? decibelText (m.rmsReMasterDb, 3) : empty,
            has (m.hasLoudness) ? decibelText (m.kReMasterLu, 3) : empty,
            has (m.hasWorstWindow) ? decibelText (m.worstWindowLu, 3) : empty,
            has (m.hasWorstWindow) ? fixed (m.worstWindowStartSeconds, 1) : empty,
            has (m.hasHighBand) ? decibelText (m.highBandRmsDbfs, 3) : empty,
            has (m.hasHighBand) ? decibelText (m.highBandReMasterDb, 3) : empty,
        };

        for (std::size_t b = 0; b < m.bandReMasterDb.size(); ++b)
            fields.push_back (has (m.hasBand[b]) ? decibelText (m.bandReMasterDb[b], 3) : empty);

        fields.push_back (r.naReason);

        std::string line;

        for (std::size_t i = 0; i < fields.size(); ++i)
            line += (i > 0 ? "," : "") + csvField (fields[i]);

        return line;
    }

    std::string makeHeatAbCsv (const std::vector<SongReport>& reports)
    {
        auto csv = heatAbHeader() + "\n";

        for (const auto& report : reports)
            for (const auto& record : report.heatAb)
                csv += heatAbRow (report, record) + "\n";

        return csv;
    }

    // 这一组设置之后要不要做同决策重放（ADR-016）：GLAZE > 0 的修坯「自动」组，且没给 --no-replay。--replay-override 只在这些组之后
    // 多加一行；GLAZE 0 的组永远没有重放行（GLAZE 0 不决策，没有「决策」可照搬）。
    bool replaysAfter (const Options& options, const Combo& combo) noexcept
    {
        return options.replay && combo.clay == ClayChoice::automatic && combo.glaze > 0.0f;
    }

    // 同决策重放（ADR-016）：「自动」这一组的第二遍刚渲染完、逐块参数在 effectiveLog 里。
    //   ① 阳性对照（copy）：修坯用「手动」填自动链的实际值，成品应复现自动链（最大样本差 ≤ replayTolerance）
    //      → automatic.replayCheck = ok / mismatch / n/a，最大差记进 replayMaxDifference；
    //   ② 重放全关（off）：同样的参数、修坯关，出一行 clay-replay-off（门槛拿它与「自动」比，tools/render/gate_check.py）。
    // T-038：transport 与自动链第二遍相同；--segments-csv 时「重放全关」也写一份逐段统计（超预算块数按自动行的风格预算数）。
    // T-048：--live-trace 时两遍重放也接实时画像追踪，与第一遍写下的那份比较（liveBook）。
    // T-096d：给了 --replay-override 时 ① 那一遍留作基准，最后再加 ③ 覆盖重放一行与 heat_ab.csv 一行（replayWithOverrides）。
    void replayAutomatic (const Options& options, const Song& song, SongReport& report, Row& automatic,
                          const EffectiveLog& effectiveLog, const juce::AudioBuffer<float>& autoAudio,
                          int autoLatency, int& failures, int& written, const Transport& transport, const SegmentGrid& grid,
                          LiveTraceBook& liveBook)
    {
        const auto notePrefix = comboLabel (automatic.combo, " ") + "：";
        const auto& blocks = effectiveLog.blocks;
        const auto clayExpressible = clayExpressibleAsManual (effectiveLog.prepared.clay)
                                  && std::all_of (blocks.begin(), blocks.end(),
                                                  [] (const celadon::ChainParams& p) { return clayExpressibleAsManual (p.clay); });
        const auto masteringExpressible = masteringReplayable (effectiveLog.prepared.mastering)
                                       && std::all_of (blocks.begin(), blocks.end(),
                                                       [] (const celadon::ChainParams& p) { return masteringReplayable (p.mastering); });
        const auto p95CapDb = automatic.hasGuardLimits ? static_cast<double> (automatic.guardLimits.limitGrP95CapDb) : 0.0;
        const auto keepBaseline = ! options.replayOverrides.empty();   // T-096d
        ReplayBaseline baseline;

        if (clayExpressible && masteringExpressible)
        {
            RenderResult copy;
            const auto copyTrace = makeLiveTracer (options, automatic.combo.genre, song.sampleRate, transport);
            replayPass (song.audio, song.sampleRate, options.blockSize, effectiveLog, ReplayClay::copy, copy, transport, false, copyTrace.get(),
                        keepBaseline ? &baseline.used : nullptr);

            if (copyTrace != nullptr)
                offerLiveTrace (options, report, liveBook, *copyTrace, "replay-copy", automatic.combo, 1, 0, failures);

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

            if (keepBaseline)
            {
                baseline.result = std::move (copy);   // 上面的视图 finished 此后不再用
                baseline.rendered = true;
            }
        }
        else
        {
            automatic.replayCheck = "n/a";
            report.notes.push_back (notePrefix + "同决策重放的阳性对照不适用："
                                    + std::string (! clayExpressible ? "有块的修坯实际值「手动」态表达不了（收单比例不是 0 或 1，或低切 / 加宽越界；手动收单固定是 1）"
                                                                     : "目标频谱用了「参考」形状，重放链没有侧链参考"));
        }

        Row row;
        row.combo = automatic.combo;
        row.combo.clay = ClayChoice::replayOff;
        std::cerr << "      重放全关：" << std::flush;

        RenderResult off;
        const auto offTrace = makeLiveTracer (options, row.combo.genre, song.sampleRate, transport);
        replayPass (song.audio, song.sampleRate, options.blockSize, effectiveLog, ReplayClay::off, off, transport, options.segmentsCsv,
                    offTrace.get());

        if (offTrace != nullptr)
            offerLiveTrace (options, report, liveBook, *offTrace, "replay-off", row.combo, 1, 0, failures);

        takeReplayStats (off, row);

        if (options.segmentsCsv)
            row.segmentsFile = writeSegments (options, report, withoutWavExtension (outputFileName (report.name, row.combo)),
                                              outputSegments (off, song.audio.getNumSamples(), song.sampleRate, grid, p95CapDb),
                                              song.sampleRate, true, failures);

        measureAlignWrite (options, song, report, off, row, failures, written);

        // 注意：automatic 引用的是 report.rows 的最后一个元素，下面 push_back 之后它可能失效——此后不再用它。
        if (off.latencySamples != autoLatency || off.latencyChanged)
            report.notes.push_back (notePrefix + "重放链的延迟与自动链不同（不该发生：Clay 的 D + G 不随参数变），对照不可信");

        if (off.nonFiniteBlocks > 0)
            report.notes.push_back (notePrefix + "重放全关链输出出现非有限值 " + std::to_string (off.nonFiniteBlocks) + " 块");

        // T-096d：覆盖重放要用的「自动」行几项，push_back 之前取（之后 automatic 可能失效）。
        const auto automaticCombo = automatic.combo;
        const auto replayCheck = automatic.replayCheck;
        const auto replayMaxDifference = automatic.replayMaxDifference;
        report.rows.push_back (std::move (row));

        if (keepBaseline)
            replayWithOverrides (options, song, report, automaticCombo, replayCheck, replayMaxDifference, effectiveLog, baseline, p95CapDb,
                                 failures, written, transport, grid);
    }

    // 第一遍从哪个样本起播：--listen-from-bar N（N > 1，要 --bpm）= 第 N 小节的起点，超出歌尾时截到歌尾（什么都听不到）；否则从头。
    int listenStartFor (const Combo& combo, const Transport& transport, int numInput)
    {
        return combo.listenFromBar > 1 && transport.enabled
                   ? static_cast<int> (std::min<std::int64_t> (transport.barStartSample (combo.listenFromBar), numInput))
                   : 0;
    }

    // 这一遍第一遍的追踪落在哪个起点小节（文件名的 from-bN 与逐遍比较的键）：真从第 N 小节起播时是 N，否则 1。
    int listenStartBar (const Combo& combo, int listenStart) noexcept
    {
        return listenStart > 0 ? combo.listenFromBar : 1;
    }

    // --live-only（T-048 最快的追踪跑法）：每个（曲风、聆听起点）只跑一遍第一遍——取网格里第一个这样的组合的参数（实时画像与 GLAZE、风格、
    // 修坯、交付无关，跑哪一个都一样；GLAZE 0 最快：链不按保护层时隙切片）——出追踪；不出成品、不做重放，指标表只有原始输入行。
    void processSongLiveOnly (const Options& options, const std::vector<Combo>& combos, const Song& song, SongReport& report,
                              const Transport& transport, LiveTraceBook& liveBook, int& failures)
    {
        const auto numInput = song.audio.getNumSamples();
        std::vector<std::pair<int, int>> done;

        for (const auto& combo : combos)
        {
            const auto key = std::make_pair (combo.genre, combo.listenFromBar);

            if (std::find (done.begin(), done.end(), key) != done.end())
                continue;

            done.push_back (key);
            const auto label = comboLabel (combo, " ");
            const auto listenStart = listenStartFor (combo, transport, numInput);

            if (listenStart >= numInput && combo.listenFromBar > 1)
                report.notes.push_back (label + "：聆听起点第 " + std::to_string (combo.listenFromBar) + " 小节已超出歌曲长度，第一遍什么都没听到");

            std::cerr << "  [追踪] " << label << "：第一遍 " << std::flush;
            const auto started = nowSeconds();
            const auto tracer = makeLiveTracer (options, combo.genre, song.sampleRate, transport);
            listenPass (song.audio, song.sampleRate, options.blockSize, makeComboParams (combo), false, transport, listenStart, tracer.get());
            std::cerr << fixed (nowSeconds() - started, 1) << " s，" << tracer->getResult().rows.size() << " 段\n";
            offerLiveTrace (options, report, liveBook, *tracer, "listen", combo, listenStartBar (combo, listenStart), listenStart, failures);
        }
    }

    // 一首歌：原始输入对照行 + 网格里的每组设置（聆听 → 读回 → 成品 → 量 → 对齐 → 写）。
    void processSong (const Options& options, const std::vector<Combo>& combos, const Song& song,
                      SongReport& report, int& failures, int& written)
    {
        // T-038：模拟走带与分段网格按这首歌自己的采样率算（不给 --bpm 时 transport 关着，HostInfo 与改动前相同）。
        const auto transport = makeTransport (options, song.sampleRate);
        const auto grid = makeSegmentGrid (transport, song.sampleRate);
        const auto numInput = song.audio.getNumSamples();

        Row inputRow;
        inputRow.isInput = true;
        inputRow.measurement = measure (song.audio, song.sampleRate);

        // --segments-csv：输入端的逐段统计（门控阈值标定用；每首歌一份，与设置无关）。
        if (options.segmentsCsv)
            inputRow.segmentsFile = writeSegments (options, report, report.name + "__input",
                                                   measureSegments (song.audio, numInput, song.sampleRate, grid), song.sampleRate, false, failures);

        report.rows.push_back (inputRow);
        std::cerr << "  原始输入：" << loudnessText (inputRow.measurement) << "\n";

        // T-048：这首歌各遍的实时画像追踪（没有 --live-trace 时一直是空的）。
        LiveTraceBook liveBook;

        if (options.liveOnly)
        {
            processSongLiveOnly (options, combos, song, report, transport, liveBook, failures);
            return;
        }

        for (std::size_t c = 0; c < combos.size(); ++c)
        {
            const auto& combo = combos[c];
            const auto label = comboLabel (combo, " ");
            const auto params = makeComboParams (combo);

            Row row;
            row.combo = combo;

            std::cerr << "  [" << (c + 1) << "/" << combos.size() << "] " << label << "：聆听 " << std::flush;

            // ① 第一遍：完整聆听，输出丢弃，最后存 GLAZE 状态。--listen-from-bar N（N > 1）从第 N 小节起播到歌尾（宿主位置照实际样本给）。
            const auto listenStart = listenStartFor (combo, transport, numInput);

            if (listenStart >= numInput && combo.listenFromBar > 1)
                report.notes.push_back (label + "：聆听起点第 " + std::to_string (combo.listenFromBar) + " 小节已超出歌曲长度，第一遍什么都没听到");

            auto started = nowSeconds();
            const auto listenTrace = makeLiveTracer (options, combo.genre, song.sampleRate, transport);
            const auto listen = listenPass (song.audio, song.sampleRate, options.blockSize, params, options.trace, transport, listenStart,
                                            listenTrace.get());
            row.listenPhase = listen.status.phase;
            row.listenCommitted = listen.committed;
            row.listenStatus = listen.status;
            row.listenStartSample = listenStart;
            std::cerr << fixed (nowSeconds() - started, 1) << " s（第一遍末 " << phaseName (listen.status.phase)
                      << (listen.status.calibrationValid ? " · 校准有效" : "") << "）→ 成品 " << std::flush;

            if (listenTrace != nullptr)
                offerLiveTrace (options, report, liveBook, *listenTrace, "listen", combo, listenStartBar (combo, listenStart), listenStart, failures);

            // ② 第二遍：新链读回存档，从头渲染成品。GLAZE > 0 的「自动」组逐块记下实际参数，留给同决策重放（ADR-016）。
            started = nowSeconds();
            RenderResult render;
            const auto replay = replaysAfter (options, combo);
            EffectiveLog effectiveLog;
            const auto renderTrace = makeLiveTracer (options, combo.genre, song.sampleRate, transport);
            renderPass (song.audio, song.sampleRate, options.blockSize, params, listen.state.get(), options.trace, render,
                        replay ? &effectiveLog : nullptr, transport, options.segmentsCsv, renderTrace.get());

            if (renderTrace != nullptr)
                offerLiveTrace (options, report, liveBook, *renderTrace, "render", combo, 1, 0, failures);
            row.effectiveCeilingDb = render.effectiveCeilingDb;
            row.beatReleaseCapMaxMs = render.beatReleaseCapMaxMs;
            row.hasGuardLimits = render.hasGuardLimits;
            row.guardLimits = render.guardLimits;
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

            // --segments-csv：成品的逐段统计（对齐前；measureAlignWrite 会原地乘对齐增益，所以在它之前量）。
            if (options.segmentsCsv)
                row.segmentsFile = writeSegments (options, report, withoutWavExtension (outputFileName (report.name, combo)),
                                                  outputSegments (render, numInput, song.sampleRate, grid,
                                                                  render.hasGuardLimits ? static_cast<double> (render.guardLimits.limitGrP95CapDb) : 0.0),
                                                  song.sampleRate, true, failures);

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
            replayAutomatic (options, song, report, automatic, effectiveLog, autoAudio, autoLatency, failures, written, transport, grid,
                             liveBook);
        }

        writeLivePasses (options, report, liveBook, failures);
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

    // 同决策重放自检用的信号（--selftest 的重放阳性对照与 T-096d 覆盖对照共用）：两路不相关白噪声（峰值 −26 dBFS）加两声道同相的
    // 8 Hz 正弦（0.1），让自动低切有东西可切；20 s，够聆听（15 s）学完、方案过渡走到校准。
    juce::AudioBuffer<float> replaySelfTestSignal()
    {
        auto signal = testNoise (static_cast<int> (20.0 * selfTestRate), 0.05f);

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < signal.getNumSamples(); ++i)
                signal.setSample (ch, i, signal.getSample (ch, i) + testSine (8.0, i, 0.1));

        return signal;
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

    // 自检用：照命令行解析一遍（argv[0] 自动补上），只要结果与错误文字。
    ParseResult parseForSelfTest (std::vector<std::string> args, std::string& error)
    {
        args.insert (args.begin(), "celadon_render");
        std::vector<char*> argv;

        for (auto& arg : args)
            argv.push_back (arg.data());

        Options options;
        return parseArguments (static_cast<int> (argv.size()), argv.data(), options, error);
    }

    // T-096d 重放覆盖（--replay-override）的自检。正反两向都要有牙（踩坑：对照要先确认被测路径真被走到）：
    //   ① 解析：合法写法、九种非法写法；行标签与文件名；GLAZE 0 无重放行（replaysAfter 与命令行两层）；
    //   ② 参数核对器：覆盖 ChainParams 全部字段（按偏移核对）、白名单的键各对上一个 float 字段、抓得住单个字段（含数组元素）的改动；
    //   ③ 差信号指标的已知答案（合成信号，期望值与容差逐项写依据）；
    //   ④ 整条链（GLAZE 35「自动」，真歌同一套两遍流程）：自动 Heat > 0（负向对照有东西可比）→ 覆盖成 0 差信号非 0、两层参数核对为真、
    //      核对抓得住改动；覆盖值 = 记录值 → 与 replay-copy 逐位相同；prepare 前那份与每一块都被覆盖（BUG-012），且这一项有牙。
    void runReplayOverrideSelfTest (SelfTest& test)
    {
        const auto heatKey = static_cast<std::size_t> (std::find_if (replayOverrideKeys.begin(), replayOverrideKeys.end(),
                                                                    [] (const ReplayOverrideKey& k) { return std::string (k.key) == "color.heat"; })
                                                       - replayOverrideKeys.begin());
        const auto view = [] (RenderResult& r) { return juce::AudioBuffer<float> (r.io.getArrayOfWritePointers(), 2, r.latencySamples, r.outputLength); };

        // ---- ① 解析、标识、GLAZE 0 无重放行 ----
        {
            ReplayOverrides parsed, half;
            std::string error;
            const auto ok = parseReplayOverrides ("color.heat=0", parsed, error);
            const auto okHalf = parseReplayOverrides (" color.heat = 12.5 ", half, error);

            test.check (heatKey < replayOverrideKeys.size() && ok && parsed.size() == 1 && parsed[0].key == heatKey && parsed[0].value == 0.0f
                            && replayOverrideId (parsed) == "replay-heat0" && replayOverrideText (parsed) == "color.heat=0"
                            && okHalf && half.size() == 1 && half[0].value == 12.5f && replayOverrideId (half) == "replay-heat12.5",
                        "重放覆盖 解析与标识", "color.heat=0 → " + replayOverrideId (parsed) + " / " + replayOverrideText (parsed)
                                               + "；color.heat = 12.5 → " + replayOverrideId (half));

            const std::array<const char*, 9> bad { { "color.heat", "color.glue=0", "color.heat=101", "color.heat=-1", "color.heat=abc",
                                                     "color.heat=0,color.heat=1", "", "color.heat=nan", "color.heat=inf" } };
            std::string accepted;

            for (const auto* text : bad)
            {
                ReplayOverrides o;
                std::string e;

                if (parseReplayOverrides (text, o, e) || e.empty())
                    accepted += (accepted.empty() ? "「" : "、「") + std::string (text) + "」";
            }

            test.check (accepted.empty(), "重放覆盖 非法写法都报错",
                        accepted.empty() ? "缺「=」、不在白名单、越界（101 / −1）、不是数（abc / nan / inf）、重复的键、空值 九种都报错"
                                         : "竟然收下了 " + accepted);

            const auto saved = replayOverrideNames;
            replayOverrideNames = { replayOverrideId (parsed), replayOverrideText (parsed) };
            Combo combo;
            combo.glaze = 35.0f;
            combo.clay = ClayChoice::replayOverride;
            const auto file = outputFileName ("Song A", combo);
            const auto label = comboLabel (combo, " · ");
            replayOverrideNames = saved;

            test.check (file == "Song A__g35_clay-replay-heat0_auto.wav" && label == "g35 · clay-replay-heat0 · auto",
                        "重放覆盖 行标签与文件名", file + "、" + label);

            Options options;
            options.replayOverrides = parsed;
            Combo zero;
            zero.glaze = 0.0f;
            zero.clay = ClayChoice::automatic;
            auto on = zero;
            on.glaze = 35.0f;
            auto offCombo = on;
            offCombo.clay = ClayChoice::off;
            auto noReplay = options;
            noReplay.replay = false;

            std::string e1, e2, e3, e4, e5;
            const auto glazeZeroOnly = parseForSelfTest ({ "--in", "a", "--out", "b", "--glaze", "0", "--replay-override", "color.heat=0" }, e1);
            const auto withNoReplay = parseForSelfTest ({ "--in", "a", "--out", "b", "--glaze", "35", "--no-replay", "--replay-override", "color.heat=0" }, e2);
            const auto clayOffOnly = parseForSelfTest ({ "--in", "a", "--out", "b", "--glaze", "35", "--clay", "off", "--replay-override", "color.heat=0" }, e3);
            const auto twice = parseForSelfTest ({ "--in", "a", "--out", "b", "--glaze", "35", "--replay-override", "color.heat=0",
                                                   "--replay-override", "color.heat=0" }, e4);
            const auto mixed = parseForSelfTest ({ "--in", "a", "--out", "b", "--glaze", "0,35", "--replay-override", "color.heat=0" }, e5);

            test.check (! replaysAfter (options, zero) && replaysAfter (options, on) && ! replaysAfter (options, offCombo) && ! replaysAfter (noReplay, on)
                            && glazeZeroOnly == ParseResult::invalid && withNoReplay == ParseResult::invalid && clayOffOnly == ParseResult::invalid
                            && twice == ParseResult::invalid && mixed == ParseResult::run,
                        "GLAZE 0 无重放行",
                        "给了覆盖时 GLAZE 0 的自动组也不重放、GLAZE 35 自动组重放、修坯全关组与 --no-replay 不重放；命令行只有 GLAZE 0 / --no-replay / "
                        "只有修坯全关 / 给两次都报错，--glaze 0,35 收下（只在 35 那组之后加行）");
        }

        // ---- ② 参数核对器 ----
        // 覆盖全字段：按偏移排好各字段，相邻两字段之间的空隙只能是对齐填充（< 后一个字段的对齐），末尾空隙 < 整个结构的对齐；
        // 否则就是有字段没列进 visitChainParamFields（或列重了）。
        {
            const auto probe = makeFactoryParams();
            struct Span { std::size_t offset, size, align; std::string name; };
            std::vector<Span> spans;
            const auto* base = reinterpret_cast<const unsigned char*> (&probe);

            visitChainParamFields (probe, probe, [&] (const char* name, int index, const auto& x, const auto&)
            {
                using Field = std::decay_t<decltype (x)>;
                spans.push_back ({ static_cast<std::size_t> (reinterpret_cast<const unsigned char*> (&x) - base), sizeof (Field), alignof (Field),
                                   fieldName (name, index) });
            });

            std::sort (spans.begin(), spans.end(), [] (const Span& a, const Span& b) { return a.offset < b.offset; });
            std::size_t end = 0;
            std::string problem;

            for (const auto& s : spans)
            {
                if (problem.empty() && s.offset < end)
                    problem = s.name + " 与前一个字段重叠（列重了？）";
                else if (problem.empty() && s.offset - end >= s.align)
                    problem = s.name + " 前面空出 " + std::to_string (s.offset - end) + " 字节，比它的对齐 " + std::to_string (s.align)
                            + " 还大：有字段没列进 visitChainParamFields";

                end = std::max (end, s.offset + s.size);
            }

            if (problem.empty() && sizeof (celadon::ChainParams) - end >= alignof (celadon::ChainParams))
                problem = "结构末尾空出 " + std::to_string (sizeof (celadon::ChainParams) - end) + " 字节：有字段没列进 visitChainParamFields";

            test.check (problem.empty(), "参数核对覆盖 ChainParams 全部字段",
                        problem.empty() ? std::to_string (spans.size()) + " 个字段，共 " + std::to_string (sizeof (celadon::ChainParams))
                                            + " 字节，相邻空隙都只是对齐填充"
                                        : problem);

            // 白名单的每个键都对得上恰好一个 float 字段，get / set 读写的正是它（只改它：核对说「只差覆盖的键」，不带覆盖时说就是它不同）。
            std::string keyProblem;

            for (std::size_t k = 0; k < replayOverrideKeys.size(); ++k)
            {
                const auto& key = replayOverrideKeys[k];
                auto matches = 0;
                auto isFloat = false;

                visitChainParamFields (probe, probe, [&] (const char* name, int index, const auto& x, const auto&)
                {
                    if (index < 0 && std::strcmp (name, key.key) == 0)
                    {
                        ++matches;
                        isFloat = std::is_same_v<std::decay_t<decltype (x)>, float>;
                    }
                });

                const auto value = key.get (probe) == static_cast<float> (key.maxValue) ? static_cast<float> (key.minValue)
                                                                                     : static_cast<float> (key.maxValue);
                auto changed = probe;
                key.set (changed, value);
                std::string whereWith, whereWithout;
                const ReplayOverrides one { { k, value } };
                const auto onlyThat = sameExceptOverrides (probe, changed, one, whereWith);
                const auto seen = ! sameExceptOverrides (probe, changed, {}, whereWithout) && whereWithout == key.key;

                if (matches != 1 || ! isFloat || ! onlyThat || ! seen || key.get (changed) != value)
                    keyProblem += std::string (keyProblem.empty() ? "" : "；") + key.key + "（对上 " + std::to_string (matches) + " 个字段"
                                + (isFloat ? "" : "、不是 float") + (onlyThat ? "" : "、set 改了别的字段：" + whereWith)
                                + (seen ? "" : "、不带覆盖时报的是「" + whereWithout + "」") + "）";
            }

            test.check (keyProblem.empty(), "白名单的键都对得上 ChainParams 的一个字段",
                        keyProblem.empty() ? std::to_string (replayOverrideKeys.size()) + " 个键（" + knownReplayOverrideKeys() + "）各对上一个 float 字段，set 只改它"
                                           : keyProblem);

            // 抓得住别的字段（标量、数组元素）的改动；覆盖的键不等于覆盖值也抓得住；只改了覆盖的键时说相同。
            const ReplayOverrides heat0 { { heatKey, 0.0f } };
            auto gain = probe;
            gain.limit.gainDb = probe.limit.gainDb + 0.5f;
            auto reference = probe;
            reference.balance.targetReferenceDb[26] = probe.balance.targetReferenceDb[26] + 1.0f;
            auto cleared = probe;
            cleared.color.heat = 0.0f;
            auto clearedAndClip = cleared;
            clearedAndClip.limit.clip = probe.limit.clip + 3.0f;
            auto notCleared = probe;
            notCleared.color.heat = 5.0f;
            std::string whereGain, whereReference, whereClip, whereHeat, whereSame;
            const auto caught = ! sameExceptOverrides (probe, gain, {}, whereGain) && whereGain == "limit.gainDb"
                             && ! sameExceptOverrides (probe, reference, {}, whereReference) && whereReference == "balance.targetReferenceDb[26]"
                             && ! sameExceptOverrides (probe, clearedAndClip, heat0, whereClip) && whereClip == "limit.clip"
                             && ! sameExceptOverrides (probe, notCleared, heat0, whereHeat) && whereHeat == "color.heat（不等于覆盖值）"
                             && sameExceptOverrides (probe, cleared, heat0, whereSame);

            test.check (caught, "参数核对抓得住单个字段的改动",
                        "改 Limit Gain → " + whereGain + "；改参考曲线第 27 段 → " + whereReference + "；覆盖 Heat 时另改 Clip → " + whereClip
                            + "；Heat 不等于覆盖值 → " + whereHeat + "；只改 Heat 为覆盖值 → 相同");
        }

        // ---- ③ 差信号指标的已知答案 ----
        // 母带 = 两声道 1 kHz 正弦（0.5），48 kHz。覆盖版 = 母带 × (1 + g) 时差 = g × 母带：K 加权、1/3 倍频程都是线性时不变，所以
        // 不加权 RMS、K 加权（整体与任何一个 3 s 窗）、1 kHz 段的「差 ÷ 母带」都 = 20·log10(g)；g = 10^(−60/20) → −60 dB。
        // 覆盖版存成 float 的舍入（≤ 3e-8）相对差（5e-4 量级）只差 0.001 dB 级，容差 0.01 dB。
        {
            const auto samples = [] (double seconds) { return static_cast<int> (seconds * selfTestRate); };
            const auto tone = [] (int i) { return testSine (1000.0, i, 0.5); };
            const auto master = testSignal (samples (10.0), tone, tone);
            const auto scaledBy = [] (const juce::AudioBuffer<float>& source, const auto& factor)
            {
                juce::AudioBuffer<float> scaled (2, source.getNumSamples());

                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < source.getNumSamples(); ++i)
                        scaled.setSample (ch, i, static_cast<float> (static_cast<double> (source.getSample (ch, i)) * (1.0 + factor (i))));

                return scaled;
            };
            const auto within = [] (double value, double expected, double tolerance) { return std::abs (value - expected) <= tolerance; };

            const auto constant = measureDifference (master, scaledBy (master, [] (int) { return 0.001; }), selfTestRate);
            const auto band1k = static_cast<std::size_t> (14);   // 第 14 段 = 1 kHz（BalanceModule：referenceBand）
            test.check (constant.hasRms && within (constant.rmsReMasterDb, -60.0, 0.01) && constant.hasLoudness && within (constant.kReMasterLu, -60.0, 0.01)
                            && constant.hasWorstWindow && within (constant.worstWindowLu, -60.0, 0.01) && constant.hasBand[band1k]
                            && within (constant.bandReMasterDb[band1k], -60.0, 0.01) && within (toDbfs (constant.maxAbsDiff), 20.0 * std::log10 (0.5e-3), 0.01)
                            && constant.hasHighBand && constant.highBandRmsDbfs < -120.0
                            && constant.hasMasterLufs && std::abs (constant.gatedMasterLufs - constant.masterLufs) <= 0.1,
                        "差信号 恒定比例 −60 dB",
                        "RMS " + fixed (constant.rmsReMasterDb, 4) + " / K 加权 " + fixed (constant.kReMasterLu, 4) + " / 最坏 3 s 窗 "
                            + fixed (constant.worstWindowLu, 4) + " / 1 kHz 段 " + fixed (constant.bandReMasterDb[band1k], 4) + " dB（期望都是 −60 ± 0.01）；"
                            + "最大样本差 " + fixed (toDbfs (constant.maxAbsDiff), 3) + " dBFS（期望 −66.021）；17.8 kHz 以上 "
                            + decibelText (constant.highBandRmsDbfs, 1) + " dBFS（期望 < −120：1 kHz 的差过 8 阶高通约 −260 dB，剩下的是 float 舍入）；"
                            + "门控母带 " + fixed (constant.gatedMasterLufs, 3) + " / 表 " + fixed (constant.masterLufs, 3) + " LUFS（差 ≤ 0.1）");

            // 17.8 kHz 以上：覆盖版 = 母带 + 1e-3·sin(20 kHz)。差只有 20 kHz 这一个正弦：每声道均方 0.5e-6 × |H(20 kHz)|²，
            // |H|² = 1 / (1 + (tan(π·fc/fs) / tan(π·f/fs))^16)（8 阶 Butterworth 经双线性变换、在 fc 预畸变）≈ 0.99939；
            // 期望 RMS = 10·log10(0.5e-6 · |H|²) ≈ −63.013 dBFS，相对母带全频（均方 0.125）≈ −53.982 dB。10 s 正好整数个周期，高通起振可忽略，容差 0.05 dB。
            const auto high = [] (int i) { return static_cast<float> (1.0e-3 * std::sin (juce::MathConstants<double>::twoPi * 20000.0 * i / selfTestRate)); };
            juce::AudioBuffer<float> withHigh (2, master.getNumSamples());

            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < master.getNumSamples(); ++i)
                    withHigh.setSample (ch, i, master.getSample (ch, i) + high (i));

            const auto highBand = measureDifference (master, withHigh, selfTestRate);
            const auto ratio = std::tan (juce::MathConstants<double>::pi * heatAbHighBandHz / selfTestRate)
                             / std::tan (juce::MathConstants<double>::pi * 20000.0 / selfTestRate);
            const auto passPower = 1.0 / (1.0 + std::pow (ratio, 16.0));
            const auto expectedHigh = 10.0 * std::log10 (0.5e-6 * passPower);
            const auto expectedHighReMaster = 10.0 * std::log10 (0.5e-6 * passPower / 0.125);
            test.check (highBand.hasHighBand && within (highBand.highBandRmsDbfs, expectedHigh, 0.05) && within (highBand.highBandReMasterDb, expectedHighReMaster, 0.05),
                        "差信号 17.8 kHz 以上",
                        fixed (highBand.highBandRmsDbfs, 3) + " dBFS（期望 " + fixed (expectedHigh, 3) + " ± 0.05）、相对母带 "
                            + fixed (highBand.highBandReMasterDb, 3) + " dB（期望 " + fixed (expectedHighReMaster, 3) + " ± 0.05）");

            // 最坏 3 s 窗与静音门：16 s，前 4 s 两份都静音（这些窗母带短期响度 −∞，过不了门、不能拿 0 / 0 当数），之后母带 1 kHz 正弦；
            // 差 = 1e-3 × 母带，只有 [8 s, 13 s) 是 1e-2 × 母带（−40 dB）。完全落在这 5 s 里的窗起点在 8.0 ～ 10.0 s，比值都是 −40；
            // 别的窗是 −40 与 −60 的混合，低于 −40。期望最坏窗 −40 ± 0.01 LU、起点在 [8.0, 10.0] s（子块网格 0.1 s）；整首在 −60 与 −40 之间。
            const auto gated = [&tone] (int i) { return i < static_cast<int> (4.0 * selfTestRate) ? 0.0f : tone (i); };
            const auto withSilence = testSignal (samples (16.0), gated, gated);
            const auto louder = scaledBy (withSilence, [] (int i)
            {
                return i >= static_cast<int> (8.0 * selfTestRate) && i < static_cast<int> (13.0 * selfTestRate) ? 0.01 : 0.001;
            });
            const auto worst = measureDifference (withSilence, louder, selfTestRate);
            test.check (worst.hasWorstWindow && within (worst.worstWindowLu, -40.0, 0.01) && worst.worstWindowStartSeconds >= 7.95
                            && worst.worstWindowStartSeconds <= 10.05 && worst.hasLoudness && worst.kReMasterLu > -60.0 && worst.kReMasterLu < -40.0,
                        "差信号 最坏 3 s 窗与静音门",
                        "最坏窗 " + fixed (worst.worstWindowLu, 4) + " LU（期望 −40 ± 0.01），起点 " + fixed (worst.worstWindowStartSeconds, 1)
                            + " s（期望 8.0 ～ 10.0），整首 " + fixed (worst.kReMasterLu, 3) + " LU（期望在 −60 与 −40 之间）");

            // 门控与插件的表一致：6 s 正弦 0.5 之后 6 s 正弦 0.05（低 20 dB，过不了 −10 LU 相对门）。自己按 BS.1770-4 门控算的母带整体响度
            // 应与 LoudnessMeter 的读数差 ≤ 0.1 LU（那张表按 0.1 LU 直方图积分）；差 = 1e-3 × 母带，K 加权响度差仍是 −60。
            const auto twoLevels = [] (int i) { return testSine (1000.0, i, i < static_cast<int> (6.0 * selfTestRate) ? 0.5 : 0.05); };
            const auto stepped = testSignal (samples (12.0), twoLevels, twoLevels);
            const auto gatedLevels = measureDifference (stepped, scaledBy (stepped, [] (int) { return 0.001; }), selfTestRate);
            test.check (gatedLevels.hasLoudness && gatedLevels.hasMasterLufs && std::abs (gatedLevels.gatedMasterLufs - gatedLevels.masterLufs) <= 0.1
                            && within (gatedLevels.kReMasterLu, -60.0, 0.01),
                        "差信号 门控与 LoudnessMeter 一致",
                        "门控母带 " + fixed (gatedLevels.gatedMasterLufs, 3) + " / 表 " + fixed (gatedLevels.masterLufs, 3) + " LUFS（差 ≤ 0.1）；K 加权 "
                            + fixed (gatedLevels.kReMasterLu, 4) + " LU（期望 −60 ± 0.01）");

            // 差为 0（覆盖什么都没改时就是这样）：各项是 −inf（文字 -inf），不是 0、不是空。
            const auto none = measureDifference (master, master, selfTestRate);
            test.check (none.maxAbsDiff == 0.0 && none.hasRms && std::isinf (none.rmsReMasterDb) && none.hasLoudness && std::isinf (none.kReMasterLu)
                            && none.kReMasterLu < 0.0 && none.hasWorstWindow && std::isinf (none.worstWindowLu) && decibelText (none.kReMasterLu, 3) == "-inf",
                        "差信号 恰好为 0", "RMS / K 加权 / 最坏窗都是 " + decibelText (none.kReMasterLu, 3));

            // 素材原因算不出：2 s 的歌没有 3 s 窗；32 kHz 的歌没有 17.8 kHz 以上——留空并写原因，其余照算。
            const auto shortMaster = testSignal (samples (2.0), tone, tone);
            const auto tooShort = measureDifference (shortMaster, scaledBy (shortMaster, [] (int) { return 0.001; }), selfTestRate);
            constexpr double lowRate = 32000.0;
            const auto lowTone = [] (int i) { return static_cast<float> (0.5 * std::sin (juce::MathConstants<double>::twoPi * 1000.0 * i / lowRate)); };
            const auto lowMaster = testSignal (static_cast<int> (5.0 * lowRate), lowTone, lowTone);
            const auto lowRateDifference = measureDifference (lowMaster, scaledBy (lowMaster, [] (int) { return 0.001; }), lowRate);
            test.check (! tooShort.hasWorstWindow && tooShort.naReason.find ("不够 3 s") != std::string::npos && tooShort.hasLoudness
                            && ! lowRateDifference.hasHighBand && lowRateDifference.naReason.find ("17.8 kHz") != std::string::npos
                            && within (lowRateDifference.kReMasterLu, -60.0, 0.01),
                        "差信号 算不出的项留空写原因",
                        "2 s：" + tooShort.naReason + "；32 kHz：" + lowRateDifference.naReason);
        }

        // ---- ④ 整条链 ----
        {
            const auto signal = replaySelfTestSignal();
            Combo combo;
            combo.glaze = 35.0f;
            combo.clay = ClayChoice::automatic;
            const auto params = makeComboParams (combo);
            const auto listen = listenPass (signal, selfTestRate, 512, params);
            RenderResult render;
            EffectiveLog effectiveLog;
            renderPass (signal, selfTestRate, 512, params, listen.state.get(), false, render, &effectiveLog);

            HeatAbRecord stats;
            heatStats (effectiveLog, stats);
            const auto autoHeat = stats.heatMax > 0.0;
            test.check (autoHeat, "覆盖对照有东西可比（自动 Heat > 0）",
                        "GLAZE 35 自动链 " + std::to_string (stats.blocks) + " 块，Heat 均值 " + fixed (stats.heatMean, 3) + " / 最大 "
                            + fixed (stats.heatMax, 3) + "，为 0 的块 " + fixed (stats.heatZeroPercent, 1) + " %（最大要 > 0，否则下面的负向对照没有牙）");

            // 负向：覆盖成 0 → 成品变了、差信号量得出数；两层参数核对为真。
            const ReplayOverrides zero { { heatKey, 0.0f } };
            const ReplayVariant heat0 (ReplayClay::copy, &zero);
            RenderResult copy, cleared;
            EffectiveLog copyUsed, clearedUsed;
            replayPass (signal, selfTestRate, 512, effectiveLog, ReplayClay::copy, copy, Transport {}, false, nullptr, &copyUsed);
            replayPass (signal, selfTestRate, 512, effectiveLog, heat0, cleared, Transport {}, false, nullptr, &clearedUsed);
            const auto copyAudio = view (copy);
            const auto clearedAudio = view (cleared);
            const auto lengthsMatch = copy.latencySamples == cleared.latencySamples && copy.outputLength == cleared.outputLength;
            const auto difference = lengthsMatch ? maxAbsDifference (copyAudio, clearedAudio) : 1.0;
            const auto metrics = measureDifference (copyAudio, clearedAudio, selfTestRate);
            test.check (autoHeat && lengthsMatch && difference > 0.0 && hashAudio (copyAudio) != hashAudio (clearedAudio) && metrics.hasLoudness
                            && std::isfinite (metrics.kReMasterLu) && metrics.kReMasterLu < 0.0 && metrics.hasWorstWindow && std::isfinite (metrics.worstWindowLu),
                        "覆盖 color.heat=0 让差信号非 0",
                        "最大样本差 " + decibelText (toDbfs (difference), 1) + " dBFS；差信号 K 加权比母带 " + decibelText (metrics.kReMasterLu, 2)
                            + " LU、最坏 3 s 窗 " + decibelText (metrics.worstWindowLu, 2) + " LU、RMS " + decibelText (metrics.rmsReMasterDb, 2) + " dB");

            HeatAbRecord record;
            checkReplayParams (effectiveLog, copyUsed, clearedUsed, heat0, record);
            test.check (record.fedParamsEqual && record.chainParamsEqual, "两条重放链除 color.heat 外逐位相同",
                        record.firstMismatch.empty() ? "交给链的与链实际交给模块的参数，prepare 前 + " + std::to_string (copyUsed.blocks.size())
                                                           + " 块都只差 color.heat（覆盖的那份逐位是 0）"
                                                     : record.firstMismatch);

            auto tampered = clearedUsed;

            if (! tampered.blocks.empty())
                tampered.blocks[tampered.blocks.size() / 2].limit.gainDb += 0.5f;

            HeatAbRecord tamperedRecord;
            checkReplayParams (effectiveLog, copyUsed, tampered, heat0, tamperedRecord);
            test.check (! tamperedRecord.chainParamsEqual && tamperedRecord.firstMismatch.find ("limit.gainDb") != std::string::npos,
                        "参数核对抓得住链里别的字段变了", "把覆盖链中间一块的 Limit Gain 改 0.5 dB → " + tamperedRecord.firstMismatch);

            // 正向（覆盖值 = 记录值 → 与 replay-copy 逐位相同）：每份记录的 Heat 都是 v（v = 自动链的最大 Heat），覆盖成 v。
            const auto v = static_cast<float> (stats.heatMax);
            auto flat = effectiveLog;
            flat.prepared.color.heat = v;

            for (auto& p : flat.blocks)
                p.color.heat = v;

            const ReplayOverrides toV { { heatKey, v } };
            const ReplayVariant keepV (ReplayClay::copy, &toV);
            RenderResult flatCopy, flatKept;
            replayPass (signal, selfTestRate, 512, flat, ReplayClay::copy, flatCopy);
            replayPass (signal, selfTestRate, 512, flat, keepV, flatKept);
            const auto flatHash = hashAudio (view (flatCopy));
            const auto keptSame = flatKept.latencySamples == flatCopy.latencySamples && flatKept.outputLength == flatCopy.outputLength
                               && hashAudio (view (flatKept)) == flatHash;
            test.check (autoHeat && keptSame, "覆盖值 = 记录值时与 replay-copy 逐位相同",
                        std::string (keptSame ? "逐位相同" : "不同") + "（记录与覆盖的 Heat 都是 " + fixed (v, 3) + "）");

            // BUG-012：记录里 Heat 全是 0（prepare 前那份也是），覆盖成 v → 必须与「每份都是 v」的 replay-copy 逐位相同。
            // 这一项有牙：只把 prepare 前那份改成 0、逐块仍是 v 的 replay-copy 与它不同（模块 prepare 时落在 0 上，开头有一段 Heat 平滑）——
            // 漏覆盖 prepare 前那份就会被上一句抓到。
            auto zeroLog = flat;
            zeroLog.prepared.color.heat = 0.0f;

            for (auto& p : zeroLog.blocks)
                p.color.heat = 0.0f;

            auto preparedOnly = flat;
            preparedOnly.prepared.color.heat = 0.0f;
            RenderResult lifted, preparedCopy;
            replayPass (signal, selfTestRate, 512, zeroLog, keepV, lifted);
            replayPass (signal, selfTestRate, 512, preparedOnly, ReplayClay::copy, preparedCopy);
            const auto liftedSame = lifted.latencySamples == flatCopy.latencySamples && lifted.outputLength == flatCopy.outputLength
                                 && hashAudio (view (lifted)) == flatHash;
            const auto preparedMatters = hashAudio (view (preparedCopy)) != flatHash;
            test.check (autoHeat && liftedSame && preparedMatters, "prepare 前那份与每一块都被覆盖（BUG-012）",
                        std::string (liftedSame ? "记录全是 0、覆盖成 v：与每份都是 v 的 replay-copy 逐位相同" : "记录全是 0、覆盖成 v：与每份都是 v 的不同")
                            + (preparedMatters ? "；只有 prepare 前是 0 的 replay-copy 确实不同（这一项有牙）"
                                               : "；只有 prepare 前是 0 的 replay-copy 竟然也相同（这一项没牙，查 Heat 的 prepare 落点）"));
        }
    }

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
            const auto signal = replaySelfTestSignal();

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
                                                  : "最大样本差 " + fixed (toDbfs (difference), 1) + " dBFS（允许 ≤ −" + fixed (-toDbfs (replayTolerance), 0) + " dBFS）"))
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

        runReplayOverrideSelfTest (test);   // T-096d

        if (test.failed == 0)
            std::cerr << "自检全部通过\n";
        else
            std::cerr << "自检有 " << test.failed << " 项失败\n";

        return test.failed == 0 ? 0 : 1;
    }

    //==============================================================================
    // T-038 新选项的自检（--selftest-house）：模拟走带、分段网格与逐段统计、曲风 / 交付 / 走带穿到链上的阳性对照、文件名与追加列。
    // 与 --selftest 分开：那一份的项目与输出保持原样；render-linux.sh 渲染前先跑它、再跑这一份（T-050），改了下面几项相关的代码也可单独跑。

    // 自检用：真歌同一套两遍流程（listenPass → renderPass，块长 512）跑一组设置，返回对齐前成品的哈希；render 里留着整段结果。
    std::uint64_t selfTestRender (const juce::AudioBuffer<float>& signal, const Combo& combo, const Transport& transport, RenderResult& render)
    {
        const auto params = makeComboParams (combo);
        const auto listen = listenPass (signal, selfTestRate, 512, params, false, transport);
        renderPass (signal, selfTestRate, 512, params, listen.state.get(), false, render, nullptr, transport);
        const juce::AudioBuffer<float> finished (render.io.getArrayOfWritePointers(), 2, render.latencySamples, render.outputLength);
        return hashAudio (finished);
    }

    double selfTestTruePeak (RenderResult& render)
    {
        const juce::AudioBuffer<float> finished (render.io.getArrayOfWritePointers(), 2, render.latencySamples, render.outputLength);
        const auto m = measure (finished, selfTestRate);
        return m.hasTruePeak ? m.truePeakDbtp : -1000.0;
    }

    int runHouseSelfTest()
    {
        SelfTest test;

        Transport tempo120;   // 120 BPM / 48 kHz：一拍正好 24000 样本、一小节（4/4）96000 样本 = 2 s = 20 个 100 ms 子块，边界上的 PPQ 是精确值
        tempo120.enabled = true;
        tempo120.bpm = 120.0;
        tempo120.sampleRate = selfTestRate;

        // ---- 模拟走带（makeHostInfo；同 PluginProcessor::readHostInfo 的各项）----
        // 96000 × 120 / (60 × 48000) = 4 与 300000 × 120 / 2880000 = 12.5 都是精确的双精度运算，所以按 == 比；前一样本 95999 的 PPQ
        // 3.99996 还在第 1 小节。6/8 = 每小节 3 个四分音符：第 72000 样本 PPQ 3、小节起点 3。不给 --bpm：四类拍点字段都是「没有」，
        // 其余（离线、在播、位置、循环）照旧——与改动前逐位相同的那一份。
        {
            const auto atBar2 = makeHostInfo (96000, tempo120);
            const auto before = makeHostInfo (95999, tempo120);
            const auto inside = makeHostInfo (300000, tempo120);
            const auto fieldsOk = atBar2.hasTempo && atBar2.bpm == 120.0 && atBar2.hasPpq && atBar2.hasBarStart && atBar2.hasTimeSignature
                               && atBar2.timeSignatureNumerator == 4 && atBar2.timeSignatureDenominator == 4 && atBar2.offline
                               && atBar2.isPlaying && atBar2.hasPosition && atBar2.positionSamples == 96000;

            test.check (fieldsOk && atBar2.ppqPosition == 4.0 && atBar2.ppqLastBarStart == 4.0 && before.ppqLastBarStart == 0.0
                            && inside.ppqPosition == 12.5 && inside.ppqLastBarStart == 12.0,
                        "模拟走带 120 BPM 4/4",
                        "第 96000 样本 PPQ " + fixed (atBar2.ppqPosition, 6) + " / 小节起点 " + fixed (atBar2.ppqLastBarStart, 6)
                            + "，第 95999 样本小节起点 " + fixed (before.ppqLastBarStart, 6) + "，第 300000 样本 PPQ "
                            + fixed (inside.ppqPosition, 6) + " / 小节起点 " + fixed (inside.ppqLastBarStart, 6) + "（期望 4 / 4、0、12.5 / 12）");

            auto compound = tempo120;
            compound.numerator = 6;
            compound.denominator = 8;
            const auto sixEight = makeHostInfo (72000, compound);

            test.check (compound.quartersPerBar() == 3.0 && sixEight.ppqPosition == 3.0 && sixEight.ppqLastBarStart == 3.0
                            && sixEight.timeSignatureNumerator == 6 && sixEight.timeSignatureDenominator == 8,
                        "模拟走带 6/8", "每小节 " + fixed (compound.quartersPerBar(), 3) + " 拍，第 72000 样本 PPQ "
                                        + fixed (sixEight.ppqPosition, 6) + " / 小节起点 " + fixed (sixEight.ppqLastBarStart, 6) + "（期望 3、3 / 3）");

            const auto none = makeHostInfo (96000);
            test.check (! none.hasTempo && ! none.hasPpq && ! none.hasBarStart && ! none.hasTimeSignature && none.offline && none.isPlaying
                            && none.hasPosition && none.positionSamples == 96000 && none.hasLoop && ! none.looping,
                        "不给 --bpm 时不给拍点", none.hasTempo || none.hasPpq || none.hasBarStart || none.hasTimeSignature
                                                     ? std::string ("却给了拍点字段") : std::string ("四类拍点字段都没有，其余同改动前"));
        }

        // ---- 分段网格：124 BPM 4/4 每小节 92903.23 样本；600 个段起点各归本段、前一样本归上一段；不给 --bpm 时每 2 秒一段 ----
        {
            auto tempo124 = tempo120;
            tempo124.bpm = 124.0;
            const auto grid = makeSegmentGrid (tempo124, selfTestRate);
            const auto seconds = makeSegmentGrid (Transport {}, selfTestRate);
            int bad = 0;

            for (std::int64_t k = 0; k < 600; ++k)
            {
                bad += grid.indexOf (grid.startOf (k)) == k ? 0 : 1;

                if (k > 0)
                    bad += grid.indexOf (grid.startOf (k) - 1) == k - 1 ? 0 : 1;
            }

            test.check (bad == 0 && grid.bars && std::abs (grid.samplesPerSegment - 4.0 * 60.0 / 124.0 * selfTestRate) < 1.0e-6
                            && tempo124.barStartSample (2) == grid.startOf (1) && ! seconds.bars
                            && seconds.samplesPerSegment == celadon::glaze::factoryConstants().house.fallbackSegmentSeconds * selfTestRate,
                        "分段网格", "124 BPM 每小节 " + fixed (grid.samplesPerSegment, 4) + " 样本，600 个边界里归错 " + std::to_string (bad)
                                    + " 个；不给 --bpm 时每段 " + fixed (seconds.samplesPerSegment, 0) + " 样本");
        }

        // ---- 逐段统计：120 BPM 五小节——静音 / 1 kHz / 1 kHz / 60 Hz + 1 kHz / 静音（正弦幅度各 0.1 = −20 dBFS，两声道相同、相位连续）----
        //   - 段响度：1 kHz 两声道 −20 dBFS = −20.0 LUFS（同 --selftest 的整体响度一项，容差 0.1 LU）；开头的静音段没有读数；
        //     紧接响段的静音段：前 3 个子块的 400 ms 门块还带着响段，按分析器的口径算有效，但它们自己的能量只剩 K 加权滤波器的余振，
        //     读数比前一段低 20 LU 以上（2026-10-01 109 实测 −44.6 LUFS）；
        //   - 子块数：段边界与 100 ms 子块对齐，三个响段各 20 个有效子块（2.0 s），末尾静音段 ≤ 3 个；
        //   - 低频占比：只有 1 kHz 的段 < 0.005；60 Hz + 1 kHz 等幅的段 = 60 Hz 落进 35.6 ～ 112 Hz 的部分 ÷ 两者之和。Hann 窗下
        //     60 Hz 有约 1.3 % 的功率漏到 35.2 Hz 以下的 bin（不在 27 段里），期望约 0.987 / 1.987 = 0.497，容差 0.02。
        {
            const auto grid = makeSegmentGrid (tempo120, selfTestRate);
            constexpr int bar = 96000;
            const auto tone = [] (int i)
            {
                const auto index = i / bar;

                if (index == 0 || index == 4)
                    return 0.0f;

                auto x = testSine (1000.0, i, 0.1);

                if (index == 3)
                    x += testSine (60.0, i, 0.1);

                return x;
            };

            const auto signal = testSignal (5 * bar, tone, tone);
            const auto report = measureSegments (signal, signal.getNumSamples(), selfTestRate, grid);
            const auto lufsOf = [&report] (std::size_t k)
            {
                const auto& s = report.segments[k];
                return s.validSubBlocks > 0 && s.energy > 0.0 ? -0.691 + 10.0 * std::log10 (s.energy / s.validSubBlocks) : -1000.0;
            };
            const auto shareOf = [&report] (std::size_t k)
            {
                const auto& s = report.segments[k];
                return s.frames > 0 && s.totalPower > 0.0 ? s.lowPower / s.totalPower : -1.0;
            };

            const auto sized = report.segments.size() == 5;
            test.check (sized && std::abs (lufsOf (1) + 20.0) < 0.1 && std::abs (lufsOf (2) + 20.0) < 0.1 && lufsOf (0) < -60.0
                            && lufsOf (4) < lufsOf (3) - 20.0 && report.segments[1].validSubBlocks == 20 && report.segments[2].validSubBlocks == 20
                            && report.segments[3].validSubBlocks == 20 && report.segments[0].validSubBlocks == 0
                            && report.segments[4].validSubBlocks <= 3,
                        "逐段响度", sized ? "第 2 / 3 小节 " + fixed (lufsOf (1), 3) + " / " + fixed (lufsOf (2), 3) + " LUFS（期望 -20.0 ± 0.1），"
                                              "第 1 / 5 小节 " + fixed (lufsOf (0), 1) + " / " + fixed (lufsOf (4), 1) + "，"
                                              "有效子块 " + std::to_string (report.segments[0].validSubBlocks) + " / "
                                              + std::to_string (report.segments[1].validSubBlocks) + " / " + std::to_string (report.segments[2].validSubBlocks)
                                              + " / " + std::to_string (report.segments[3].validSubBlocks) + " / "
                                              + std::to_string (report.segments[4].validSubBlocks) + "（期望 0 / 20 / 20 / 20 / ≤ 3）"
                                        : "段数 " + std::to_string (report.segments.size()) + "（期望 5）");

            // 短期响度 S（3 s 窗、不门限）取每段最后一个子块处：第 2 小节结尾的窗是 1 s 静音 + 2 s 正弦 → −20 + 10·log10(2/3) = −21.76，
            // 第 3 小节结尾的窗全是正弦 → −20.0（容差 0.1 LU）；第 1 小节结尾还不满 3 s，没有读数。
            const auto expectedPartial = -20.0 + 10.0 * std::log10 (2.0 / 3.0);
            test.check (sized && ! report.segments[0].hasShortTerm && report.segments[1].hasShortTerm && report.segments[2].hasShortTerm
                            && std::abs (report.segments[1].shortTermLufs - expectedPartial) < 0.1
                            && std::abs (report.segments[2].shortTermLufs + 20.0) < 0.1,
                        "逐段短期响度", sized ? "第 2 / 3 小节结尾 " + fixed (report.segments[1].shortTermLufs, 3) + " / "
                                                  + fixed (report.segments[2].shortTermLufs, 3) + " LUFS（期望 " + fixed (expectedPartial, 2)
                                                  + " / -20.0 ± 0.1），第 1 小节" + (report.segments[0].hasShortTerm ? "有读数（不该有）" : "没有读数")
                                            : "段数不对");

            test.check (sized && shareOf (1) >= 0.0 && shareOf (1) < 0.005 && std::abs (shareOf (3) - 0.497) < 0.02 && ! report.droppedFrames,
                        "逐段低频占比", sized ? "只有 1 kHz 的段 " + fixed (shareOf (1), 4) + "（期望 < 0.005），60 Hz + 1 kHz 等幅的段 "
                                                  + fixed (shareOf (3), 4) + "（期望 0.497 ± 0.02）" + (report.droppedFrames ? "；丢过帧" : "")
                                            : "段数不对");
        }

        // ---- 曲风 / 交付 / 走带穿到链上（真歌同一套两遍流程）：信号同 --selftest 的同决策重放（白噪声 −26 dBFS 峰值 + 8 Hz 正弦 0.1，20 s）----
        //   - 通用档给不给拍点逐位相同（设计规格-曲风与交付 第 1 节：新行为都在 isHouse 之后；通用档不看拍点）；
        //   - House 档 + 124 BPM：交给限幅器的慢段释放上限 = 60000 / 124 / 3 = 161.29 ms（设计规格-曲风-House学习方式 第 8 节；出厂
        //     beatReleaseDivisor 3，出厂自动释放）；House 档不给拍点、通用档给拍点都是 0（不封顶）——证明模拟走带真的送到了链上。
        {
            auto signal = testNoise (static_cast<int> (20.0 * selfTestRate), 0.05f);

            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < signal.getNumSamples(); ++i)
                    signal.setSample (ch, i, signal.getSample (ch, i) + testSine (8.0, i, 0.1));

            auto tempo124 = tempo120;
            tempo124.bpm = 124.0;

            Combo general;
            general.glaze = 35.0f;
            RenderResult plain, withTempo, tempoAgain;
            const auto plainHash = selfTestRender (signal, general, Transport {}, plain);
            const auto tempoHash = selfTestRender (signal, general, tempo124, withTempo);
            const auto tempoAgainHash = selfTestRender (signal, general, tempo124, tempoAgain);
            const auto plainPeak = selfTestTruePeak (plain);
            const auto tempoPeak = selfTestTruePeak (withTempo);

            // 阶段 2：通用档的释放上限仍是 0，但连续编排器会吃 PPQ，成品不必与不给拍点逐位相同。
            // 重复给拍点必须逐位相同，真峰值仍不超过 Ceiling。
            std::cerr << "信息  通用档给拍点：不给 " << plainHash << "，给 " << tempoHash
                      << (plainHash == tempoHash ? "（哈希相同）" : "（哈希不同：连续编排器吃 PPQ）") << "\n";
            test.check (plain.beatReleaseCapMaxMs == 0.0f && withTempo.beatReleaseCapMaxMs == 0.0f && tempoHash == tempoAgainHash
                            && plainPeak <= -1.0 + 0.01 && tempoPeak <= -1.0 + 0.01,
                        "通用档给拍点：释放上限为 0，重复走带逐位相同",
                        "释放上限 " + fixed (plain.beatReleaseCapMaxMs, 2) + " / " + fixed (withTempo.beatReleaseCapMaxMs, 2)
                            + " ms（期望 0 / 0），重复走带 " + (tempoHash == tempoAgainHash ? "逐位相同" : "竟不同")
                            + "，真峰值 " + fixed (plainPeak, 3) + " / " + fixed (tempoPeak, 3) + " dBTP");

            auto deep = general;
            deep.genre = static_cast<int> (celadon::glaze::Genre::deepHouse);
            RenderResult deepTempo, deepPlain;
            selfTestRender (signal, deep, tempo124, deepTempo);
            selfTestRender (signal, deep, Transport {}, deepPlain);
            const auto expectedCap = 60000.0 / 124.0 / 3.0;

            test.check (std::abs (static_cast<double> (deepTempo.beatReleaseCapMaxMs) - expectedCap) < 0.01 && deepPlain.beatReleaseCapMaxMs == 0.0f,
                        "House 档限幅跟拍", "124 BPM 时上限 " + fixed (deepTempo.beatReleaseCapMaxMs, 3) + " ms（期望 " + fixed (expectedCap, 3)
                                              + "），不给拍点 " + fixed (deepPlain.beatReleaseCapMaxMs, 3) + " ms（期望 0）");
        }

        // ---- 交付「流媒体」：有效 Ceiling = min(用户 −1.0 − 编码余量 0, −2.0) = −2.0 dBTP（设计规格-曲风与交付 第 4.1 节），成品真峰值跟着降 ----
        // 信号：两路白噪声幅度 0.9（采样峰值 −0.9 dBFS，真峰值更高）：两档限幅器都得动作，真峰值贴着各自的有效 Ceiling（+0.01 dB 容差同门槛）。
        {
            const auto hot = testNoise (static_cast<int> (10.0 * selfTestRate), 0.9f);
            Combo standard;
            standard.glaze = 35.0f;
            auto streaming = standard;
            streaming.delivery = static_cast<int> (celadon::glaze::Delivery::streaming);
            RenderResult standardRender, streamingRender;
            selfTestRender (hot, standard, Transport {}, standardRender);
            selfTestRender (hot, streaming, Transport {}, streamingRender);
            const auto standardPeak = selfTestTruePeak (standardRender);
            const auto streamingPeak = selfTestTruePeak (streamingRender);

            test.check (standardRender.effectiveCeilingDb == -1.0f && streamingRender.effectiveCeilingDb == -2.0f
                            && standardPeak <= -1.0 + 0.01 && streamingPeak <= -2.0 + 0.01 && streamingPeak < standardPeak - 0.5,
                        "交付流媒体 −2 dBTP", "有效 Ceiling 标准 " + fixed (standardRender.effectiveCeilingDb, 2) + " / 流媒体 "
                                                + fixed (streamingRender.effectiveCeilingDb, 2) + " dBTP（期望 -1 / -2），成品真峰值 "
                                                + fixed (standardPeak, 3) + " / " + fixed (streamingPeak, 3) + " dBTP");
        }

        // ---- --listen-from-bar：第一遍从第 3 小节（120 BPM = 第 4 s、第 192000 样本）起播——逐块追踪的第一行就在 4.0000 s ----
        {
            const auto noise = testNoise (static_cast<int> (8.0 * selfTestRate), 0.05f);
            const auto start = static_cast<int> (tempo120.barStartSample (3));
            const auto listen = listenPass (noise, selfTestRate, 512, makeComboParams (Combo {}), true, tempo120, start);
            const auto newline = listen.trace.find ('\n');
            const auto firstRow = newline == std::string::npos ? std::string() : listen.trace.substr (newline + 1, 7);
            const auto rows = static_cast<int> (std::count (listen.trace.begin(), listen.trace.end(), '\n')) - 1;
            const auto expectedRows = (noise.getNumSamples() - start + 511) / 512;

            test.check (start == 192000 && firstRow == "4.0000," && rows == expectedRows, "第一遍从第 N 小节起播",
                        "起点 " + std::to_string (start) + " 样本，追踪第一行「" + firstRow + "」，" + std::to_string (rows) + " 块（期望 192000、"
                            "「4.0000,」、" + std::to_string (expectedRows) + " 块）");
        }

        // ---- 文件名、标签与追加列个数 ----
        {
            Combo combo;
            combo.glaze = 35.0f;
            const auto plain = outputFileName ("Song A", combo);
            combo.genre = static_cast<int> (celadon::glaze::Genre::deepHouse);
            combo.delivery = static_cast<int> (celadon::glaze::Delivery::club);
            combo.listenFromBar = 33;
            const auto extended = outputFileName ("Song A", combo);
            const auto label = comboLabel (combo, " · ");

            test.check (plain == "Song A__g35_clay-auto_auto.wav" && extended == "Song A__g35_clay-auto_auto_deep_club_from-b33.wav"
                            && label == "g35 · clay-auto · auto · deep · club · from-b33" && withoutWavExtension (extended)
                                                                                                == "Song A__g35_clay-auto_auto_deep_club_from-b33",
                        "T-038 成品文件名", plain + "、" + extended + "、" + label);

            Options options;
            options.extended = true;
            options.bpm = 124.0;
            SongReport report;
            report.sampleRate = selfTestRate;
            Row row;
            row.combo = combo;
            const std::string header = csvExtendedHeader();
            const auto columns = static_cast<std::size_t> (std::count (header.begin(), header.end(), ',')) + 1;
            const auto fields = csvExtendedFields (report, row, options).size();

            test.check (columns == fields, "T-038 追加列个数", std::to_string (columns) + " 个列名、" + std::to_string (fields) + " 个值");
        }

        if (test.failed == 0)
            std::cerr << "T-038 自检全部通过\n";
        else
            std::cerr << "T-038 自检有 " << test.failed << " 项失败\n";

        return test.failed == 0 ? 0 : 1;
    }

    //==============================================================================
    // T-048 实时画像追踪的自检（--selftest-live）：合成 House 曲走真歌同一套第一遍 / 第二遍，核对追踪管线本身（段首尾相接、按小节对齐、
    // 块长与 GLAZE 无关、模型与常数真的穿到画像、不改链的输出）与几个命令行约定。与 --selftest / --selftest-house 分开（那两份的项目与
    // 输出保持原样）；改了追踪相关的代码在服务器上手动跑（几十秒）。

    // 自检用的合成 House 曲（transport 的速度与采样率、4/4）：前奏 introBars 小节只有鼓（底鼓 + 反拍踩镲）→ drop（底鼓更响 + 55 Hz 持续低音
    // 1 ～ 6 次谐波 + 和声垫 + 踩镲）→ breakdown（只有和声垫：比 drop 轻 10 LU 以上、没有低音）→ drop，最后一段 drop 再多放 tailBars 小节
    // （不满一小节）就截断。歌尾不留静音：最后一块的 Balance 帧落在有声、过帧门的段里，关实时模式时链补交这些帧（LiveTracer::finish）才考得到；最后一段是
    // 不满一小节的尾巴，追踪靠 finish 交出来。写法照 tests/MusicAnalyzerTests.cpp 的 makeHouseTrack，另加 breakdown 与第二段 drop。
    juce::AudioBuffer<float> makeLiveTestTrack (const Transport& transport, int introBars, int dropBars, int breakdownBars, int lastDropBars,
                                                double tailBars)
    {
        const auto beat = 60.0 / transport.bpm;
        const auto barSeconds = transport.quartersPerBar() * beat;
        const auto musicBars = introBars + dropBars + breakdownBars + lastDropBars;
        const auto numSamples = static_cast<int> (std::llround ((static_cast<double> (musicBars) + tailBars) * barSeconds * transport.sampleRate));
        const auto twoPi = juce::MathConstants<double>::twoPi;
        juce::AudioBuffer<float> signal (2, numSamples);

        for (int ch = 0; ch < 2; ++ch)
        {
            juce::Random random (20261002 + 7919 * ch);   // 踩镲噪声两声道不相关，其余左右相同（和声垫差一点相位）
            double previous = 0.0;

            for (int i = 0; i < numSamples; ++i)
            {
                const auto t = static_cast<double> (i) / transport.sampleRate;
                const auto bar = static_cast<int> (std::floor (t / barSeconds));
                const auto intro = bar < introBars;
                const auto drop = (bar >= introBars && bar < introBars + dropBars) || bar >= introBars + dropBars + breakdownBars;   // 含尾巴
                const auto breakdown = bar >= introBars + dropBars && bar < introBars + dropBars + breakdownBars;
                const auto beatPosition = std::fmod (t, beat);
                const auto offBeatPosition = std::fmod (t + 0.5 * beat, beat);
                const auto white = static_cast<double> (random.nextFloat()) * 2.0 - 1.0;
                double x = 0.0;

                if (intro || drop)
                {
                    x += (drop ? 0.5 : 0.35) * std::exp (-beatPosition / 0.12)
                       * std::sin (twoPi * (50.0 * beatPosition + 70.0 * 0.03 * (1.0 - std::exp (-beatPosition / 0.03))));
                    x += (drop ? 0.10 : 0.08) * std::exp (-offBeatPosition / 0.025) * (white - previous);
                }

                if (drop)
                    for (int h = 1; h <= 6; ++h)
                        x += 0.22 * std::sin (twoPi * 55.0 * h * t) / h;

                if (drop || breakdown)
                    for (const auto hz : { 220.0, 277.18, 329.63 })
                        x += 0.05 * std::sin (twoPi * hz * t + 0.7 * ch);

                previous = white;
                signal.setSample (ch, i, static_cast<float> (x));
            }
        }

        return signal;
    }

    // 自检用：真歌同一套第一遍，traced 时接追踪（主段模型按曲风）。返回这一遍的结果（存档、状态）与追踪。
    struct LiveSelfRun
    {
        ListenResult listen;
        LiveTraceResult trace;
    };

    LiveSelfRun liveSelfListen (const juce::AudioBuffer<float>& signal, const Combo& combo, int blockSize, const Transport& transport,
                                const celadon::glaze::LiveConstants& constants, bool traced)
    {
        LiveSelfRun run;
        LiveTracer tracer (selfTestRate, constants, celadon::glaze::mainSectionModelFor (celadon::glaze::genreFromIndex (combo.genre)), transport);
        run.listen = listenPass (signal, selfTestRate, blockSize, makeComboParams (combo), false, transport, 0, traced ? &tracer : nullptr);
        run.trace = tracer.getResult();
        return run;
    }

    bool sameTrace (const LiveTraceResult& a, const LiveTraceResult& b)
    {
        return a.rows == b.rows && a.digest.value == b.digest.value;
    }

    // 命令行（自检用）：words 前面补上程序名，交给 parseArguments。
    ParseResult parseWords (std::vector<std::string> words, Options& options, std::string& error)
    {
        words.insert (words.begin(), "celadon_render");
        std::vector<char*> argv;

        for (auto& word : words)
            argv.push_back (word.data());

        return parseArguments (static_cast<int> (argv.size()), argv.data(), options, error);
    }

    int runLiveSelfTest()
    {
        SelfTest test;

        // ---- --live-constants：键表 37 个键（LiveProfile.h 的公开表）；改得到字段、收 inf（只在允许的键上）；坏值一律拒收（不悄悄换出厂值）----
        {
            celadon::glaze::LiveConstants c;
            std::string error;
            const auto ok = parseLiveConstants ("fourOnFloor.gateWidthLu=3.5, newProgramLu=8,referenceMemorySeconds=240,noMainSeconds=inf,"
                                                "loudness.memoryBars=-inf,newProgramRangeLu=inf,publishDelaySeconds=0.25", c, error);
            int keys = 0;
            visitLiveConstantKeys (c, [&keys] (const std::string&, auto&, LiveDomain) { ++keys; });

            test.check (ok && c.fourOnFloor.gateWidthLu == 3.5f && c.newProgramLu == 8.0f && c.referenceMemorySeconds == 240.0
                            && std::isinf (c.noMainSeconds) && c.noMainSeconds > 0.0 && std::isinf (c.loudness.memoryBars) && c.loudness.memoryBars < 0.0
                            && std::isinf (c.newProgramRangeLu) && c.publishDelaySeconds == 0.25
                            && c.fourOnFloor.lowGateFactor == celadon::glaze::LiveConstants {}.fourOnFloor.lowGateFactor && keys == 37,
                        "--live-constants 解析", ok ? "改到 7 个字段（含 inf / -inf），其余保持出厂值；键表 " + std::to_string (keys) + " 个键（期望 37）"
                                                    : "解析失败：" + error);

            const std::array<const char*, 12> bad { { "nosuch=1", "mainThreshold=1.5", "fourOnFloor.gateWidthLu=inf", "newProgramLu=8,newProgramLu=9",
                                                      "newProgramLu", "newProgramLu=abc", "loudness.referencePercentile=0", "peakTailSeconds=nan",
                                                      "fourOnFloor.loudnessRampLu=0", "newProgramKeep=1e400", "publishDelaySeconds=inf",
                                                      "newProgramRangeLu=-1" } };
            std::string accepted;

            for (const auto* text : bad)
            {
                celadon::glaze::LiveConstants scratch;
                std::string why;

                if (parseLiveConstants (text, scratch, why) || why.empty())
                    accepted += std::string (accepted.empty() ? "" : "、") + text;
            }

            test.check (accepted.empty(), "--live-constants 拒收坏值",
                        accepted.empty() ? std::to_string (bad.size()) + " 种坏写法（不认识的键、越界、不许 inf 的 inf、重复、缺「=」、不是数、NaN、溢出、负数）都报错"
                                         : "这些没报错：" + accepted);
        }

        // ---- 命令行：附属选项离不开 --live-trace；改常数 / 模型必须给标签；标签不能跳出 live/ ----
        {
            Options plain, tagless, full, escape;
            std::string e1, e2, e3, e4;
            const auto r1 = parseWords ({ "--in", "a", "--out", "b", "--live-only" }, plain, e1);
            const auto r2 = parseWords ({ "--in", "a", "--out", "b", "--live-trace", "--live-constants", "newProgramLu=8" }, tagless, e2);
            const auto r3 = parseWords ({ "--in", "a", "--out", "b", "--live-trace", "--live-only", "--live-constants", "newProgramLu=8",
                                          "--live-tag", "np8", "--live-model", "loudness" }, full, e3);
            const auto r4 = parseWords ({ "--in", "a", "--out", "b", "--live-trace", "--live-tag", "../x" }, escape, e4);
            const auto fullOk = r3 == ParseResult::run && full.liveTrace && full.liveOnly && full.liveConstants.newProgramLu == 8.0f
                             && full.liveConstantsText == "newProgramLu=8" && full.liveModelOverride
                             && full.liveModel == celadon::glaze::MainSectionModel::loudness && full.liveTag == "np8" && ! full.extended;

            test.check (r1 == ParseResult::invalid && r2 == ParseResult::invalid && fullOk && r4 == ParseResult::invalid, "追踪选项的命令行约定",
                        std::string (r1 == ParseResult::invalid ? "" : "--live-only 单独给却没报错；")
                            + (r2 == ParseResult::invalid ? "" : "改常数不给标签却没报错；") + (fullOk ? "" : "完整的一组没解析对（" + e3 + "）；")
                            + (r4 == ParseResult::invalid ? "" : "标签 ../x 没报错；")
                            + "单独 --live-only、改常数不给标签、标签 ../x 都报错，完整的一组解析对");
        }

        // ---- 合成 House 曲：Deep House、124 BPM、GLAZE 0、块长 512、出厂常数 ----
        Transport tempo124;
        tempo124.enabled = true;
        tempo124.bpm = 124.0;
        tempo124.sampleRate = selfTestRate;
        constexpr int introBars = 8, dropBars = 12, breakdownBars = 8, lastDropBars = 8;
        constexpr int musicBars = introBars + dropBars + breakdownBars + lastDropBars;
        constexpr double tailBars = 0.4;   // 歌尾：最后一段 drop 多放 0.4 小节就截断（约 0.77 s，有声，见 makeLiveTestTrack）
        const auto track = makeLiveTestTrack (tempo124, introBars, dropBars, breakdownBars, lastDropBars, tailBars);
        const auto grid = makeSegmentGrid (tempo124, selfTestRate);
        const celadon::glaze::LiveConstants factory;

        Combo deep;
        deep.glaze = 0.0f;
        deep.genre = static_cast<int> (celadon::glaze::Genre::deepHouse);
        const auto base = liveSelfListen (track, deep, 512, tempo124, factory, true);
        const auto& records = base.trace.records;

        // 首尾相接、交出歌尾：第一段从 0 起，相邻两段首尾相接，最后一段止于音频末尾（finish 把开着的那一截交出来了）。
        {
            auto contiguous = ! records.empty() && records.front().start == 0 && records.back().end == track.getNumSamples();

            for (std::size_t k = 1; k < records.size(); ++k)
                contiguous = contiguous && records[k].start == records[k - 1].end;

            test.check (contiguous && ! base.trace.overrun && ! base.trace.mismatch, "段首尾相接",
                        std::to_string (records.size()) + " 段，" + (contiguous ? "从 0 起首尾相接、止于音频末尾 " + std::to_string (track.getNumSamples()) + " 样本"
                                                                     : std::string ("有缺口或重叠")) + (base.trace.overrun ? "；读提交落后" : "")
                            + (base.trace.mismatch ? "；记录与快照配不上" : ""));
        }

        // 按小节、与工具的小节网格逐样本对齐：第 k 段 = 第 k + 1 小节 [round(k × 每小节样本数), round((k + 1) × …))，最后一段是不满一小节的尾巴。
        {
            int misaligned = 0;

            for (std::size_t k = 0; k < records.size(); ++k)
            {
                const auto& r = records[k];
                const auto start = grid.startOf (static_cast<std::int64_t> (k));
                const auto end = grid.startOf (static_cast<std::int64_t> (k) + 1);
                const auto last = k + 1 == records.size();
                const auto aligned = r.mode == celadon::glaze::SegmentMode::bars && r.start == start && (r.end == end || (last && r.end < end));
                misaligned += aligned ? 0 : 1;
            }

            test.check (records.size() == static_cast<std::size_t> (musicBars + 1) && misaligned == 0, "按小节对齐",
                        std::to_string (records.size()) + " 段（期望 " + std::to_string (musicBars) + " 小节 + 1 段尾巴），不在小节线上的 "
                            + std::to_string (misaligned) + " 段（期望 0）");
        }

        // 从第 N 小节起播（--listen-from-bar）：分析器时间轴从起播样本算 0，追踪的小节号要换回歌曲的小节网格——第一段是第 N 小节（账本锚点
        // 取整时前面可能多一段不到 1 拍的、属于第 N − 1 小节的碎段），段起点与网格差 ≤ 1 样本，最后一段是第 musicBars + 1 小节的尾巴。
        {
            constexpr int fromBar = introBars + 1;
            LiveTracer tracer (selfTestRate, factory, celadon::glaze::MainSectionModel::fourOnFloor, tempo124);
            listenPass (track, selfTestRate, 512, makeComboParams (deep), false, tempo124, static_cast<int> (tempo124.barStartSample (fromBar)), &tracer);
            const auto& rows = tracer.getResult().rows;
            int before = 0, offGrid = 0, firstBar = 0, lastBar = 0;

            for (const auto& row : rows)
            {
                const auto cells = splitList (row);
                const auto bar = cells.size() > 2 ? std::atoi (cells[1].c_str()) : 0;
                const auto offset = cells.size() > 2 ? std::atoll (cells[2].c_str()) : 0;

                if (bar < fromBar)
                {
                    ++before;
                    continue;
                }

                firstBar = firstBar == 0 ? bar : firstBar;
                lastBar = bar;
                offGrid += std::llabs (offset) <= 1 ? 0 : 1;
            }

            test.check (before <= 1 && firstBar == fromBar && lastBar == musicBars + 1 && offGrid == 0, "从第 N 小节起播的小节号",
                        "从第 " + std::to_string (fromBar) + " 小节起播：" + std::to_string (rows.size()) + " 段，第一段整段在第 " + std::to_string (firstBar)
                            + " 小节（期望 " + std::to_string (fromBar) + "），最后一段在第 " + std::to_string (lastBar) + " 小节（期望 "
                            + std::to_string (musicBars + 1) + "），起点小节之前的碎段 " + std::to_string (before) + " 个（期望 ≤ 1），离网格超过 1 样本的 "
                            + std::to_string (offGrid) + " 段（期望 0）");
        }

        // I 一直有效（第一段就有有效音频：冷启动以自己为参考，m = 1）；管线的阳性对照：breakdown 各小节 m < 0.5、第二段 drop 各小节 m ≥ 0.5。
        {
            const auto allValid = std::all_of (records.begin(), records.end(), [] (const celadon::glaze::LiveCommitRecord& r) { return r.mainLevelValid; });
            int breakdownMain = 0, dropMissed = 0;

            for (std::size_t k = 0; k < records.size() && k < static_cast<std::size_t> (musicBars); ++k)
            {
                const auto bar = static_cast<int> (k);

                if (bar >= introBars + dropBars && bar < introBars + dropBars + breakdownBars)
                    breakdownMain += records[k].mainness >= 0.5f ? 1 : 0;
                else if (bar >= introBars + dropBars + breakdownBars)
                    dropMissed += records[k].mainness < 0.5f ? 1 : 0;
            }

            test.check (allValid && breakdownMain == 0 && dropMissed == 0, "I 有效、段落门",
                        std::string (allValid ? "每段之后 I 都有效" : "有段之后 I 无效") + "；breakdown 里 m ≥ 0.5 的 " + std::to_string (breakdownMain)
                            + " 小节、第二段 drop 里 m < 0.5 的 " + std::to_string (dropMissed) + " 小节（期望 0 / 0）；最终 I "
                            + fixed (base.trace.finalProfile.integratedLufs.value, 2) + " LUFS、c " + fixed (base.trace.finalDiagnostics.confidence, 3));
        }

        // 块长无关：4096、480（不是 2 的幂）与 512 的追踪逐字节相同、位级摘要相同（段与宿主分块无关，发布时刻按分析器报的交齐位置，
        // 关实时模式时链补交最后一块的帧——不补时歌尾一两段随块长变，那一项的对照在 MusicAnalyzerLive 用例 5d）。
        {
            const auto big = liveSelfListen (track, deep, 4096, tempo124, factory, true);
            const auto odd = liveSelfListen (track, deep, 480, tempo124, factory, true);
            test.check (sameTrace (big.trace, base.trace) && sameTrace (odd.trace, base.trace), "块长 512 / 4096 / 480",
                        "追踪" + std::string (sameTrace (big.trace, base.trace) ? "" : "（4096 不同）") + (sameTrace (odd.trace, base.trace) ? "" : "（480 不同）")
                            + (sameTrace (big.trace, base.trace) && sameTrace (odd.trace, base.trace) ? "逐字节相同，摘要 " + hex64 (base.trace.digest.value) : ""));
        }

        // 与 GLAZE、风格无关（最快的追踪跑法 = GLAZE 0 的依据）：GLAZE 70、温润的第一遍追踪与 GLAZE 0 的逐字节相同；
        // 存档与不追踪时逐字相同（追踪不改编排器）；第二遍成品与不追踪时逐位相同（追踪不改链的输出）。
        Combo warm70 = deep;
        warm70.glaze = 70.0f;
        warm70.style = static_cast<int> (celadon::glaze::Style::warm);
        const auto traced70 = liveSelfListen (track, warm70, 512, tempo124, factory, true);
        const auto plain70 = liveSelfListen (track, warm70, 512, tempo124, factory, false);
        {
            const auto sameState = traced70.listen.state != nullptr && plain70.listen.state != nullptr
                                && traced70.listen.state->toString() == plain70.listen.state->toString();
            test.check (sameTrace (traced70.trace, base.trace), "与 GLAZE、风格无关",
                        std::string (sameTrace (traced70.trace, base.trace) ? "GLAZE 70 温润与 GLAZE 0 的追踪逐字节相同" : "GLAZE 70 温润的追踪与 GLAZE 0 不同"));
            // 外接 setLiveSegmentSink 把段从链的 LiveProfile 引走，所以接上追踪之后存下的 LIVE 与不追踪不同。
            test.check (! sameState, "外接追踪改写 LIVE 存档",
                        sameState ? "接不接追踪，存档却逐字相同" : "接了追踪，第一遍存下的 GLAZE 状态与不追踪不同");
        }

        {
            const auto params70 = makeComboParams (warm70);
            RenderResult plainRender, plainAgain, tracedRender, tracedAgain;
            renderPass (track, selfTestRate, 512, params70, plain70.listen.state.get(), false, plainRender, nullptr, tempo124);
            renderPass (track, selfTestRate, 512, params70, plain70.listen.state.get(), false, plainAgain, nullptr, tempo124);
            LiveTracer renderTracer (selfTestRate, factory, celadon::glaze::MainSectionModel::fourOnFloor, tempo124);
            LiveTracer renderTracerAgain (selfTestRate, factory, celadon::glaze::MainSectionModel::fourOnFloor, tempo124);
            renderPass (track, selfTestRate, 512, params70, plain70.listen.state.get(), false, tracedRender, nullptr, tempo124, false, &renderTracer);
            renderPass (track, selfTestRate, 512, params70, plain70.listen.state.get(), false, tracedAgain, nullptr, tempo124, false, &renderTracerAgain);
            const juce::AudioBuffer<float> a (plainRender.io.getArrayOfWritePointers(), 2, plainRender.latencySamples, plainRender.outputLength);
            const juce::AudioBuffer<float> b (tracedRender.io.getArrayOfWritePointers(), 2, tracedRender.latencySamples, tracedRender.outputLength);
            const juce::AudioBuffer<float> a2 (plainAgain.io.getArrayOfWritePointers(), 2, plainAgain.latencySamples, plainAgain.outputLength);
            const juce::AudioBuffer<float> b2 (tracedAgain.io.getArrayOfWritePointers(), 2, tracedAgain.latencySamples, tracedAgain.outputLength);
            const auto plainSame = plainRender.outputLength == plainAgain.outputLength && hashAudio (a) == hashAudio (a2);
            const auto tracedSame = tracedRender.outputLength == tracedAgain.outputLength && hashAudio (b) == hashAudio (b2);
            const auto diverted = plainRender.outputLength != tracedRender.outputLength || hashAudio (a) != hashAudio (b);
            const auto peakOk = selfTestTruePeak (plainRender) <= -1.0 + 0.01 && selfTestTruePeak (tracedRender) <= -1.0 + 0.01;
            test.check (plainSame && tracedSame && diverted && peakOk, "追踪探针对输出的契约",
                        std::string (plainSame ? "两次不追踪逐位相同" : "两次不追踪竟不同")
                            + (tracedSame ? "；两次追踪逐位相同" : "；两次追踪竟不同")
                            + (diverted ? "；接上追踪后成品与不追踪不同（段被引走）" : "；接上追踪成品却没变")
                            + "；真峰值 " + fixed (selfTestTruePeak (plainRender), 3) + " / " + fixed (selfTestTruePeak (tracedRender), 3) + " dBTP");

            // 信息（不判定）：第二遍（读档、不学习）的追踪与第一遍有多少段相同、第一处不同差在哪些列——读档之后分析器不学习，
            // 输入尾部还多了延迟与余振的补零。
            const auto& second = renderTracer.getResult().rows;
            std::size_t k = 0;

            while (k < second.size() && k < base.trace.rows.size() && second[k] == base.trace.rows[k])
                ++k;

            std::cerr << "信息  第二遍追踪：" << second.size() << " 段，与第一遍前 " << k << " / " << base.trace.rows.size() << " 段逐字相同"
                      << (k < second.size() && k < base.trace.rows.size() ? "；第一处不同：" + differingColumns (base.trace.rows[k], second[k])
                                                                         : std::string())
                      << "\n";
        }

        // 对照有牙：通用档（响度型）的追踪必须与 Deep House（4/4 型）的不同（段数相同：分段与曲风无关）；常数穿到画像：M0 从出厂 6 小节放到 64，最终 c 更低。
        {
            auto general = deep;
            general.genre = static_cast<int> (celadon::glaze::Genre::general);
            const auto loudness = liveSelfListen (track, general, 512, tempo124, factory, true);
            test.check (loudness.trace.rows.size() == base.trace.rows.size() && loudness.trace.digest.value != base.trace.digest.value,
                        "主段模型穿到画像", "通用档（响度型）" + std::to_string (loudness.trace.rows.size()) + " 段，摘要 " + hex64 (loudness.trace.digest.value)
                                            + (loudness.trace.digest.value != base.trace.digest.value ? "，与 4/4 型不同" : "，竟与 4/4 型相同"));

            celadon::glaze::LiveConstants slow;
            std::string error;
            const auto parsed = parseLiveConstants ("fourOnFloor.confidenceBars=64", slow, error);
            const auto slowRun = liveSelfListen (track, deep, 512, tempo124, slow, true);
            test.check (parsed && slowRun.trace.finalDiagnostics.confidence < base.trace.finalDiagnostics.confidence,
                        "--live-constants 穿到画像", "fourOnFloor.confidenceBars=64 时最终 c " + fixed (slowRun.trace.finalDiagnostics.confidence, 3)
                                                    + "（出厂 6 小节 " + fixed (base.trace.finalDiagnostics.confidence, 3) + "，期望更低）");
        }

        if (test.failed == 0)
            std::cerr << "T-048 自检全部通过\n";
        else
            std::cerr << "T-048 自检有 " << test.failed << " 项失败\n";

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

        case ParseResult::selfTestHouse:
            return runHouseSelfTest();

        case ParseResult::selfTestLive:
            return runLiveSelfTest();

        case ParseResult::invalid:
            std::cerr << "celadon_render：" << error << "\n\n";
            printUsage();
            return 2;

        case ParseResult::run:
            break;
    }

    measurementStreamingTarget = options.streamingTargetLufs;

    if (! options.replayOverrides.empty())   // T-096d：覆盖行的标识与原文（clayId / statusText 读）
        replayOverrideNames = { replayOverrideId (options.replayOverrides), replayOverrideText (options.replayOverrides) };

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
    const auto heatAbCsv = options.outputDir.getChildFile ("heat_ab.csv");   // T-096d：只在给了 --replay-override 时写

    std::cerr << "celadon_render：" << songs.size() << " 首歌 × " << combos.size() << " 组（" << gridText (options)
              << "），宿主块长 " << options.blockSize
              << (options.noAlign ? "，不对齐\n" : "，对齐到 " + fixed (options.matchLufs, 1) + " LUFS\n")
              << "输出目录：" << options.outputDir.getFullPathName().toStdString() << "\n";

    if (options.liveTrace)
        std::cerr << "T-048 实时画像追踪：live/" << options.liveTag << "/"
                  << (options.liveOnly ? "（--live-only：只跑第一遍，每个曲风 × 聆听起点一遍）" : "") << "\n";

    if (! options.replayOverrides.empty())
        std::cerr << "T-096d 重放覆盖：" << replayOverrideNames.text << "（每组 GLAZE > 0 的修坯「自动」之后多一行 clay-"
                  << replayOverrideNames.id << "，差信号写 heat_ab.csv）\n";

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
            || ! writeText (metricsCsv, makeCsv (reports, options)))
        {
            std::cerr << "celadon_render：写不了指标表 " << metricsMd.getFullPathName().toStdString() << "\n";
            return 1;
        }

        if (! options.replayOverrides.empty() && ! writeText (heatAbCsv, makeHeatAbCsv (reports)))
        {
            std::cerr << "celadon_render：写不了 " << heatAbCsv.getFullPathName().toStdString() << "\n";
            return 1;
        }
    }

    std::cerr << "完成：" << written << " 个成品，" << failures << " 处失败，用时 " << fixed (nowSeconds() - started, 1)
              << " s；指标表 " << metricsMd.getFullPathName().toStdString() << "、metrics.csv\n";
    return failures > 0 ? 1 : 0;
}
