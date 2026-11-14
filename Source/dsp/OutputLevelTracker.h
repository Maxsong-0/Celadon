/*
  ==============================================================================

    OutputLevelTracker.h
    T-048 阶段 2 输出侧主段电平（Q9）。不用第二份 LiveProfile。

    门是输入侧已发布的 m′（noteMainness）。能量是出口 K 加权、按同一段累计。
    m′ ≥ 0.5 的段才进入主段电平。G_trim 读 evidenceAt()。调用方对不该当证据的块交 m′ = 0（T-106：方案还没到位的块）。

    clear() 只用于冷启动。宿主 reset 只清 K 加权滤波器（resetFilters），不清段累计。
    滤波器状态不进 LIVE（草图第 5 节：滤波器从零开始）；段累计进 LIVE。

    T-106（ADR-038）：G_trim 的误差 = 方案净增益 − 实测净增益，实测净增益 = 出口主段电平 − 入口主段电平，两侧是这里记的
    同一批段、同样的权重（evidenceAt）。历史里的段是过去录的，有两样东西会过时，各补一份记录；再补一份入口能量。
      一、增益。每段另记一份「折回 0 dB Limit Gain」的出口能量（实录能量 ÷ 当块 Limit Gain 的功率倍数）。读数 = 这份能量的
          主段电平 + g（g = 此刻不含守门退让的 Limit Gain）。T-106 之前直接拿实录的主段电平去比：G_trim 或方案把增益改了
          以后旧段不会跟着变，积分器看不到自己的动作（T-097 追踪里 +3 → −3 的慢滑）。折算按「增益 1 dB = 电平 1 dB」，
          只在录制增益附近成立（限幅器越压越不到 1 : 1），所以录制增益离 g 远的段按远近降权（evidenceAt）。
      二、主段判定。每段另记段内输入侧读数的平均（段落账的暂判段响度、低频占比）。rejudge() 按此刻的参考把已收的每一段重判
          一遍，只有此刻仍算主段的段才计入电平——同输入侧实时画像的回看重判（LiveProfile.h 第一节），规格原文「输出侧用同一组
          段边界、同一个 m」。不重判的话，从头播时前奏先被暂判成主段（冷启动以自己为参考），drop 来了以后输入侧的主段电平
          已经不含前奏、这里却还留着，读数偏低一两个 dB，G_trim 就被推上去，等前奏被挤出历史再慢慢滑回来。
          只重判当时进了账的段：当时不是主段的没有存，参考后来降下去也补不回来（等新段进账）。正开着的那一段不在重判之列：
          它每块都按此刻的参考现判（调用方交来的 m′ 就是拿此刻的参考算的），跌到 0.5 以下当块就翻成非主段、之前攒的收进账。
      三、入口能量。每段另记同一批样本在模块处理前的 K 加权能量（processInput）。改前拿出口主段电平去比「画像的输入响度 I
          + 方案净增益」，可 I 是画像自己那套窗口算的：全部有效音频的门控积分（规格第 1 节表；T-108 前 4/4 型
          另是 512 段、按主段时长指数遗忘的主段加权平均）——和这里「环里 m′ ≥ 0.5 的段」不是同一批段。差出来的不是链的偏差，
          G_trim 却照着去追：前奏轻、后面响，I 还带着前奏、这里已经只剩响段，G_trim 被拉下去 1 ～ 2 dB 再慢慢回来。
          两侧都用这里的段，差值就只剩链实际做了什么。入口比出口早一个链延迟 L（CeladonChain.h：48 kHz、Lookahead 默认 2 ms 时
          1384 个样本、约 29 ms，随 Lookahead 1 ～ 10 ms 变），按段算能量时不对齐。连着的主段之间，错开的那一截前后抵掉；只在
          一串主段的开头（前面是非主段）差一截：出口那 L 是前一段的内容。读数是整个历史的和，每一串主段的开头差一份「L ÷ 历史里
          的主段时长 × (前一段能量 ÷ 这一段能量 − 1)」。L 取 1384 个样本、一分钟的主段历史（刚开始攒的时候；攒满约四分钟时
          只有四分之一）：前一段响 10 dB 约差 +0.02 dB，轻 10 dB 约差 −0.002 dB；跳得越大、历史里这样的开头越多，差得越多。
          单看 2 s 的一段，前一段响 10 dB 时约差 0.5 dB，但读数从不只看一段。
      四、历史多长。已收的主段放在 128 格的环里，满了挤掉最旧的（非主段不进账、不占格）。编排器在输入侧段落账换段时收段
          （验证报告 docs/validation/2026-11-12-T106-gtrim-error-basis 第 9 节第 18 条），一格约是一个输入段——有拍点时一小节，
          没有速度时 2 s——环里装得下约四分钟的主段（124 BPM 约 248 s），同段边界改之前（一段最长 8 s、32 格，最长约 256 s）
          相当。主段 → 非主段的翻转（含方案还没到位的块）也收一段、占一格。环只放 32 格时只装得下约一分钟，G_trim 跟着段落
          起伏走：DGX 2026-11-13 重渲 20 首，≥ 60 s 的追踪里「60 s 内变化」最大从 0.96 dB 涨到 1.21 dB，128 格回到 0.95 dB
          （同上，第 9 节第 25 条）。存档里 closed 属性照旧按 32 × 24 字节写最新的 32 段给 0.1.8.10 读，全部的段另写进
          history / historyExtra（createStateXml）。
      当证据的段要三样齐全（evidenceAt）：入口配齐（每个样本都有入口能量，两侧的样本都是有限值）、有出口能量（量得出录制
      增益）、此刻仍算主段。缺一样就不算，也不占证据时长。

  ==============================================================================
*/

#pragma once

#include "GlazeTypes.h"
#include "LoudnessMeter.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>

namespace celadon::glaze
{
    class OutputLevelTracker
    {
    public:
        void prepare (double sampleRate) noexcept;
        void clear() noexcept;
        void resetFilters() noexcept;

        // 本段暂判的 m′。跨过 0.5，或调用方 closeSegment()，就把上一段收进主段账。编排器在输入侧段落账换段时调 closeSegment()
        // （外部第二意见 F1：段边界跟输入侧对齐；回绕、跳播、宿主 reset 处段落账都会换段）；此外一段满 8 s 自己切。
        // measured = 这一块的 m′ 是拿输入侧读数（inputLufs 段响度、lowShare 低频占比）算的：读数按样本数计入本段的平均，
        // 留给 rejudge()。静段、读不出、没有实时画像时为假，这一块不计入平均。
        void noteMainness (float mainness, bool measured = false, float inputLufs = 0.0f, float lowShare = 0.0f) noexcept;
        void closeSegment() noexcept;

        // T-106：这一片的入口样本（模块处理前；链里 analyzeInput 那一刻的缓冲）。能量先记成「待入账」，等同一片的 processOutput
        // 一起进本段——出口样本进哪一段，这一片的入口能量就进哪一段。这一片没有 processOutput（冻结、旁通）就作废。
        // 这一片里有非有限的样本（NaN / Inf）：量到的能量缺了那几个样本，不记待入账，这一片不配对。
        void processInput (const juce::AudioBuffer<float>& input) noexcept;
        // 这一片不会有 processOutput 时调（冻结、旁通：出口不是方案在听的信号）：这一片交来的入口能量作废。这样下一片就算
        // 没交入口，这份能量也配不到它头上——「一片的入口只配这一片的出口」不靠调用方每片都先交入口来保证。
        void discardInput() noexcept;

        // limitGainDb = 这一片交给限幅器的 Limit Gain（手动 + 自动增量 + G_trim + 守门退让）；按限幅器实际执行的值折算
        // （executedLimitGainDb：钳进 0 ～ 24 dB，非有限值按 0 dB）。
        // 待入账的入口能量按样本均摊进本段（片中间满 8 s 切段时两边各拿各的份）；样本数和这一片对不上就不入账，
        // 所在的段入口没配齐、不进 evidenceAt()。非有限的出口样本照旧计入段样本数、不计能量，也不算配上了入口。
        void processOutput (const juce::AudioBuffer<float>& output, float limitGainDb) noexcept;

        // T-106 回看重判：judge (段内平均的输入侧段响度, 低频占比) → 按此刻的参考算的 m′（LiveProfile::mainnessFor）。
        // 只重判已收的段：之后 evidenceAt() / mainSamples() 在已收的段里只收重判后 m′ ≥ 0.5 的；没有读数的段保持当时的判定；
        // 不调它 = 一直按当时的判定。正开着的那一段不归它管，按 noteMainness 最近交来的 m′ 算（文件头第二条）。
        template <typename Judge>
        void rejudge (Judge&& judge) noexcept
        {
            for (std::size_t i = 0; i < closed.size(); ++i)
            {
                const auto& extra = closedExtra[i];

                if (closed[i].samples <= 0 || extra.inputSamples <= 0)
                {
                    closedNow[i] = closed[i].mainness;
                    continue;
                }

                const auto count = (double) extra.inputSamples;
                const auto now = (float) judge ((float) (extra.inputLufsSum / count), (float) (extra.lowShareSum / count));
                closedNow[i] = std::isfinite (now) ? now : 0.0f;
            }
        }

        // T-106：G_trim 用的读数。只收入口配齐了的主段（每个样本都有入口能量），每段按「录它时的 Limit Gain 离 limitGainDb
        // 多远」加权：差 ≤ fullWithinDb 全算，≥ zeroBeyondDb 不算，中间线性。录制增益 = 10·log10(实录能量 ÷ 折算能量)
        // （段内按能量平均）。没有出口能量的段（数字静音）量不出录制增益、折算不了：不收，也不占时长。
        // 两个电平用同一批段、同样的权重：
        //   levelLufs = 出口主段电平，折算到 limitGainDb；inputLufs = 入口主段电平；seconds = 加权后的主段时长（证据有多少）。
        // 没有证据、那一侧能量为 0 或增益不是有限值时电平为 −100。
        struct Evidence
        {
            float levelLufs = -100.0f;
            float inputLufs = -100.0f;
            double seconds = 0.0;

            bool valid() const noexcept { return levelLufs > -100.0f && inputLufs > -100.0f; }
            // 实测净增益 = 链在这批主段上把电平改了多少（折算到 limitGainDb）。valid() 为假时没有意义。
            float netGainDb() const noexcept { return levelLufs - inputLufs; }
        };

        Evidence evidenceAt (float limitGainDb, float fullWithinDb, float zeroBeyondDb) const noexcept;
        bool openSegmentIsMain() const noexcept;                    // 本段暂判是主段（G_trim 只在这时积分）
        // 此刻计入电平的主段样本数（重判之后；含本段）。诊断与测试用。
        std::int64_t mainSamples() const noexcept;

        // 存档：0.1.8.10 就有的八个属性照它的格式写——closed 是最新的至多 32 段（从旧到新，closedCount 段、closedWrite 在最新一段
        // 的下一格），0.1.8.10 读得了这一节；T-106 另写十个属性，其中 historyCount / history / historyExtra 是全部已收的段（至多
        // 128 段，从旧到新）。
        std::unique_ptr<juce::XmlElement> createStateXml() const;
        // 没有 T-106 字段的旧存档（0.1.8.10 及更早）：校验同 0.1.8.10（标签、六个标量属性、closed 的长度、closedCount /
        // closedWrite 的范围；段的内容不看），然后段累计清空、restoredLegacyState() 置 true（旧历史不知道是在什么增益下录的，
        // 折算不了）。带 T-106 字段的新格式：旧的八个属性照上面那样验（closed 只验长度，内容不用），十个新属性要齐，用到的每一个
        // 量都验——三个样本数是严格的十进制非负整数，historyCount 在 0 ～ 128、history / historyExtra 的长度正好对得上，开着的一段
        // 与 history 里各段的能量有限且非负、样本数在 0 ～ 8 s × 768 kHz、主段度有限，每段另记的量见 validExtra，存档时的采样率
        // 有限且为正；有一项不合规矩就整节拒收、状态不动，同别的节点。读回后各段先按当时的判定，下一次 rejudge() 起按此刻的参考。
        // 存档时的采样率和此刻（最近一次 prepare）的不同：段里的样本数对不上此刻的时长，段累计清空（不打旧存档标记）。
        bool restoreStateXml (const juce::XmlElement&);
        bool restoredLegacyState() const noexcept { return legacyState; }

        // 读档拆两步（T-099，ADR-040）：restoreStateXml = stageStateXml + adoptRestoredState，规则同上。
        // stageStateXml 在 staged 里解码、校验（只写 staged；只读本对象的采样率——它只在 prepare 里改，可与音频线程同时调）；
        // adoptRestoredState 把 staged 里读档定下的那些量（段累计、开着的一段、旧存档标记）换进来、待入账的入口作废
        // （不与音频线程同时调；定长拷贝，不分配）。滤波器状态与本块的输入侧读数留本对象的，同改前就地读档。
        // 存档在插件里对一份拷贝调 createStateXml（编排器在回调锁里整份值拷贝这个对象，锁外再编码；对象里没有堆上的东西）。
        bool stageStateXml (const juce::XmlElement&, OutputLevelTracker& staged) const;
        void adoptRestoredState (const OutputLevelTracker& staged) noexcept;

        // 限幅器对交来的 Limit Gain 实际执行的值：照抄 LimiterModule.cpp sanitised() 里 gainDb 那一行（finiteInRange (raw.gainDb,
        // 0, 24, 默认 0)）——有限值钳进 0 ～ 24 dB，非有限值回默认 0 dB。processOutput 按它折算；那一行改了这里要跟着改
        // （OutputLevelTrackerTests 第 9 条按这个规矩逐位核对）。
        static constexpr float minimumLimitGainDb = 0.0f;
        static constexpr float maximumLimitGainDb = 24.0f;
        static constexpr float defaultLimitGainDb = 0.0f;

        static float executedLimitGainDb (float limitGainDb) noexcept
        {
            return std::isfinite (limitGainDb) ? juce::jlimit (minimumLimitGainDb, maximumLimitGainDb, limitGainDb) : defaultLimitGainDb;
        }

    private:
        // 布局不能动：存档的 closed 属性按这个布局存最新的至多 32 段（legacyClosedCapacity 格，0.1.8.10 按 32 × 24 字节读），
        // history 属性按这个布局存全部已收的段。T-106 的量另放 Extra。
        // reserved（T-109）：把原来 float 后面隐式的 4 字节填充写成显式的 0。整块存档时隐式填充里是什么由编译器决定：2026-11-13
        // 在 108 上实测，MSVC 19.44 存出来这 4 字节不为 0、同一状态两次存出来不一样，读进来的非 0 值还会跟着再存出去；GCC 15 下碰巧
        // 是 0。显式成员有初值、随每次拷贝一起走，存出来恒为 0。0.1.8.10 读 closed 时不看这 4 字节，布局与大小都没变。
        struct Segment
        {
            double energy = 0.0;
            std::int64_t samples = 0;
            float mainness = 0.0f;
            std::uint32_t reserved = 0;
        };

        static_assert (std::is_trivially_copyable_v<Segment> && sizeof (Segment) == 24 && offsetof (Segment, samples) == 8
                           && offsetof (Segment, mainness) == 16 && offsetof (Segment, reserved) == 20
                           && sizeof (Segment) == sizeof (double) + sizeof (std::int64_t) + sizeof (float) + sizeof (std::uint32_t),
                       "closed 属性按这个布局整块存，0.1.8.10 按 32 × 24 字节读；四个成员正好排满 24 字节，没有隐式填充");

        // T-106 每段另记的量（与 Segment 同下标；布局固定，存档的 historyExtra 属性按这个布局逐段存，次序同 history）。
        struct Extra
        {
            double unityEnergy = 0.0;         // 折回 0 dB Limit Gain 的出口能量
            double inputEnergy = 0.0;         // 入口 K 加权能量（模块处理前），只累计配上了的样本
            double inputLufsSum = 0.0;        // 输入侧段响度读数 × 样本数（只累计有读数的样本）
            double lowShareSum = 0.0;         // 低频占比读数 × 样本数
            std::int64_t inputSamples = 0;    // 有读数的样本数（0 = 这一段没有读数，重判时保持当时的判定）
            std::int64_t pairedSamples = 0;   // 有入口能量的样本数（= 段样本数才算配齐，进 evidenceAt）
        };

        static_assert (std::is_trivially_copyable_v<Extra> && sizeof (Extra) == 48 && offsetof (Extra, inputEnergy) == 8
                           && offsetof (Extra, inputLufsSum) == 16 && offsetof (Extra, lowShareSum) == 24
                           && offsetof (Extra, inputSamples) == 32 && offsetof (Extra, pairedSamples) == 40,
                       "historyExtra 属性按这个布局逐段存（每段 48 字节，没有填充）");

        bool decodeState (const juce::XmlElement& xml);
        void pushClosed (const Segment& segment, const Extra& extra) noexcept;
        void clearSegments() noexcept;
        void resetChannel (int channel) noexcept;
        void resetInputChannel (int channel) noexcept;
        // 此刻计入电平的段（重判后 m′ ≥ 0.5 的已收段，再加正在开着的主段），从新到旧：visit (段, 它的 Extra)。只在 .cpp 里用。
        template <typename Visit>
        void forEachMainSegment (Visit&& visit) const noexcept;
        static bool validSegment (const Segment& segment) noexcept;
        static bool validExtra (const Extra& extra, std::int64_t segmentSamples) noexcept;

        // 已收主段的环（文件头第四节）。存档另给 0.1.8.10 写一份 legacyClosedCapacity 格的（它的环就这么大，按 32 × 24 字节读）。
        static constexpr int closedCapacity = 128;
        static constexpr int legacyClosedCapacity = 32;
        static constexpr float mainnessGate = 0.5f;

        double sampleRate = 48000.0;
        std::int64_t maxOpenSamples = 384000;
        LoudnessMeter::Biquad shelf {}, highpass {};
        std::array<double, 2> shelfS1 {}, shelfS2 {}, highpassS1 {}, highpassS2 {};
        // T-106：入口那一路的 K 加权滤波器状态（系数同出口）。同出口：不存档，宿主 reset 时清。
        std::array<double, 2> inputShelfS1 {}, inputShelfS2 {}, inputHighpassS1 {}, inputHighpassS2 {};
        Segment open;
        std::array<Segment, (std::size_t) closedCapacity> closed {};
        Extra openExtra;                                                  // T-106：本段
        std::array<Extra, (std::size_t) closedCapacity> closedExtra {};   // 与 closed 同下标（存档里是 historyExtra）
        std::array<float, (std::size_t) closedCapacity> closedNow {};     // 重判后的 m′；不存档（读回后先等于当时的判定）
        int closedCount = 0;
        int closedWrite = 0;
        bool haveOpenMainness = false;
        bool openWasMain = false;
        bool legacyState = false;
        // 这一块的输入侧读数（noteMainness 交来，processOutput 按样本计入本段）。不存档。
        bool blockMeasured = false;
        float blockInputLufs = 0.0f;
        float blockLowShare = 0.0f;
        // 这一片待入账的入口能量（processInput 交来，同一片的 processOutput 取走）。不存档。
        double pendingInputEnergy = 0.0;
        int pendingInputSamples = 0;
    };
}
