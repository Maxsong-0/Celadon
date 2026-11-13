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
          + 方案净增益」，可 I 是画像自己那套窗口算的：4/4 型是 512 段、按主段时长指数遗忘的主段加权平均，其余曲风是
          全部有效音频的门控积分（规格第 1 节表）——和这里「最近 32 段里 m′ ≥ 0.5 的」不是同一批段。差出来的不是链的偏差，
          G_trim 却照着去追：通用曲风里前奏轻、后面响，I 还带着前奏、这里已经只剩响段，G_trim 被拉下去 1 ～ 2 dB 再慢慢回来。
          两侧都用这里的段，差值就只剩链实际做了什么。入口比出口早一个链延迟（几十毫秒），按段算能量时不对齐：
          8 s 一段、电平跳 10 dB 的边界上最多差 0.05 dB。
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

        // 本段暂判的 m′。跨过 0.5，或调用方 closeSegment()，就把上一段收进主段账。
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

        // limitGainDb = 这一片交给限幅器的 Limit Gain（手动 + 自动增量 + G_trim + 守门退让）；非有限值按 0 dB 记。
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

        std::unique_ptr<juce::XmlElement> createStateXml() const;
        // 没有 T-106 字段的旧存档（0.1.8.10 及更早）：校验同 0.1.8.10（标签、六个标量属性、closed 的长度、closedCount /
        // closedWrite 的范围；段的内容不看），然后段累计清空、restoredLegacyState() 置 true（旧历史不知道是在什么增益下录的，
        // 折算不了）。带 T-106 字段的新格式：七个属性要齐，用到的每一个量都验——三个样本数是严格的十进制非负整数，
        // 开着的一段与已收的 32 段各自的能量有限且非负、样本数在 0 ～ 8 s × 768 kHz、主段度有限，每段另记的量见 validExtra；
        // 有一项不合规矩就整节拒收、状态不动，同别的节点。读回后各段先按当时的判定，下一次 rejudge() 起按此刻的参考。
        bool restoreStateXml (const juce::XmlElement&);
        bool restoredLegacyState() const noexcept { return legacyState; }

    private:
        // 布局不能动：closed 数组整块 memcpy 进存档的 closed 属性，0.1.8.10 按这个大小读。T-106 的量另放 Extra。
        struct Segment
        {
            double energy = 0.0;
            std::int64_t samples = 0;
            float mainness = 0.0f;
        };

        static_assert (std::is_trivially_copyable_v<Segment> && sizeof (Segment) == 24 && offsetof (Segment, samples) == 8
                           && offsetof (Segment, mainness) == 16,
                       "closed 属性按这个布局整块存，0.1.8.10 按 32 × 24 字节读");

        // T-106 每段另记的量（与 Segment 同下标；布局固定，closedExtra 数组整块进存档的 closedExtra 属性）。
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
                       "closedExtra 属性按这个布局整块存（32 × 48 字节，没有填充）");

        void pushClosed (const Segment& segment, const Extra& extra) noexcept;
        void clearSegments() noexcept;
        void resetChannel (int channel) noexcept;
        void resetInputChannel (int channel) noexcept;
        // 此刻计入电平的段（重判后 m′ ≥ 0.5 的已收段，再加正在开着的主段），从新到旧：visit (段, 它的 Extra)。只在 .cpp 里用。
        template <typename Visit>
        void forEachMainSegment (Visit&& visit) const noexcept;
        static bool validSegment (const Segment& segment) noexcept;
        static bool validExtra (const Extra& extra, std::int64_t segmentSamples) noexcept;

        static constexpr int closedCapacity = 32;
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
        std::array<Extra, (std::size_t) closedCapacity> closedExtra {};   // 与 closed 同下标
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
