/*
  ==============================================================================

    GlazeController.cpp
    GLAZE 编排器（设计规格-GLAZE-时间线与保存；决策与校准第 2～4、6～8 节；ADR-008）。公开接口与线程约定见头文件。

    本文件里的内部常数与理由：
      - 首次聆听时低置信度方案每 1 s（已处理样本）按候选画像更新一次，每次 1 s 线性过渡：规格「随学习逐步放开，秒级平滑」；
        更新间隔 = 过渡长度，前一次过渡正好走完下一次才来，两端都随 GLAZE 即时重算，旋钮在聆听中不失灵。
      - 校准：参考条件连续 1 s 不变才开新一轮（连续自动化期间永远不满足 → 不重启校准、只用前馈）；轮中条件变了且 3 s 没变回来
        就放弃本轮（= 出厂 settle 长度：再等也采不到这一轮的数据），以新条件为底重来。
      - 校准偏差按 1 dB/s 线性施加（与保护层退让同速；线性斜坡正好落在建议值上，校准器按「实际执行 ≈ 建议」核对执行器是否
        饱和，指数平滑会差一个尾巴）。hostReset / 读取工程 / 采样率重建时直接落到目标（规格「平滑器到目标」）。
      - 方案重算最短间隔 5 ms：GLAZE / 手动值连续自动化时，192 kHz 块 32 也不会每块都调决策器（约 1 µs），「每 hop 的决策计算
        摊到固定位置，不产生新尖峰」；新方案源（过渡、提交、读取）第一次用到时立即算，不受这个间隔限制。48 k 块 ≥ 240 时就是每块。
      - 约束反馈：每次下调 1 dB，两次之间至少隔「一个方案过渡 + 3 s 观察」（PHASE2-NOTES 设计决定 1；按保护层的时间轴即有效音乐
        计，T-032 核验 M2）；上游约束（Glue 均值 / p95、Heat 驱动）按 100 ms 音乐时隙、保护层同样的窗口口径（3 s 窗、累计超出
        ≥ 1 s）判定，容差 Glue 0.1 dB、Heat 0.5；
        p95 近似为「超过 p95 上限的时隙 > 5 %」（与保护层同一近似）。保护层的「方案受限」按进入的那一刻（边沿）计一次，
        它在下限停留期间不重复触发（它要等恢复才清除，否则同一次超限会连降好几档）。
      - 已提交版本与执行快照每块尝试发布：固定大小拷贝，音频线程只 try-lock，拿不到下块再发，不分配或等待。
        执行快照保存刚刚听到的参数依据，未完成的候选与校准不混入正式版本。
      - 状态快照用单写者序列锁（seqlock）：音频线程每块写一次，读者（界面 30 ms 一次）读前后两次序号、不相等就重读，
        数据字按 32 位原子量逐字存取（没有数据竞争）；重试有上限（64 次），用完（实际碰不到）给最后一次读到的内容。
      - 播放位置不连续的判定容差 max(2 样本, 1 ms)：宿主报的位置偶有一两个样本的舍入；真正的跳播远大于此。
        循环打开且位置往回跳 = 循环回绕，其余 = 跳播；停着时挪光标不算（开始播放时位置就是新起点）。

    v0.1.7.0 修坯 Clay 的接线（设计规格-Clay 第 5 ～ 7 节、分析决策与验收第 2 ～ 3 节、ADR-009；A6）：
      - 每块 beginBlock（自动增量算完之后）：自动值 = 方案的 plan.clay，方案过渡中与自动增量同一进度混合（blendClayPlans）→
        ClayGuard.update（view = 分析器短期视图，seconds = 上一块的有效音频秒数，active = GLAZE > 0）→ 实际参数
        effective.clay = resolveClayParams(三态设置, 保护后的自动值, GLAZE)。GLAZE ≤ 0 的快速路径同样算 effective.clay
        （自动 = 中性、手动照样生效、关 = 中性），只动这一项，原有 7 个参数仍逐位等于手动值。
      - 保护的时间轴：endBlock 按切片累计「有效音乐」的样本（分析器 musicActive、不在宿主旁通、宿主在播），下一块 beginBlock
        一次交给 ClayGuard；静音 / 旁通 / 停播块 = 0 秒 → 保护冻结（保持期也不走），与保护层同一口径。
      - 自动 Clay 只在方案源带 Clay 特征时才有：画像格式 ≥ 2 且方案是本版本算法算的。旧画像（格式 1：特征全是「不知道」）、
        旧算法方案（照用保存的增量，那时还没有 Clay）、没有画像：自动值一律中性，ClayGuard 也不工作（回中性）——旧工程重新
        聆听（或拧 GLAZE / 换风格后按新算法重算）之前，Clay 逐位直通、声音不变。过渡的一端不支持时按中性参与混合，
        所以重算 / 重学之后 Clay 随方案过渡平滑启用。
      - 校准参考条件：Clay 三态设置并入 manualParamsHash，但只有影响声音的部分（三项各自的态、「手动」项的旋钮值、Bypass），
        而且三项都「自动」且没旁通（默认，也是旧工程读入后的状态）时一个字节都不加——哈希与 v0.1.6.2 逐位相同，旧存档的
        校准重开后照样适用。「自动 / 关」态下拖旋钮不生效，也就不让校准待验证。
      - 执行快照：plan.clay 存过渡混合后（保护之前）的值，另存保护之后实际执行的 liveClay 与 ClayGuard 状态。恢复（保存重开、
        宿主 reset 沿用执行量、同规格重建）后第一块直接用快照里的 liveClay（与 liveTone / liveTame 同一套首块语义），保护按
        快照状态接着走；不沿用执行量的宿主 reset、GLAZE 回 0、重建时保护回中性。
      - ClayGuard 的常数跟着方案源（过渡终点）的标定快照走：换了方案源才 setConstants（只拷几个数，不分配）。

    ADR-012 修坯根治（设计规格-Clay-根治修订 第 3 节；T-012 阶段 4，P4）：
      - 「先试后用」：决策器把因代价被减量 / 否决的项记在 ClayPlan::costLimitedMask。方案过渡中取目标方案的（blendClayPlans），
        收拾（sanitizeClay）时只保留三个有效位；Status::clayCostLimitedMask = 此刻生效方案（含过渡混合）的 mask，但只报告
        「自动」态的项、且只在 GLAZE > 0 时报告——「关 / 手动」项不经决策器，谈不上被代价减量。它只给界面解释，不影响处理。
      - 反相保护的新触发规则（慢路 / 门内快路、须连续成立 0.15 s）在 ClayGuard 里；编排器照旧每块交视图与有效音乐秒数。
      - 恢复后第一块只在方案源支持自动 Clay 时才用快照值（审核 V2-03）：不支持（画像格式 1、旧算法方案，例如手工改出的存档）
        就走中性分支、作废快照值，与旧工程「Clay 逐位直通」同一口径。

    BUG-004 重开工程后校准丢失（T-026，ADR-018）：
      - 校准参考条件的手动参数分两份：离散哈希（线性旋钮吸附在步进上，重开的归一化往返后逐位不变）+ 三个对数刻度值
        （Limit Release、Clay 低切 / 收单分频点：不吸附，往返后漂几个 ulp），后者按相对 1e-4 比。旧口径哈希（manualParamsHash，
        含三项、逐位同 v0.1.8.0）照算照存：旧版本读新存档、新版本与旧存档比较都靠它。
      - 旧存档的参考（只有旧口径哈希）在读取后第一块按此刻的手动值升格：三项各自 ±16 ulp 内的候选码按前缀逐层代进旧口径哈希，
        对得上就换成新口径（upgradeLegacyConditions）。对不上的照旧只按旧口径哈希比，与修前相同。prepare 沿用已听到的执行快照
        之前，对那份副本同样升格（它可能还是读进来的旧口径）。
      - 稳定计时（条件连续 1 s 不变才开新一轮）照旧逐块精确比（多比一次旧口径哈希），容差只用于「校准适不适用」。

    T-032（2026-09-29 深度研究，实施 A6；docs/validation/2026-09-29-T032-deep-research/IMPL-A6-glaze-controller.md）：
      - 「刚对同一条件试过」只记结束为无效的那一轮（finishRound）；放弃的轮、换方案丢掉的轮不算，条件变回原样照样再开（R5-01）。
      - 方案槽的重算序号取全局单调计数：保护层与上游监视的上限逐次跟上每一份新方案（R5-02；修前两槽各自计数、隔次撞号）。
      - 「g_保护 多久没变过」与约束反馈的观察期只在保护层的时间轴（有效音乐、不旁通）上走：静音 / 停播 / 旁通不把临时退让
        判成「卡住」、不让观察期空转（R5-03、核验 M2）。
      - 解除执行快照的冻结时，快照里并进去的保护退让交给 carry、按方案过渡同长淡出（R5-04，与约束反馈吸收同一套）；carry 淡完
        之前不开新的校准轮。
      - 读档清画像失配的累计与提示、冷却视为已过；提交新画像时按新标定快照重折算「卡住」门槛与上游监视窗（R5-08）。
      - 实时 Tame 许可 = 上限 × GLAZE × κ × 证据，只在已提交画像之后放开（R6-04；κ = 1 时逐位同修前）。
      - 同一次调用里同输入的纯函数只算一次（R5-06：Tame / Tone 的混合上限与 κ、偏差目标的修正范围与「校准适用」、音色 /
        密度跟随的斜率回归），逐位不变；不做跨块缓存。
      - 保护层与上游监视只核对自动部分（R6-02 收尾，实施 A11，IMPL-A11-r602-guard-auto-share.md）：每项有效上限 = max(风格上限,
        手动单独用量)。手动链单独已超出风格预算的项（Plan::manualBudgetMask，决策器在方案重算时算好）自动部分恒为 0，按「不设约束」
        核对，其余照风格上限；这组项变了就清空两个窗口（g_保护 不动）。之后画像失配、保护层、上游监视、校准与约束反馈只在电平类自动
        全部撤回（Plan::automaticLevelWithdrawn）时停，limitedByManual 只给界面。手动没超预算时掩码为 0，与修前逐位相同。

    T-038 House 档（设计规格-曲风-House学习方式 第 3、4、7、9 节；阶段 2 B）。一切都在 isHouse(currentGenre) 之后，通用档逐位同改前：
      - 分析器：每块 beginBlock 交此刻的曲风（setGenre，不变时什么都不做）；切片的宿主 PPQ 按片的偏移推过再交（与位置同一处理），
        段落账的锚点检查才对得上；House 常数在 prepare / 换常数时交给分析器。
      - 锁定条件（houseLockSatisfied）：画像的门控结果按分段方式取一组——小节：已听 ≥ lockSegments、满编 ≥ requiredFullSegments；
        2 秒段：已听有效音频 ≥ fallbackLockSeconds、满编 ≥ fallbackFullSeconds——再加现有的 listeningComplete。常数取本版本的标定快照
        （新学的画像用它，同 listeningComplete）。
      - 暂定方案：首次聆听中分析器的 House 画像每重算一次（每定稿一段，约每小节一次）就按它重算方案。一般 3 s 过渡（同 planTransitionSeconds）；
        这次重算前有新响段定稿就用 fastTransitionSeconds：正在走的过渡先按进度不变地压缩到同样的时长内走完（hurryTransition：
        length' = 剩余 ÷ (1 − t)，进度 t 不跳），新方案排在它后面、也是快过渡——否则排队要等最多 3 s，drop 被暂定方案压着推。
        通用档首次聆听的「每 1 s 更新一次」在 House 档不用（段落账一小节才变一次）。暂定期没有已提交画像，校准本来就不开。
      - 锁定后防漏网（updateHouseLocked）：按段落账的定稿事件数「连续更响更满」的段（同一连续时间段里；跨不连续点、静音段都断开），
        够 reopenSegments 就进入「待重开」：段落账也够锁定条件时按它门控重算画像（O(段数)，只在段落账有新定稿时试一次），
        比已提交画像更响才提交——重开次数 +1 记进画像（houseGate.reopenCount，画像号随之变），fastTransitionSeconds 过渡、
        校准作废重来两轮；够不上锁定条件（刚读档、段落账从空开始）就旧画像照常、接着等。每份画像最多 maxReopens 次。新段里
        先试后用的代价（锁定后候选不跑）不够 10 s 时分析器改用全部段的块；再不够就沿用已提交画像的代价（同一首歌试过的结论）。
      - 换曲风（updateHouseGenre，进 / 出 House 档那一块做一次）：通用 → House 正在聆听就同一种聆听重新开始（段落账从空开始，
        与通用累计对齐）；House → 通用时为 House 开的那一轮取消、用户点的与首次聆听接着按通用规则走完。没在聆听、已提交画像不是
        House 锁定过的（houseProfileLocked：格式 3 且门控结果有效）就开一轮 relearn 式聆听，旧画像照常生效（maybeStartHouseListen）；
        用户取消这一轮之后不再自动开，换曲风 / 提交 / 读档时才重新允许。读取工程：段落账清空（旧画像在新画像就绪前照常）。
      - 反相提示：updateClay 里在 ClayGuard 之后调 updateHouseHint（资格 = 段落账此刻这一段暂判满编，时长 = phaseHintSegments 小节，
        拿不到拍点用 phaseHintFallbackSeconds），结果拷进状态。GLAZE ≤ 0、方案源不支持自动 Clay 时提示熄灭（同反相保护的指示灯）。

  ==============================================================================
*/

#include "GlazeController.h"

#include "ClayGuard.h"
#include "GlazeCalibrator.h"
#include "GlazeGuard.h"
#include "GlazePlanner.h"
#include "MusicAnalyzer.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <type_traits>

namespace celadon::glaze
{
    namespace
    {
        constexpr double learningUpdateSeconds     = 1.0;
        constexpr double calibrationStableSeconds  = 1.0;
        constexpr double calibrationAbandonSeconds = 3.0;
        constexpr float  biasSlewDbPerSecond       = 1.0f;
        constexpr double planIntervalSeconds       = 0.005;
        constexpr float  feedbackStepDb            = 1.0f;
        constexpr double feedbackObserveSeconds    = 3.0;
        constexpr float  feedbackMinimumChangeDb   = 0.01f;

        constexpr double monitorSlotSeconds        = 0.1;
        constexpr int    monitorCapacity           = 64;     // ≥ 窗口时隙数（出厂 3 s = 30 格；可容纳 6.4 s 的窗）
        constexpr float  glueToleranceDb           = 0.1f;
        constexpr float  heatTolerance             = 0.5f;
        constexpr double monitorP95Fraction        = 0.05;
        constexpr double statePublishSeconds       = 0.1;
        // 宿主报的位置与「上一块位置 + 块长」相差多少以内不算跳播。宿主的播放头常有小偏差：把自己的缓冲拆成几块送时
        // 重复报同一个位置、按拍子换算的位置有舍入、有的每几块报一次旧值（v0.6 验收后诊断：按 1 ms 判时，这类宿主上每块都算
        // 跳播，聆听永远凑不满）。用户真的挪播放位置、pattern 循环回到开头，差距都远大于 50 ms。
        constexpr double positionToleranceSeconds  = 0.05;
        constexpr float  unlimitedGainDb           = 1.0e9f;
        constexpr int    subBlockCapacity          = LoudnessMeter::subBlockRingCapacity;
        constexpr int    statusReadAttempts        = 64;

        constexpr const char* controllerTag = "GLAZE_CONTROLLER";
        constexpr int controllerFormatVersion = 1;
        constexpr int heatCurveMigrationAlgorithmVersion = 6;  // v6 改变 Heat 高频残差保护，旧补偿不能配新 DSP 沿用。

        bool isFinite (float x) noexcept   { return std::isfinite (x); }

        bool sameBits (float a, float b) noexcept
        {
            return std::memcmp (&a, &b, sizeof (float)) == 0;
        }

        std::int64_t toSamples (double seconds, double sampleRate) noexcept
        {
            const auto s = std::isfinite (seconds) ? juce::jlimit (0.0, 1.0e6, seconds) : 0.0;
            return (std::int64_t) std::llround (s * sampleRate);
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

        AutoIncrements blendIncrements (const AutoIncrements& a, const AutoIncrements& b, float t) noexcept
        {
            auto mix = [t] (float x, float y) noexcept { return x + (y - x) * t; };

            AutoIncrements r;
            r.balanceRecover = mix (a.balanceRecover, b.balanceRecover);
            r.balanceTame    = mix (a.balanceTame,    b.balanceTame);
            r.colorToneDb    = mix (a.colorToneDb,    b.colorToneDb);
            r.colorGlue      = mix (a.colorGlue,      b.colorGlue);
            r.colorHeat      = mix (a.colorHeat,      b.colorHeat);
            r.limitGainDb    = mix (a.limitGainDb,    b.limitGainDb);
            r.limitClip      = mix (a.limitClip,      b.limitClip);
            return r;
        }

        bool sameCalibrationPlan (const Plan& a, const Plan& b) noexcept
        {
            const auto& x = a.increments;
            const auto& y = b.increments;
            return a.algorithmVersion == b.algorithmVersion && a.constantsVersion == b.constantsVersion
                && sameBits (x.balanceRecover, y.balanceRecover) && sameBits (x.balanceTame, y.balanceTame)
                && sameBits (x.colorToneDb, y.colorToneDb) && sameBits (x.colorGlue, y.colorGlue)
                && sameBits (x.colorHeat, y.colorHeat) && sameBits (x.limitGainDb, y.limitGainDb)
                && sameBits (x.limitClip, y.limitClip) && sameBits (a.effectiveGainDb, b.effectiveGainDb)
                && sameBits (a.effectiveTargetLufs, b.effectiveTargetLufs)
                && sameBits (a.weights.warm, b.weights.warm) && sameBits (a.weights.punchy, b.weights.punchy)
                && sameBits (a.weights.clean, b.weights.clean);
        }

        // v0.1.7.0 修坯 Clay 的三态设置是否逐位相同（浮点按位、枚举与布尔按值）。
        bool sameClaySettings (const ClaySettings& a, const ClaySettings& b) noexcept
        {
            return a.lowCutMode == b.lowCutMode && a.monoMode == b.monoMode && a.widthMode == b.widthMode
                && sameBits (a.lowCutHz, b.lowCutHz) && sameBits (a.monoHz, b.monoHz) && sameBits (a.widthDb, b.widthDb)
                && a.bypass == b.bypass;
        }

        bool anyAutomaticClay (const ClaySettings& s) noexcept
        {
            return s.lowCutMode == ClayMode::automatic || s.monoMode == ClayMode::automatic || s.widthMode == ClayMode::automatic;
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

        // 存进执行快照的 ClayGuard 状态按快照读取时核对的范围收拾（合法状态原样通过）：比例 0 ～ 1、秒数与上限不为负。
        ClayGuardState sanitizeClayGuard (const ClayGuardState& s) noexcept
        {
            auto r = s;
            r.phaseFloor = isFinite (s.phaseFloor) ? juce::jlimit (0.0f, 1.0f, s.phaseFloor) : 0.0f;
            r.phaseHoldSeconds = isFinite (s.phaseHoldSeconds) ? juce::jmax (0.0f, s.phaseHoldSeconds) : 0.0f;
            r.widthCapDb = isFinite (s.widthCapDb) ? juce::jmax (0.0f, s.widthCapDb) : clayWidthUncappedDb;
            return r;
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

        // 状态快照的内容：对外的 Status + getStatus 叠加「已点、音频线程还没接到的重新聆听」要用的几项。
        struct StatusPayload
        {
            Status status;
            Phase committedPhase = Phase::unlearned;   // 不算聆听时的阶段（重新聆听被取消后回到它）
            float minListenSeconds = 15.0f;
            bool offline = false;                      // 宿主离线标记；音频状态机仍按同一规则运行
        };

        //==============================================================================
        // 上游约束（Glue、Heat）的超限监视：Limit Gain 在它们下游，保护层退让管不到它们（设计决定 1），只能降方案。
        class UpstreamMonitor
        {
        public:
            void configure (double sampleRate, const Constants& c) noexcept
            {
                slotLength = juce::jmax (1, (int) std::lround (sampleRate * monitorSlotSeconds));

                auto toSlots = [] (double seconds)
                {
                    const auto s = std::isfinite (seconds) ? juce::jlimit (0.0, 1.0e3, seconds) : 0.0;
                    return (int) std::lround (s / monitorSlotSeconds);
                };

                windowSlots = juce::jlimit (1, monitorCapacity, toSlots (c.guardWindowSeconds));
                triggerSlots = juce::jlimit (1, windowSlots, toSlots (c.guardTriggerSeconds));
                clear();
            }

            void setCaps (const StyleConstants& caps) noexcept
            {
                glueMeanCap = caps.glueGrMeanCapDb;
                glueP95Cap = caps.glueGrP95CapDb;
                // Color 的 v3 heatDrive 统计是参数 h 映射后的驱动量 h² / 400（满档 25），不是 0～100 的旋钮值。
                // 预算仍以旋钮单位保存；先映射到同一统计域再比较。非正/非有限沿用「不设约束」。
                const auto heatAmount = isFinite (caps.heatDriveCap) && caps.heatDriveCap > 0.0f
                                            ? juce::jmin (caps.heatDriveCap, 100.0f) : 0.0f;
                heatCap = heatAmount * heatAmount / 400.0f;
                heatCapTolerance = ((heatAmount + heatTolerance) * (heatAmount + heatTolerance)
                                      - heatAmount * heatAmount) / 400.0f;
            }

            void clear() noexcept
            {
                partGlue = partHeat = 0.0;
                partSamples = 0;
                count = 0;
                writeIndex = 0;
                overCount = 0;
            }

            // 只计有效音乐（静音、停播、旁通冻结时不计，与保护层同一时间轴）。
            void add (float glueMeanDb, float heatDrive, int numSamples, bool musicActive) noexcept
            {
                if (! musicActive || numSamples <= 0)
                    return;

                const auto glue = (double) (glueMeanDb > 0.0f && isFinite (glueMeanDb) ? juce::jmin (glueMeanDb, 1000.0f) : 0.0f);
                const auto heat = (double) (heatDrive > 0.0f && isFinite (heatDrive) ? juce::jmin (heatDrive, 1000.0f) : 0.0f);

                for (auto remaining = numSamples; remaining > 0;)
                {
                    const auto take = juce::jmin (remaining, slotLength - partSamples);
                    partGlue += glue * take;
                    partHeat += heat * take;
                    partSamples += take;
                    remaining -= take;

                    if (partSamples >= slotLength)
                        completeSlot();
                }
            }

            // 窗内被判为超出的时隙累计 ≥ 触发时长（出厂 3 s 窗内 ≥ 1 s）。
            bool isOver() const noexcept   { return overCount >= triggerSlots; }

            // 折算后的窗与触发时隙数（只给测试探针读，T-032 R5-08）。
            int getWindowSlots() const noexcept    { return windowSlots; }
            int getTriggerSlots() const noexcept   { return triggerSlots; }

        private:
            static bool enforced (float cap) noexcept   { return isFinite (cap) && cap > 0.0f; }

            void completeSlot() noexcept
            {
                const auto length = (double) slotLength;
                glueSlots[(size_t) writeIndex] = (float) (partGlue / length);
                heatSlots[(size_t) writeIndex] = (float) (partHeat / length);
                partGlue = partHeat = 0.0;
                partSamples = 0;

                const auto newest = writeIndex;
                writeIndex = (writeIndex + 1) % monitorCapacity;
                count = juce::jmin (count + 1, windowSlots);

                double glueSum = 0.0, heatSum = 0.0;
                int aboveP95 = 0;

                for (int k = 0; k < count; ++k)
                {
                    const auto i = (size_t) ((newest - k + monitorCapacity) % monitorCapacity);
                    glueSum += glueSlots[i];
                    heatSum += heatSlots[i];
                    aboveP95 += enforced (glueP95Cap) && glueSlots[i] > glueP95Cap + glueToleranceDb ? 1 : 0;
                }

                const auto meanGlue = glueSum / (double) count;
                const auto meanHeat = heatSum / (double) count;

                overSlots[(size_t) newest] = (enforced (glueMeanCap) && meanGlue > (double) (glueMeanCap + glueToleranceDb))
                                          || (enforced (glueP95Cap) && (double) aboveP95 > monitorP95Fraction * (double) count)
                                          || (enforced (heatCap) && meanHeat > (double) (heatCap + heatCapTolerance));

                overCount = 0;

                for (int k = 0; k < count; ++k)
                    overCount += overSlots[(size_t) ((newest - k + monitorCapacity) % monitorCapacity)] ? 1 : 0;
            }

            int slotLength = 4800, windowSlots = 30, triggerSlots = 10;
            float glueMeanCap = 0.0f, glueP95Cap = 0.0f, heatCap = 0.0f, heatCapTolerance = 0.0f;
            double partGlue = 0.0, partHeat = 0.0;
            int partSamples = 0;
            std::array<float, (size_t) monitorCapacity> glueSlots {}, heatSlots {};
            std::array<bool, (size_t) monitorCapacity> overSlots {};
            int count = 0, writeIndex = 0, overCount = 0;
        };

        //==============================================================================
        // 保护层与上游监视生效的上限（T-032 R6-02 收尾，A11）：每项 = max(风格上限, 手动单独用量)。风格上限按方案权重混合；手动链单独已
        // 超预算的项（Plan::manualBudgetMask，只认三个已定义的位）置 0 = 不设约束（GlazeGuard 与 UpstreamMonitor 同一约定），其余项手动
        // 单独用量不超风格上限，max 就是风格上限。released = 0 时逐位原样返回。置位的项为什么是「不设约束」而不是拿决策器估的手动单独
        // 用量当上限：
        //   - Heat（只有上游监视看）：自动 Heat 此时恒为 0，驱动量全是手动的——实测的就是手动单独用量，max 永不越限；
        //   - Glue 均值 / p95：自动 Glue 此时恒为 0，GR 全是手动的，保护层退让 Limit Gain、约束反馈降方案都减不了它。决策器估的手动单独
        //     GR 是前向模型的估计，实测比它大一点就被当成自动的超限、白白磨光自动 Gain（IMPL-A11 第 4 节实测）；实测的就是手动单独用量，
        //     max 永不越限；
        //   - Clip 深度 / 活动：决策器撤回了自动 Clip、且不再按 Clip 预算限制自动 Gain（feasible，F-02 逐模块钳住）。保护层若按
        //     「max(风格上限, 手动单独深度)」核对，会与决策器顶牛——自动 Gain 一推，削波就比手动单独深，保护层退到底、约束反馈把上限
        //     磨到手动链，与修前同一种磨光。所以与决策器一致：不核对这两项，限幅器三项照常核对自动 Gain。
        StyleConstants releaseManualBudget (StyleConstants caps, std::uint8_t released) noexcept
        {
            if ((released & manualBudgetHeat) != 0)
                caps.heatDriveCap = 0.0f;

            if ((released & manualBudgetGlue) != 0)
                caps.glueGrMeanCapDb = caps.glueGrP95CapDb = 0.0f;

            if ((released & manualBudgetClip) != 0)
                caps.clipDepthCapDb = caps.clipActivityCap = 0.0f;

            return caps;
        }

        //==============================================================================
        // 方案源：决策器除 GLAZE 与手动值以外的全部输入。方案切换 = 换方案源（过渡）；GLAZE / 手动值变 = 同一方案源重算。
        struct PlanSource
        {
            bool hasProfile = false;
            MusicProfile profile;
            bool hasLockedWeights = false;
            StyleWeights lockedWeights;
            Style style = Style::automatic;
            Genre genre = Genre::general;              // T-038：方案源自己的曲风与交付（过渡时新旧方案各用各的）
            Delivery delivery = Delivery::standard;
            Constants constants;
            float maxGainDb = unlimitedGainDb;
            bool useSavedPlan = false;       // 旧算法版本的已提交方案：照用保存的自动增量（时间线与保存第 5 节）
            Plan savedPlan;
            std::uint32_t generation = 0;
        };

        struct PlanSlot
        {
            PlanSource source;
            Plan plan;
            bool valid = false;
            std::uint32_t evaluatedGeneration = 0;
            float evaluatedGlaze = 0.0f;
            ChainParams evaluatedManual;
            // 最近一次重算的全局序号（Impl::evaluationCounter，每次 evaluate 递增；保护层上限等据此知道方案变了）。
            // 必须是全局单调而不是每槽各自计数（T-032 R5-02）：两槽轮流当过渡终点，各自计数每隔一次方案切换就撞上一次
            //（槽 0 = 1 记 1 → 槽 1 = 1 跳过 → 槽 0 = 2 更新 → 槽 1 = 2 跳过……），首次聆听 / 提交 / 换风格后保护层与上游监视
            // 有一半几率沿用上一份方案的上限，直到用户碰任一进方案的旋钮。
            std::uint32_t evaluations = 0;
        };

        // 随工程保存的内容：已提交版本 + 约束反馈上限。
        struct SaveState
        {
            CommittedState committed;
            float feedbackMaxGainDb = unlimitedGainDb;
            float clayPressureScale = 1.0f;   // ADR-014；旧工程没有这一项 = 1（不限）
            ExecutionState execution;
        };

        juce::String floatToHex (float value)
        {
            std::uint32_t bits = 0;
            std::memcpy (&bits, &value, sizeof (bits));
            return juce::String::toHexString ((juce::int64) bits).paddedLeft ('0', 8);
        }

        bool hexToFloat (const juce::String& text, float& out)
        {
            if (text.length() != 8 || ! text.containsOnly ("0123456789abcdefABCDEF"))
                return false;

            const auto bits = (std::uint32_t) text.getHexValue64();
            std::memcpy (&out, &bits, sizeof (out));
            return true;
        }
    }

    //==============================================================================
    struct GlazeController::Impl
    {
        enum class Learning { none, firstListen, relearn };

        Impl()
        {
            committed.constants = constants;
            published.committed = committed;
            pendingRestore = published;

            for (auto& slot : slots)
                slot.source.constants = constants;

            updateTiming();
            publishStatus();
        }

        //==============================================================================
        // ---- 子单元 ----
        MusicAnalyzer analyzer;
        GlazeCalibrator calibrator;
        GlazeGuard guard;
        UpstreamMonitor monitor;

        Constants constants = factoryConstants();   // 本版本的标定快照（新学的画像用它；测试可替换）

        // ---- 规格与按样本折算的时长 ----
        double sampleRate = 48000.0;
        double analyzedSampleRate = 0.0;     // 分析器上次按什么规格建的（它只依赖采样率与声道数，与块长无关）
        int analyzedChannels = 0;
        bool prepared = false;          // prepare 之前（处理器先交参数再 prepare）不启动聆听：分析器还没按规格建好
        std::int64_t learningUpdateSamples = 48000, planTransitionSamples = 144000;
        std::int64_t stableSamples = 48000, abandonSamples = 144000, feedbackCooldownSamples = 288000;
        std::int64_t planIntervalSamples = 240, statePublishSamples = 4800, positionTolerance = 48;

        // ---- 已提交版本 ----
        CommittedState committed;
        float feedbackMaxGainDb = unlimitedGainDb;
        float clayPressureScale = 1.0f;        // 修坯压力反馈（ADR-014）：自动 Clay 三项缩放的目标，随工程保存；新画像（提交）时回到 1
        ClayPressureMonitor clayPressure;      // 代价格的监测（运行状态，不存档）
        float clayScaleRampStart = 1.0f;       // 降档时从此刻的实际缩放线性走到目标（方案过渡同长），不开方案过渡
        std::int64_t clayScaleRampElapsed = 0, clayScaleRampLength = 0;
        ClayPlan clayPlannedUnscaled;          // 上一块方案（含过渡混合）给的自动值，缩放之前（监测只在它有东西可缩时工作）
        Phase committedPhase = Phase::unlearned;
        bool savedPlanMode = false;
        bool savedPlanGlazeKnown = false;
        float savedPlanGlaze = 0.0f;
        ExecutionState execution, lastExecution;
        bool executionHeld = false;
        bool restoredTamePending = false;
        bool heldBiasKnown = false;
        bool executionRequiresCalibration = false;
        float heldBiasDb = 0.0f, calibrationBaseBiasDb = 0.0f;
        CalibrationReference heldBiasReference;

        // ---- 聆听 ----
        Learning learning = Learning::none;
        std::int64_t samplesSinceLearningUpdate = 0;

        // ---- 方案源与过渡 ----
        std::array<PlanSlot, 2> slots;
        int toIndex = 0;
        bool transitionActive = false;
        std::int64_t transitionElapsed = 0, transitionLength = 0;
        bool hasPending = false;
        PlanSource pendingSource;
        std::int64_t pendingLength = 0;
        std::uint32_t generationCounter = 0;
        std::uint32_t evaluationCounter = 0;   // 方案重算的全局序号（PlanSlot::evaluations 取它；R5-02）
        std::int64_t samplesSincePlan = 0;

        // ---- 本块 ----
        ChainParams manual;
        ChainParams effective;
        AutoIncrements increments;
        bool active = false;             // 本块 GLAZE > 0
        bool paused = false;             // 本块宿主旁通：暂停校准与 Tone 跟随；Listen 只影响最终监听增益。
        Style currentStyle = Style::automatic;
        Genre currentGenre = Genre::general;           // T-038：与 currentStyle 同一套规则（换了就换方案源、3 s 过渡、重新校准）
        Delivery currentDelivery = Delivery::standard;
        bool styleKnown = false;
        float executedBiasDb = 0.0f, executedGuardDb = 0.0f;
        int lastLatency = 0;

        // ---- 校准 ----
        bool roundActive = false;
        CalibrationReference roundReference, currentReference, lastAttemptReference;
        CalibrationReference previousUserConditions;   // 用户能动的条件（不含画像号与约束反馈）：稳定计时只看它

        // 参考条件的输入与上一块逐位相同时不重算哈希（稳态下每块省掉约 20 次取整与哈希）。
        ChainParams referenceManual;
        std::uint32_t referenceProfileId = 0;
        Style referenceStyle = Style::automatic;
        Genre referenceGenre = Genre::general;
        Delivery referenceDelivery = Delivery::standard;
        float referenceFeedback = 0.0f;
        bool referenceSavedPlan = false;
        double referenceRate = 0.0;
        bool haveLastAttempt = false, referenceKnown = false;
        std::int64_t referenceStableSamples = 0, roundMismatchSamples = 0;

        // 参考条件的手动参数部分（setManualConditions：两份哈希与三个对数刻度值）按上一次的输入缓存（T-027）。makeReference 每块
        // 被调 2 ～ 3 次（beginBlock 比执行快照、captureExecution、publishState），手动参数不变时结果逐位相同，不必每次重算两份哈希
        // （约 110 ～ 170 ns / 次）。键 = samePlanningManual（setManualConditions 读的每一项它都比到：浮点按位，「Mast」段的参考频谱
        // 按值——只有 ±0 按值相等而位不同，两者量化码相同；是超集）+ 约束反馈上限逐位相同：命中时拷贝的与重算的逐位相同。
        // 改 setManualConditions / emitConditions 读哪些项时，samePlanningManual 必须仍覆盖它们。
        // makeReference 是 const，缓存因此 mutable；与 manual 一样只由音频线程读写（prepare 与测试探针调 makeReference 时音频线程
        // 不在跑），固定大小拷贝，不分配。
        mutable CalibrationReference manualConditions;   // 只有手动参数那几项有值，其余是默认值
        mutable ChainParams manualConditionsInput;
        mutable float manualConditionsFeedback = 0.0f;
        mutable bool manualConditionsKnown = false;

        float appliedBiasDb = 0.0f;
        bool snapBias = true;
        // 短期明暗只叠在固定画像的 Tone 上，不改变风格、响度目标或保存的计划。
        float liveToneOffsetDb = 0.0f; // 满强度单位；GLAZE / 置信度在应用时缩放。
        std::uint32_t liveToneProfileId = 0;
        float liveHeatReduction = 0.0f, liveGlueReduction = 0.0f;
        std::uint32_t liveColorProfileId = 0;

        // ---- 保护层与约束反馈 ----
        bool guardClean = true;
        std::uint32_t guardLimitsEvaluation = 0;
        bool guardLimitsKnown = false;
        // 保护层与上游监视此刻按哪些项放开了约束（上一次 updateGuardLimits 交出去的 manualBudgetMask，T-032 A11）；变了就清两个窗口。
        // 保护层 reset / prepare 不动上限，所以这里也不跟着清。
        std::uint8_t guardReleasedMask = 0;
        bool guardWasPlanLimited = false;
        bool planLimitedLatched = false;
        float lastGuardDb = 0.0f;
        // g_保护 多久没变过（只数保护层自己的时间轴：有效音乐的块；静音 / 停播 / 旁通冻结时不推进也不清，R5-03）；「卡住」的门槛
        //（activeConstants().guardRecoverAfterSeconds + 1 s：超过它自己的恢复等待仍没动 = 停在回差区、不会自己回来）。
        std::int64_t guardSteadySamples = 0, guardStuckSamples = 288000;

        // 沿用执行快照（保存重开、宿主 reset、同规格重建）时 adoptExecution 并进冻结方案的保护退让（≤ 0）。解除冻结时新方案按已提交
        // 画像重算、不含这一份，改由下面的 carry 接着施加、随方案过渡同长淡出（releaseAbsorbedGuard，R5-04）：修前直接丢掉，
        // 重开后第一次拧旋钮 / 换风格净增益阶跃最多 3 dB。
        float absorbedGuardDb = 0.0f;

        // 约束反馈把保护层此刻的退让吸收进方案时：保护层清零，它原来的退让量改由这里接着施加、随方案过渡线性淡出，
        // 两者相加净增益连续（不会先双算一次退让、再慢慢恢复）。
        float guardCarryStartDb = 0.0f;
        std::int64_t guardCarryElapsed = 0, guardCarryLength = 0;
        std::int64_t samplesSinceFeedback = std::numeric_limits<std::int64_t>::max() / 4;

        // ---- 画像失配（v0.6 验收 F-01；门槛与时长在标定快照 Constants::mismatch*）----
        // 有效音乐的 M（400 ms）比方案所用画像的 I 高出门槛、累计满持续时长 → 这一段与画像严重不符（典型：首次聆听只听到
        // 安静前奏）。把这一段的响度与峰值当画像再求一次方案，它的净增益直接作为上限（与约束反馈同一个上限：只降不升、
        // 随工程保存），短过渡换过去。只降增益，不重学（规格「不自动重学」）；状态行提示「建议重新聆听」。
        std::int64_t mismatchSustainSamples = 48000, mismatchTransitionSamples = 24000, mismatchCooldownSamples = 96000;
        std::int64_t mismatchSamples = 0;                 // 累计的有效音乐样本（M 在回差线以上）
        double mismatchEnergy = 0.0;                      // 这段时间 M 的能量和（按样本加权），求这一段的平均响度
        float mismatchPeakDb = -100.0f;                   // 这段时间入口真峰值的最大值
        std::int64_t samplesSinceMismatch = std::numeric_limits<std::int64_t>::max() / 4;
        bool profileMismatch = false;                     // 状态行提示；提交新画像、读取工程时清

        // ---- 修坯 Clay（v0.1.7.0；口径见文件开头）----
        ClayGuard clayGuard;
        ClayPlan clayPlanned;                  // 上一块方案（含过渡混合）给的自动值（保护之前；不支持 Clay 时中性）
        ClayPlan liveClay;                     // 上一块保护之后、三态合成之前的自动值（执行快照存它）
        ClayPlan restoredClay;                 // 执行快照里实际执行过的自动值：恢复后第一块直接用
        bool clayRestorePending = false;       // 下一块用 restoredClay（保存重开 / 沿用执行量的宿主 reset / 同规格重建之后）
        bool claySupported = false;            // 上一块的方案源带 Clay 特征（画像格式 ≥ 2、当前算法方案）
        double claySeconds = 0.0;              // 自上一次 beginBlock 以来的有效音乐秒数（endBlock 按切片累计）
        float lastClaySeconds = 0.0f;          // 上一块交给 ClayGuard 的秒数（只给测试探针）
        bool clayRestoredLastBlock = false;    // 上一块用的是快照值（只给测试探针）
        std::uint32_t clayConstantsGeneration = 0;
        bool clayConstantsKnown = false;       // ClayGuard 已按 clayConstantsGeneration 那个方案源的标定快照设过常数

        // ---- T-038 House 档（口径见文件开头；通用档不读写下面任何一项）----
        bool houseWasActive = false;           // 上一块是 House 档（进 / 出 House 档那一块做一次性处理）
        bool houseListen = false;              // 这一轮聆听是为 House 锁定条件自动开的（回通用档时取消）
        bool houseListenDeclined = false;      // 用户取消了自动开的那一轮：不再自动开（换曲风 / 提交 / 读档时清）
        std::uint32_t houseProfileSeen = 0;    // 暂定方案上次用到的分析器 House 画像版本
        std::uint32_t houseSurgeSeen = 0;      // 上次看到的新响段计数
        std::uint64_t houseEventCursor = 0;    // 段落账定稿事件读到的序号
        int houseReopenRun = 0;                // 锁定后连续「更响更满」的段数
        std::uint32_t houseReopenEpoch = 0;    // 这一串段所在的连续时间段
        bool houseReopenArmed = false;         // 已满足重开条件，等段落账够锁定条件
        std::uint32_t houseReopenTriedAt = 0;  // 上次试重开时段落账的定稿计数（没有新定稿不再重算）
        std::int64_t houseFastSamples = 24000; // fastTransitionSeconds 折算的样本数
        ClayGuard::HouseHint houseHint;        // 上一块的 House 反相提示（拷进状态）

        // ---- 宿主 ----
        HostInfo host;
        std::int64_t hostInputOffset = 0; // 当前宿主块已交给分析器的样本；内部统计切片不重复用原位置。
        bool hostFresh = false;
        bool offline = false;
        bool expectKnown = false, lastPlaying = false;
        std::int64_t expectedPosition = 0;
        bool unlearnedOfflineWarning = false;

        // ---- 两块表的子块 ----
        std::int64_t inputCursor = 0, outputCursor = 0;
        bool spectrumTakenAtBlockStart = false;   // 本块开头已由 analyzeSpectrum 交过 Balance 的帧
        std::array<LoudnessMeter::SubBlock, (size_t) subBlockCapacity> inputBlocks {}, outputBlocks {};

        // ---- 跨线程 ----
        SeqLockSnapshot<StatusPayload> statusSnapshot;      // 写者：音频线程
        SeqLockSnapshot<StatusPayload> restoredSnapshot;    // 写者：消息线程（restoreStateXml）
        std::atomic<int> pendingRelearn { 0 };
        std::atomic<bool> restorePending { false };
        mutable juce::SpinLock stateLock;                   // 保护 published / pendingRestore
        SaveState published, pendingRestore;
        bool stateDirtyUrgent = false, stateDirtyLazy = false;
        std::int64_t samplesSincePublish = 0;

        TestProbe probe;

        //==============================================================================
        void updateTiming() noexcept
        {
            const auto sr = sampleRate;
            learningUpdateSamples  = juce::jmax ((std::int64_t) 1, toSamples (learningUpdateSeconds, sr));
            planTransitionSamples  = toSamples (constants.planTransitionSeconds, sr);
            stableSamples          = toSamples (calibrationStableSeconds, sr);
            abandonSamples         = juce::jmax ((std::int64_t) 1, toSamples (calibrationAbandonSeconds, sr));
            feedbackCooldownSamples = planTransitionSamples + toSamples (feedbackObserveSeconds, sr);
            mismatchSustainSamples = juce::jmax ((std::int64_t) 1, toSamples (constants.mismatchSustainSeconds, sr));
            mismatchTransitionSamples = toSamples (constants.mismatchTransitionSeconds, sr);
            mismatchCooldownSamples = toSamples (constants.mismatchCooldownSeconds, sr);
            planIntervalSamples    = toSamples (planIntervalSeconds, sr);
            guardStuckSamples      = toSamples (activeConstants().guardRecoverAfterSeconds + 1.0, sr);
            statePublishSamples    = toSamples (statePublishSeconds, sr);
            positionTolerance      = juce::jmax ((std::int64_t) 2, toSamples (positionToleranceSeconds, sr));
            houseFastSamples       = juce::jmax ((std::int64_t) 1, toSamples (constants.house.fastTransitionSeconds, sr));
        }

        const Constants& activeConstants() const noexcept
        {
            return committed.hasProfile ? committed.constants : constants;
        }

        PlanSlot& toSlot() noexcept               { return slots[(size_t) toIndex]; }
        const PlanSlot& toSlot() const noexcept   { return slots[(size_t) toIndex]; }
        PlanSlot& fromSlot() noexcept             { return slots[(size_t) (1 - toIndex)]; }
        const PlanSlot& fromSlot() const noexcept { return slots[(size_t) (1 - toIndex)]; }

        bool hostPlaying() const noexcept   { return offline || ! host.hasPlayState || host.isPlaying; }

        //==============================================================================
        // ---- 方案源 ----

        // 此刻方案源：适用的执行快照、已提交版本，或首次聆听的候选；实时与离线遵循相同规则。
        PlanSource desiredSource() noexcept
        {
            PlanSource s;
            s.style = currentStyle;
            s.genre = currentGenre;
            s.delivery = currentDelivery;

            if (executionHeld)
            {
                s.hasProfile = execution.hasProfile;
                s.profile = execution.profile;
                s.hasLockedWeights = true;
                s.lockedWeights = execution.plan.weights;
                s.constants = execution.constants;
                s.maxGainDb = feedbackMaxGainDb;
                s.useSavedPlan = true;
                s.savedPlan = execution.plan;
            }
            else if (committed.hasProfile)
            {
                s.hasProfile = true;
                s.profile = committed.profile;
                s.hasLockedWeights = true;
                s.lockedWeights = committed.lockedWeights;
                s.constants = committed.constants;
                s.maxGainDb = feedbackMaxGainDb;
                s.useSavedPlan = savedPlanMode;
                s.savedPlan = committed.plan;
            }
            else
            {
                s.constants = constants;
                s.maxGainDb = feedbackMaxGainDb;   // 首次聆听中画像失配给的上限（提交时回到不限）

                if (learning == Learning::firstListen)
                {
                    s.hasProfile = true;
                    s.profile = analyzer.getCandidateProfile();
                }
            }

            s.generation = ++generationCounter;
            return s;
        }

        void loadSlot (PlanSlot& slot, const PlanSource& source) noexcept
        {
            slot.source = source;
            slot.valid = false;
        }

        void snapTo (const PlanSource& source) noexcept
        {
            loadSlot (toSlot(), source);
            transitionActive = false;
            hasPending = false;
            pendingLength = 0;
        }

        void startTransition (const PlanSource& source, std::int64_t length) noexcept
        {
            toIndex = 1 - toIndex;          // 旧的终点变成起点（连同它已算好的方案）
            loadSlot (toSlot(), source);
            transitionActive = true;
            transitionElapsed = 0;
            transitionLength = length;
        }

        // 换方案源：GLAZE ≤ 0（没有声音上的意义）或长度为 0 → 直接落到终点；正在过渡 → 排队（只留最新的终点）。
        void requestSource (const PlanSource& source, std::int64_t length) noexcept
        {
            if (! active || length <= 0)
            {
                snapTo (source);
                return;
            }

            if (transitionActive)
            {
                pendingSource = source;
                pendingLength = hasPending ? juce::jmax (pendingLength, length) : length;
                hasPending = true;
                return;
            }

            startTransition (source, length);
        }

        void requestPlanTransition() noexcept
        {
            requestPlanTransition (planTransitionSamples);
        }

        void requestPlanTransition (std::int64_t length) noexcept
        {
            ++probe.planTransitions;

            // 方案要换了：正在进行的校准轮量的是旧方案，数据不能带到新方案上（「不同方案的数据绝不混用」）——直接放弃，
            // 过渡结束、条件稳定后按新方案重来。
            if (roundActive)
            {
                calibrator.reset();
                roundActive = false;
            }

            requestSource (desiredSource(), length);
        }

        // 过渡直接走到终点（宿主 reset、读取工程、GLAZE 回 0、采样率重建）。
        void snapTransitions() noexcept
        {
            if (hasPending)
                loadSlot (toSlot(), pendingSource);

            transitionActive = false;
            hasPending = false;
            pendingLength = 0;
        }

        void evaluate (PlanSlot& slot, float glaze) noexcept
        {
            if (slot.source.useSavedPlan)
            {
                slot.plan = slot.source.savedPlan;
            }
            else
            {
                PlanInputs in;
                in.profile = slot.source.hasProfile ? &slot.source.profile : nullptr;
                in.style = slot.source.style;
                in.genre = slot.source.genre;
                in.delivery = slot.source.delivery;
                in.glazePercent = glaze;
                in.manual = manual;
                in.constants = &slot.source.constants;
                in.maxGainDb = slot.source.maxGainDb;
                in.lockedWeights = slot.source.hasLockedWeights ? &slot.source.lockedWeights : nullptr;
                slot.plan = makePlan (in);
            }

            slot.valid = true;
            slot.evaluatedGeneration = slot.source.generation;
            slot.evaluatedGlaze = glaze;
            slot.evaluatedManual = manual;
            slot.evaluations = ++evaluationCounter;   // 全局单调（R5-02），不是每槽各自 +1
        }

        // 方案按本块的 GLAZE 与手动值更新：新方案源立即算；同一方案源的输入变了，按最短间隔重算。
        void refreshPlans (float glaze) noexcept
        {
            const auto rateOk = samplesSincePlan >= planIntervalSamples;
            bool evaluated = false;

            auto refresh = [&] (PlanSlot& slot)
            {
                const auto fresh = ! slot.valid || slot.evaluatedGeneration != slot.source.generation;
                const auto changed = ! sameBits (slot.evaluatedGlaze, glaze) || ! samePlanningManual (slot.evaluatedManual, manual);

                if (fresh || (changed && rateOk))
                {
                    evaluate (slot, glaze);
                    evaluated = true;
                }
            };

            refresh (toSlot());

            if (transitionActive)
                refresh (fromSlot());

            if (evaluated)
            {
                samplesSincePlan = 0;

                // 方案随工程保存（旧算法版本时照用它）：目标方案属于已提交版本时一并更新。
                if (committed.hasProfile && ! savedPlanMode && toSlot().source.hasLockedWeights)
                {
                    committed.plan = toSlot().plan;
                    stateDirtyLazy = true;
                }
            }
        }

        AutoIncrements currentIncrements() const noexcept
        {
            if (! transitionActive || transitionLength <= 0)
                return toSlot().plan.increments;

            const auto t = (float) juce::jlimit (0.0, 1.0, (double) transitionElapsed / (double) transitionLength);
            return blendIncrements (slots[(size_t) (1 - toIndex)].plan.increments, toSlot().plan.increments, t);
        }

        void advanceTransition (std::int64_t n) noexcept
        {
            if (transitionActive)
            {
                transitionElapsed += n;

                if (transitionElapsed >= transitionLength)
                {
                    transitionActive = false;

                    if (hasPending)
                    {
                        hasPending = false;
                        const auto length = pendingLength;
                        pendingLength = 0;

                        if (active && length > 0)
                            startTransition (pendingSource, length);
                        else
                            loadSlot (toSlot(), pendingSource);
                    }
                }
            }

            if (! transitionActive && committedPhase == Phase::planTransition)
                committedPhase = Phase::locked;
        }

        //==============================================================================
        // ---- 聆听 ----

        void startLearning (Learning kind) noexcept
        {
            analyzer.startLearning();
            learning = kind;
            samplesSinceLearningUpdate = 0;
            ++probe.learningStarts;

            // House 档：段落账随分析器清空了，锁定后防漏网的计数也从头来（新一轮聆听里不重开）；暂定方案等第一段定稿再更新
            // （清空本身让画像版本 + 1，不为一份空画像开一次过渡）。
            if (isHouse (currentGenre))
            {
                clearHouseReopen();
                houseProfileSeen = analyzer.getHouseProfileGeneration();
            }
        }

        void stopLearning() noexcept
        {
            analyzer.stopLearning();
            learning = Learning::none;
        }

        // 聆听结束：有效音频满 minListen 且主要特征置信度够（LRA 不列入：它前 60 s 本来就「未稳定」），或满 maxListen
        // （超时不等于达标：照样按这时的画像生成低置信度方案）。「主要特征」只列一定能测到、且决定增益与处理量的：I、真峰值、
        // PLR、频谱斜率、低 / 高频占比。起音不列入：它只影响「自动」风格的权重；类混音素材上一致性系数把它的置信度压在
        // 0.5 左右，列入就几乎每首歌都要听满 30 s（v0.6 验收 F-03；起音检测本身在试听标定时改进）。
        bool listeningComplete (const MusicProfile& p) const noexcept
        {
            const auto& c = constants;

            if (p.effectiveSeconds >= c.maxListenSeconds)
                return true;

            if (p.effectiveSeconds < c.minListenSeconds)
                return false;

            const auto full = c.fullConfidence;
            auto sure = [full] (const Feature& f) { return f.valid && f.confidence >= full; };

            return sure (p.integratedLufs) && sure (p.truePeakDbtp) && sure (p.plrDb) && sure (p.spectralSlopeDbPerOct)
                && sure (p.lowShare) && sure (p.highShare);
        }

        // 候选可用：整体替换已提交版本（画像、锁定的风格权重、本版本的标定快照；校准作废、约束反馈回到不限），3 s 过渡过去。
        void commitCandidate() noexcept
        {
            commitProfile (analyzer.getCandidateProfile(), planTransitionSamples);
        }

        // 提交一份画像（候选，或 T-038 House 档锁定后重开时按段落账重算的那一份），transitionSamples 内过渡过去。
        void commitProfile (const MusicProfile& candidate, std::int64_t transitionSamples) noexcept
        {
            committed.hasProfile = true;
            committed.profile = candidate;
            committed.profileId = computeProfileId (candidate);
            committed.lockedWeights = resolveAutomaticWeights (candidate, constants);
            committed.constants = constants;
            committed.calibration = Calibration {};
            executionHeld = false;
            absorbedGuardDb = 0.0f;   // 冻结方案留在过渡起点槽里，并进去的退让随增量混合淡出，不需要 carry（R5-04）
            execution.valid = false;
            heldBiasKnown = false;
            executionRequiresCalibration = false;
            restoredTamePending = false;

            stopLearning();
            feedbackMaxGainDb = unlimitedGainDb;
            setClayScaleNow (1.0f);        // 新画像：先试后用重新试过，压力反馈也从头判断（ADR-014）
            savedPlanMode = false;
            profileMismatch = false;
            clearMismatch();
            roundActive = false;
            calibrator.reset();
            planLimitedLatched = false;
            unlearnedOfflineWarning = false;
            committedPhase = Phase::planTransition;
            ++probe.commits;

            // 已提交的标定快照换了：编排器侧按它折算的「卡住」门槛与上游监视窗一并重折算（R5-08；修前只在读档 / prepare 折算，
            // 旧快照读回后重新聆听提交新画像，这两项仍按旧快照的时长）。几个乘法 + 固定数组清零，音频线程可调；监视窗清空
            // 对新方案来说本来就合理（旧数据是旧方案的）。保护层自己的时长随 updateGuardLimits → setLimits 跟着方案源走。
            updateTiming();
            monitor.configure (sampleRate, activeConstants());

            // House 档：这一轮聆听结束了（自动开的标记、用户取消过的记号都作废），锁定后防漏网从新画像起数。
            if (isHouse (currentGenre))
            {
                houseListen = false;
                houseListenDeclined = false;
                clearHouseReopen();
            }

            requestPlanTransition (transitionSamples);
            stateDirtyUrgent = true;
        }

        void updateLearning (std::int64_t n) noexcept
        {
            if (learning == Learning::none)
                return;

            // T-038 House 档：锁定条件与暂定方案另走一条（updateHouseLearning）；通用档照旧。
            if (isHouse (currentGenre))
            {
                updateHouseLearning();
                return;
            }

            // 提交依据有效音频样本数，实时与离线一致；不能因导出标志放弃已有候选或另换保守方案。
            if (listeningComplete (analyzer.getCandidateProfile()))
            {
                commitCandidate();
                return;
            }

            if (learning == Learning::firstListen && ! executionHeld)
            {
                samplesSinceLearningUpdate += n;

                if (samplesSinceLearningUpdate >= learningUpdateSamples)
                {
                    samplesSinceLearningUpdate = 0;
                    ++probe.learningUpdates;
                    requestSource (desiredSource(), learningUpdateSamples);
                }
            }
        }

        // 「重新聆听」：首次聆听中 = 从 0 重来；候选在建 = 取消（奇数次）；已学过 = 开始建候选（奇数次）；
        // 实时与离线接受相同的显式请求；有效音乐证据不足时不会提交。
        void consumeRelearnRequests() noexcept
        {
            if (! prepared || pendingRelearn.load (std::memory_order_relaxed) == 0)
                return;

            const auto count = pendingRelearn.exchange (0, std::memory_order_acq_rel);

            if (count <= 0)
                return;

            const auto toggle = (count % 2) == 1;

            // T-038 House 档：用户点了按钮，这一轮就是用户的（取消自动开的那一轮 = 不再自动开，见 maybeStartHouseListen）。
            if (isHouse (currentGenre))
            {
                if (learning == Learning::relearn && toggle && houseListen)
                    houseListenDeclined = true;

                houseListen = false;
            }

            switch (learning)
            {
                case Learning::firstListen:
                    startLearning (Learning::firstListen);
                    break;

                case Learning::relearn:
                    if (toggle)
                        stopLearning();
                    break;

                case Learning::none:
                default:
                    if (committed.hasProfile)
                    {
                        if (toggle)
                            startLearning (Learning::relearn);
                    }
                    else
                    {
                        startLearning (Learning::firstListen);
                    }
                    break;
            }
        }

        //==============================================================================
        // ---- T-038 House 档（口径见文件开头；只在 isHouse(currentGenre) 时调）----

        // 已提交画像是 House 档锁定过的（格式 3、门控结果有效）。旧格式 1 / 2、通用档学的画像不是：House 档下视为锁定条件未满足。
        static bool houseProfileLocked (const MusicProfile& p) noexcept
        {
            return p.formatVersion >= houseProfileFormatVersion && p.houseGate.valid;
        }

        // 锁定条件第 1、2 条（门控结果按分段方式取一组）。常数取本版本的标定快照（同 listeningComplete）。
        bool houseCountsSatisfied (const HouseGateSummary& g) const noexcept
        {
            const auto& h = constants.house;

            if (! g.valid)
                return false;

            if (g.mode == SegmentMode::seconds)
                return g.heardSeconds >= h.fallbackLockSeconds && g.fullSeconds >= h.fallbackFullSeconds;

            return g.heardSegments >= h.lockSegments && g.fullSegments >= h.requiredFullSegments;
        }

        // 三条锁定条件（设计规格-曲风-House学习方式 第 3.1 节）：已听够、满编段够、现有的置信度条件。
        bool houseLockSatisfied (const MusicProfile& p) const noexcept
        {
            return p.formatVersion >= houseProfileFormatVersion && houseCountsSatisfied (p.houseGate) && listeningComplete (p);
        }

        // 正在进行的过渡在 remaining 个样本内走完，进度 t 不跳：length' = remaining ÷ (1 − t)，elapsed' = length' − remaining。
        void hurryTransition (std::int64_t remaining) noexcept
        {
            if (! transitionActive || transitionLength <= 0 || remaining <= 0 || transitionLength - transitionElapsed <= remaining)
                return;

            const auto t = juce::jlimit (0.0, 1.0, (double) transitionElapsed / (double) transitionLength);
            transitionLength = juce::jmax (remaining, (std::int64_t) std::llround ((double) remaining / (1.0 - t)));
            transitionElapsed = transitionLength - remaining;
        }

        // 暂定方案换方案源。urgent（新响段）：正在走的过渡压到 length 内走完，新方案接在后面、同样快（不按 requestSource 的「排队取最长」）。
        void requestHouseSource (const PlanSource& source, std::int64_t length, bool urgent) noexcept
        {
            if (urgent && active && transitionActive && length > 0)
            {
                hurryTransition (length);
                pendingSource = source;
                pendingLength = length;
                hasPending = true;
                return;
            }

            requestSource (source, length);
        }

        // House 档的聆听（updateLearning 里替代通用档那一段）：锁定条件满足就提交；首次聆听中按新的门控结果更新暂定方案。
        void updateHouseLearning() noexcept
        {
            const auto& candidate = analyzer.getCandidateProfile();

            if (houseLockSatisfied (candidate))
            {
                commitCandidate();
                return;
            }

            if (learning != Learning::firstListen || executionHeld)
                return;

            const auto generation = analyzer.getHouseProfileGeneration();

            if (generation == houseProfileSeen)
                return;

            houseProfileSeen = generation;
            const auto surges = analyzer.getSegmentLedger().getSurgeCount();
            const auto urgent = surges != houseSurgeSeen;
            houseSurgeSeen = surges;
            ++probe.houseUpdates;

            if (urgent)
                ++probe.houseFastTransitions;

            requestHouseSource (desiredSource(), urgent ? houseFastSamples : planTransitionSamples, urgent);
        }

        void clearHouseReopen() noexcept
        {
            houseReopenRun = 0;
            houseReopenArmed = false;
            houseEventCursor = analyzer.getSegmentLedger().getEventCount();   // 之前定稿的段不再数
            houseSurgeSeen = analyzer.getSegmentLedger().getSurgeCount();
            houseReopenTriedAt = analyzer.getSegmentLedger().getFinalizedCount();
        }

        // 进 / 出 House 档的那一块（beginBlock，曲风已定）。子类之间换不算（学习方式相同）。
        void updateHouseGenre() noexcept
        {
            const auto house = isHouse (currentGenre);

            if (house == houseWasActive)
                return;

            houseWasActive = house;
            houseListenDeclined = false;

            if (house)
            {
                // 通用 → House：正在聆听就同一种聆听从此刻重新开始（段落账从空开始，与通用累计对齐）；没在聆听的由 maybeStartHouseListen 处理。
                clearHouseReopen();

                if (learning != Learning::none)
                {
                    const auto wasHouseListen = houseListen;
                    startLearning (learning);
                    houseListen = wasHouseListen;
                }

                return;
            }

            // House → 通用：为 House 自动开的那一轮取消（旧版本照常）；首次聆听与用户点的重新聆听接着按通用档的规则走完。
            if (houseListen && learning == Learning::relearn)
                stopLearning();

            houseListen = false;
            houseReopenRun = 0;
            houseReopenArmed = false;
            clayGuard.updateHouseHint (analyzer.getShortTermView(), 0.0, false, 0.0);   // 提示熄灭、计时清零
            houseHint = ClayGuard::HouseHint {};
        }

        // House 档里已提交的画像还没满足 House 锁定条件（旧格式 1 / 2、通用档学的）：开一轮聆听，旧画像照常生效（设计规格-曲风与交付
        // 第 4.3 节「不清空已有的画像」）。与首次聆听同一个时机（宿主在播或离线导出）。
        void maybeStartHouseListen() noexcept
        {
            if (! prepared || ! committed.hasProfile || learning != Learning::none || houseListenDeclined
                || houseProfileLocked (committed.profile) || ! (hostPlaying() || offline))
                return;

            startLearning (Learning::relearn);
            houseListen = true;
            ++probe.houseListenStarts;
        }

        // 锁定后防漏网（endBlock，House 档、不在聆听时）：数段落账新定稿的段，够了就试着重开。
        void updateHouseLocked() noexcept
        {
            const auto& ledger = analyzer.getSegmentLedger();
            std::array<SegmentLedger::Event, 8> events {};

            for (int attempt = 0; attempt < 64; ++attempt)
            {
                const auto got = ledger.readEvents (houseEventCursor, events.data(), (int) events.size());
                const auto count = std::abs (got);

                if (got < 0)
                    houseReopenRun = 0;   // 落后太多、丢了事件：「连续」断了

                for (int i = 0; i < count; ++i)
                    noteHouseSegment (events[(size_t) i]);

                if (count < (int) events.size())
                    break;
            }

            if (houseReopenArmed && ledger.getFinalizedCount() != houseReopenTriedAt)
            {
                houseReopenTriedAt = ledger.getFinalizedCount();
                tryHouseReopen();
            }
        }

        // 一段定稿：锁定之后（不在聆听、已提交的是 House 锁定过的画像、还能重开）才数「连续更响更满」。
        void noteHouseSegment (const SegmentLedger::Event& event) noexcept
        {
            const auto& p = committed.profile;
            const auto& h = committed.constants.house;

            if (learning != Learning::none || ! committed.hasProfile || ! houseProfileLocked (p) || p.houseGate.reopenCount >= h.maxReopens
                || ! p.integratedLufs.valid || ! p.lowShare.valid)
            {
                houseReopenRun = 0;
                return;
            }

            if (event.epoch != houseReopenEpoch)
            {
                houseReopenRun = 0;   // 跳播 / 回绕 / 宿主 reset 之后不算连续
                houseReopenEpoch = event.epoch;
            }

            const auto louder = event.heard && event.loudnessValid
                             && event.loudnessLufs >= p.integratedLufs.value + h.reopenAboveProfileLu;
            const auto fuller = event.lowShareValid && event.lowShare >= p.lowShare.value * h.lowGateFactor;
            houseReopenRun = louder && fuller ? houseReopenRun + 1 : 0;

            if (houseReopenRun >= juce::jmax (1, h.reopenSegments))
                houseReopenArmed = true;
        }

        // 待重开：段落账也够锁定条件时按它门控重算画像，比已提交画像更响才提交（只往更响更满的方向重开）。
        void tryHouseReopen() noexcept
        {
            const auto& p = committed.profile;

            if (! committed.hasProfile || ! houseProfileLocked (p) || p.houseGate.reopenCount >= committed.constants.house.maxReopens)
            {
                houseReopenArmed = false;
                return;
            }

            if (! houseCountsSatisfied (analyzer.getSegmentLedger().getSummary()))
                return;   // 段落账还不够锁定条件（例如刚读档）：旧画像照常，接着等

            analyzer.recomputeHouseProfile();
            auto reopened = analyzer.getCandidateProfile();

            if (! houseLockSatisfied (reopened))
                return;

            if (! (reopened.integratedLufs.valid && reopened.integratedLufs.value > p.integratedLufs.value))
            {
                houseReopenArmed = false;   // 重算出来并没有更响：不重开
                houseReopenRun = 0;
                return;
            }

            // 新进来的段在锁定之后，先试后用的候选没跑：分析器改用全部段的块仍不够时，沿用已提交画像的代价（同一首歌试过的结论）。
            if (! reopened.lowCutPeakCostDb.valid)
            {
                reopened.lowCutPeakCostDb = p.lowCutPeakCostDb;
                reopened.monoPeakCostDb = p.monoPeakCostDb;
                reopened.widthPeakCostDb = p.widthPeakCostDb;
            }

            reopened.houseGate.reopenCount = p.houseGate.reopenCount + 1;
            ++probe.houseReopens;
            commitProfile (reopened, houseFastSamples);
        }

        //==============================================================================
        // ---- 宿主时间线 ----

        void onTransportJump() noexcept
        {
            // 跳播：分析器丢弃跨越不连续点的窗口（已完成的累计保留）；方案、过渡、保护层照走（保护层是慢速退让，窗口 3 s 内
            // 自己换成新位置的内容；最终输出的真峰值检测从不停）。
            // 校准子窗不作废：校准器按插件自己处理过的样本时间轴把输入、输出子块配对（链的延迟线是连续的），宿主时间线怎么跳
            // 都不影响这一配对，量到的仍是「这条链对正在播的音乐」的净增益。原来按规格在这里作废当前子窗、再等 settle，
            // 在 FL Studio 的 Pattern 模式（几秒一循环、不报循环信息，每次回到开头都像跳播）下校准永远凑不满，状态一直「校准中」
            // （v0.6 验收后 Max 报告；设计规格-GLAZE-实现修订 第 7 节）。真会让配对失效的——宿主 reset、改参数、宿主旁通、
            // 保护层改了实际增益——照旧作废。
            analyzer.handleDiscontinuity (Discontinuity::transportJump);
            clearMismatch();
            ++probe.transportJumps;
        }

        void onLoopWrap() noexcept
        {
            // 循环回绕：同跳播（校准子窗同样不作废）；另外分析器按宿主位置识别重复覆盖，同一段反复循环不增加代表性（它自己记覆盖图）。
            // live Tone 保持当前值，等新的频谱窗口稳定后继续慢速跟随；跳播同样不把它硬切回计划。
            analyzer.handleDiscontinuity (Discontinuity::loopWrap);
            clearMismatch();
            ++probe.loopWraps;
        }

        void onHostReset() noexcept
        {
            // reset 与保存重开采用同一块实际执行量：检测器清空，已听到的偏差和音色保留。
            // 参数已经改变、GLAZE 0、旧算法或没有有效执行块时沿用原清理规则，不能套用过期快照。
            const auto keepExecution = active && ! savedPlanMode && lastExecution.valid
                && isCompatibleAlgorithm (lastExecution.plan.algorithmVersion)
                && isCompatibleAlgorithm (lastExecution.constants.algorithmVersion)
                && GlazeCalibrator::referenceMatches (lastExecution.reference, makeReference (manual));
            if (keepExecution)
            {
                adoptExecution (lastExecution);
                snapTo (desiredSource());
                calibrator.reset();
                roundActive = false;
                haveLastAttempt = false;
                if (committedPhase == Phase::calibrating)
                    committedPhase = Phase::locked;
            }
            else
            {
                calibrator.invalidateCurrentWindow();
                snapTransitions();
                resetClayRuntime();   // 不沿用执行量：Clay 保护回中性（与保护层清零同一口径）
            }
            // 聆听已累计的有效时长保留；不调 analyzer.reset，那会清掉候选累计。
            analyzer.handleDiscontinuity (Discontinuity::hostReset);
            guard.reset();
            guardWasPlanLimited = false;
            clearGuardCarry();
            monitor.clear();
            snapBias = true;
            if (! executionHeld)
            {
                liveToneOffsetDb = 0.0f;
                liveToneProfileId = 0;
                liveHeatReduction = liveGlueReduction = 0.0f;
                liveColorProfileId = 0;
            }
            clearMismatch();
            ++probe.hostResets;
        }

        // 保存重开和宿主 reset 共用执行快照入口；不恢复检测器/滤波器历史，不把未完成校准标成完成。
        void adoptExecution (const ExecutionState& saved) noexcept
        {
            execution = saved;
            executionHeld = execution.valid;
            restoredTamePending = execution.valid;
            heldBiasKnown = execution.valid;
            executionRequiresCalibration = execution.valid && (execution.requiresCalibration
                || execution.protectionDb != 0.0f
                || (committed.calibration.valid && ! sameCalibrationPlan (execution.plan, committed.plan)));
            heldBiasDb = execution.gainBiasDb;
            heldBiasReference = execution.reference;
            calibrationBaseBiasDb = 0.0f;
            if (executionHeld)
            {
                // 保护检测器归零，但其已执行退让吸收到计划/偏差，避免冷启动第一块突然撤回保护。
                const auto absorbed = juce::jmax (-execution.plan.increments.limitGainDb, execution.protectionDb);
                execution.plan.increments.limitGainDb += absorbed;
                execution.plan.effectiveGainDb += absorbed;
                execution.plan.effectiveTargetLufs += absorbed;
                heldBiasDb += execution.protectionDb - absorbed;
                execution.protectionDb = 0.0f;
                absorbedGuardDb = absorbed;   // 解除冻结时由 carry 接着施加、随过渡淡出（releaseAbsorbedGuard，R5-04）
            }
            else
            {
                absorbedGuardDb = 0.0f;
            }
            liveToneOffsetDb = execution.valid ? execution.liveToneOffsetDb : 0.0f;
            liveToneProfileId = execution.valid && committed.hasProfile ? committed.profileId : 0;
            liveHeatReduction = execution.valid ? execution.liveHeatReduction : 0.0f;
            liveGlueReduction = execution.valid ? execution.liveGlueReduction : 0.0f;
            liveColorProfileId = execution.valid && committed.hasProfile ? committed.profileId : 0;

            // Clay：保护状态按快照接着走（setState 自己钳到合法范围），恢复后第一块直接用快照里实际执行过的自动值
            // （updateClay；与上面 liveTone / liveTame 同一套首块语义）；快照无效 = 中性。保护的样本时间从这一刻重新计：
            // 此前累计的属于快照之前的音频，不能再推进恢复后的状态（保存重开与宿主 reset 因此逐位同一起点）。
            if (execution.valid)
                clayGuard.setState (execution.clayGuard);
            else
                clayGuard.reset();

            restoredClay = execution.valid ? sanitizeClay (execution.liveClay) : ClayPlan {};
            clayRestorePending = execution.valid;
            claySeconds = 0.0;
        }

        void setHostInfo (const HostInfo& info) noexcept
        {
            host = info;
            hostFresh = true;
            hostInputOffset = 0;
            offline = info.offline;
            auto analysisHost = info;
            if (offline)
                analysisHost.isPlaying = true; // 部分宿主导出时报告 transport stopped，但连续送来的音频仍有有效位置。
            analyzer.setHostInfo (analysisHost);

            if (! info.hasPosition || ! expectKnown)
                return;

            const auto playingNow = hostPlaying();
            const auto difference = info.positionSamples - expectedPosition;

            if (std::abs (difference) <= positionTolerance || ! playingNow)
                return;

            if (lastPlaying && info.hasLoop && info.looping && difference < 0)
                onLoopWrap();
            else
                onTransportJump();
        }

        void rememberHostPosition (std::int64_t n) noexcept
        {
            if (hostFresh && host.hasPosition)
            {
                const auto playingNow = hostPlaying();
                expectedPosition = host.positionSamples + (playingNow ? n : 0);
                lastPlaying = playingNow;
                expectKnown = true;
            }
            else if (! hostFresh && expectKnown && host.hasPosition)
            {
                // 同一宿主块被按保护统计边界切成多段；每段都推进样本位置，不丢掉下一宿主块的跳播判定。
                if (hostPlaying())
                    expectedPosition += n;
            }
            else
            {
                expectKnown = false;   // 这一块没拿到位置：下一块无从比较，不当作跳播
            }

            hostFresh = false;
        }

        //==============================================================================
        // ---- 校准 ----

        // 手动参数部分（setManualConditions）按输入缓存，见成员 manualConditions 的说明。
        const CalibrationReference& manualConditionsFor (const ChainParams& m) const noexcept
        {
            if (! manualConditionsKnown || ! samePlanningManual (m, manualConditionsInput)
                || ! sameBits (feedbackMaxGainDb, manualConditionsFeedback))
            {
                manualConditions = CalibrationReference {};
                setManualConditions (manualConditions, m, feedbackMaxGainDb);
                manualConditionsInput = m;
                manualConditionsFeedback = feedbackMaxGainDb;
                manualConditionsKnown = true;
            }

            return manualConditions;
        }

        CalibrationReference makeReference (const ChainParams& m) const noexcept
        {
            // 从手动参数部分起步，其余各项下面逐一覆盖（参考条件的每一项都在这里填到）。
            auto r = manualConditionsFor (m);
            r.profileId = committed.hasProfile ? committed.profileId : 0u;
            r.style = currentStyle;
            r.genre = currentGenre;
            r.delivery = currentDelivery;
            r.glazePercent = m.glazePercent;
            r.ceilingDb = m.limit.ceilingDb;
            r.lookaheadMs = m.limit.lookaheadMs;
            r.sampleRate = sampleRate;
            return r;
        }

        // 修正范围 = calibrationMaxDb × GLAZE / 100 × κ（与方案同一个置信度系数；GLAZE 近 0 时近乎为 0）。
        float correctionLimitDb() const noexcept
        {
            if (! committed.hasProfile || ! active)
                return 0.0f;

            const auto& c = committed.constants;
            const auto glaze = juce::jmin (manual.glazePercent, 100.0f) * 0.01f;
            const auto kappa = confidenceScale (&committed.profile, c);
            const auto limit = c.calibrationMaxDb * glaze * kappa;
            return isFinite (limit) ? juce::jmax (0.0f, limit) : 0.0f;
        }

        bool calibrationApplicable() const noexcept
        {
            return committed.hasProfile && committed.calibration.valid && ! executionRequiresCalibration && referenceKnown
                && GlazeCalibrator::referenceMatches (committed.calibration.reference, currentReference);
        }

        bool calibratorRunning() const noexcept
        {
            const auto state = calibrator.getState();
            return roundActive && (state == GlazeCalibrator::State::settling || state == GlazeCalibrator::State::collecting);
        }

        void beginRound() noexcept
        {
            calibrationBaseBiasDb = heldBiasKnown ? heldBiasDb : 0.0f;
            calibrator.setConstants (committed.constants);
            calibrator.begin (toSlot().plan.effectiveGainDb, correctionLimitDb(), currentReference, lastLatency);
            roundActive = true;
            roundReference = currentReference;   // 「刚对同一条件试过」只在结束为无效时记（finishRound，R5-01），开轮不记
            roundMismatchSamples = 0;
            committedPhase = Phase::calibrating;
            ++probe.calibrationBegins;
        }

        void finishRound() noexcept
        {
            const auto limit = correctionLimitDb();   // 同输入只算一次（R5-06）
            committed.calibration = calibrator.getResult();
            committed.calibration.gainBiasDb = juce::jlimit (-limit, limit, calibrationBaseBiasDb + committed.calibration.gainBiasDb);

            // 「刚对同一条件试过、不反复重开」只记真正结束为无效的一轮（begin() 目标非有限时立即结束为无效：finish() 的其余
            // 路径都 valid = true），否则没有这道门每块都会重开。放弃的轮（updateCalibration：条件变了 3 s 没变回来）与换方案
            // 丢掉的轮（requestPlanTransition）不经这里、不算「试过」——条件变回原样（校准中把 GLAZE 拧到 0 或切 Limit Bypass
            // 做 A/B ≥ 3 s 再拧回、换风格再切回）照样能再开（T-032 R5-01 / 核验 M1：修前 beginRound 一开轮就记，放弃后参考逐位
            // 相同、状态行写「稳定后自动校准」却永远不校准）。
            if (! committed.calibration.valid)
            {
                lastAttemptReference = roundReference;
                haveLastAttempt = true;
            }

            heldBiasKnown = false;
            executionRequiresCalibration = false;
            calibrationBaseBiasDb = 0.0f;
            roundActive = false;
            committedPhase = Phase::locked;
            stateDirtyUrgent = true;
            ++probe.calibrationFinishes;

            // 预算受限（保护层反复介入）：以当时的实际净提升作上限重做方案（A3 遗留 2、设计决定 1）。
            if (committed.calibration.valid && committed.calibration.budgetLimited)
            {
                ++probe.feedbackFromBudget;
                applyFeedback (0.0f);
            }
        }

        void updateCalibration (int numInput, int numOutput, std::int64_t n) noexcept
        {
            if (roundActive)
            {
                const auto matches = referenceKnown && GlazeCalibrator::referenceMatches (roundReference, currentReference);
                roundMismatchSamples = matches ? 0 : roundMismatchSamples + n;

                if (roundMismatchSamples >= abandonSamples)
                {
                    // 条件变了、3 s 没变回来：本轮的数据属于旧方案，放弃；稳定后以新条件为底重来（手动修改代表意图）。
                    calibrator.reset();
                    roundActive = false;
                    committedPhase = Phase::locked;
                }
                else
                {
                    // 诊断（TestProbe::calibrationInterruptMask，位的含义见那里）
                    probe.calibrationInterruptMask = (paused ? 1 : 0) | (! active ? 2 : 0) | (transitionActive ? 4 : 0) | (! matches ? 8 : 0)
                                                   | (toSlot().plan.automaticLevelWithdrawn ? 16 : 0) | (guard.isEngaged() ? 32 : 0);

                    // 电平类自动全部撤回（T-032 R6-02 收尾）：没有自动加上去的增益可校准。手动只让 Heat / Glue / Clip 某一项超预算时照常采集
                    // （自动 Gain 仍在推响；保护层与上游监视已只核对自动部分，不会因手动的超出介入）。
                    if (paused || ! active || transitionActive || ! matches || toSlot().plan.automaticLevelWithdrawn)
                        calibrator.invalidateCurrentWindow();

                    calibrator.process (inputBlocks.data(), numInput, outputBlocks.data(), numOutput,
                                        executedBiasDb - calibrationBaseBiasDb, guard.isEngaged(), (int) n);

                    if (calibrator.getState() == GlazeCalibrator::State::finished)
                        finishRound();
                }
            }

            // 开新一轮：已学过、GLAZE > 0、不在宿主旁通、没有方案过渡、电平类自动没有全部撤回、已提交的校准对此刻不适用、
            // 条件连续 1 s 没变，且不是刚刚对同一条件试过（试过而失败的不反复重试，等条件变了再说）。
            // 保护层：在参考状态（g_保护 = 0）才开；它若停在某个退让值上一直不动（超过它自己「安全多久才恢复」的等待 + 1 s，
            // 说明卡在回差区、不会自己回来），也开——这一轮会因保护层持续介入结束为「预算受限」，约束反馈把这点退让吸收进方案
            // （规格「不无限等待」）。正在退让或恢复时不开：量了也不能用，还会白白耗掉这一轮的保护预算。
            // 编排器接着施加的退让（guardCarryDb：约束反馈吸收、解除执行快照冻结时随过渡长度淡出的那一份）同样在改实际增益，
            // 淡完之前也不开（T-032 R5-04：解除冻结的淡出不伴随方案过渡，没有这一条，条件稳定 1 s 后开的轮可能在淡出中途采集；
            // 约束反馈的 carry 与方案过渡同长、比过渡早一块结束，过渡本来就挡着，这一条对它不起作用）。
            const auto guardSettled = (! guard.isEngaged() || guardSteadySamples >= guardStuckSamples) && guardCarryLength <= 0;

            // 诊断（TestProbe::calibrationGateMask，位的含义见那里；与下面开新一轮的条件逐项对应）
            if (roundActive)
            {
                probe.calibrationGateMask = 0;
            }
            else
            {
                probe.calibrationInterruptMask = 0;
                probe.calibrationGateMask = (! committed.hasProfile ? 1 : 0) | (! active ? 2 : 0) | (paused ? 4 : 0) | (transitionActive ? 8 : 0)
                                          | (toSlot().plan.automaticLevelWithdrawn ? 16 : 0) | (! guardSettled ? 32 : 0) | (! referenceKnown ? 64 : 0)
                                          | (calibrationApplicable() ? 128 : 0) | (referenceStableSamples < stableSamples ? 256 : 0)
                                          | (haveLastAttempt && GlazeCalibrator::referenceMatches (lastAttemptReference, currentReference) ? 512 : 0);
            }

            if (! roundActive && committed.hasProfile && active && ! paused && ! transitionActive
                && ! toSlot().plan.automaticLevelWithdrawn && guardSettled && referenceKnown && ! calibrationApplicable()
                && referenceStableSamples >= stableSamples
                && ! (haveLastAttempt && GlazeCalibrator::referenceMatches (lastAttemptReference, currentReference)))
            {
                beginRound();
            }
        }

        // 此刻应施加的校准偏差：轮中 = 校准器的建议（条件暂时不一致或暂停时保持不动：已完成的子窗是在这个偏差下测的）；
        // 已提交的校准适用 = 它的偏差（钳到此刻的修正范围）；否则 0（纯前馈）。GLAZE ≤ 0、电平类自动全部撤回、没学过：0。
        float biasTarget() const noexcept
        {
            if (! active || ! committed.hasProfile || toSlot().plan.automaticLevelWithdrawn)
                return 0.0f;

            // 修正范围与「校准适用」下面各会用到两次，同输入只算一次（R5-06）。
            const auto limit = correctionLimitDb();
            auto target = 0.0f;

            if (calibratorRunning())
            {
                const auto matches = GlazeCalibrator::referenceMatches (roundReference, currentReference);
                target = matches && ! paused ? calibrationBaseBiasDb + calibrator.getResult().gainBiasDb : appliedBiasDb;
            }
            else
            {
                const auto applicable = calibrationApplicable();

                // 第一块只恢复真正执行的偏差；之后已验证、计划未变的目标继续以原有 1 dB/s 追完。
                // 保护吸收/计划改写后校准不适用，保留偏差作为新轮基线，不能把旧目标补回去。
                if (heldBiasKnown && (snapBias || ! applicable))
                    target = heldBiasDb;
                else if (applicable)
                    target = juce::jlimit (-limit, limit, committed.calibration.gainBiasDb);
            }

            return isFinite (target) ? juce::jlimit (-limit, limit, target) : 0.0f;
        }

        // 下一块要施加的校准偏差（按 1 dB/s 线性走向目标；要求直接到位时落到目标）。
        void updateBias (std::int64_t n) noexcept
        {
            const auto target = biasTarget();

            if (snapBias || ! active)
            {
                appliedBiasDb = target;
                snapBias = false;
                return;
            }

            const auto step = biasSlewDbPerSecond * (float) ((double) n / sampleRate);
            appliedBiasDb = appliedBiasDb < target ? juce::jmin (target, appliedBiasDb + step)
                                                   : juce::jmax (target, appliedBiasDb - step);
        }

        //==============================================================================
        // ---- 保护层与约束反馈 ----

        void updateGuardLimits() noexcept
        {
            const auto& slot = toSlot();

            if (guardLimitsKnown && guardLimitsEvaluation == slot.evaluations)
                return;

            // 只核对自动部分（T-032 R6-02 收尾，A11）：手动单独已超预算的项不设约束，其余照风格上限（releaseManualBudget 的说明）。
            // 手动没超预算（掩码 0）时交出去的上限与修前逐位相同。
            const auto released = static_cast<std::uint8_t> (slot.plan.manualBudgetMask & manualBudgetAll);
            // T-038：保护层 / 上游监视与方案同一套上限（俱乐部档放宽限幅预算；其余交付逐位同两参数版）。
            const auto caps = releaseManualBudget (blendConstants (slot.source.constants, slot.plan.weights, slot.source.genre,
                                                                   slot.source.delivery), released);
            guard.setLimits (caps, slot.source.constants);
            monitor.setCaps (caps);

            // 「手动超预算」的项变了：两个窗口里的数据是按另一套口径攒的——手动 Heat 25 拧回 15（通透上限 20）那一刻，上游监视窗里全是
            // 手动造成的超出，按新上限一判立即满「3 s 窗内累计 ≥ 1 s」、约束反馈白降 1 dB（只降不升、随工程保存；IMPL-A7 第 3 节末段）；
            // 保护层同理会按旧时隙退让自动 Gain。清空两个窗口，按新口径从头判定；保护层已执行的退让不动（照常按规则恢复，净增益不跳）。
            // 掩码一直是 0（手动没超预算）时永不进这里，与修前逐位相同。
            if (released != guardReleasedMask)
            {
                monitor.clear();
                guard.clearStatistics();
                guardReleasedMask = released;
            }

            guardLimitsEvaluation = slot.evaluations;
            guardLimitsKnown = true;
        }

        // 手动链自己的预计净增益（GLAZE 0 的方案）：约束反馈上限的下限——再低就是在收回手动部分了。
        float manualNetGainDb() noexcept
        {
            auto source = desiredSource();
            source.useSavedPlan = false;

            PlanInputs in;
            in.profile = source.hasProfile ? &source.profile : nullptr;
            in.style = source.style;
            in.genre = source.genre;
            in.delivery = source.delivery;
            in.glazePercent = 0.0f;
            in.manual = manual;
            in.constants = &source.constants;
            in.lockedWeights = source.hasLockedWeights ? &source.lockedWeights : nullptr;
            const auto plan = makePlan (in);
            return plan.effectiveGainDb;
        }

        // 以当前实际净提升（方案 + 实际执行的校准偏差与保护偏移）为基础下调上限 stepDb，下限为手动链的净增益。
        void applyFeedback (float stepDb) noexcept
        {
            // 上限约束的是方案自己的净提升（plan.effectiveGainDb，净响度目标）；校准偏差是为达到该目标补的 Limit Gain 控制量，不属于
            // 目标域。所以新上限只含方案净提升与实际退让：新上限 = 方案净提升 + 此刻实际执行的保护退让 − step，退让随方案过渡淡出。
            // 含了偏差就会把目标抬高 |偏差|——偏差为正时「降上限」反而把实际净增益抬高，保护层停在回差区永远收不住（T-012 阶段 2：
            // Clay 介入的热素材上校准偏差 +1.54 dB，上限降不下去，保护层停在 −0.89 dB；偏差为负时则多降）。
            // 反馈改了参考条件（feedbackMaxGainDb 在参考条件的哈希里，emitConditions）之后校准怎么办，按反馈的性质分（BUG-003，见 lowerGainCap）：
            //   - step > 0（方案受限、上游超限）：工作点真的降了，已提交的校准不适用、偏差按 1 dB/s 归零，过渡结束后重测（审核 V2-02）；
            //   - step = 0（校准结束为预算受限、锁定后保护层卡住）：只是把保护层已执行的退让并进方案，净增益连续、工作点不变，
            //     已提交的校准改绑到新的参考条件、继续适用，不再重测。
            const auto planNetWithGuard = toSlot().plan.effectiveGainDb + executedGuardDb;
            lowerGainCap (planNetWithGuard - stepDb, planTransitionSamples, ! (stepDb > 0.0f));
        }

        // 修坯压力反馈（ADR-014，ClayGuard.h ClayPressureMonitor / scaleClayPlan）：把自动 Clay 的缩放降一档，从此刻的实际缩放
        // 线性走过去（与方案过渡同长）。缩放作用在方案（含过渡混合）之后、ClayGuard 之前，只动 Clay：不开方案过渡、不离开执行
        // 快照、不作废校准、阶段不变（电平方案、Heat / Glue 的实时回收、校准轮都不受打扰）；尽快存档。
        float currentClayScale() const noexcept
        {
            if (clayScaleRampLength <= 0 || clayScaleRampElapsed >= clayScaleRampLength)
                return clayPressureScale;

            const auto t = (float) ((double) clayScaleRampElapsed / (double) clayScaleRampLength);
            return clayScaleRampStart + (clayPressureScale - clayScaleRampStart) * t;
        }

        void setClayScaleNow (float scale) noexcept
        {
            clayPressureScale = scale;
            clayScaleRampStart = scale;
            clayScaleRampElapsed = clayScaleRampLength = 0;
            clayPressure.reset();
        }

        void lowerClayScale() noexcept
        {
            const auto next = ClayPressureMonitor::nextScale (clayPressureScale);

            if (! (next < clayPressureScale))
                return;

            clayScaleRampStart = currentClayScale();
            clayPressureScale = next;
            clayScaleRampElapsed = 0;
            clayScaleRampLength = juce::jmax ((std::int64_t) 1, planTransitionSamples);
            ++probe.clayPressureSteps;
            stateDirtyUrgent = true;
        }

        // 自动方案此刻有没有可缩的量：三态为「自动」的项里至少一项非 0。全手动 / 全关 / 方案本来就中性时，降档改变不了任何东西。
        bool clayScaleCanAct() const noexcept
        {
            const auto& u = manual.claySettings;
            const auto& p = clayPlannedUnscaled;
            return (u.lowCutMode == ClayMode::automatic && p.lowCutHz > 0.0f)
                || (u.monoMode == ClayMode::automatic && p.monoAmount > 0.0f)
                || (u.widthMode == ClayMode::automatic && p.widthDb > 0.0f);
        }

        // 把净增益上限降到 next（下限为手动链的净增益），重做方案并在 lengthSamples 内过渡过去。约束反馈与画像失配共用。
        // 返回是否真的降了。
        // absorbsGuard = 吸收型反馈（step = 0：预算受限、锁定后保护层卡住）：新方案 = 旧方案 + 保护层已执行的退让，退让同时淡出，
        // 实际净增益与工作点不变；此前已提交且适用的校准（「往上被保护层挡住」的结论与偏差）对新方案照样成立，改绑到新的参考条件、
        // 继续适用，不开新一轮（BUG-003）。修前一律失效重测：新一轮从偏差 0 起步，又测到同样的欠量、又往上推回保护层，
        // 「预算受限 → 反馈 → 重校」一圈约 30 s，真歌整首停在「校准中」（时间线第 2 节：预算受限即「已锁定、方案固定、稳定」；
        // 决策第 6 节：校准器有结束条件，不做持续闭环）。
        bool lowerGainCap (float next, std::int64_t lengthSamples, bool absorbsGuard = false) noexcept
        {
            samplesSinceFeedback = 0;
            planLimitedLatched = false;
            monitor.clear();

            const auto floorDb = manualNetGainDb();

            if (! isFinite (next))
                return false;

            if (isFinite (floorDb))
                next = juce::jmax (next, floorDb);

            if (! (next < feedbackMaxGainDb - feedbackMinimumChangeDb))
                return false;   // 已到下限（或上限本来就更低）：不再降，状态行靠保护层的「方案受限」说明

            const auto keepCalibration = absorbsGuard && ! roundActive && calibrationApplicable();

            feedbackMaxGainDb = next;
            savedPlanMode = false;
            executionHeld = false;
            absorbedGuardDb = 0.0f;   // 冻结方案留在过渡起点槽里，并进去的退让随增量混合淡出，不需要 carry（R5-04）

            if (keepCalibration)
            {
                // 参考条件只有上限变了（下一块 beginBlock 按同一份手动值重算出的就是它）；这里同步 currentReference，
                // 状态行不会在这一块闪成「未校准」。手动值若在下一块变了，照常不适用。
                committed.calibration.reference = makeReference (manual);
                currentReference = committed.calibration.reference;
            }

            if (committed.hasProfile)
                committedPhase = Phase::planTransition;   // 首次聆听中（还没提交）阶段仍是「聆听中」

            ++probe.feedbackSteps;
            requestPlanTransition (lengthSamples);
            stateDirtyUrgent = true;

            // 新方案已按「含保护层退让的实际净提升」定：保护层清零重新开始判定，它此刻实际执行的退让随方案过渡淡出
            // （方案降下来的同时退让撤掉，净增益连续；不这样做，同一份退让会被方案与保护层各算一次，而且保护层卡在回差区时
            // 每一轮都会再降一次）。
            guardCarryStartDb = executedGuardDb;
            guardCarryElapsed = 0;
            guardCarryLength = juce::jmax ((std::int64_t) 1, lengthSamples);
            guard.reset();
            guardWasPlanLimited = false;
            lastGuardDb = 0.0f;
            guardSteadySamples = 0;
            return true;
        }

        //==============================================================================
        // ---- 画像失配（v0.6 验收 F-01；常数见文件开头）----

        void clearMismatch() noexcept
        {
            mismatchSamples = 0;
            mismatchEnergy = 0.0;
            mismatchPeakDb = -100.0f;
        }

        // blockTruePeakDb：本块入口真峰值（IN 表）。
        void updateMismatch (std::int64_t n, float blockTruePeakDb) noexcept
        {
            samplesSinceMismatch += n;

            const auto& source = toSlot().source;
            const auto& profileLoudness = source.profile.integratedLufs;

            if (! active || savedPlanMode || ! source.hasProfile || ! profileLoudness.valid || ! isFinite (profileLoudness.value)
                || toSlot().plan.automaticLevelWithdrawn)   // T-032 R6-02：只在电平类自动全部撤回时不判（手动单独超某模块预算时自动 Gain 仍在推响）
            {
                clearMismatch();
                return;
            }

            const auto& view = analyzer.getShortTermView();
            const auto& snapshot = source.constants;

            if (! view.musicActive || ! view.momentaryLufs.valid || ! isFinite (view.momentaryLufs.value))
                return;   // 静音、门下：不计入，也不清（段落之间的空拍不打断累计）

            if (! (view.momentaryLufs.value - profileLoudness.value >= snapshot.mismatchReleaseLu))
            {
                clearMismatch();
                return;
            }

            mismatchSamples += n;
            mismatchEnergy += (double) n * std::pow (10.0, 0.1 * (double) view.momentaryLufs.value);

            if (isFinite (blockTruePeakDb))
                mismatchPeakDb = juce::jmax (mismatchPeakDb, blockTruePeakDb);

            if (mismatchSamples < mismatchSustainSamples || samplesSinceMismatch < mismatchCooldownSamples)
                return;

            const auto sectionLufs = (float) (10.0 * std::log10 (mismatchEnergy / (double) mismatchSamples));

            if (! isFinite (sectionLufs) || ! (sectionLufs - profileLoudness.value >= snapshot.mismatchThresholdLu))
                return;

            // 把这一段当画像：响度换成这一段的平均 M，真峰值取「这一段实测」与「画像真峰值按同样的抬高量平移」中大的
            // （峰均比不小于画像的），其余特征照用。它的方案净增益就是这一段应有的上限。
            auto section = source.profile;
            const auto shift = sectionLufs - profileLoudness.value;
            section.integratedLufs.value = sectionLufs;

            if (section.truePeakDbtp.valid && isFinite (section.truePeakDbtp.value))
                section.truePeakDbtp.value = juce::jmax (section.truePeakDbtp.value + shift, mismatchPeakDb);
            else if (mismatchPeakDb > -100.0f)
                section.truePeakDbtp = { mismatchPeakDb, true, section.integratedLufs.effectiveSeconds, section.integratedLufs.confidence };

            if (section.truePeakDbtp.valid)
                section.plrDb.value = section.truePeakDbtp.value - sectionLufs;

            PlanInputs in;
            in.profile = &section;
            in.style = source.style;
            in.genre = source.genre;
            in.delivery = source.delivery;
            in.glazePercent = manual.glazePercent;
            in.manual = manual;
            in.constants = &source.constants;
            in.lockedWeights = source.hasLockedWeights ? &source.lockedWeights : nullptr;
            const auto sectionPlan = makePlan (in);

            samplesSinceMismatch = 0;
            clearMismatch();

            // 首次聆听中候选画像自己会跟上（每秒更新），只降增益、不提示；提交之后才提示「建议重新聆听」。
            if (committed.hasProfile)
                profileMismatch = true;

            if (lowerGainCap (sectionPlan.effectiveGainDb, mismatchTransitionSamples))
                ++probe.mismatchSteps;
        }

        // musicActive = 本块保护层的时间轴在走（有效音乐、不在宿主旁通、Color / Limit 没旁通，见 endBlock）。「过渡 + 3 s 观察」
        // 的观察期只在这样的块累加（T-032 核验 M2）：上游监视窗只在这样的块加时隙，观察期若按宿主时间走，反馈 → 过渡 →
        // 静音 / 停播 3 s 之后冷却到期，会按过渡期装进窗里的旧数据在静音中再降 1 dB，而设计要的是观察新方案 3 s。
        void updateFeedback (std::int64_t n, bool musicActive) noexcept
        {
            if (musicActive)
                samplesSinceFeedback += n;

            const auto planLimited = guard.isPlanLimited();

            if (planLimited && ! guardWasPlanLimited)
                planLimitedLatched = true;   // 进入「方案受限」的那一刻计一次

            guardWasPlanLimited = planLimited;

            // 电平类自动全部撤回时没有自动增益可降（T-032 R6-02 收尾：手动只让某一项超预算时照常，保护层与上游监视已不核对那一项）。
            if (! active || ! toSlot().source.hasProfile || transitionActive || toSlot().plan.automaticLevelWithdrawn
                || samplesSinceFeedback < feedbackCooldownSamples)
                return;

            if (planLimitedLatched || monitor.isOver())
            {
                ++(planLimitedLatched ? probe.feedbackFromGuardLimit : probe.feedbackFromUpstream);
                applyFeedback (feedbackStepDb);
                return;
            }

            // 锁定之后保护层卡在部分退让上（停在回差区，自己不会回来）：已提交的校准对此刻适用，就不会再开校准轮、
            // 也就走不到「预算受限 → 吸收」那条路，方案目标会一直比实际高出这段退让。按规格「不无限等待」同样吸收进方案：
            // 上限 = 此刻实际净提升（不再额外降 1 dB），保护层清零、原退让随方案过渡淡出（净增益连续）。
            // 只看实际执行的退让（设计决定 4 可能把它钳成 0：那时没有东西可吸收）。
            if (guard.isEngaged() && guardSteadySamples >= guardStuckSamples && calibrationApplicable()
                && executedGuardDb < -feedbackMinimumChangeDb)
            {
                ++probe.feedbackFromStuckGuard;
                applyFeedback (0.0f);
            }
        }

        float guardCarryDb() const noexcept
        {
            if (guardCarryLength <= 0 || guardCarryElapsed >= guardCarryLength)
                return 0.0f;

            const auto remaining = 1.0 - (double) guardCarryElapsed / (double) guardCarryLength;
            return (float) ((double) guardCarryStartDb * remaining);
        }

        void clearGuardCarry() noexcept
        {
            guardCarryStartDb = 0.0f;
            guardCarryElapsed = guardCarryLength = 0;
        }

        // 解除执行快照的冻结（beginBlock：参考条件变了——拧 GLAZE / 手动值 / 换风格）时调（R5-04）：快照方案里并进去的保护退让
        //（adoptExecution 的 absorbed，守护本身在沿用时已清零）不随新方案一起消失，改由编排器接着施加、按方案过渡同长线性淡出，
        // 与 lowerGainCap 吸收退让时同一套 carry（computeEffective 里 guardDb = 守护 + carry）：净增益在这一项上连续，而方案
        // 本身仍按 GLAZE 即时重算、不做过渡（时间线与保存 §4「拧 GLAZE 不重做方案过渡」）。换风格那一支先经这里再开 3 s 过渡，
        // 两者同长同步淡出。过渡中途保存的混合值差仍是阶跃（少见，接受）。GLAZE ≤ 0 时不用：自动部分整个关掉、保护偏移本来
        // 就归零，留着的 carry 反而会在 GLAZE 回来时凭空施加一段退让。此刻若已有 carry 在走（理论上不会：沿用期间没有），
        // 从它的剩余量接着算，同样连续。
        void releaseAbsorbedGuard() noexcept
        {
            const auto absorbed = absorbedGuardDb;
            absorbedGuardDb = 0.0f;

            if (! active || ! (absorbed < 0.0f))
                return;

            guardCarryStartDb = absorbed + guardCarryDb();
            guardCarryElapsed = 0;
            guardCarryLength = juce::jmax ((std::int64_t) 1, planTransitionSamples);
        }

        //==============================================================================
        // ---- 实际参数 ----

        bool matchingLiveSlopes (float& referenceSlope, float& currentSlope) const noexcept
        {
            const auto& view = analyzer.getShortTermView();
            const auto& learned = committed.profile.spectralSlopeDbPerOct;
            const auto& current = view.liveSpectralSlopeDbPerOct;
            if (! learned.valid || ! isFinite (learned.value) || learned.confidence < 0.5f
                || ! current.valid || ! isFinite (current.value) || current.confidence < 0.6f)
                return false;
            // 以同一有效段/Huber 权重拟合长期参考，避免窄峰令两种统计口径产生假漂移。
            double sw = 0.0, sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0, trustedWeight = 0.0;
            for (int b = 0; b < numBands; ++b)
            {
                const auto w = (double) view.liveSlopeWeights[(size_t) b];
                const auto y = committed.profile.bandLongTermDb[(size_t) b];
                const auto confidence = committed.profile.bandConfidence[(size_t) b];
                if (! isFinite (y) || (w > 0.0 && (! isFinite (confidence) || ! (confidence > 0.0f))))
                    return false;
                trustedWeight += w * juce::jlimit (0.0f, 1.0f, confidence);
                const auto x = (double) b / 3.0;
                sw += w; sx += w * x; sy += w * y; sxx += w * x * x; sxy += w * x * y;
            }
            const auto denominator = sw * sxx - sx * sx;
            if (! (denominator > 1.0e-9) || trustedWeight < 0.5 * sw)
                return false;
            referenceSlope = (float) ((sw * sxy - sx * sy) / denominator);
            currentSlope = current.value;
            return true;
        }

        // 同一块里 updateLiveTone 与 updateLiveColor 都要这份 27 段加权回归，输入（分析器视图、已提交画像）在两次之间不变：
        // 只算一次、结果逐位相同（R5-06）。按需算——两处的门都没过就一次也不算。
        struct LiveSlopes
        {
            bool computed = false, ok = false;
            float referenceSlope = 0.0f, currentSlope = 0.0f;
        };

        bool liveSlopes (LiveSlopes& s, float& referenceSlope, float& currentSlope) const noexcept
        {
            if (! s.computed)
            {
                s.ok = matchingLiveSlopes (s.referenceSlope, s.currentSlope);
                s.computed = true;
            }

            referenceSlope = s.referenceSlope;
            currentSlope = s.currentSlope;
            return s.ok;
        }

        void updateLiveColor (std::int64_t n, LiveSlopes& slopes) noexcept
        {
            if (! active || savedPlanMode || ! committed.hasProfile)
            {
                liveHeatReduction = liveGlueReduction = 0.0f;
                liveColorProfileId = 0;
                return;
            }
            if (liveColorProfileId != committed.profileId)
            {
                liveColorProfileId = committed.profileId;
                liveHeatReduction = liveGlueReduction = 0.0f;
            }
            const auto& view = analyzer.getShortTermView();
            const auto& profile = committed.profile;
            if (paused || transitionActive || manual.color.bypass || ! (manual.color.mixPercent > 0.0f)
                || ! view.musicActive || ! view.momentaryLufs.valid || ! isFinite (view.momentaryLufs.value)
                || view.momentaryLufs.value < -50.0f || ! profile.integratedLufs.valid
                || profile.integratedLufs.confidence < 0.5f || profile.representativeness < 0.5f
                || view.momentaryLufs.value < profile.integratedLufs.value - 15.0f
                || ! view.psr3sDb.valid || ! isFinite (view.psr3sDb.value)
                || ! profile.plrDb.valid || ! isFinite (profile.plrDb.value) || profile.plrDb.confidence < 0.5f)
                return; // 静音、低置信度、未填满的窗口保持已听到的量，不追底噪。
            float referenceSlope = 0.0f, currentSlope = 0.0f;
            if (! liveSlopes (slopes, referenceSlope, currentSlope))
                return;
            const auto& c = toSlot().source.constants;
            if (! isFinite (c.liveColorDensityRangeDb) || ! (c.liveColorDensityRangeDb > 0.0f)
                || ! isFinite (c.liveColorBrightRangeDbPerOct) || ! (c.liveColorBrightRangeDbPerOct > 0.0f)
                || ! isFinite (c.liveColorDensityDeadbandDb) || ! isFinite (c.liveColorBrightDeadbandDbPerOct))
                return;
            // 除相对画像的密度变化，还识别已经非常密集（PSR < 8 dB）/高频上扬的段落。
            const auto density = juce::jlimit (0.0f, 1.0f, juce::jmax (
                profile.plrDb.value - view.psr3sDb.value - c.liveColorDensityDeadbandDb,
                8.0f - view.psr3sDb.value) / c.liveColorDensityRangeDb);
            const auto brightness = juce::jlimit (0.0f, 1.0f,
                (currentSlope - juce::jmin (referenceSlope, 0.0f) - c.liveColorBrightDeadbandDbPerOct)
                    / c.liveColorBrightRangeDbPerOct);
            auto approach = [&] (float& reduction, float risk, float maximum)
            {
                if (! isFinite (maximum))
                    return;
                maximum = juce::jlimit (0.0f, 1.0f, maximum);
                const auto target = maximum * risk;
                // 2.5% 自动份额回差：小摆动不反复追，真正回到 0 时可完全恢复规划量。
                if (target != 0.0f && std::abs (target - reduction) < 0.025f)
                    return;
                const auto speed = target > reduction ? c.liveColorAttackPerSecond : c.liveColorReleasePerSecond;
                if (! isFinite (speed) || ! (speed > 0.0f))
                    return;
                const auto step = juce::jmin (1.0f, speed) * (float) ((double) n / sampleRate);
                reduction = juce::jlimit (0.0f, maximum, reduction + juce::jlimit (-step, step, target - reduction));
            };
            approach (liveHeatReduction, juce::jmax (density, brightness), c.liveHeatMaxReduction);
            approach (liveGlueReduction, juce::jmax (density, 0.5f * brightness), c.liveGlueMaxReduction);
        }

        void updateLiveTone (std::int64_t n, LiveSlopes& slopes) noexcept
        {
            if (! active || savedPlanMode || ! committed.hasProfile)
            {
                liveToneOffsetDb = 0.0f;
                liveToneProfileId = 0;
                return;
            }
            if (liveToneProfileId != committed.profileId)
            {
                liveToneProfileId = committed.profileId;
                liveToneOffsetDb = 0.0f;
            }

            const auto& view = analyzer.getShortTermView();
            const auto& learned = committed.profile.spectralSlopeDbPerOct;
            const auto& current = view.liveSpectralSlopeDbPerOct;
            if (paused || transitionActive || manual.color.bypass || ! (manual.color.mixPercent > 0.0f)
                || ! view.musicActive || ! current.valid || ! isFinite (current.value) || current.confidence < 0.6f
                || ! learned.valid || ! isFinite (learned.value) || learned.confidence < 0.5f
                || ! view.momentaryLufs.valid || view.momentaryLufs.value < -50.0f
                || (committed.profile.integratedLufs.valid
                    && view.momentaryLufs.value < committed.profile.integratedLufs.value - 15.0f))
                return; // 不足证据和静音保持已听到的音色，绝不追着底噪提亮。

            const auto& c = toSlot().source.constants;
            const auto caps = blendConstants (c, toSlot().plan.weights);
            const auto strength = juce::jlimit (0.0f, 1.0f, manual.glazePercent * 0.01f) * confidenceScale (&committed.profile, c);
            if (! (strength > 0.0f) || ! isFinite (caps.toneMaxDb) || ! (caps.toneMaxDb > 0.0f)
                || ! isFinite (c.toneDbPerSlopeDb))
                return;
            float referenceSlope = 0.0f, currentSlope = 0.0f;
            if (! liveSlopes (slopes, referenceSlope, currentSlope))
                return;
            const auto difference = referenceSlope - currentSlope;
            const auto outside = std::copysign (juce::jmax (0.0f, std::abs (difference) - automaticToneSlopeDeadbandDbPerOct), difference);
            const auto base = toSlot().plan.increments.colorToneDb / strength;
            // T-038：往亮的上限与决策器同一口径（House 档 = 子类的 toneMaxBrightDb；通用档 = toneMaxDb，逐位同改前）。
            const auto brightCap = toneBrightCapDb (caps, c, toSlot().source.genre);
            const auto target = juce::jlimit (-caps.toneMaxDb, brightCap,
                                             base + outside * juce::jmax (0.0f, c.toneDbPerSlopeDb)) - base;
            // 最快满量 0.15 dB/s；一次镲片/短暂编曲空洞只可能带来极小变化。纯样本时间，导出同逻辑。
            const auto step = 0.15f * (float) ((double) n / sampleRate);
            liveToneOffsetDb += juce::jlimit (-step, step, target - liveToneOffsetDb);
        }

        void computeEffective() noexcept
        {
            increments = currentIncrements();
            effective = applyIncrements (manual, increments);

            if (active && ! savedPlanMode && committed.hasProfile && liveColorProfileId == committed.profileId
                && ! manual.color.bypass && manual.color.mixPercent > 0.0f)
            {
                // 用钳位后真正剩余的自动份额减量，绝不收回用户的手动 Heat/Glue。
                if (isFinite (manual.color.heat))
                    effective.color.heat -= juce::jmax (0.0f, effective.color.heat - manual.color.heat) * liveHeatReduction;
                if (isFinite (manual.color.glue))
                    effective.color.glue -= juce::jmax (0.0f, effective.color.glue - manual.color.glue) * liveGlueReduction;
            }

            // 目标方案源的标定快照、按方案权重混合后的上限、已提交画像的置信度 κ：下面 Tame 与 Tone 两处同输入，各只算一次
            //（R5-06；纯函数，逐位相同）。都按需算——门没过的块一次也不算。
            const auto& snapshot = toSlot().source.constants;
            StyleConstants caps;
            float kappa = 0.0f;
            bool capsKnown = false, kappaKnown = false;

            const auto blendedCaps = [&]() noexcept -> const StyleConstants&
            {
                if (! capsKnown)
                {
                    caps = blendConstants (snapshot, toSlot().plan.weights);
                    capsKnown = true;
                }

                return caps;
            };

            const auto profileKappa = [&]() noexcept
            {
                if (! kappaKnown)
                {
                    kappa = confidenceScale (&committed.profile, snapshot);
                    kappaKnown = true;
                }

                return kappa;
            };

            // 当前频谱仅提高自动 Tame 权限，不改已提交画像、计划增量、响度目标或校准参考。
            // 证据已在分析器按音频帧平滑；Balance 还会逐 hop 插值。离线与实时同逻辑，旧保存方案保持原行为。
            // 许可 = 上限 × GLAZE × κ × 证据：与方案里的自动 Tame（GlazePlanner：GLAZE × κ）、updateLiveTone 同一口径，且只在
            // 已提交画像之后放开（规格 7.2「学习结束后」；T-032 R6-04：修前不乘 κ、未学习 / 首次聆听中也按满置信度放许可，
            // 与方案侧口径不一致）。κ = 1 时与修前逐位相同。恢复路径不变：证据还没稳定的块沿用执行快照里实际执行过的份额；
            // 未学习的沿用期间也只用快照份额，不放新许可。
            if (active && ! savedPlanMode && ! manual.balance.bypass && toSlot().valid)
            {
                const auto& evidence = analyzer.getShortTermView().liveTameEvidenceDb;
                if (committed.hasProfile && evidence.valid && isFinite (evidence.value) && snapshot.tameEvidenceFullDb > 0.0f)
                {
                    restoredTamePending = false;
                    const auto glaze = juce::jlimit (0.0f, 1.0f, manual.glazePercent * 0.01f);
                    const auto tameCap = isFinite (blendedCaps().tameCap) ? juce::jlimit (0.0f, 100.0f, blendedCaps().tameCap) : 0.0f;
                    const auto liveTame = tameCap * glaze * profileKappa()
                                        * juce::jlimit (0.0f, 1.0f, evidence.value / snapshot.tameEvidenceFullDb);
                    effective.balance.tame = juce::jlimit (0.0f, 100.0f,
                        manual.balance.tame + juce::jmax (increments.balanceTame, liveTame));
                    effective.balance.automaticTame = juce::jmax (0.0f, effective.balance.tame - manual.balance.tame);
                }
                else if (restoredTamePending)
                {
                    effective.balance.tame = juce::jlimit (0.0f, 100.0f, manual.balance.tame + execution.liveTame);
                    effective.balance.automaticTame = juce::jmax (0.0f, effective.balance.tame - manual.balance.tame);
                }
            }

            if (active && ! savedPlanMode && committed.hasProfile && liveToneProfileId == committed.profileId
                && ! manual.color.bypass && manual.color.mixPercent > 0.0f && toSlot().valid)
            {
                const auto cap = blendedCaps().toneMaxDb;
                const auto strength = juce::jlimit (0.0f, 1.0f, manual.glazePercent * 0.01f) * profileKappa();
                if (isFinite (cap) && cap > 0.0f)
                {
                    // T-038：往亮的上限同上（House 档按子类，通用档 = cap，逐位同改前）。
                    const auto bright = toneBrightCapDb (blendedCaps(), snapshot, toSlot().source.genre);
                    // 0.01 dB 量化避免稳定段不断重算搁架系数；模块仍以原来的平滑器过渡。
                    const auto offset = std::round (liveToneOffsetDb * 100.0f) * 0.01f;
                    const auto tone = juce::jlimit (-cap * strength, bright * strength, increments.colorToneDb + offset * strength);
                    if (tone != increments.colorToneDb && isFinite (manual.color.toneDb))
                        effective.color.toneDb = juce::jlimit (-3.0f, 3.0f, manual.color.toneDb + tone);
                }
            }

            // 旧算法存档沿用原来的 Recover 行为；用户主动重算后才启用新增的自动逐段保护。
            if (savedPlanMode)
            {
                effective.balance.automaticRecover = manual.balance.automaticRecover;
                effective.balance.automaticTame = manual.balance.automaticTame;
            }

            executedBiasDb = 0.0f;
            executedGuardDb = 0.0f;

            // 电平类自动全部撤回（Plan::automaticLevelWithdrawn）：没有自动加上去的增益，校准偏差与保护偏移都不施加（手动造成的超限只提示，
            // 不偷偷改）。手动只让 Heat / Glue / Clip 某一项超预算时（界面照旧「受手动设置限制」）自动 Gain 仍在推响，偏差与保护照常施加：
            // 保护层与上游监视已只核对自动部分，手动单独超预算的那一项不设约束（updateGuardLimits，T-032 R6-02 收尾 A11）；执行量本来就
            // 只收回自动加上去的那一份（下面设计决定 4）。
            if (toSlot().plan.automaticLevelWithdrawn)
                return;

            const auto manualGain = manual.limit.gainDb;
            const auto afterIncrement = effective.limit.gainDb;

            if (! isFinite (manualGain) || ! isFinite (afterIncrement))
                return;

            // 实际 Limit Gain 永远不低于手动值（设计决定 4）：自动部分 = 增量 + 校准偏差，偏差最多把它收回到 0；
            // 保护层只能收回自动加上去的部分（按实际执行量），要的比能收回的多由约束反馈降方案。
            const auto autoPart = juce::jmax (0.0f, afterIncrement - manualGain);
            const auto bias = juce::jmax (appliedBiasDb, -autoPart);
            const auto guardDb = guard.getProtectionDb() + guardCarryDb();
            const auto automaticTotal = juce::jmax (0.0f, autoPart + bias);
            const auto guardExecuted = juce::jmin (0.0f, juce::jmax (guardDb, -automaticTotal));

            if (bias == 0.0f && guardExecuted == 0.0f)
                return;

            const auto withBias = juce::jlimit (0.0f, 24.0f, afterIncrement + bias);
            executedBiasDb = withBias - afterIncrement;
            effective.limit.gainDb = juce::jlimit (0.0f, 24.0f, afterIncrement + bias + guardExecuted);
            executedGuardDb = effective.limit.gainDb - withBias;
        }

        //==============================================================================
        // ---- 修坯 Clay（v0.1.7.0；口径见文件开头）----

        // 方案源带不带 Clay 所需的立体声 / 超低频特征：画像格式 ≥ 2。格式 1 的旧画像读入后这些特征全是「不知道」；没有画像
        //（未学习、也没在聆听）同样没有依据。
        static bool sourceSupportsClay (const PlanSource& source) noexcept
        {
            return source.hasProfile && source.profile.formatVersion >= clayProfileFormatVersion;
        }

        // 这个方案槽能给出 Clay 自动值：已算过、是本版本算法的方案（旧算法版本保存的方案照用它自己的增量，那时还没有 Clay，
        // 一律中性——就算存档里碰巧有 Clay 属性也不采用）、方案源带 Clay 特征。
        static bool slotSupportsClay (const PlanSlot& slot) noexcept
        {
            return slot.valid && slot.plan.algorithmVersion == currentAlgorithmVersion && sourceSupportsClay (slot.source);
        }

        static ClayPlan slotClay (const PlanSlot& slot) noexcept
        {
            return slotSupportsClay (slot) ? sanitizeClay (slot.plan.clay) : ClayPlan {};
        }

        // 过渡里「关」的一端参与对数混合用的下限频率：取终点（其次起点）标定快照的 clayLowCutFloorHz（出厂 10 Hz），钳到
        // ClayModule 的低切范围；旧快照缺这个字段（读成 0）就用 ClayModule 的最低低切频率。
        float clayLowCutFloorHz() const noexcept
        {
            for (const auto* slot : { &toSlot(), &fromSlot() })
            {
                const auto hz = slot->source.constants.clayLowCutFloorHz;

                if (isFinite (hz) && hz > 0.0f)
                    return juce::jlimit (ClayModule::minLowCutHz, ClayModule::maxLowCutHz, hz);
            }

            return ClayModule::minLowCutHz;
        }

        // 方案给的 Clay 自动值：方案过渡中与自动增量同一个进度 t（currentIncrements 的同一个式子）混合；不支持 Clay 的一端
        // 按中性参与（旧算法方案 / 旧画像 → 新方案时 Clay 随过渡平滑启用）。
        ClayPlan plannedClay() const noexcept
        {
            const auto target = slotClay (toSlot());

            if (! transitionActive || transitionLength <= 0)
                return target;

            const auto t = (float) juce::jlimit (0.0, 1.0, (double) transitionElapsed / (double) transitionLength);
            return blendClayPlans (slotClay (fromSlot()), target, t, clayLowCutFloorHz());
        }

        // 此刻有没有方案源支持自动 Clay（过渡的任一端）。都不支持时自动值中性、ClayGuard 不工作。
        bool clayAutomaticSupported() const noexcept
        {
            return slotSupportsClay (toSlot()) || (transitionActive && slotSupportsClay (fromSlot()));
        }

        // ClayGuard 的常数跟着方案源（过渡终点）的标定快照走：换了方案源才重设（只拷几个数，不分配）。
        void syncClayConstants() noexcept
        {
            const auto& source = toSlot().source;

            if (clayConstantsKnown && clayConstantsGeneration == source.generation)
                return;

            clayGuard.setConstants (source.constants);
            clayConstantsGeneration = source.generation;
            clayConstantsKnown = true;
        }

        // Clay 运行状态回中性（不沿用执行量的宿主 reset、GLAZE 回 0、重建时）：保护清零，快照的首块值作废，样本时间重新计。
        void resetClayRuntime() noexcept
        {
            clayGuard.reset();
            clayRestorePending = false;
            claySeconds = 0.0;
        }

        // 每块一次（beginBlock 里，自动增量与其他实际参数算完之后）：自动值 = 方案（含过渡混合）→ ClayGuard 保护；
        // 再按三态合成交给 ClayModule 的实际参数（只写 effective.clay，不碰其余参数）。
        void updateClay() noexcept
        {
            const auto seconds = claySeconds;   // 上一块的有效音乐秒数（静音 / 宿主旁通 / 停播块 = 0 → 保护冻结）
            claySeconds = 0.0;
            lastClaySeconds = 0.0f;

            clayPlannedUnscaled = plannedClay();
            clayPlanned = scaleClayPlan (clayPlannedUnscaled, currentClayScale(), clayLowCutFloorHz());   // ADR-014 压力反馈
            claySupported = clayAutomaticSupported();

            // 快照的首块值只用一次：不管这一块用没用上都作废（不支持时留着，会在以后某块方案源变得支持时拿过期的值出来）。
            const auto restoring = clayRestorePending && claySupported;
            clayRestorePending = false;
            clayRestoredLastBlock = restoring;

            if (restoring)
            {
                // 保存重开 / 沿用执行量的宿主 reset / 同规格重建之后的第一块：直接用快照里实际执行过的自动值（保护状态已按快照
                // 恢复、这一块不推进）；下一块起保护按快照状态接着走。只在方案源支持自动 Clay 时才用（审核 V2-03）：手工改出的
                // 「画像格式 1 / 旧算法方案 + 非中性 liveClay」存档，否则会有一块交给 ClayModule 非中性值——走下面的中性分支。
                syncClayConstants();
                liveClay = restoredClay;
            }
            else if (claySupported)
            {
                syncClayConstants();
                liveClay = sanitizeClay (clayGuard.update (clayPlanned, analyzer.getShortTermView(), seconds, true));
                lastClaySeconds = (float) seconds;
            }
            else
            {
                // 旧画像 / 旧算法方案 / 没有画像：自动 Clay 中性，保护也不工作——旧工程重新聆听之前 Clay 逐位直通。
                clayGuard.reset();
                liveClay = ClayPlan {};
            }

            effective.clay = resolveClayParams (manual.claySettings, liveClay, manual.glazePercent);

            // T-038 House 档反相提示（设计规格-曲风-House学习方式 第 9 节；判定在 ClayGuard，只提示、不改声音）：资格 = 段落账此刻这一段
            // 暂判满编，须连续 phaseHintSegments 小节（拿不到拍点时 phaseHintFallbackSeconds 秒）。方案源不支持自动 Clay 时熄灭（同反相指示灯）。
            if (isHouse (currentGenre))
            {
                if (claySupported)
                {
                    const auto& ledger = analyzer.getSegmentLedger();
                    const auto& h = toSlot().source.constants.house;
                    const auto barSeconds = ledger.getMode() == SegmentMode::bars ? ledger.getCurrentSegmentSeconds() : 0.0;
                    const auto required = barSeconds > 0.0 ? (double) juce::jmax (1, h.phaseHintSegments) * barSeconds
                                                           : h.phaseHintFallbackSeconds;
                    clayGuard.updateHouseHint (analyzer.getShortTermView(), seconds, ledger.isCurrentSegmentFull(), required);
                    houseHint = clayGuard.getHouseHint();
                }
                else
                {
                    houseHint = ClayGuard::HouseHint {};
                }
            }
        }

        const ChainParams& beginBlock (const ChainParams& newManual, bool hostBypassed) noexcept
        {
            const auto restored = consumeRestore();
            consumeRelearnRequests();

            manual = newManual;
            paused = hostBypassed;
            active = manual.glazePercent > 0.0f;   // 「> 0」：NaN 与负数都当作 0

            // 读取工程后的第一块：读回来的旧存档参考（只有旧口径哈希）按此刻的手动参数升格（BUG-004）——对数刻度值往返后漂了
            // 几个 ulp，旧口径哈希可能已经翻转；邻域里有一组码对得上就是同一条件。要在下面第一次比较之前做；没生效的参考不管。
            if (restored)
                probe.legacyConditionsUpgrades += upgradeLegacyConditions (
                    { committed.calibration.valid ? &committed.calibration.reference : nullptr,
                      executionHeld ? &execution.reference : nullptr,
                      heldBiasKnown ? &heldBiasReference : nullptr },
                    manual, feedbackMaxGainDb);

            // 换风格：方案源换成新风格（约束反馈上限回到不限），3 s 过渡；GLAZE ≤ 0 时直接到位。
            // 第一块与刚读取工程的这一块：方案源按此刻的风格直接落到位（读取后立即生效，不做过渡）。
            // T-038：曲风与交付与风格同一套规则（设计规格-曲风与交付 第 4.3 节：不重新聆听，方案重算、过渡、重新校准）。
            const auto style = styleFromIndex (manual.glazeStyle);
            const auto genre = genreFromIndex (manual.glazeGenre);
            const auto delivery = deliveryFromIndex (manual.glazeDelivery);

            if (prepared && executionHeld)
            {
                auto reference = makeReference (manual);
                reference.style = style;
                reference.genre = genre;
                reference.delivery = delivery;
                if (! GlazeCalibrator::referenceMatches (execution.reference, reference))
                {
                    executionHeld = false;
                    execution.valid = false;
                    heldBiasKnown = false;
                    executionRequiresCalibration = false;
                    restoredTamePending = false;
                    liveToneOffsetDb = 0.0f;
                    liveHeatReduction = liveGlueReduction = 0.0f;
                    liveColorProfileId = 0;
                    clayRestorePending = false;   // 条件变了：快照里的 Clay 值同样不再适用（保护状态照常接着走）
                    snapTo (desiredSource());
                    releaseAbsorbedGuard();       // 快照里并进去的保护退让改由 carry 淡出，净增益不阶跃（R5-04）
                }
            }

            if (restored || ! styleKnown)
            {
                currentStyle = style;
                currentGenre = genre;
                currentDelivery = delivery;
                styleKnown = true;
                snapTo (desiredSource());

                if (restored)
                    publishStatus();
            }
            else if (style != currentStyle || genre != currentGenre || delivery != currentDelivery)
            {
                currentStyle = style;
                currentGenre = genre;
                currentDelivery = delivery;
                feedbackMaxGainDb = unlimitedGainDb;
                savedPlanMode = false;
                planLimitedLatched = false;

                if (committed.hasProfile)
                {
                    committedPhase = Phase::planTransition;
                    stateDirtyUrgent = true;
                }

                requestPlanTransition();
            }

            // T-038：分析器跟着此刻的曲风（不变时什么都不做）；进 / 出 House 档的一次性处理（文件开头「换曲风」）。
            analyzer.setGenre (currentGenre);

            if (isHouse (currentGenre) || houseWasActive)
                updateHouseGenre();

            // 旧算法版本的已提交方案：照用保存的增量，直到拧 GLAZE（或换风格、重学）才按新算法重算。
            if (savedPlanMode)
            {
                if (! savedPlanGlazeKnown)
                {
                    savedPlanGlaze = manual.glazePercent;
                    savedPlanGlazeKnown = true;
                }
                else if (! sameBits (savedPlanGlaze, manual.glazePercent))
                {
                    savedPlanMode = false;

                    if (committed.hasProfile)
                        committedPhase = Phase::planTransition;

                    requestPlanTransition();
                }
            }

            // 实时与导出都按音频样本学习。离线宿主可能不报告 Playing，但传来的有效音乐同样是证据。
            if (prepared && ! committed.hasProfile && learning == Learning::none && (hostPlaying() || offline))
                startLearning (Learning::firstListen);

            // T-038 House 档：已提交画像不是 House 锁定过的 → 开一轮聆听，旧画像照常生效。
            if (isHouse (currentGenre))
                maybeStartHouseListen();

            unlearnedOfflineWarning = false;
            if (! committed.hasProfile && learning == Learning::firstListen && ! executionHeld && ! toSlot().source.hasProfile
                     && ! transitionActive && ! hasPending)
            {
                requestSource (desiredSource(), learningUpdateSamples);
            }

            // 校准参考条件（只在 GLAZE > 0 时有意义；GLAZE 0 时视为一直在变）。「条件稳定」只看用户能动的那几项
            //（GLAZE、手动值、风格、Ceiling、Lookahead、采样率）：提交新画像、约束反馈改了上限这类编排器自己的变化会让已提交的校准
            // 不适用，但不重新计时——方案过渡一结束就开始校准（时间线第 2 节「校准中 | 方案过渡结束」）。吸收型反馈例外：
            // lowerGainCap 已把适用的校准改绑到新上限（BUG-003），这里重算出的参考与它一致，不开新一轮。
            if (active)
            {
                const auto profileId = committed.hasProfile ? committed.profileId : 0u;
                const auto unchanged = referenceKnown && samePlanningManual (manual, referenceManual)
                                        && sameBits (manual.glazePercent, referenceManual.glazePercent)
                                        && profileId == referenceProfileId && currentStyle == referenceStyle
                                        && currentGenre == referenceGenre && currentDelivery == referenceDelivery
                                        && sameBits (feedbackMaxGainDb, referenceFeedback) && savedPlanMode == referenceSavedPlan
                                        && sampleRate == referenceRate;

                if (! unchanged)
                {
                    currentReference = makeReference (manual);

                    auto user = currentReference;
                    user.profileId = 0;
                    setManualConditions (user, manual, unlimitedGainDb);

                    // 稳定计时（用户有没有在动旋钮）逐块精确比，与修前逐位等价：另比旧口径哈希，它对全部手动参数都是精确的（对数刻度值
                    // 差 1e-4 以内也算动了）。参考条件的容差只管「校准还适不适用」——重开工程的往返漂移只在读档那一次，之后值不再变；
                    // 稳定计时要是也按容差，比 1e-4/s 还慢的扫描就会被当成「稳定」，校准轮开了又放弃（BUG-004 评审）。
                    if (! referenceKnown || ! GlazeCalibrator::referenceMatches (user, previousUserConditions)
                        || user.manualParamsHash != previousUserConditions.manualParamsHash)
                        referenceStableSamples = 0;

                    previousUserConditions = user;
                    referenceKnown = true;
                    referenceManual = manual;
                    referenceProfileId = profileId;
                    referenceStyle = currentStyle;
                    referenceGenre = currentGenre;
                    referenceDelivery = currentDelivery;
                    referenceFeedback = feedbackMaxGainDb;
                    referenceSavedPlan = savedPlanMode;
                    referenceRate = sampleRate;
                }
            }
            else
            {
                referenceKnown = false;
                referenceStableSamples = 0;
            }

            if (prepared && heldBiasKnown && (! referenceKnown || ! GlazeCalibrator::referenceMatches (heldBiasReference, currentReference)))
            {
                heldBiasKnown = false;
                executionRequiresCalibration = false;
            }

            if (! active)
            {
                // GLAZE ≤ 0：实际参数就是手动值（逐位，不经任何运算）；过渡落到终点、校准偏差归零（保护层在 endBlock 里清）。
                snapTransitions();
                appliedBiasDb = 0.0f;
                executedBiasDb = executedGuardDb = 0.0f;
                increments = AutoIncrements {};
                effective = manual;

                // Clay 照样按三态合成（ADR-009）：「自动」在 GLAZE 0 时是中性（Clay 逐位直通）、「手动」严格按旋钮值生效、
                // 「关」中性；Clay 保护回中性。只写 effective.clay，原有 7 个参数仍逐位等于手动值。
                resetClayRuntime();
                clayPlanned = liveClay = ClayPlan {};
                claySupported = false;
                clayRestoredLastBlock = false;
                lastClaySeconds = 0.0f;
                effective.clay = resolveClayParams (manual.claySettings, ClayPlan {}, manual.glazePercent);

                if (isHouse (currentGenre))
                    houseHint = ClayGuard::HouseHint {};   // T-038：House 反相提示同反相指示灯，GLAZE ≤ 0 时熄灭

                lastExecution.valid = false;
                return effective;
            }

            refreshPlans (manual.glazePercent);

            // 读取工程 / 宿主 reset / 重建之后的第一块：校准偏差直接落到目标（不从旧值滑过去）。
            if (snapBias)
            {
                appliedBiasDb = biasTarget();
                snapBias = false;
            }

            computeEffective();
            updateClay();
            captureExecution();
            return effective;
        }

        void captureExecution() noexcept
        {
            auto& state = lastExecution;
            state.valid = active && toSlot().valid;
            if (! state.valid)
                return;
            state.hasProfile = toSlot().source.hasProfile;
            state.provisional = ! committed.hasProfile;
            state.requiresCalibration = executionRequiresCalibration;
            state.profile = toSlot().source.profile;
            state.constants = toSlot().source.constants;
            state.plan = toSlot().plan;
            state.plan.increments = increments;
            if (transitionActive && transitionLength > 0)
            {
                const auto t = (float) juce::jlimit (0.0, 1.0, (double) transitionElapsed / (double) transitionLength);
                const auto& from = fromSlot().plan;
                auto mix = [t] (float a, float b) { return a + (b - a) * t; };
                state.plan.effectiveGainDb = mix (from.effectiveGainDb, state.plan.effectiveGainDb);
                state.plan.effectiveTargetLufs = mix (from.effectiveTargetLufs, state.plan.effectiveTargetLufs);
                state.plan.weights.warm = mix (from.weights.warm, state.plan.weights.warm);
                state.plan.weights.punchy = mix (from.weights.punchy, state.plan.weights.punchy);
                state.plan.weights.clean = mix (from.weights.clean, state.plan.weights.clean);
            }
            state.reference = makeReference (manual);
            state.gainBiasDb = executedBiasDb;
            state.protectionDb = executedGuardDb;
            state.liveToneOffsetDb = liveToneOffsetDb;
            state.liveTame = effective.balance.automaticTame;
            state.liveHeatReduction = liveHeatReduction;
            state.liveGlueReduction = liveGlueReduction;
            // Clay：方案里存过渡混合后（保护之前）的自动值——与上面的 increments 同一口径，恢复后作为被沿用方案的 plan.clay；
            // 另存保护之后实际执行的值与保护状态（恢复后第一块直接用、保护接着走）。都已按读取时核对的范围收拾过。
            state.plan.clay = clayPlanned;
            state.liveClay = liveClay;
            state.clayGuard = sanitizeClayGuard (clayGuard.getState());
        }

        //==============================================================================
        void endBlock (const BalanceModule& balance, const LoudnessMeter& inputMeter, const LoudnessMeter& outputMeter,
                       const ModuleStats& stats, int chainLatencySamples, bool spectrumDeferred) noexcept
        {
            const auto n = (std::int64_t) juce::jmax (0, stats.numSamples);
            lastLatency = juce::jmax (0, chainLatencySamples);

            // 链在下一块开头交帧（analyzeSpectrum）；不经链的调用方没交，就在这里交（原来约定的位置）。
            if (! spectrumTakenAtBlockStart && ! spectrumDeferred)
                analyzer.processSpectrum (balance);

            spectrumTakenAtBlockStart = false;
            const auto& view = analyzer.getShortTermView();

            // 保护层的时间轴（T-032 R5-03 / 核验 M2）：有效音乐、不在宿主旁通、Color / Limit 没旁通（旁通期间输出是干路，统计不代表
            // 听到的）。保护层本身、上游监视、「g_保护 多久没变过」与约束反馈的观察期都按它走；静音 / 停播 / 旁通冻结的块一律不计。
            const auto guardMusicActive = view.musicActive && ! paused && ! manual.color.bypass && ! manual.limit.bypass;

            // Clay 保护的时间轴（下一块 beginBlock 一次交给 ClayGuard）：只推进有效音乐——静音（分析器门下）、宿主旁通、停播都
            // 冻结，保持期也不走；按切片累计，与宿主块怎么切无关。
            if (view.musicActive && ! paused && hostPlaying())
                claySeconds += (double) n / sampleRate;

            // 修坯压力反馈（ADR-014）：GLAZE 活跃、方案源支持自动 Clay、缩放还没降到 0 时，按有效音乐时间监测本切片走完的代价格
            // （不经守护时相关峰值会超出界多少 dB）；持续超过「先试后用」照做的上限 → 降一档。旁通 / 三项为 0 时代价本来就是 0。
            // 时间按代价格计（ClayModule 从 reset 起的 10 ms 网格），与宿主块怎么切无关；只有「有效音乐」的门按切片取。
            if (active && claySupported && clayPressureScale > 0.0f && clayScaleCanAct())
            {
                const auto musicSeconds = view.musicActive && ! paused && hostPlaying() ? stats.clayPressureSeconds : 0.0;

                if (clayPressure.update ((double) stats.clayPressureDb, musicSeconds))
                    lowerClayScale();
            }
            else
            {
                clayPressure.reset();
            }

            if (clayScaleRampElapsed < clayScaleRampLength)
                clayScaleRampElapsed = juce::jmin (clayScaleRampLength, clayScaleRampElapsed + n);

            // 两块表本块新完成的子块，一次交齐（自己的游标；落后丢过子块 = 中断）。
            auto numInput = inputMeter.readSubBlocksSince (inputCursor, inputBlocks.data(), subBlockCapacity);
            auto numOutput = outputMeter.readSubBlocksSince (outputCursor, outputBlocks.data(), subBlockCapacity);

            if (numInput < 0 || numOutput < 0)
            {
                calibrator.invalidateCurrentWindow();
                numInput = std::abs (numInput);
                numOutput = std::abs (numOutput);
            }

            // ---- 保护层（GLAZE > 0 才工作；宿主旁通、Color / Limit 旁通期间冻结：输出是干路，统计不代表听到的）----
            // 电平类自动全部撤回时清零、不工作：没有自动加上去的增益可收回（设计决定 4），它若照常累计退让，等手动设置改回来那一刻会
            // 突然施加一大步。手动只让 Heat / Glue / Clip 某一项超预算时照常工作（规格「学习、过渡、校准期间保护层同样生效」）：
            // updateGuardLimits 把那一项放开、只核对自动部分——修前按「手动 + 自动」的总量核对，会白白退让自动 Gain、约束反馈每约 3.5 s
            // 降 1 dB 把自动增益磨光（T-032 R6-02，IMPL-A7 第 3 节 107 实测），所以当时这道门只好连同这些组合一起关着。
            if (active && ! toSlot().plan.automaticLevelWithdrawn)
            {
                updateGuardLimits();

                auto guardStats = stats;
                guardStats.musicActive = guardMusicActive;
                if (! guardStats.musicActive)
                    guard.discardPartialSlot(); // 冻结只丢未完成片段；完整历史/保护保留，避免 1-sample 切片风暴。
                guard.process (guardStats);
                guardClean = false;

                // 「g_保护 多久没变过」按保护层自己的时间轴计（T-032 R5-03）：它在静音 / 停播 / 旁通的块整块不计、g_保护 不动，
                // 这些块若也算「没变」，音乐停 6 s（或一段空拍、Limit Bypass 开着）就把一次临时退让当成「卡住」永久并进方案
                // 上限（只降不升、随工程保存），而规格「静音、停播不计入恢复时间」——音乐一回来本可 5 s 后恢复到 0。
                // 冻结块不加也不清；lastGuardDb 照旧每块更新。
                const auto guardDb = guard.getProtectionDb();
                if (guardMusicActive)
                    guardSteadySamples = sameBits (guardDb, lastGuardDb) ? guardSteadySamples + n : 0;
                lastGuardDb = guardDb;

                if (toSlot().source.hasProfile)
                    monitor.add (guardStats.glueGrMeanDb, guardStats.heatDrive, (int) n, guardStats.musicActive);
            }
            else if (! guardClean)
            {
                guard.reset();
                guardWasPlanLimited = false;
                clearGuardCarry();
                monitor.clear();
                guardClean = true;
            }

            // 先走过渡：本块刚走完的过渡，校准在同一块就能开始（「校准中 | 方案过渡结束」，中间不闪一下「待校准」）；
            // 本块里新开的过渡（提交、约束反馈）不在同一块被推进，从下一块起按样本走满全长。
            advanceTransition (n);
            updateCalibration (numInput, numOutput, n);
            updateLearning (n);

            if (isHouse (currentGenre) && learning == Learning::none)
                updateHouseLocked();   // T-038：锁定后防漏网（文件开头）

            updateMismatch (n, juce::Decibels::gainToDecibels (inputMeter.getLastBlockTruePeakLinear(), -100.0f));
            updateFeedback (n, guardMusicActive);
            updateBias (n);

            LiveSlopes slopes;   // 音色 / 密度跟随共用的斜率回归，同一块只算一次（R5-06）
            updateLiveTone (n, slopes);
            updateLiveColor (n, slopes);

            if (guardCarryLength > 0)
            {
                guardCarryElapsed += n;

                if (guardCarryElapsed >= guardCarryLength)
                    clearGuardCarry();
            }

            samplesSincePlan += n;
            referenceStableSamples += n;
            rememberHostPosition (n);

            publishStatus();
            publishState (n);
        }

        //==============================================================================
        // ---- 状态与保存 ----

        StatusPayload makeStatusPayload() const noexcept
        {
            StatusPayload p;
            auto& s = p.status;
            const auto& plan = toSlot().plan;

            s.phase = learning != Learning::none ? Phase::listening : committedPhase;
            s.profileReady = committed.hasProfile;
            s.planReady = committed.hasProfile || executionHeld;
            s.calibrationValid = calibrationApplicable();

            if (learning != Learning::none)
            {
                const auto listened = analyzer.getCandidateProfile().effectiveSeconds;
                s.listenedSeconds = (float) listened;
                s.requiredSeconds = (float) (listened < constants.minListenSeconds ? constants.minListenSeconds
                                                                                   : constants.maxListenSeconds);
            }
            else
            {
                s.listenedSeconds = 0.0f;
                s.requiredSeconds = (float) constants.minListenSeconds;
            }

            s.effectiveTargetLufs = (committed.hasProfile || executionHeld) && active && toSlot().valid && isFinite (plan.effectiveTargetLufs)
                                        ? plan.effectiveTargetLufs : std::numeric_limits<float>::quiet_NaN();
            s.protectionDb = active ? executedGuardDb : 0.0f;
            s.planLimited = active && guard.isPlanLimited();
            s.limitedByManual = active && plan.limitedByManual;
            s.calibrationPaused = paused;
            s.relearnInProgress = learning == Learning::relearn;
            // 「自动→温润」：GLAZE 0 时方案不算（不花 CPU），偏向哪种直接由风格与锁定的权重定（与方案里的权重同一个 weightsFor）。
            s.resolvedStyle = dominantStyle (active && toSlot().valid ? plan.weights
                                                                      : weightsFor (currentStyle, committed.hasProfile ? committed.lockedWeights
                                                                                                                       : StyleWeights {}));
            s.unlearnedOfflineWarning = unlearnedOfflineWarning && ! committed.hasProfile;
            s.profileMismatch = active && profileMismatch && committed.hasProfile;
            s.waitingForPlayback = ! hostPlaying();
            s.savedPlanFromOlderAlgorithm = savedPlanMode;

            // 修坯 Clay（ClayIndicator 与抽屉的实际值小圆点）：上一块交给 ClayModule 的实际值（旁通与否由界面自己读参数）；
            // 保护灯只在该项为「自动」态时报告（「关 / 手动」不受保护改动）；「点重新聆听后启用自动修坯」：GLAZE > 0、至少一项
            // 自动、已提交画像是格式 1（缺 Clay 所需特征，只有重新聆听能补上）。
            const auto& clayState = clayGuard.getState();
            s.clayLowCutHz = effective.clay.lowCutHz;
            s.clayMonoHz = effective.clay.monoHz;
            s.clayMonoAmount = effective.clay.monoAmount;
            s.clayWidthDb = effective.clay.widthDb;
            s.clayPhaseGuardActive = active && manual.claySettings.monoMode == ClayMode::automatic && clayState.phaseActive;
            s.clayWidthGuardActive = active && manual.claySettings.widthMode == ClayMode::automatic && clayState.widthActive;
            s.clayNeedsRelearn = active && anyAutomaticClay (manual.claySettings) && committed.hasProfile
                              && committed.profile.formatVersion < clayProfileFormatVersion;
            // ADR-012「先试后用」：此刻生效方案（含过渡混合）里因代价被减量 / 否决的项，只报「自动」态的、只在 GLAZE > 0 时报
            // （方案源不支持自动 Clay 时 clayPlanned 是中性值，mask 为 0）。
            s.clayCostLimitedMask = active ? reportedCostLimitedMask (clayPlanned.costLimitedMask, manual.claySettings)
                                           : static_cast<std::uint8_t> (0);

            // T-038 House 档（GLAZE 卡片状态行与 CLAY 卡片读；通用档保持默认）。计数是段落账此刻的门控结果（锁定之后也照常更新）；
            // houseLockPending = House 档正在聆听（暂定方案或候选在建）或锁定后待重开。
            if (isHouse (currentGenre))
            {
                const auto& ledger = analyzer.getSegmentLedger();
                const auto& g = ledger.getSummary();
                const auto& h = constants.house;
                s.houseActive = true;
                s.houseSegmentMode = ledger.getMode();
                s.houseHeardSegments = g.heardSegments;
                s.houseTargetSegments = h.lockSegments;
                s.houseFullSegments = g.fullSegments;
                s.houseRequiredFullSegments = h.requiredFullSegments;
                s.houseHeardSeconds = (float) g.heardSeconds;
                s.houseTargetSeconds = (float) h.fallbackLockSeconds;
                s.houseMainSectionHeard = s.houseSegmentMode == SegmentMode::seconds ? g.fullSeconds >= h.fallbackFullSeconds
                                                                                     : g.fullSegments >= h.requiredFullSegments;
                s.houseLockPending = learning != Learning::none || houseReopenArmed;
                s.houseLedgerFull = ledger.isStoreFull();
                s.housePhaseHint = houseHint.active;
                s.housePhaseMonoLossDb = houseHint.active ? houseHint.monoLossDb : 0.0f;
            }

            p.committedPhase = committedPhase;
            p.minListenSeconds = (float) constants.minListenSeconds;
            p.offline = offline;
            return p;
        }

        void publishStatus() noexcept
        {
            statusSnapshot.write (makeStatusPayload());
        }

        void publishState (std::int64_t n) noexcept
        {
            samplesSincePublish += n;

            // 每块发布执行快照，保存不落后 100 ms；仅 try-lock 和固定大小拷贝，不分配或等待。
            if (! stateLock.tryEnter())
                return;   // 拿不到锁（消息线程正在读 / 写）：下块再发，不等

            // 消息线程刚读入一份存档、音频线程还没接到（restorePending 在锁里置位）：published 已经是那份存档，
            // 这里若把本块的 committed（读取之前的版本）写回去，读取之后的 getStateInformation 就会存出上一个工程。
            // 不发布、脏标志留着；下一块 consumeRestore 接到存档后 committed 就是它。
            if (restorePending.load (std::memory_order_acquire))
            {
                stateLock.exit();
                return;
            }

            published.committed = committed;
            published.feedbackMaxGainDb = feedbackMaxGainDb;
            published.clayPressureScale = clayPressureScale;
            published.execution = lastExecution;
            // 本块末尾可能刚提交画像/安全上限：执行量仍是实际听到的，参考绑定本次保存的正式状态。
            published.execution.reference = makeReference (manual);
            published.execution.provisional = ! committed.hasProfile;
            published.execution.requiresCalibration = executionRequiresCalibration;
            stateLock.exit();

            stateDirtyUrgent = stateDirtyLazy = false;
            samplesSincePublish = 0;
        }

        // 读取工程：已提交版本立即生效（不重学、不过渡），运行状态按统一规则初始化。返回 true = 这一块接到了一份存档
        //（方案源在 beginBlock 读到本块的风格参数之后再直接落到位：风格参数是随同一份工程读回来的，不算「换风格」）。
        bool consumeRestore() noexcept
        {
            if (! restorePending.load (std::memory_order_acquire) || ! stateLock.tryEnter())
                return false;

            // 标志在锁里清：消息线程写 pendingRestore 也在锁里、写完才置标志，所以这里拿到的至少是最新的那一份
            //（它若在我们清标志之后又写了一份，会再置标志，下一块再接一次，内容相同或更新）。
            const auto restored = pendingRestore;
            restorePending.store (false, std::memory_order_release);
            stateLock.exit();

            if (learning != Learning::none)
                stopLearning();
            analyzer.handleDiscontinuity (Discontinuity::hostReset); // 旧实例的实时频谱不能覆盖新恢复的执行量。

            // T-038 House 档：读取工程后段落账从空开始（旧画像在新画像就绪前照常生效，设计规格-曲风-House学习方式 第 5 节）；
            // 自动聆听的记号与锁定后的计数作废。只动 House 档的状态（通用档的读数不看这些）。
            analyzer.clearHouseLedger();
            houseListen = false;
            houseListenDeclined = false;
            clearHouseReopen();

            committed = restored.committed;
            feedbackMaxGainDb = isFinite (restored.feedbackMaxGainDb) ? restored.feedbackMaxGainDb : unlimitedGainDb;
            setClayScaleNow (isFinite (restored.clayPressureScale) ? juce::jlimit (0.0f, 1.0f, restored.clayPressureScale) : 1.0f);
            committedPhase = committed.hasProfile ? Phase::locked : Phase::unlearned;
            savedPlanMode = committed.hasProfile && ! isCompatibleAlgorithm (committed.plan.algorithmVersion);
            savedPlanGlazeKnown = false;
            adoptExecution (restored.execution);   // 含 Clay：保护状态与首块值按快照（没有快照 = 中性）
            clayConstantsKnown = false;            // 读回的标定快照可能不同：下一块按方案源重设 ClayGuard 常数

            if (! committed.hasProfile)
                committed.constants = constants;

            roundActive = false;
            calibrator.reset();
            haveLastAttempt = false;
            guard.reset();
            guardClean = true;
            guardWasPlanLimited = planLimitedLatched = false;
            clearGuardCarry();
            monitor.clear();
            unlearnedOfflineWarning = false;
            samplesSinceFeedback = feedbackCooldownSamples;

            snapTransitions();
            guardLimitsKnown = false;
            snapBias = true;
            stateDirtyUrgent = false;
            stateDirtyLazy = false;

            // 旧工程的标定快照里保护层的时长可能不同：按它重新折算。
            updateTiming();
            monitor.configure (sampleRate, activeConstants());

            // 画像失配的运行态属于读档前那份画像（T-032 R5-08，与 commitCandidate 换画像时同一口径）：累计的音乐不能拿来与读回的新
            // 画像比（修前播放中读档可能在第一块就跳过「持续满 1 s」的要求给新方案降上限），冷却视为已过；「建议重新聆听」的提示
            // 说的也是旧画像，一并清掉（修前播放中换读另一份工程 / 预设，旧提示一直挂着，直到下一次提交）。失配给的上限随工程保存、
            // 已随 feedbackMaxGainDb 读回，不受影响。
            clearMismatch();
            samplesSinceMismatch = mismatchCooldownSamples;
            profileMismatch = false;
            return true;
        }

        static StatusPayload statusForRestored (const SaveState& state, const Constants& constants) noexcept
        {
            StatusPayload p;
            auto& s = p.status;
            const auto& c = state.committed;

            s.phase = c.hasProfile ? Phase::locked : Phase::unlearned;
            s.profileReady = c.hasProfile;
            s.planReady = c.hasProfile || state.execution.valid;
            s.calibrationValid = c.hasProfile && c.calibration.valid
                && (! state.execution.valid || (! state.execution.requiresCalibration && state.execution.protectionDb == 0.0f
                    && sameCalibrationPlan (state.execution.plan, c.plan)));
            s.requiredSeconds = (float) constants.minListenSeconds;
            s.effectiveTargetLufs = c.hasProfile && isFinite (c.plan.effectiveTargetLufs) ? c.plan.effectiveTargetLufs
                                                                                           : std::numeric_limits<float>::quiet_NaN();
            s.limitedByManual = c.hasProfile && c.plan.limitedByManual;
            s.resolvedStyle = dominantStyle (c.plan.weights);
            s.savedPlanFromOlderAlgorithm = c.hasProfile && ! isCompatibleAlgorithm (c.plan.algorithmVersion);

            p.committedPhase = s.phase;
            p.minListenSeconds = (float) constants.minListenSeconds;
            return p;
        }

        // 已点、音频线程还没接到的「重新聆听」立刻反映在状态上（宿主停着时按钮也马上换字）。规则同 consumeRelearnRequests。
        static void overlayPendingRelearn (StatusPayload& p, int count) noexcept
        {
            // 与音频线程相同的请求语义；实时和离线均接收重新聆听。
            if (count <= 0)
                return;

            auto& s = p.status;
            const auto toggle = (count % 2) == 1;

            if (s.phase == Phase::listening && ! s.relearnInProgress)
            {
                s.listenedSeconds = 0.0f;
                s.requiredSeconds = p.minListenSeconds;
            }
            else if (s.relearnInProgress)
            {
                if (toggle)
                {
                    s.relearnInProgress = false;
                    s.phase = p.committedPhase;
                    s.listenedSeconds = 0.0f;
                }
            }
            else if (s.profileReady)
            {
                if (toggle)
                {
                    s.phase = Phase::listening;
                    s.relearnInProgress = true;
                    s.listenedSeconds = 0.0f;
                    s.requiredSeconds = p.minListenSeconds;
                }
            }
            else
            {
                s.phase = Phase::listening;
                s.listenedSeconds = 0.0f;
                s.requiredSeconds = p.minListenSeconds;
            }
        }
    };

    //==============================================================================
    GlazeController::GlazeController() : impl (std::make_unique<Impl>()) {}
    GlazeController::~GlazeController() = default;

    void GlazeController::prepare (const juce::dsp::ProcessSpec& spec)
    {
        auto& s = *impl;

        if (s.prepared && s.roundActive && s.referenceKnown)
        {
            s.heldBiasKnown = true;
            s.heldBiasDb = s.executedBiasDb;
            s.heldBiasReference = s.currentReference;
        }

        if (spec.sampleRate > 0.0 && std::isfinite (spec.sampleRate))
            s.sampleRate = spec.sampleRate;

        // 分析器只依赖采样率与声道数。两者都没变（处理器只因最大块长变了而整体重建链）：不重建它，当作一次宿主 reset
        // （丢弃跨越的窗口；两块表随链重建，它自己核对计数接上新时间轴），进行中的聆听与重新聆听的候选都保留（v0.6 验收 V2-05）。
        // 变了：按新规格整体重建（清掉候选累计：进行中的聆听作废，没学过的实例下次播放重新开始）。
        const auto channels = (int) spec.numChannels;
        bool adoptedHeard = false;   // 沿用了已听到的执行快照（Clay 保护随之按快照接着走）

        // T-038：House 档的门控 / 分段常数随本版本的标定快照（只拷贝几个数；通用档不用它们）。
        s.analyzer.setHouseConstants (s.constants.house);

        if (s.prepared && s.analyzedSampleRate == s.sampleRate && s.analyzedChannels == channels)
        {
            // prepareToPlay 会先 setParams，lastExecution 此时可能已是下一块尚未听到的量。
            // 同规格重建从 endBlock 已发布的执行量冷启动，沿用保存重开对保护/校准偏差的吸收规则。
            // 待恢复存档优先；参数已改变、GLAZE 0 或旧算法时不套用过期执行快照。
            ExecutionState heard;
            {
                const juce::SpinLock::ScopedLockType lock (s.stateLock);
                if (! s.restorePending.load (std::memory_order_acquire))
                    heard = s.published.execution;
            }

            // 刚接到存档的那一块（处理器 prepareToPlay 先交参数、后 prepare）已把编排器里的参考升成新口径，但 published 要等
            // endBlock 才刷新：这里拿到的可能还是读进来的旧口径副本，照样按此刻的手动值升格，再比（BUG-004）。消息线程，开销可忽略。
            if (heard.valid)
                upgradeLegacyConditions ({ &heard.reference }, s.manual, s.feedbackMaxGainDb);
            if (s.active && ! s.savedPlanMode && heard.valid
                && isCompatibleAlgorithm (heard.plan.algorithmVersion)
                && isCompatibleAlgorithm (heard.constants.algorithmVersion)
                && GlazeCalibrator::referenceMatches (heard.reference, s.makeReference (s.manual)))
            {
                s.adoptExecution (heard);
                s.snapTo (s.desiredSource());
                adoptedHeard = true;
            }
            s.analyzer.handleDiscontinuity (Discontinuity::hostReset);
        }
        else
        {
            s.analyzer.prepare (spec);
            s.analyzedSampleRate = s.sampleRate;
            s.analyzedChannels = channels;

            if (s.learning != Impl::Learning::none)
                s.learning = Impl::Learning::none;

            s.houseListen = false;   // T-038：分析器整体重建，段落账随之清空；下一块按此刻的曲风重新判断要不要聆听
            s.clearHouseReopen();
        }

        s.clearMismatch();

        s.calibrator.prepare (s.sampleRate, (int) spec.maximumBlockSize);
        s.calibrator.setConstants (s.activeConstants());
        s.roundActive = false;
        s.calibrationBaseBiasDb = 0.0f;

        if (s.committedPhase == Phase::calibrating)
            s.committedPhase = Phase::locked;

        s.guard.prepare (s.sampleRate);
        s.guardClean = true;
        s.guardLimitsKnown = false;
        s.guardWasPlanLimited = s.planLimitedLatched = false;
        s.clearGuardCarry();
        s.monitor.configure (s.sampleRate, s.activeConstants());

        // Clay 保护：沿用了已听到的执行快照就按它接着走（adoptExecution 已恢复状态与首块值），否则回中性（与保护层重建清零
        // 同一口径；采样率变了分析器整体重建，实时视图也从头来）。常数在下一块按方案源重设。
        if (! adoptedHeard)
            s.resetClayRuntime();

        s.clayConstantsKnown = false;

        // 两块表随链一起重建，子块序号从 0 起。
        s.inputCursor = s.outputCursor = 0;
        s.expectKnown = false;

        s.updateTiming();

        // 已提交的画像、风格权重、方案、约束反馈上限都保留；采样率变了，校准的参考条件（含采样率）不再一致 → 待校准，
        // 条件稳定后自动再校准。频带定义与采样率无关（27 段中心频率固定），画像照用。
        s.haveLastAttempt = false;
        s.snapTransitions();
        s.snapBias = true;
        s.prepared = true;
        s.publishStatus();
    }

    void GlazeController::setHostInfo (const HostInfo& info) noexcept     { impl->setHostInfo (info); }
    void GlazeController::hostReset() noexcept                            { impl->onHostReset(); }
    int GlazeController::getGuardSamplesUntilBoundary() const noexcept    { return impl->guard.getSamplesUntilSlotBoundary(); }

    void GlazeController::handleDiscontinuity (Discontinuity kind) noexcept
    {
        switch (kind)
        {
            case Discontinuity::transportJump:  impl->onTransportJump(); break;
            case Discontinuity::loopWrap:       impl->onLoopWrap(); break;
            case Discontinuity::hostReset:
            default:                            impl->onHostReset(); break;
        }
    }

    const ChainParams& GlazeController::beginBlock (const ChainParams& manual, bool hostBypassed) noexcept
    {
        return impl->beginBlock (manual, hostBypassed);
    }

    void GlazeController::analyzeInput (const juce::AudioBuffer<float>& input, const LoudnessMeter& inputMeter) noexcept
    {
        auto analysisHost = impl->host;
        if (impl->offline)
            analysisHost.isPlaying = true;
        if (analysisHost.hasPosition && impl->hostPlaying())
            analysisHost.positionSamples += impl->hostInputOffset;

        // T-038 House 档：切片的 PPQ 同样按片的偏移推过（宿主只报块起点的 PPQ），段落账的锚点检查才对得上（小节起点不动，账本自己补整小节）。
        if (isHouse (impl->currentGenre) && impl->hostInputOffset > 0 && impl->hostPlaying() && analysisHost.hasPpq
            && analysisHost.hasTempo && std::isfinite (analysisHost.bpm) && analysisHost.bpm > 0.0)
            analysisHost.ppqPosition += (double) impl->hostInputOffset * analysisHost.bpm / (60.0 * impl->sampleRate);

        impl->analyzer.setHostInfo (analysisHost);
        impl->analyzer.processInput (input, inputMeter);
        impl->hostInputOffset += input.getNumSamples();
    }

    void GlazeController::analyzeSpectrum (const BalanceModule& balance) noexcept
    {
        impl->analyzer.processSpectrum (balance);
        impl->spectrumTakenAtBlockStart = true;
    }

    void GlazeController::endBlock (const BalanceModule& balance, const LoudnessMeter& inputMeter, const LoudnessMeter& outputMeter,
                                    const ModuleStats& stats, int chainLatencySamples, bool spectrumDeferred) noexcept
    {
        impl->endBlock (balance, inputMeter, outputMeter, stats, chainLatencySamples, spectrumDeferred);
    }

    void GlazeController::requestRelearn() noexcept
    {
        impl->pendingRelearn.fetch_add (1, std::memory_order_acq_rel);
    }

    std::unique_ptr<juce::XmlElement> GlazeController::createStateXml() const
    {
        SaveState copy;

        {
            const juce::SpinLock::ScopedLockType lock (impl->stateLock);
            copy = impl->published;
        }

        auto xml = toXml (copy.committed);

        // 编排器自己的一项：约束反馈上限（设计决定 1：随已提交版本保存，重开时照用）。挂在 GLAZE_STATE 下的独立子节点，
        // fromXml(CommittedState) 只认它自己的四个子节点，不受影响；旧工程没有这个节点 = 不限。
        auto* controller = xml->createNewChildElement (controllerTag);
        controller->setAttribute ("formatVersion", controllerFormatVersion);
        controller->setAttribute ("feedbackMaxGainDb", floatToHex (copy.feedbackMaxGainDb));
        controller->setAttribute ("clayPressureScale", floatToHex (copy.clayPressureScale));   // ADR-014；可缺省 = 1
        if (copy.execution.valid)
            xml->addChildElement (toXml (copy.execution).release());
        return xml;
    }

    void GlazeController::restoreStateXml (const juce::XmlElement* xml)
    {
        SaveState parsed;
        parsed.committed.constants = impl->constants;
        bool ok = false;

        if (xml != nullptr)
        {
            CommittedState state;
            state.constants = impl->constants;

            if (fromXml (*xml, state))
            {
                parsed.committed = state;
                ok = true;
                if (const auto* execution = xml->getChildByName (executionTag))
                {
                    ExecutionState candidate;
                    if (fromXml (*execution, candidate) && candidate.valid
                        && isCompatibleAlgorithm (candidate.plan.algorithmVersion)
                        && isCompatibleAlgorithm (candidate.constants.algorithmVersion)
                        && candidate.reference.profileId == (state.hasProfile ? state.profileId : 0u)
                        && candidate.provisional == ! state.hasProfile)
                        parsed.execution = candidate;
                }

                if (const auto* controller = xml->getChildByName (controllerTag))
                {
                    float maxGain = unlimitedGainDb;

                    if (controller->getIntAttribute ("formatVersion", 0) == controllerFormatVersion
                        && hexToFloat (controller->getStringAttribute ("feedbackMaxGainDb"), maxGain) && std::isfinite (maxGain))
                        parsed.feedbackMaxGainDb = maxGain;

                    // 修坯压力反馈的缩放（ADR-014，v0.1.7.0 加的可缺省属性）：缺了、坏了、超出 0 ～ 1 → 1（不限）。
                    float clayScale = 1.0f;

                    if (controller->getIntAttribute ("formatVersion", 0) == controllerFormatVersion
                        && controller->hasAttribute ("clayPressureScale")
                        && hexToFloat (controller->getStringAttribute ("clayPressureScale"), clayScale)
                        && std::isfinite (clayScale) && clayScale >= 0.0f && clayScale <= 1.0f)
                        parsed.clayPressureScale = clayScale;
                }
            }
        }

        if (! ok)
        {
            parsed = SaveState {};                          // 读取失败 = 未学习
            parsed.committed.constants = impl->constants;
        }

        else if (parsed.committed.hasProfile
                 && parsed.committed.constants.algorithmVersion < heatCurveMigrationAlgorithmVersion)
        {
            // 只迁移旧 Heat 模型相关的已算方案和校准，保留歌曲画像/id及 APVTS 手动旋钮。
            // 下一音频块用当前手动参数重算；不能把旧曲线的大额损失补偿直接送到新曲线。
            auto& state = parsed.committed;
            state.constants = impl->constants;
            state.lockedWeights = resolveAutomaticWeights (state.profile, state.constants);
            state.plan = Plan {};
            state.plan.algorithmVersion = currentAlgorithmVersion;
            state.calibration = Calibration {};
            parsed.feedbackMaxGainDb = unlimitedGainDb;
            parsed.clayPressureScale = 1.0f;
            parsed.execution = ExecutionState {};
        }

        // 快照先写（getStatus 看到 restorePending 时它已经是这一份）；published / pendingRestore 与 restorePending 在同一把锁里写，
        // 音频线程的 publishState 在锁里看到 restorePending 就不会把读取之前的版本写回 published。
        impl->restoredSnapshot.write (Impl::statusForRestored (parsed, impl->constants));

        {
            const juce::SpinLock::ScopedLockType lock (impl->stateLock);
            impl->published = parsed;
            impl->pendingRestore = parsed;
            impl->restorePending.store (true, std::memory_order_release);
        }
    }

    Status GlazeController::getStatus() const noexcept
    {
        StatusPayload payload;
        auto haveSnapshot = false;

        if (impl->restorePending.load (std::memory_order_acquire))
            haveSnapshot = impl->restoredSnapshot.read (payload);

        if (! haveSnapshot)
            impl->statusSnapshot.read (payload);   // 读不到一致的一份（实际碰不到）时是最后一次读到的内容

        Impl::overlayPendingRelearn (payload, impl->pendingRelearn.load (std::memory_order_acquire));
        return payload.status;
    }

    void GlazeController::setConstantsForTesting (const Constants& constants)
    {
        auto& s = *impl;
        s.constants = constants;
        s.analyzer.setHouseConstants (constants.house);

        if (! s.committed.hasProfile)
            s.committed.constants = constants;

        s.calibrator.setConstants (s.activeConstants());
        s.monitor.configure (s.sampleRate, s.activeConstants());
        s.guardLimitsKnown = false;
        s.clayConstantsKnown = false;
        s.updateTiming();
        s.snapTo (s.desiredSource());
        s.publishStatus();
    }

    CommittedState GlazeController::getCommittedStateForTesting() const   { return impl->committed; }

    Phase GlazeController::getPhaseForTesting() const noexcept
    {
        return impl->learning != Impl::Learning::none ? Phase::listening : impl->committedPhase;
    }

    GlazeController::TestProbe GlazeController::getProbeForTesting() const noexcept
    {
        const auto& s = *impl;
        auto p = s.probe;
        p.transitionActive = s.transitionActive;
        p.transitionProgress = s.transitionActive && s.transitionLength > 0
                                   ? (float) juce::jlimit (0.0, 1.0, (double) s.transitionElapsed / (double) s.transitionLength)
                                   : 1.0f;
        p.learning = s.learning != Impl::Learning::none;
        p.relearning = s.learning == Impl::Learning::relearn;
        p.calibratorState = (int) s.calibrator.getState();
        p.appliedBiasDb = s.appliedBiasDb;
        p.executedBiasDb = s.executedBiasDb;
        p.executedGuardDb = s.executedGuardDb;
        p.guardDb = s.guard.getProtectionDb();
        p.feedbackMaxGainDb = s.feedbackMaxGainDb;
        p.clayPressureScale = s.clayPressureScale;
        p.clayPressureSmoothed = s.clayPressure.smoothed;
        p.savedPlanMode = s.savedPlanMode;
        p.executionSnapshotActive = s.executionHeld;
        p.liveHeatReduction = s.liveHeatReduction;
        p.liveGlueReduction = s.liveGlueReduction;
        p.offline = s.offline;
        p.increments = s.increments;
        p.targetPlan = s.toSlot().plan;
        p.clayPlanned = s.clayPlanned;
        p.liveClay = s.liveClay;
        p.clayGuard = s.clayGuard.getState();
        p.clayGuardSeconds = s.lastClaySeconds;
        p.clayAutomaticSupported = s.claySupported;
        p.clayRestoredFirstBlock = s.clayRestoredLastBlock;
        p.manualParamsHash = s.makeReference (s.manual).manualParamsHash;
        p.liveLowCorrelation = s.analyzer.getShortTermView().liveLowCorrelation;
        p.liveLowCorrelationFast = s.analyzer.getShortTermView().liveLowCorrelationFast;

        p.roundActive = s.roundActive;
        p.calibrationApplicable = s.calibrationApplicable();
        const auto progress = s.calibrator.getProgress();
        p.calibrationRoundsDone = progress.round;
        p.calibrationSubWindows = progress.completedSubWindows;
        p.calibrationSubWindowSeconds = progress.currentSubWindowSeconds;
        p.calibrationGuardSeconds = progress.guardSeconds;
        p.calibrationLastErrorDb = progress.lastRoundErrorDb;
        p.calibrationSuggestedDb = s.calibrator.getResult().gainBiasDb;

        p.guardLimitsSet = s.guard.hasLimits();
        p.guardLimits = s.guard.getLimits();
        p.guardSteadySamples = s.guardSteadySamples;
        p.guardStuckSamples = s.guardStuckSamples;
        p.samplesSinceFeedback = s.samplesSinceFeedback;
        p.monitorWindowSlots = s.monitor.getWindowSlots();
        p.monitorTriggerSlots = s.monitor.getTriggerSlots();
        p.absorbedGuardDb = s.absorbedGuardDb;
        p.guardCarryDb = s.guardCarryDb();
        p.mismatchSamples = s.mismatchSamples;

        p.houseReopenRun = s.houseReopenRun;
        p.houseReopenArmed = s.houseReopenArmed;
        p.transitionLengthSamples = s.transitionActive ? s.transitionLength : 0;
        return p;
    }

    //==============================================================================
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
