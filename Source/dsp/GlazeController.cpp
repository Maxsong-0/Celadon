/*
  ==============================================================================

    GlazeController.cpp
    T-048 阶段 2 编排器（代理 A）：去掉聆听 / 锁定 / 校准状态机。
    插件路径只用链持有的 LiveProfile。每 1 s（以及旋钮 / 风格 / 曲风 / 交付变化）用已发布画像 makePlan，
    速率限制和增益棘轮把已施加增量推向目标。Limit Gain 另加 G_trim 与三个守门的最小退让。
    工程只存 LIVE（GLAZE_STATE formatVersion 2，外层 zlib + base64）。

    音频线程不分配、不加锁。setConstants 只在消息线程、音频已停时由 syncLiveModel 调用。
    宿主 reset 只清滤波器和未完成的保护时隙，不清 LIVE。

  ==============================================================================
*/

#include "GlazeController.h"

#include "ClayGuard.h"
#include "ClayModule.h"
#include "ContrastGuard.h"
#include "GlazeGuard.h"
#include "GlazePlanner.h"
#include "LiveProfile.h"
#include "MusicAnalyzer.h"
#include "OutputLevelTracker.h"
#include "PsrGuard.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

namespace celadon::glaze
{
    namespace
    {
        constexpr int statusReadAttempts = 64;

        bool isFinite (float x) noexcept   { return std::isfinite (x); }

        bool sameBits (float a, float b) noexcept
        {
            return std::memcmp (&a, &b, sizeof (float)) == 0;
        }

        Style styleFromIndex (int index) noexcept
        {
            return static_cast<Style> (juce::jlimit ((int) Style::automatic, (int) Style::clean, index));
        }

        // 方案权重里最大的一项（界面「自动→温润」）。并列时按 通透 > 温润 > 冲击（偏保守的说法）。
        Style dominantStyle (const StyleWeights& w) noexcept
        {
            auto best = Style::clean;
            auto bestWeight = w.clean;

            if (w.warm > bestWeight)   { best = Style::warm;   bestWeight = w.warm; }
            if (w.punchy > bestWeight) { best = Style::punchy; bestWeight = w.punchy; }

            return best;
        }

        // v0.1.7.0 修坯 Clay 的三态设置是否逐位相同（浮点按位、枚举与布尔按值）。
        bool sameClaySettings (const ClaySettings& a, const ClaySettings& b) noexcept
        {
            return a.lowCutMode == b.lowCutMode && a.monoMode == b.monoMode && a.widthMode == b.widthMode
                && sameBits (a.lowCutHz, b.lowCutHz) && sameBits (a.monoHz, b.monoHz) && sameBits (a.widthDb, b.widthDb)
                && a.bypass == b.bypass;
        }

        // 决策器会读到的手动值是否逐位相同（glazePercent / glazeStyle 另算）。浮点按位比，布尔与枚举按值比。
        // v0.1.7.0 起并入 Clay 的三态设置：决策器算 Plan::clay 本身不看它（自动值只随画像、风格、GLAZE、标定快照定），但校准参考
        // 条件看（manualParamsHash），beginBlock 靠这里判断「参考条件的输入变了没有」；三态一变多算一次方案无害（同输入同输出）。
        bool sameAdvancedBalance (const BalanceModule::Params& a, const BalanceModule::Params& b) noexcept
        {
            if (! sameBits (a.harshAmount, b.harshAmount) || ! sameBits (a.targetAmount, b.targetAmount)) return false;
            if (a.harshAmount > 0.0f && ! sameBits (a.transientPreserve, b.transientPreserve)) return false;
            if (a.targetAmount > 0.0f)
            {
                if (a.hasTargetReference != b.hasTargetReference || ! sameBits (a.targetSlopeDbPerOct, b.targetSlopeDbPerOct)) return false;
                if (a.hasTargetReference && a.targetReferenceDb != b.targetReferenceDb) return false;
            }
            return true;
        }

        // 它也是 makeReference 手动参数缓存的键（T-027）：必须覆盖 setManualConditions / emitConditions 读的每一项
        // （含 v0.1.9.0 的「Mast」段，由上面的 sameAdvancedBalance 比）。
        bool samePlanningManual (const ChainParams& a, const ChainParams& b) noexcept
        {
            return sameAdvancedBalance (a.balance, b.balance) && sameClaySettings (a.claySettings, b.claySettings)
                && sameBits (a.balance.recover, b.balance.recover) && sameBits (a.balance.tame, b.balance.tame)
                && sameBits (a.balance.brighten, b.balance.brighten) && a.balance.speed == b.balance.speed
                && a.balance.bypass == b.balance.bypass
                && sameBits (a.color.toneDb, b.color.toneDb) && sameBits (a.color.glue, b.color.glue)
                && sameBits (a.color.heat, b.color.heat) && sameBits (a.color.mixPercent, b.color.mixPercent)
                && sameBits (a.color.outputDb, b.color.outputDb) && a.color.bypass == b.color.bypass
                && sameBits (a.limit.gainDb, b.limit.gainDb) && sameBits (a.limit.ceilingDb, b.limit.ceilingDb)
                && sameBits (a.limit.releaseMs, b.limit.releaseMs) && a.limit.autoRelease == b.limit.autoRelease
                && sameBits (a.limit.lookaheadMs, b.limit.lookaheadMs) && sameBits (a.limit.clip, b.limit.clip)
                && a.limit.bypass == b.limit.bypass;
        }

        // 校准参考条件里「影响声音的手动参数」的哈希：各项量化到 1e-4 后 FNV-1a。
        // 另把约束反馈上限并进来——它改变方案、却不在参考条件的其他项里（「不同方案的数据绝不混用」）。
        // Listen 不算：它在所有测点之后施加监听增益，不改变方案或校准。Ceiling / Lookahead 是参考条件自己的项。
        //
        // 两种口径同一趟算（BUG-004，ADR-018）。取整不是容差：值落在取整边界附近时差 1 ulp 就换一个整数。线性旋钮都吸附在步进上，
        // 重开工程的归一化往返后逐位不变，取整稳定；三个对数刻度值（Limit Release、Clay 低切 / 收单分频点）不吸附，往返后漂几个
        // ulp（ParameterRanges.h），取整会翻转——修前约 0.7 %（Release）、3 %（收单）的旋钮位置重开后校准就不适用了。
        //   - 旧口径（manualParamsHash）：含这三项，逐位同 v0.1.8.0。旧存档里只有它，旧版本读新存档也只看它。
        //   - 离散哈希（discreteParamsHash）：去掉这三项；三项的值另存进参考条件，按相对容差比（GlazeCalibrator::referenceMatches）。
        struct Fnv1a
        {
            std::uint32_t h = 2166136261u;

            void add (std::uint64_t v) noexcept
            {
                for (int i = 0; i < 8; ++i)
                {
                    h ^= (std::uint32_t) ((v >> (8 * i)) & 0xffu);
                    h *= 16777619u;
                }
            }
        };

        constexpr double manualQuantum = 1.0e4;     // 手动参数量化到 1e-4
        constexpr double feedbackQuantum = 1.0e3;   // 约束反馈上限量化到 1e-3 dB
        constexpr std::uint64_t nonFiniteCode = 0x7ff8dead7ff8deadULL;   // 非有限值：固定记号（NaN 与 NaN 视为同一条件）

        // 旧存档升格时，此刻的对数刻度值前后各看多少个 float（upgradeLegacyConditions）：一次重开（APVTS 两趟往返）实测最多漂
        // 12 ulp（Release；低切 4、收单 6，GCC / glibc 穷举，2026-09-29）。
        constexpr int legacyDriftUlps = 16;

        // 一项的量化码（钳在 ±1e12 再取整；超出的值本身就是坏数据）。
        std::int64_t roundedCode (float x, double scale) noexcept
        {
            return std::llround (juce::jlimit (-1.0e12, 1.0e12, (double) x * scale));
        }

        std::uint64_t quantizedCode (float x, double scale) noexcept
        {
            return isFinite (x) ? (std::uint64_t) roundedCode (x, scale) : nonFiniteCode;
        }

        // v0.1.7.0 修坯 Clay 的三态设置（设计规格-Clay 第 6 节「计入 manualParamsHash」）：只并影响声音的部分——三项各自的态、
        // 「手动」项的旋钮值、Bypass。三项都「自动」且没旁通（默认，也是旧工程读入后新参数回到的状态）时一个字节都不加：
        // 哈希与 v0.1.6.2 逐位相同，旧工程存档里的校准重开后照样适用（否则偏差撤回、声音就变了）。其余任何设置都先加一个
        // 记号字再逐项加，与默认状态、与彼此都不相同；改回默认就回到原来的哈希，已提交的校准重新适用（不必重校）。
        bool clayDefaultSettings (const ClaySettings& clay) noexcept
        {
            return clay.lowCutMode == ClayMode::automatic && clay.monoMode == ClayMode::automatic
                && clay.widthMode == ClayMode::automatic && ! clay.bypass;
        }

        // Clay 一项的旋钮值计不计入：Clay 非默认设置且该项「手动」。「自动 / 关」态下旋钮不生效，不计入。
        bool clayValueCounted (const ClaySettings& clay, ClayMode mode) noexcept
        {
            return ! clayDefaultSettings (clay) && mode == ClayMode::manual;
        }

        // 手动参数这一串输入只写这一遍，交给不同的接收方（sink）：线性项、开关、态走 sink.word；三个对数刻度值走
        // sink.continuous（按出现顺序：Release 总在，Clay 低切 / 收单只在计入时出现）。
        //   - HashSink：同一趟算旧口径与离散两份哈希（setManualConditions）；
        //   - WordRecorder：把旧口径的输入串记成一列字，对数刻度值记成「槽」（upgradeLegacyConditions 按前缀逐层代入候选码）。
        template <typename Sink>
        void emitConditions (Sink& sink, const ChainParams& m, float feedbackMaxGainDb) noexcept
        {
            const auto quantized = [&sink] (float x) noexcept { sink.word (quantizedCode (x, manualQuantum)); };
            const auto flag = [&sink] (bool b) noexcept { sink.word (b ? 1u : 0u); };

            quantized (m.balance.recover);
            quantized (m.balance.tame);
            quantized (m.balance.brighten);
            sink.word ((std::uint64_t) (int) m.balance.speed);
            flag (m.balance.bypass);
            quantized (m.color.toneDb);
            quantized (m.color.glue);
            quantized (m.color.heat);
            quantized (m.color.mixPercent);
            quantized (m.color.outputDb);
            flag (m.color.bypass);
            quantized (m.limit.gainDb);
            sink.continuous (m.limit.releaseMs);
            flag (m.limit.autoRelease);
            quantized (m.limit.clip);
            flag (m.limit.bypass);
            sink.word (quantizedCode (feedbackMaxGainDb, feedbackQuantum));
            // 不并「照用旧算法方案」：那份方案就是保存时的方案，存档里的校准正是在它之下量的（重开时仍适用）；
            // 从旧方案换到新算法只会发生在拧 GLAZE、换风格、约束反馈、提交新画像时，这些本身就在参考条件里。

            const auto& clay = m.claySettings;

            if (! clayDefaultSettings (clay))
            {
                sink.word (0x436c6179ULL);   // 「Clay」四个字母：与前面各项的编码分开

                // 每项：它的态；「手动」态再加旋钮值。低切 / 收单分频点是对数刻度；加宽是线性（0.01 dB 步进）。
                sink.word ((std::uint64_t) (int) clay.lowCutMode);
                if (clay.lowCutMode == ClayMode::manual)
                    sink.continuous (clay.lowCutHz);

                sink.word ((std::uint64_t) (int) clay.monoMode);
                if (clay.monoMode == ClayMode::manual)
                    sink.continuous (clay.monoHz);

                sink.word ((std::uint64_t) (int) clay.widthMode);
                if (clay.widthMode == ClayMode::manual)
                    quantized (clay.widthDb);

                flag (clay.bypass);
            }

            // v0.1.9.0 高级母带控制（去刺耳 / 频谱目标，Codex a2927c8）：只在开着时加（默认关，哈希与 v0.1.8.0 相同）。这些值存读工程
            // 逐位不变（0.1 步进的线性参数乘 GLAZE、常数斜率、参考频谱按可往返的十进制存），两份哈希都进，不走容差。
            const auto& advanced = m.balance;

            if (advanced.harshAmount > 0.0f || advanced.targetAmount > 0.0f)
            {
                sink.word (0x4d617374ULL);   // 「Mast」四个字母
                quantized (advanced.harshAmount);
                if (advanced.harshAmount > 0.0f)
                    quantized (advanced.transientPreserve);

                quantized (advanced.targetAmount);

                if (advanced.targetAmount > 0.0f)
                {
                    flag (advanced.hasTargetReference);

                    if (advanced.hasTargetReference)
                        for (const auto value : advanced.targetReferenceDb)
                            quantized (value);
                    else
                        quantized (advanced.targetSlopeDbPerOct);
                }
            }
        }

        // 两份哈希：对数刻度值只进旧口径。
        struct HashSink
        {
            Fnv1a legacy, discrete;

            void word (std::uint64_t w) noexcept          { legacy.add (w); discrete.add (w); }
            void continuous (float value) noexcept        { legacy.add (quantizedCode (value, manualQuantum)); }
        };

        // 旧口径的输入串：最多 57 个字（Clay 段全开 25 个，再加高级母带段 32 个：参考频谱 27 格），三个槽各占一格（位置记在 slotAt，
        // 值记在 slotValue）。
        struct WordRecorder
        {
            static constexpr int maxWords = 64;
            static constexpr int maxSlots = 3;

            std::array<std::uint64_t, maxWords> words {};
            std::array<int, maxSlots> slotAt {};
            std::array<float, maxSlots> slotValue {};
            int numWords = 0, numSlots = 0;

            void word (std::uint64_t w) noexcept
            {
                jassert (numWords < maxWords);
                if (numWords < maxWords)
                    words[(size_t) numWords++] = w;
            }

            void continuous (float value) noexcept
            {
                jassert (numSlots < maxSlots && numWords < maxWords);
                if (numSlots < maxSlots && numWords < maxWords)
                {
                    slotAt[(size_t) numSlots] = numWords;
                    slotValue[(size_t) numSlots] = value;
                    ++numSlots;
                    words[(size_t) numWords++] = 0;   // 占位：代入的是候选码
                }
            }
        };

        // 旧存档升格时一项的候选量化码：此刻的值前后各 legacyDriftUlps 个 float 内，旧口径可能取到的整数（取整单调，端点定区间）。
        // 非有限值只有那个固定记号。
        struct CodeRange
        {
            std::int64_t first = 0, last = 0;
        };

        CodeRange candidateCodes (float x) noexcept
        {
            if (! isFinite (x))
                return { (std::int64_t) nonFiniteCode, (std::int64_t) nonFiniteCode };

            auto low = x, high = x;

            for (int i = 0; i < legacyDriftUlps; ++i)
            {
                low = std::nextafter (low, -std::numeric_limits<float>::infinity());
                high = std::nextafter (high, std::numeric_limits<float>::infinity());
            }

            return { roundedCode (low, manualQuantum), roundedCode (high, manualQuantum) };
        }

        // 升格时候选组合数的上限。三项都在参数范围内时最多 21 × 3 × 11 = 693 组（Release 在 1000 ms 附近、低切 60 Hz、收单 300 Hz）；
        // 超过就是越界的坏值（范围外 ulp 更大、候选随量级线性增长），不升格、照旧按旧口径比，音频线程不会被拖住。
        constexpr std::int64_t maxLegacyCandidates = 1024;

        // 按前缀逐层代入候选码：state 是第 slot 个槽之前的哈希状态；每层只喂「这个槽 + 到下一个槽之前的字」，末层把完整的旧口径
        // 哈希交给 visit。visit 返回 true = 不必再找（要升格的都升完了）。槽号是编译期常数（最多 3 层），不分配。
        template <int slot, typename Visit>
        bool searchLegacyCodes (const WordRecorder& recorder, const std::array<CodeRange, WordRecorder::maxSlots>& ranges,
                                const Fnv1a& state, Visit& visit) noexcept
        {
            static_assert (slot >= 0 && slot < WordRecorder::maxSlots, "slot out of range");
            constexpr auto index = (size_t) slot;

            auto hasNext = false;
            auto end = recorder.numWords;

            if constexpr (slot + 1 < WordRecorder::maxSlots)
            {
                hasNext = slot + 1 < recorder.numSlots;

                if (hasNext)
                    end = recorder.slotAt[index + 1];
            }

            for (auto code = ranges[index].first; code <= ranges[index].last; ++code)
            {
                auto next = state;
                next.add ((std::uint64_t) code);

                for (auto i = recorder.slotAt[index] + 1; i < end; ++i)
                    next.add (recorder.words[(size_t) i]);

                auto done = false;

                if constexpr (slot + 1 < WordRecorder::maxSlots)
                {
                    done = hasNext ? searchLegacyCodes<slot + 1> (recorder, ranges, next, visit) : visit (next.h);
                }
                else
                {
                    juce::ignoreUnused (hasNext);
                    done = visit (next.h);
                }

                if (done)
                    return true;
            }

            return false;
        }

        // ClayPlan::costLimitedMask 的三个有效位（GlazeTypes.h clayCostLimited*）。
        constexpr auto clayCostLimitedAll = static_cast<std::uint8_t> (clayCostLimitedLowCut | clayCostLimitedMono | clayCostLimitedWidth);

        // 方案给的 Clay 自动值按 ClayModule 的参数范围收拾一遍：决策器本来就在范围内（范围内的值原样通过、逐位不变），
        // 这里只防坏数据——非有限值 = 该项中性。执行快照里存的也就总是 ClayModule 能直接接受的值（GlazeProfile 读快照时
        // 按同一范围核对）。低切 0 = 关；大于 0 就钳到 10 ～ 60 Hz。代价 mask（ADR-012，只给界面解释）原样带上，只留有效位。
        ClayPlan sanitizeClay (const ClayPlan& p) noexcept
        {
            const ClayPlan neutral {};
            ClayPlan r;
            r.lowCutHz = isFinite (p.lowCutHz) && p.lowCutHz > 0.0f
                             ? juce::jlimit (ClayModule::minLowCutHz, ClayModule::maxLowCutHz, p.lowCutHz) : 0.0f;
            r.monoHz = isFinite (p.monoHz) ? juce::jlimit (ClayModule::minMonoHz, ClayModule::maxMonoHz, p.monoHz) : neutral.monoHz;
            r.monoAmount = isFinite (p.monoAmount) ? juce::jlimit (0.0f, 1.0f, p.monoAmount) : 0.0f;
            r.widthDb = isFinite (p.widthDb) ? juce::jlimit (ClayModule::minWidthDb, ClayModule::maxWidthDb, p.widthDb) : 0.0f;
            r.costLimitedMask = static_cast<std::uint8_t> (p.costLimitedMask & clayCostLimitedAll);
            return r;
        }

        // Status::clayCostLimitedMask：只报告「自动」态的项（「关 / 手动」项不经决策器，谈不上被代价减量）。GLAZE ≤ 0 由调用方清零。
        std::uint8_t reportedCostLimitedMask (std::uint8_t planned, const ClaySettings& s) noexcept
        {
            unsigned int mask = 0;

            if (s.lowCutMode == ClayMode::automatic)
                mask |= static_cast<unsigned int> (planned & clayCostLimitedLowCut);

            if (s.monoMode == ClayMode::automatic)
                mask |= static_cast<unsigned int> (planned & clayCostLimitedMono);

            if (s.widthMode == ClayMode::automatic)
                mask |= static_cast<unsigned int> (planned & clayCostLimitedWidth);

            return static_cast<std::uint8_t> (mask);
        }

        //==============================================================================
        // 单写者序列锁：写者（每个快照只有一个线程写）逐字写入 32 位原子量，读者前后比序号。无锁、读者不阻塞写者。
        template <typename Payload>
        class SeqLockSnapshot
        {
        public:
            static_assert (std::is_trivially_copyable_v<Payload>, "快照必须平凡可拷贝");

            void write (const Payload& payload) noexcept
            {
                std::array<std::uint32_t, numWords> raw {};
                std::memcpy (raw.data(), &payload, sizeof (Payload));

                const auto s = sequence.load (std::memory_order_relaxed);
                sequence.store (s + 1u, std::memory_order_relaxed);
                std::atomic_thread_fence (std::memory_order_release);

                for (size_t i = 0; i < numWords; ++i)
                    words[i].store (raw[i], std::memory_order_relaxed);

                sequence.store (s + 2u, std::memory_order_release);
            }

            // 读到一致的一份返回 true。写者一直在写、重试用完（实际上碰不到：写一次约 20 ns、每块一次）时返回 false，
            // out 是最后一次读到的内容——每个字都是某一次写入的值（逐字原子），只是可能新旧混在一起；给界面显示一帧无妨。
            bool read (Payload& out) const noexcept
            {
                std::array<std::uint32_t, numWords> raw {};

                for (int attempt = 0; attempt < statusReadAttempts; ++attempt)
                {
                    const auto s1 = sequence.load (std::memory_order_acquire);

                    for (size_t i = 0; i < numWords; ++i)
                        raw[i] = words[i].load (std::memory_order_relaxed);

                    std::atomic_thread_fence (std::memory_order_acquire);

                    if ((s1 & 1u) == 0u && sequence.load (std::memory_order_relaxed) == s1)
                    {
                        std::memcpy (static_cast<void*> (&out), raw.data(), sizeof (Payload));   // Payload 平凡可拷贝（上面 static_assert）；转 void* 免 GCC -Wclass-memaccess 误报
                        return true;
                    }
                }

                std::memcpy (static_cast<void*> (&out), raw.data(), sizeof (Payload));   // Payload 平凡可拷贝（上面 static_assert）；转 void* 免 GCC -Wclass-memaccess 误报
                return false;
            }

        private:
            static constexpr size_t numWords = (sizeof (Payload) + sizeof (std::uint32_t) - 1) / sizeof (std::uint32_t);

            std::atomic<std::uint32_t> sequence { 0 };
            std::array<std::atomic<std::uint32_t>, numWords> words {};
        };
    }

    struct GlazeController::Impl
    {
        static constexpr float gainUpDbPerSecond = 0.2f;
        static constexpr float gainDownDbPerSecond = 1.0f;
        static constexpr float gainDeadbandEnterDb = 0.3f;
        static constexpr float gainDeadbandExitDb = 0.1f;
        static constexpr float unitUpPerSecond = 2.0f;
        static constexpr float unitDownPerSecond = 10.0f;
        static constexpr float trimDbPerSecond = 0.05f;
        static constexpr float trimLimitDb = 3.0f;
        static constexpr float weightPerSecond = 0.05f;

        MusicAnalyzer analyzer;
        LiveProfile* live = nullptr;
        bool sinkProbe = false;
        SegmentSink* externalSink = nullptr;
        const BalanceModule* pendingFrames = nullptr;

        GlazeGuard budget;
        ContrastGuard contrast;
        PsrGuard psr;
        OutputLevelTracker outputLevel;
        ClayGuard clayGuard;
        ClayPressureMonitor clayPressure;
        ClayGuardKeepMonitor clayKeep;

        Constants constantsOverride {};
        bool constantsOverridden = false;

        double sampleRate = 48000.0;
        bool prepared = false;
        std::int64_t timelineSamples = 0;
        std::int64_t nextGridSample = 48000;
        std::int64_t positionTolerance = 2400;

        ChainParams manual {};
        ChainParams effective {};
        ChainParams lastManual {};
        bool planningKnown = false;
        bool paused = false;
        bool active = false;
        Style currentStyle = Style::automatic;
        Genre currentGenre = Genre::general;
        Delivery currentDelivery = Delivery::standard;
        Genre pendingModel = Genre::general;
        float lastGlaze = 0.0f;

        MusicProfile followed {};
        bool haveFollowed = false;
        Plan targetPlan {};
        bool haveTarget = false;
        StyleWeights automaticWeights {};
        AutoIncrements applied {};
        bool gainInDeadband = false;
        float gTrim = 0.0f;
        GuardOutput budgetOut {}, contrastOut {}, psrOut {};
        float currentMainness = 1.0f;

        float liveToneOffsetDb = 0.0f;
        float liveHeatReduction = 0.0f;
        float liveGlueReduction = 0.0f;
        float pressureScale = 1.0f;
        ClayPlan liveClay {};
        ClayPlan plannedClay {};
        bool claySupported = false;
        double lastMusicSeconds = 0.0;

        HostInfo host {};
        bool hostFresh = false;
        bool expectKnown = false;
        bool lastPlaying = false;
        std::int64_t expectedPosition = 0;
        bool offline = false;

        SeqLockSnapshot<Status> statusSnapshot;
        std::atomic<bool> legacyDiscarded { false };
        TestProbe probe {};

        const Constants& activeConstants() const noexcept
        {
            return constantsOverridden ? constantsOverride : factoryConstants();
        }

        bool hostPlaying() const noexcept
        {
            if (offline || ! host.hasPlayState)
                return true;

            return host.isPlaying;
        }

        void bindLive()
        {
            if (! prepared)
                return;

            if (sinkProbe)
            {
                if (externalSink != nullptr)
                {
                    analyzer.setSegmentSink (externalSink);
                    analyzer.setLiveMode (true, pendingFrames);
                }
                else
                {
                    analyzer.setLiveMode (false, pendingFrames);
                    analyzer.setSegmentSink (nullptr);
                }
            }
            else if (live != nullptr)
            {
                analyzer.setSegmentSink (live);
                analyzer.setLiveMode (true);
            }
            else
            {
                analyzer.setLiveMode (false);
                analyzer.setSegmentSink (nullptr);
            }
        }

        void snapAutomatic() noexcept
        {
            applied = {};
            gTrim = 0.0f;
            gainInDeadband = false;
            liveToneOffsetDb = liveHeatReduction = liveGlueReduction = 0.0f;
            budget.snapRelief();
            contrast.snapRelief();
            psr.snapRelief();
            budgetOut = {};
            contrastOut = {};
            psrOut = {};
        }

        static void slew (float& current, float target, float upPerSecond, float downPerSecond, double dt, bool downwardOnly) noexcept
        {
            if (! std::isfinite (current))
                current = 0.0f;

            if (! std::isfinite (target))
                return;

            auto error = target - current;

            if (downwardOnly && error > 0.0f)
                error = 0.0f;

            const auto rate = error > 0.0f ? upPerSecond : downPerSecond;
            const auto step = rate * (float) dt;
            current += juce::jlimit (-step, step, error);
        }

        void slewGain (float target, float mainness, double dt) noexcept
        {
            const auto error = target - applied.limitGainDb;
            const auto magnitude = std::abs (error);

            if (gainInDeadband)
            {
                if (magnitude > gainDeadbandEnterDb)
                    gainInDeadband = false;
            }
            else if (magnitude < gainDeadbandExitDb)
            {
                gainInDeadband = true;
            }

            if (gainInDeadband)
                return;

            slew (applied.limitGainDb, target, gainUpDbPerSecond, gainDownDbPerSecond, dt, mainness < 0.5f);
        }

        void slewWeights (const StyleWeights& target, double dt) noexcept
        {
            slew (automaticWeights.warm, target.warm, weightPerSecond, weightPerSecond, dt, false);
            slew (automaticWeights.punchy, target.punchy, weightPerSecond, weightPerSecond, dt, false);
            slew (automaticWeights.clean, target.clean, weightPerSecond, weightPerSecond, dt, false);
            automaticWeights.warm = juce::jlimit (0.0f, 1.0f, automaticWeights.warm);
            automaticWeights.punchy = juce::jlimit (0.0f, 1.0f, automaticWeights.punchy);
            automaticWeights.clean = juce::jlimit (0.0f, 1.0f, automaticWeights.clean);
            const auto sum = automaticWeights.warm + automaticWeights.punchy + automaticWeights.clean;

            if (sum > 0.0f)
            {
                automaticWeights.warm /= sum;
                automaticWeights.punchy /= sum;
                automaticWeights.clean /= sum;
            }
        }

        void replan (const MusicProfile* profile, double dt) noexcept
        {
            const auto& constants = activeConstants();

            if (profile != nullptr)
                slewWeights (resolveAutomaticWeights (*profile, constants), dt);

            PlanInputs inputs;
            inputs.profile = profile;
            inputs.style = currentStyle;
            inputs.genre = currentGenre;
            inputs.delivery = currentDelivery;
            inputs.glazePercent = manual.glazePercent;
            inputs.manual = manual;
            inputs.constants = &constants;
            inputs.maxGainDb = 1.0e9f;
            inputs.lockedWeights = &automaticWeights;
            targetPlan = makePlan (inputs);
            haveTarget = true;

            auto caps = blendConstants (constants, targetPlan.weights, currentGenre, currentDelivery);
            const auto mask = static_cast<std::uint8_t> (targetPlan.manualBudgetMask & manualBudgetAll);

            if ((mask & manualBudgetHeat) != 0)
                caps.heatDriveCap = 0.0f;

            if ((mask & manualBudgetGlue) != 0)
                caps.glueGrMeanCapDb = caps.glueGrP95CapDb = 0.0f;

            if ((mask & manualBudgetClip) != 0)
                caps.clipDepthCapDb = caps.clipActivityCap = 0.0f;

            budget.setLimits (caps, constants);
            clayGuard.setConstants (constants);
        }

        void slewApplied (float mainness, double dt) noexcept
        {
            const auto& target = targetPlan.increments;
            slew (applied.balanceRecover, target.balanceRecover, unitUpPerSecond, unitDownPerSecond, dt, false);
            slew (applied.balanceTame, target.balanceTame, unitUpPerSecond, unitDownPerSecond, dt, false);
            slew (applied.colorToneDb, target.colorToneDb, gainUpDbPerSecond, gainDownDbPerSecond, dt, false);
            slew (applied.colorGlue, target.colorGlue, unitUpPerSecond, unitDownPerSecond, dt, false);
            slew (applied.colorHeat, target.colorHeat, unitUpPerSecond, unitDownPerSecond, dt, false);
            slew (applied.limitClip, target.limitClip, unitUpPerSecond, unitDownPerSecond, dt, false);
            slewGain (target.limitGainDb, mainness, dt);
        }

        bool matchingLiveSlopes (float& referenceSlope, float& currentSlope) const noexcept
        {
            const auto& view = analyzer.getShortTermView();
            const auto& learned = followed.spectralSlopeDbPerOct;
            const auto& current = view.liveSpectralSlopeDbPerOct;

            if (! haveFollowed || ! learned.valid || ! isFinite (learned.value) || learned.confidence < 0.5f
                || ! current.valid || ! isFinite (current.value) || current.confidence < 0.6f)
                return false;

            double sw = 0.0, sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0, trustedWeight = 0.0;

            for (int band = 0; band < numBands; ++band)
            {
                const auto w = (double) view.liveSlopeWeights[(std::size_t) band];
                const auto y = followed.bandLongTermDb[(std::size_t) band];
                const auto confidence = followed.bandConfidence[(std::size_t) band];

                if (! isFinite (y) || (w > 0.0 && (! isFinite (confidence) || ! (confidence > 0.0f))))
                    return false;

                trustedWeight += w * (double) juce::jlimit (0.0f, 1.0f, confidence);
                const auto x = (double) band / 3.0;
                sw += w;
                sx += w * x;
                sy += w * y;
                sxx += w * x * x;
                sxy += w * x * y;
            }

            const auto denominator = sw * sxx - sx * sx;

            if (! (denominator > 1.0e-9) || trustedWeight < 0.5 * sw)
                return false;

            referenceSlope = (float) ((sw * sxy - sx * sy) / denominator);
            currentSlope = current.value;
            return true;
        }

        void updateFollowers (int numSamples) noexcept
        {
            const auto& view = analyzer.getShortTermView();
            const auto& constants = activeConstants();
            const auto dt = (double) numSamples / sampleRate;

            if (! active || ! haveFollowed || paused || manual.color.bypass || ! (manual.color.mixPercent > 0.0f)
                || ! view.musicActive)
                return;

            float referenceSlope = 0.0f, currentSlope = 0.0f;
            const auto slopesOk = matchingLiveSlopes (referenceSlope, currentSlope);

            if (slopesOk && view.momentaryLufs.valid && isFinite (view.momentaryLufs.value)
                && view.momentaryLufs.value >= -50.0f
                && ! (followed.integratedLufs.valid && view.momentaryLufs.value < followed.integratedLufs.value - 15.0f)
                && isFinite (constants.toneDbPerSlopeDb))
            {
                const auto caps = blendConstants (constants, targetPlan.weights, currentGenre, currentDelivery);
                const auto strength = juce::jlimit (0.0f, 1.0f, manual.glazePercent * 0.01f) * confidenceScale (&followed, constants);

                if (strength > 0.0f && isFinite (caps.toneMaxDb) && caps.toneMaxDb > 0.0f)
                {
                    const auto difference = referenceSlope - currentSlope;
                    const auto outside = std::copysign (juce::jmax (0.0f, std::abs (difference) - automaticToneSlopeDeadbandDbPerOct), difference);
                    const auto base = strength > 0.0f ? targetPlan.increments.colorToneDb / strength : 0.0f;
                    const auto brightCap = toneBrightCapDb (caps, constants, currentGenre);
                    const auto target = juce::jlimit (-caps.toneMaxDb, brightCap, base + outside * juce::jmax (0.0f, constants.toneDbPerSlopeDb)) - base;
                    const auto step = 0.15f * (float) dt;
                    liveToneOffsetDb += juce::jlimit (-step, step, target - liveToneOffsetDb);
                }
            }

            if (slopesOk && view.momentaryLufs.valid && isFinite (view.momentaryLufs.value)
                && view.momentaryLufs.value >= -50.0f && followed.integratedLufs.valid
                && followed.integratedLufs.confidence >= 0.5f && followed.representativeness >= 0.5f
                && view.momentaryLufs.value >= followed.integratedLufs.value - 15.0f
                && view.psr3sDb.valid && isFinite (view.psr3sDb.value)
                && followed.plrDb.valid && isFinite (followed.plrDb.value) && followed.plrDb.confidence >= 0.5f
                && constants.liveColorDensityRangeDb > 0.0f && constants.liveColorBrightRangeDbPerOct > 0.0f)
            {
                const auto density = juce::jlimit (0.0f, 1.0f, juce::jmax (
                    followed.plrDb.value - view.psr3sDb.value - constants.liveColorDensityDeadbandDb,
                    8.0f - view.psr3sDb.value) / constants.liveColorDensityRangeDb);
                const auto brightness = juce::jlimit (0.0f, 1.0f,
                    (currentSlope - juce::jmin (referenceSlope, 0.0f) - constants.liveColorBrightDeadbandDbPerOct)
                        / constants.liveColorBrightRangeDbPerOct);

                auto approach = [&] (float& reduction, float risk, float maximum)
                {
                    if (! isFinite (maximum))
                        return;

                    maximum = juce::jlimit (0.0f, 1.0f, maximum);
                    const auto target = maximum * risk;

                    if (target != 0.0f && std::abs (target - reduction) < 0.025f)
                        return;

                    const auto speed = target > reduction ? constants.liveColorAttackPerSecond : constants.liveColorReleasePerSecond;

                    if (! (speed > 0.0f) || ! isFinite (speed))
                        return;

                    const auto step = juce::jmin (1.0f, speed) * (float) dt;
                    reduction = juce::jlimit (0.0f, maximum, reduction + juce::jlimit (-step, step, target - reduction));
                };

                approach (liveHeatReduction, juce::jmax (density, brightness), constants.liveHeatMaxReduction);
                approach (liveGlueReduction, juce::jmax (density, 0.5f * brightness), constants.liveGlueMaxReduction);
            }
        }

        void updateClay() noexcept
        {
            const auto seconds = lastMusicSeconds;
            lastMusicSeconds = 0.0;
            const auto& constants = activeConstants();
            const auto floorHz = isFinite (constants.clayLowCutFloorHz) && constants.clayLowCutFloorHz > 0.0f
                                     ? constants.clayLowCutFloorHz : ClayModule::minLowCutHz;
            plannedClay = haveTarget ? sanitizeClay (scaleClayPlan (targetPlan.clay, pressureScale, floorHz)) : ClayPlan {};
            claySupported = active && haveFollowed && followed.formatVersion >= clayProfileFormatVersion;
            const auto& view = analyzer.getShortTermView();

            if (claySupported)
                liveClay = sanitizeClay (clayGuard.update (plannedClay, view, seconds, true));
            else
                liveClay = sanitizeClay (clayGuard.update (ClayPlan {}, view, seconds, active));

            effective.clay = resolveClayParams (manual.claySettings, liveClay, manual.glazePercent);

            if (isHouse (currentGenre) && claySupported)
            {
                const auto& ledger = analyzer.getSegmentLedger();
                const auto& house = constants.house;
                const auto barSeconds = ledger.getMode() == SegmentMode::bars ? ledger.getCurrentSegmentSeconds() : 0.0;
                const auto required = barSeconds > 0.0 ? (double) juce::jmax (1, house.phaseHintSegments) * barSeconds
                                                       : house.phaseHintFallbackSeconds;
                clayGuard.updateHouseHint (view, seconds, ledger.isCurrentSegmentFull(), required);
            }
        }

        void applyLimitExtras() noexcept
        {
            if (! active || targetPlan.automaticLevelWithdrawn)
                return;

            const auto manualGain = manual.limit.gainDb;
            const auto afterIncrement = effective.limit.gainDb;

            if (! isFinite (manualGain) || ! isFinite (afterIncrement))
                return;

            const auto autoPart = juce::jmax (0.0f, afterIncrement - manualGain);
            const auto trim = gTrim > 0.0f ? gTrim : juce::jmax (gTrim, -autoPart);
            const auto withTrim = juce::jlimit (0.0f, 24.0f, afterIncrement + trim);
            auto relief = budgetOut.reliefDb;
            GuardCause cause = budgetOut.reliefDb < 0.0f ? budgetOut.cause : GuardCause::none;

            auto consider = [&] (const GuardOutput& guard)
            {
                if (guard.reliefDb < relief)
                {
                    relief = guard.reliefDb;
                    cause = guard.cause;
                }
            };

            consider (contrastOut);
            consider (psrOut);
            juce::ignoreUnused (cause);
            const auto automaticTotal = juce::jmax (0.0f, withTrim - manualGain);
            const auto reliefExecuted = juce::jmin (0.0f, juce::jmax (relief, -automaticTotal));
            effective.limit.gainDb = juce::jlimit (0.0f, 24.0f, withTrim + reliefExecuted);
        }

        void publishStatus() noexcept
        {
            Status status;
            status.phase = Phase::unlearned;
            status.calibrationValid = false;
            status.requiredSeconds = 0.0f;
            status.houseTargetSegments = 0;
            status.houseRequiredFullSegments = 0;
            status.houseTargetSeconds = 0.0f;
            status.limitedByManual = haveTarget && targetPlan.limitedByManual;
            status.resolvedStyle = currentStyle == Style::automatic ? dominantStyle (haveTarget ? targetPlan.weights : automaticWeights) : currentStyle;
            status.effectiveTargetLufs = active && haveTarget ? targetPlan.effectiveTargetLufs : std::numeric_limits<float>::quiet_NaN();
            status.clayLowCutHz = effective.clay.lowCutHz;
            status.clayMonoHz = effective.clay.monoHz;
            status.clayMonoAmount = effective.clay.monoAmount;
            status.clayWidthDb = effective.clay.widthDb;
            const auto guardState = clayGuard.getState();
            status.clayPhaseGuardActive = guardState.phaseActive;
            status.clayWidthGuardActive = guardState.widthActive;
            status.clayPeakGuardActive = clayKeep.active();
            status.clayPeakGuardKept = clayKeep.keptFraction();
            status.clayCostLimitedMask = active ? reportedCostLimitedMask (plannedClay.costLimitedMask, manual.claySettings) : 0;
            status.houseActive = isHouse (currentGenre);

            if (status.houseActive)
            {
                const auto hint = clayGuard.getHouseHint();
                status.housePhaseHint = hint.active;
                status.housePhaseMonoLossDb = hint.monoLossDb;
            }

            LiveDiagnostics diagnostics;

            if (live != nullptr)
                diagnostics = live->getDiagnostics();

            status.confidence = diagnostics.confidence;
            status.currentMainness = currentMainness;
            status.coldExport = offline && diagnostics.confidence < 0.5f;

            auto relief = budgetOut.reliefDb;
            auto cause = budgetOut.reliefDb < 0.0f ? budgetOut.cause : GuardCause::none;
            auto engaged = budgetOut.engaged;

            auto consider = [&] (const GuardOutput& guard)
            {
                engaged = engaged || guard.engaged;

                if (guard.reliefDb < relief)
                {
                    relief = guard.reliefDb;
                    cause = guard.cause;
                }
            };

            consider (contrastOut);
            consider (psrOut);
            status.protectionDb = active ? relief : 0.0f;
            status.guardCause = active ? cause : GuardCause::none;
            status.guardEngaged = active && engaged;
            status.planLimited = active && budget.isPlanLimited();
            statusSnapshot.write (status);
        }

        void coldStart (bool legacy) noexcept
        {
            snapAutomatic();
            haveTarget = false;
            targetPlan = {};
            automaticWeights = {};
            timelineSamples = 0;
            nextGridSample = (std::int64_t) std::llround (sampleRate);
            pressureScale = 1.0f;
            liveClay = {};
            plannedClay = {};
            haveFollowed = false;
            followed = {};
            budget.clear();
            contrast.clear();
            psr.clear();
            outputLevel.clear();
            clayGuard.reset();
            clayPressure.reset();
            clayKeep.reset();
            currentMainness = 1.0f;

            if (prepared && live != nullptr)
                live->reset();

            legacyDiscarded.store (legacy, std::memory_order_relaxed);
        }

        bool restoreBody (const juce::XmlElement& body)
        {
            if (live != nullptr)
            {
                const auto* profile = body.getChildByName ("LIVE_PROFILE");

                if (profile == nullptr || ! live->restoreStateXml (*profile))
                    return false;
            }

            const auto* control = body.getChildByName ("CONTROL");
            const auto* budgetXml = body.getChildByName ("BUDGET_GUARD");
            const auto* contrastXml = body.getChildByName ("CONTRAST_GUARD");
            const auto* psrXml = body.getChildByName ("PSR_GUARD");
            const auto* levelXml = body.getChildByName ("OUTPUT_LEVEL");
            const auto* clayXml = body.getChildByName ("CLAY");

            if (control == nullptr || budgetXml == nullptr || contrastXml == nullptr || psrXml == nullptr
                || levelXml == nullptr || clayXml == nullptr)
                return false;

            auto restoredApplied = applied;
            auto restoredWeights = automaticWeights;
            auto restoredTrim = gTrim;
            auto restoredDeadband = gainInDeadband;
            auto restoredTone = liveToneOffsetDb;
            auto restoredHeat = liveHeatReduction;
            auto restoredGlue = liveGlueReduction;
            xmlio::Reader reader (*control);
            reader.field ("balanceRecover", restoredApplied.balanceRecover);
            reader.field ("balanceTame", restoredApplied.balanceTame);
            reader.field ("colorToneDb", restoredApplied.colorToneDb);
            reader.field ("colorGlue", restoredApplied.colorGlue);
            reader.field ("colorHeat", restoredApplied.colorHeat);
            reader.field ("limitGainDb", restoredApplied.limitGainDb);
            reader.field ("limitClip", restoredApplied.limitClip);
            reader.field ("gTrim", restoredTrim);
            reader.field ("warm", restoredWeights.warm);
            reader.field ("punchy", restoredWeights.punchy);
            reader.field ("clean", restoredWeights.clean);
            reader.field ("gainDeadband", restoredDeadband);
            reader.field ("liveToneOffsetDb", restoredTone);
            reader.field ("liveHeatReduction", restoredHeat);
            reader.field ("liveGlueReduction", restoredGlue);

            const auto timeline = control->getStringAttribute ("timelineSamples").getLargeIntValue();
            const auto nextGrid = control->getStringAttribute ("nextGridSample").getLargeIntValue();

            if (! reader.ok || ! control->hasAttribute ("timelineSamples") || ! control->hasAttribute ("nextGridSample"))
                return false;

            auto restoredScale = pressureScale;
            auto restoredClay = liveClay;
            ClayGuardState restoredGuard;
            xmlio::Reader clayReader (*clayXml);
            clayReader.field ("pressureScale", restoredScale);
            clayReader.field ("lowCutHz", restoredClay.lowCutHz);
            clayReader.field ("monoHz", restoredClay.monoHz);
            clayReader.field ("monoAmount", restoredClay.monoAmount);
            clayReader.field ("widthDb", restoredClay.widthDb);
            clayReader.field ("costLimitedMask", restoredClay.costLimitedMask);
            clayReader.field ("phaseFloor", restoredGuard.phaseFloor);
            clayReader.field ("phaseHoldSeconds", restoredGuard.phaseHoldSeconds);
            clayReader.field ("widthCapDb", restoredGuard.widthCapDb);
            clayReader.field ("phaseActive", restoredGuard.phaseActive);
            clayReader.field ("widthActive", restoredGuard.widthActive);

            if (! clayReader.ok || ! budget.restoreStateXml (*budgetXml) || ! contrast.restoreStateXml (*contrastXml)
                || ! psr.restoreStateXml (*psrXml) || ! outputLevel.restoreStateXml (*levelXml))
                return false;

            applied = restoredApplied;
            automaticWeights = restoredWeights;
            gTrim = restoredTrim;
            gainInDeadband = restoredDeadband;
            liveToneOffsetDb = restoredTone;
            liveHeatReduction = restoredHeat;
            liveGlueReduction = restoredGlue;
            timelineSamples = timeline;
            nextGridSample = nextGrid;
            pressureScale = restoredScale;
            liveClay = sanitizeClay (restoredClay);
            clayGuard.setState (restoredGuard);
            budgetOut.reliefDb = budget.getProtectionDb();
            budgetOut.cause = budget.getProtectionDb() < 0.0f ? GuardCause::budget : GuardCause::none;
            budgetOut.engaged = budget.isEngaged();
            haveTarget = false;
            legacyDiscarded.store (false, std::memory_order_relaxed);
            return true;
        }

        std::unique_ptr<juce::XmlElement> createStateXml() const
        {
            auto body = std::make_unique<juce::XmlElement> (liveBodyTag);

            if (live != nullptr)
                body->addChildElement (live->createStateXml().release());

            auto control = std::make_unique<juce::XmlElement> ("CONTROL");
            xmlio::Writer writer (*control);
            writer.field ("balanceRecover", applied.balanceRecover);
            writer.field ("balanceTame", applied.balanceTame);
            writer.field ("colorToneDb", applied.colorToneDb);
            writer.field ("colorGlue", applied.colorGlue);
            writer.field ("colorHeat", applied.colorHeat);
            writer.field ("limitGainDb", applied.limitGainDb);
            writer.field ("limitClip", applied.limitClip);
            writer.field ("gTrim", gTrim);
            writer.field ("warm", automaticWeights.warm);
            writer.field ("punchy", automaticWeights.punchy);
            writer.field ("clean", automaticWeights.clean);
            writer.field ("gainDeadband", gainInDeadband);
            writer.field ("liveToneOffsetDb", liveToneOffsetDb);
            writer.field ("liveHeatReduction", liveHeatReduction);
            writer.field ("liveGlueReduction", liveGlueReduction);
            control->setAttribute ("timelineSamples", juce::String (timelineSamples));
            control->setAttribute ("nextGridSample", juce::String (nextGridSample));
            body->addChildElement (control.release());
            body->addChildElement (budget.createStateXml().release());
            body->addChildElement (contrast.createStateXml().release());
            body->addChildElement (psr.createStateXml().release());
            body->addChildElement (outputLevel.createStateXml().release());

            auto clay = std::make_unique<juce::XmlElement> ("CLAY");
            xmlio::Writer clayWriter (*clay);
            clayWriter.field ("pressureScale", pressureScale);
            clayWriter.field ("lowCutHz", liveClay.lowCutHz);
            clayWriter.field ("monoHz", liveClay.monoHz);
            clayWriter.field ("monoAmount", liveClay.monoAmount);
            clayWriter.field ("widthDb", liveClay.widthDb);
            clayWriter.field ("costLimitedMask", liveClay.costLimitedMask);
            const auto guardState = clayGuard.getState();
            clayWriter.field ("phaseFloor", guardState.phaseFloor);
            clayWriter.field ("phaseHoldSeconds", guardState.phaseHoldSeconds);
            clayWriter.field ("widthCapDb", guardState.widthCapDb);
            clayWriter.field ("phaseActive", guardState.phaseActive);
            clayWriter.field ("widthActive", guardState.widthActive);
            body->addChildElement (clay.release());

            auto state = std::make_unique<juce::XmlElement> (stateTag);
            state->setAttribute ("formatVersion", liveContainerFormatVersion);
            state->addChildElement (compressLiveBody (*body, factoryConstants().version).release());
            return state;
        }

        void prepare (const juce::dsp::ProcessSpec& spec)
        {
            if (spec.sampleRate > 0.0 && std::isfinite (spec.sampleRate))
                sampleRate = spec.sampleRate;

            positionTolerance = std::max<std::int64_t> (2, (std::int64_t) std::llround (sampleRate * 0.05));
            analyzer.prepare (spec);
            budget.prepare (sampleRate);
            contrast.prepare (sampleRate);
            psr.prepare (sampleRate);
            outputLevel.prepare (sampleRate);
            clayGuard.reset();
            clayGuard.setConstants (activeConstants());
            clayPressure.reset();
            clayKeep.reset();
            timelineSamples = 0;
            nextGridSample = (std::int64_t) std::llround (sampleRate);
            haveTarget = false;
            snapAutomatic();
            sinkProbe = false;
            externalSink = nullptr;
            pendingFrames = nullptr;
            prepared = true;
            bindLive();
        }

        const ChainParams& beginBlock (const ChainParams& newManual, bool hostBypassed) noexcept
        {
            manual = newManual;
            paused = hostBypassed;
            active = manual.glazePercent > 0.0f;
            pendingModel = genreFromIndex (manual.glazeGenre);

            if (! prepared)
            {
                effective = manual;
                effective.clay = resolveClayParams (manual.claySettings, ClayPlan {}, manual.glazePercent);
                return effective;
            }

            const auto style = styleFromIndex (manual.glazeStyle);
            const auto genre = genreFromIndex (manual.glazeGenre);
            const auto delivery = deliveryFromIndex (manual.glazeDelivery);
            const auto changed = ! planningKnown || style != currentStyle || genre != currentGenre || delivery != currentDelivery
                                 || ! sameBits (lastGlaze, manual.glazePercent) || ! samePlanningManual (manual, lastManual);
            currentStyle = style;
            currentGenre = genre;
            currentDelivery = delivery;
            lastGlaze = manual.glazePercent;
            lastManual = manual;
            planningKnown = true;
            probe.transitionActive = changed;
            analyzer.setGenre (currentGenre);

            if (! active)
                snapAutomatic();

            updateClay();
            auto increments = applied;

            if (active && haveFollowed && ! manual.color.bypass && manual.color.mixPercent > 0.0f)
            {
                const auto& constants = activeConstants();
                const auto caps = blendConstants (constants, haveTarget ? targetPlan.weights : automaticWeights, currentGenre, currentDelivery);
                const auto strength = juce::jlimit (0.0f, 1.0f, manual.glazePercent * 0.01f) * confidenceScale (&followed, constants);
                const auto cap = caps.toneMaxDb;

                if (isFinite (cap) && cap > 0.0f && strength > 0.0f)
                {
                    const auto bright = toneBrightCapDb (caps, constants, currentGenre);
                    const auto offset = std::round (liveToneOffsetDb * 100.0f) * 0.01f;
                    increments.colorToneDb = juce::jlimit (-cap * strength, bright * strength, applied.colorToneDb + offset * strength);
                }
            }

            effective = applyIncrements (manual, active ? increments : AutoIncrements {});

            if (active && ! manual.color.bypass && manual.color.mixPercent > 0.0f && isFinite (manual.color.heat))
                effective.color.heat -= juce::jmax (0.0f, effective.color.heat - manual.color.heat) * liveHeatReduction;

            if (active && ! manual.color.bypass && manual.color.mixPercent > 0.0f && isFinite (manual.color.glue))
                effective.color.glue -= juce::jmax (0.0f, effective.color.glue - manual.color.glue) * liveGlueReduction;

            if (active && ! manual.balance.bypass)
            {
                const auto& evidence = analyzer.getShortTermView().liveTameEvidenceDb;
                const auto& constants = activeConstants();

                if (haveFollowed && evidence.valid && isFinite (evidence.value) && constants.tameEvidenceFullDb > 0.0f)
                {
                    const auto caps = blendConstants (constants, haveTarget ? targetPlan.weights : automaticWeights);
                    const auto glaze = juce::jlimit (0.0f, 1.0f, manual.glazePercent * 0.01f);
                    const auto tameCap = isFinite (caps.tameCap) ? juce::jlimit (0.0f, 100.0f, caps.tameCap) : 0.0f;
                    const auto liveTame = tameCap * glaze * confidenceScale (&followed, constants)
                                        * juce::jlimit (0.0f, 1.0f, evidence.value / constants.tameEvidenceFullDb);
                    effective.balance.tame = juce::jlimit (0.0f, 100.0f, manual.balance.tame + juce::jmax (applied.balanceTame, liveTame));
                }
            }

            effective.clay = resolveClayParams (manual.claySettings, liveClay, manual.glazePercent);
            applyLimitExtras();
            return effective;
        }

        void endBlock (const ModuleStats& stats, const juce::AudioBuffer<float>* output, const LoudnessMeter& inputMeter,
                       const LoudnessMeter& outputMeter) noexcept
        {
            if (! prepared)
                return;

            const auto numSamples = std::max (0, stats.numSamples);
            const auto music = analyzer.getShortTermView().musicActive && ! paused && hostPlaying();
            lastMusicSeconds = music ? (double) numSamples / sampleRate : 0.0;

            float loudness = 0.0f, lowShare = 0.0f;
            const auto reading = analyzer.getSegmentLedger().getCurrentReading (loudness, lowShare);

            if (reading == SegmentLedger::Reading::quiet)
                currentMainness = 0.0f;
            else if (reading == SegmentLedger::Reading::measured && live != nullptr)
                currentMainness = live->mainnessFor (true, loudness, true, lowShare);
            else
                currentMainness = 1.0f;

            outputLevel.noteMainness (currentMainness);

            // 宿主旁通、听原声、Color / Limit 旁通时出口不是方案在听的信号。半格时隙留着会让链按 1 个样本切片。
            const auto guardFrozen = paused || manual.color.bypass || manual.limit.bypass;

            if (output != nullptr && ! guardFrozen)
                outputLevel.processOutput (*output);

            if (live != nullptr)
            {
                followed = live->getProfile();
                haveFollowed = true;
            }

            const auto dt = (double) numSamples / sampleRate;
            timelineSamples += numSamples;
            auto wantPlan = ! haveTarget || probe.transitionActive;
            const auto grid = (std::int64_t) std::llround (sampleRate);

            if (grid > 0 && timelineSamples >= nextGridSample)
            {
                wantPlan = true;

                while (timelineSamples >= nextGridSample)
                    nextGridSample += grid;
            }

            probe.transitionActive = false;

            if (! active)
            {
                snapAutomatic();
            }
            else
            {
                if (wantPlan)
                    replan (haveFollowed ? &followed : nullptr, dt);

                auto guardStats = stats;
                guardStats.musicActive = music;

                if (guardFrozen)
                {
                    budget.discardPartialSlot();
                }
                else if (! targetPlan.automaticLevelWithdrawn)
                {
                    budgetOut = budget.process (guardStats);
                    const auto inputLufs = inputMeter.getShortTermLufs();
                    const auto outputLufs = outputMeter.getShortTermLufs();
                    contrastOut = contrast.process (inputLufs, outputLufs, music, numSamples, currentDelivery == Delivery::club);
                    const auto& psrReading = analyzer.getShortTermView().psr3sDb;
                    psrOut = psr.process (psrReading.value, psrReading.valid, music, numSamples, currentDelivery);
                }

                slewApplied (currentMainness, dt);

                if (! guardFrozen && ! targetPlan.automaticLevelWithdrawn && ! budget.isRetreating() && outputLevel.mainLevelValid()
                    && haveTarget && isFinite (targetPlan.effectiveTargetLufs))
                {
                    const auto error = targetPlan.effectiveTargetLufs - outputLevel.mainLevelLufs();
                    const auto step = trimDbPerSecond * (float) dt;
                    gTrim = juce::jlimit (-trimLimitDb, trimLimitDb, gTrim + juce::jlimit (-step, step, error * (float) dt));
                }

                updateFollowers (numSamples);

                if (claySupported && pressureScale > 0.0f)
                {
                    const ClayPressureMonitor::PeakBlocks blocks { stats.clayPeakBlocks, stats.clayPeakBlockCount, stats.clayPeakBlocksDropped };
                    const auto musicSeconds = music ? stats.clayPressureSeconds : 0.0;
                    const auto blendSeconds = music ? stats.clayBlendSeconds : 0.0;

                    if (clayPressure.update (blocks, music, musicSeconds, stats.clayBlendIntendedEnergy, stats.clayBlendOutputEnergy, blendSeconds))
                    {
                        pressureScale = ClayPressureMonitor::nextScale (pressureScale);
                        ++probe.clayPressureSteps;
                    }
                }

                clayKeep.update (stats.clayGuardChangeEnergy, stats.clayGuardKeptEnergy, dt, sampleRate);
            }

            rememberHostPosition (numSamples);
            publishStatus();
        }

        void setHostInfo (const HostInfo& info) noexcept
        {
            host = info;
            hostFresh = true;
            offline = info.offline;
            auto analysisHost = info;

            if (offline)
                analysisHost.isPlaying = true;

            if (prepared)
                analyzer.setHostInfo (analysisHost);

            if (! info.hasPosition || ! expectKnown)
                return;

            const auto playingNow = hostPlaying();
            const auto difference = info.positionSamples - expectedPosition;

            if (std::abs (difference) <= positionTolerance || ! playingNow)
                return;

            if (lastPlaying && info.hasLoop && info.looping && difference < 0)
            {
                if (prepared)
                    analyzer.handleDiscontinuity (Discontinuity::loopWrap);

                ++probe.loopWraps;
            }
            else
            {
                if (prepared)
                    analyzer.handleDiscontinuity (Discontinuity::transportJump);

                ++probe.transportJumps;
            }
        }

        void rememberHostPosition (std::int64_t numSamples) noexcept
        {
            if (hostFresh && host.hasPosition)
            {
                const auto playingNow = hostPlaying();
                expectedPosition = host.positionSamples + (playingNow ? numSamples : 0);
                lastPlaying = playingNow;
                expectKnown = true;
            }
            else if (! hostFresh && expectKnown && host.hasPosition)
            {
                if (hostPlaying())
                    expectedPosition += numSamples;
            }
            else
            {
                expectKnown = false;
            }

            hostFresh = false;
        }

        void hostReset() noexcept
        {
            if (prepared)
                analyzer.handleDiscontinuity (Discontinuity::hostReset);

            budget.discardPartialSlot();
            outputLevel.resetFilters();
            clayPressure.reset();
            clayKeep.reset();
            ++probe.hostResets;
        }

        void restoreStateXml (const juce::XmlElement* xml)
        {
            if (xml == nullptr)
            {
                coldStart (false);
                return;
            }

            const auto version = xml->getIntAttribute ("formatVersion", 0);

            if (version > liveContainerFormatVersion)
            {
                coldStart (false);
                return;
            }

            if (version == liveContainerFormatVersion)
            {
                const auto* liveNode = xml->getChildByName (liveTag);
                auto body = liveNode != nullptr ? decompressLiveBody (*liveNode) : std::unique_ptr<juce::XmlElement> {};

                if (body == nullptr || ! restoreBody (*body))
                    coldStart (false);

                return;
            }

            const auto legacy = version > 0 || xml->getChildByName (profileTag) != nullptr || xml->getChildByName ("PLAN") != nullptr
                                || xml->getChildByName ("CALIBRATION") != nullptr || xml->getChildByName (executionTag) != nullptr;
            coldStart (legacy);
        }

        CommittedState committedForTesting() const
        {
            CommittedState state;
            state.constants = activeConstants();
            state.lockedWeights = automaticWeights;
            state.plan = targetPlan;

            if (haveFollowed)
            {
                state.profile = followed;
                state.hasProfile = true;
                state.profileId = computeProfileId (followed);
            }

            return state;
        }

        TestProbe probeForTesting() const noexcept
        {
            auto copy = probe;
            copy.increments = applied;
            copy.targetPlan = targetPlan;
            copy.guardDb = budget.getProtectionDb();
            copy.executedGuardDb = budgetOut.reliefDb;
            copy.appliedBiasDb = gTrim;
            copy.executedBiasDb = gTrim;
            copy.liveClay = liveClay;
            copy.clayPlanned = plannedClay;
            copy.clayGuard = clayGuard.getState();
            copy.clayPressureScale = pressureScale;
            copy.clayAutomaticSupported = claySupported;
            copy.offline = offline;
            copy.liveHeatReduction = liveHeatReduction;
            copy.liveGlueReduction = liveGlueReduction;
            copy.guardLimitsSet = budget.hasLimits();
            copy.guardLimits = budget.getLimits();
            copy.feedbackMaxGainDb = 1.0e9f;
            return copy;
        }
    };

    GlazeController::GlazeController() : impl (std::make_unique<Impl>()) {}
    GlazeController::~GlazeController() = default;

    void GlazeController::prepare (const juce::dsp::ProcessSpec& spec)                         { impl->prepare (spec); }
    void GlazeController::setHostInfo (const HostInfo& info) noexcept                          { impl->setHostInfo (info); }
    void GlazeController::hostReset() noexcept                                                  { impl->hostReset(); }
    int GlazeController::getGuardSamplesUntilBoundary() const noexcept                          { return impl->budget.getSamplesUntilSlotBoundary(); }
    void GlazeController::handleDiscontinuity (Discontinuity kind) noexcept
    {
        if (kind == Discontinuity::hostReset)
            impl->hostReset();
        else if (impl->prepared)
            impl->analyzer.handleDiscontinuity (kind);
    }
    const ChainParams& GlazeController::beginBlock (const ChainParams& manual, bool hostBypassed) noexcept
    {
        return impl->beginBlock (manual, hostBypassed);
    }
    void GlazeController::analyzeInput (const juce::AudioBuffer<float>& input, const LoudnessMeter& inputMeter) noexcept
    {
        if (impl->prepared)
            impl->analyzer.processInput (input, inputMeter);
    }
    void GlazeController::analyzeSpectrum (const BalanceModule& balance) noexcept
    {
        if (impl->prepared)
            impl->analyzer.processSpectrum (balance);
    }
    void GlazeController::endBlock (const BalanceModule&, const LoudnessMeter& inputMeter, const LoudnessMeter& outputMeter,
                                    const ModuleStats& stats, int, bool, const juce::AudioBuffer<float>* output) noexcept
    {
        impl->endBlock (stats, output, inputMeter, outputMeter);
    }
    void GlazeController::requestRelearn() noexcept {}
    std::unique_ptr<juce::XmlElement> GlazeController::createStateXml() const                   { return impl->createStateXml(); }
    void GlazeController::restoreStateXml (const juce::XmlElement* xml)                         { impl->restoreStateXml (xml); }
    void GlazeController::setLiveSegmentSink (SegmentSink* sink, const BalanceModule* pendingFrames)
    {
        impl->sinkProbe = true;
        impl->externalSink = sink;
        impl->pendingFrames = pendingFrames;
        impl->bindLive();
    }
    void GlazeController::attachLiveProfile (LiveProfile* profile) noexcept                     { impl->live = profile; }
    void GlazeController::notePendingModel (Genre genre) noexcept                               { impl->pendingModel = genre; }
    void GlazeController::syncLiveModel() noexcept
    {
        if (impl->live != nullptr)
            impl->live->setConstants (LiveConstants {}, mainSectionModelFor (impl->pendingModel));
    }
    bool GlazeController::hasPrepared() const noexcept                                           { return impl->prepared; }
    Status GlazeController::getStatus() const noexcept
    {
        Status status;
        impl->statusSnapshot.read (status);
        status.legacySessionDiscarded = impl->legacyDiscarded.load (std::memory_order_relaxed);
        return status;
    }
    void GlazeController::setConstantsForTesting (const Constants& constants)
    {
        impl->constantsOverride = constants;
        impl->constantsOverridden = true;
        impl->clayGuard.setConstants (constants);
    }
    CommittedState GlazeController::getCommittedStateForTesting() const                         { return impl->committedForTesting(); }
    Phase GlazeController::getPhaseForTesting() const noexcept                                   { return Phase::unlearned; }
    GlazeController::TestProbe GlazeController::getProbeForTesting() const noexcept             { return impl->probeForTesting(); }

    ClayPlan blendClayPlans (const ClayPlan& from, const ClayPlan& to, float t, float lowCutFloorHz) noexcept
    {
        // 两端逐位原样（过渡开始的那一块与走完之后都不经任何运算）；NaN 当作还没开始。
        if (! (t > 0.0f))
            return from;

        if (! (t < 1.0f))
            return to;

        const auto floorHz = isFinite (lowCutFloorHz) && lowCutFloorHz > 0.0f ? lowCutFloorHz : ClayModule::minLowCutHz;

        const auto linearMix = [t] (float a, float b) noexcept { return a + (b - a) * t; };

        const auto logMix = [t] (float a, float b) noexcept
        {
            if (! (a > 0.0f && b > 0.0f && isFinite (a) && isFinite (b)))
                return a + (b - a) * t;   // 不是正的有限频率（调用方的坏数据）：退回线性，不产生 NaN

            const auto logA = std::log ((double) a);
            const auto logB = std::log ((double) b);
            const auto mixed = (float) std::exp (logA + (logB - logA) * (double) t);
            return juce::jlimit (juce::jmin (a, b), juce::jmax (a, b), mixed);   // 舍入不越过两端
        };

        ClayPlan r;

        // 低切：「关」（0）的一端按 floor 参与对数混合；两端都关就还是关。
        if (from.lowCutHz > 0.0f || to.lowCutHz > 0.0f)
            r.lowCutHz = logMix (from.lowCutHz > 0.0f ? from.lowCutHz : floorHz, to.lowCutHz > 0.0f ? to.lowCutHz : floorHz);
        else
            r.lowCutHz = 0.0f;

        // 收单分频点：收单比例为 0 的一端没有分频点可言，用另一端的（收单从无到有 / 从有到无时分频点不扫动）。
        auto fromMonoHz = from.monoHz;
        auto toMonoHz = to.monoHz;

        if (! (from.monoAmount > 0.0f) && to.monoAmount > 0.0f)
            fromMonoHz = to.monoHz;
        else if (from.monoAmount > 0.0f && ! (to.monoAmount > 0.0f))
            toMonoHz = from.monoHz;

        r.monoHz = logMix (fromMonoHz, toMonoHz);
        r.monoAmount = linearMix (from.monoAmount, to.monoAmount);
        r.widthDb = linearMix (from.widthDb, to.widthDb);

        // 代价 mask（ADR-012，只给界面解释「哪几项因试过会抬高峰值而减量 / 否决」）：过渡中取目标方案的——正在往哪份方案走，
        // 就解释哪份方案的取舍（例如从「低切照做」过渡到「低切否决」时，低切正在收，第 3 行立刻写「已自动减量 / 跳过」）。
        r.costLimitedMask = to.costLimitedMask;
        return r;
    }

    //==============================================================================
    // 校准参考条件的手动参数部分（BUG-004，ADR-018；口径见文件前面 Fnv1a / emitConditions 的注释）。
    void setManualConditions (CalibrationReference& reference, const ChainParams& manual, float feedbackMaxGainDb) noexcept
    {
        HashSink sink;
        emitConditions (sink, manual, feedbackMaxGainDb);

        const auto& clay = manual.claySettings;
        reference.conditionsFormat = continuousConditionsFormat;
        reference.manualParamsHash = sink.legacy.h;
        reference.discreteParamsHash = sink.discrete.h;
        reference.limitReleaseMs = manual.limit.releaseMs;
        reference.clayLowCutHz = clayValueCounted (clay, clay.lowCutMode) ? clay.lowCutHz : 0.0f;
        reference.clayMonoHz = clayValueCounted (clay, clay.monoMode) ? clay.monoHz : 0.0f;
    }

    int upgradeLegacyConditions (std::initializer_list<CalibrationReference*> references, const ChainParams& manual,
                                 float feedbackMaxGainDb) noexcept
    {
        const auto pending = [&references]
        {
            for (auto* r : references)
                if (r != nullptr && r->conditionsFormat != continuousConditionsFormat)
                    return true;

            return false;
        };

        if (! pending())
            return 0;

        // 旧口径的输入串与各槽的候选区间。组合数超上限（越界的坏值）就不升格。
        WordRecorder recorder;
        emitConditions (recorder, manual, feedbackMaxGainDb);

        if (recorder.numSlots <= 0)
            return 0;

        std::array<CodeRange, WordRecorder::maxSlots> ranges {};
        std::int64_t combinations = 1;

        for (int k = 0; k < recorder.numSlots; ++k)
        {
            const auto index = (size_t) k;
            ranges[index] = candidateCodes (recorder.slotValue[index]);
            const auto count = ranges[index].last - ranges[index].first + 1;

            if (count > maxLegacyCandidates)
                return 0;

            combinations *= count;

            if (combinations > maxLegacyCandidates)
                return 0;
        }

        CalibrationReference current;
        setManualConditions (current, manual, feedbackMaxGainDb);

        Fnv1a prefix;

        for (int i = 0; i < recorder.slotAt[0]; ++i)
            prefix.add (recorder.words[(size_t) i]);

        int upgraded = 0;

        auto visit = [&] (std::uint32_t legacy) noexcept
        {
            for (auto* reference : references)
            {
                if (reference == nullptr || reference->conditionsFormat == continuousConditionsFormat
                    || reference->manualParamsHash != legacy)
                    continue;

                // 同一条件（只差往返漂移）：手动参数部分换成此刻的新口径，其余项（画像号、风格、GLAZE、Ceiling……）原样。
                reference->conditionsFormat = current.conditionsFormat;
                reference->manualParamsHash = current.manualParamsHash;
                reference->discreteParamsHash = current.discreteParamsHash;
                reference->limitReleaseMs = current.limitReleaseMs;
                reference->clayLowCutHz = current.clayLowCutHz;
                reference->clayMonoHz = current.clayMonoHz;
                ++upgraded;
            }

            return ! pending();
        };

        searchLegacyCodes<0> (recorder, ranges, prefix, visit);
        return upgraded;
    }
}
