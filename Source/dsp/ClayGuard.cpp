/*
  ==============================================================================

    ClayGuard.cpp
    修坯 Clay 的实时保护与三态合成（设计规格-Clay-分析决策与验收 第 3 节、ADR-009；
    04-协调/2026-09-26-v0.1.7.0-修坯实现计划 A2）。

    本文件的口径与理由：
      - 状态只有 ClayGuardState 这一份（float，与执行快照 ExecutionState::clayGuard 里存的逐位相同）：每块把它读成 double
        推进、末尾取回 float。所以「某块结束时存快照 → 恢复 → 继续」与一路跑下去逐位相同（反相条件正在资格期里时例外：资格期
        计时不存档，见下）；代价是不同块长的轨迹只在
        float 舍入内一致（每块 ≤ 半个 ulp；ClayGuardTests 在 17 ～ 8192 样本块上按 2e-3 的容差核对，估算的最坏累计 < 7e-4）。
        这与编排器里 liveToneOffsetDb 等实时量「块量化、按容差比较」的口径相同（tools/perf rendercheck 换块长只比容差）。
      - 时间一律按调用方给的有效音频秒数（与 GlazeGuard 同口径；静音 / 宿主旁通 / 停播块传 0）。一块之内实时视图不变
        （分析器每个 Balance hop 才更新一次），下面的运动都是「朝一个固定目标的线性移动 + 到点钳住」，同一视图下按任意
        块长切分都得到同一条轨迹（除舍入）。

      - 反相提示（ADR-012 的反相保护；ADR-015 起只提示、不改声音）：
          条件 = 慢路 ρ < clayGuardPhaseCorrelation（出厂 −0.2），或（慢路 ρ < clayGuardPhaseFastGate（出厂 0.1）且快路 ρ <
          clayGuardPhaseCorrelation）。为什么这样：40 ～ 125 Hz 在 2048 点 FFT 里只有 4 ～ 5 个 bin，快路（120 ms）的估计散布约
          ±0.5，宽立体声低音常被它判成反相（审核 V1-01：60 ms 快路与慢路取小时，长期 ρ 0.87 的底鼓 + 宽垫 30 s 触发 6 次；真歌
          kskdhjf 75 %、KINOSEI 57 % 的时间在保护）。慢路（300 ms）稳，但整体反相时过零慢；所以快路只在慢路已经明显离开同相
          （< 0.1；原 0.3 在慢路 0.1 ～ 0.3 的宽低音上仍会误触发，ADR-014）时才参与加速。阈值 −0.2 对应单声道回放低音损失
          10·log10((1 + ρ) / 2) < −4 dB，是真破坏，保护不看代价。
          条件须连续成立 clayGuardPhaseMinSeconds（出厂 0.15 s）才算触发：底鼓与宽垫之间、音头之类的瞬态在快路上一闪而过，
          不该让低频宽度来回跳（「低频呼吸」）。资格期里按「未触发」走（保持期照耗、释放照走），满资格的那一刻起按「已触发」走；
          同一块里跨过满资格的时刻时，块内时间按先后拆开（与保持期末尾同一种块长无关的做法）；满资格的判断带 1 µs 容差、满资格即装
          保持期，消掉 double 累加在恰好满资格的块末翻转造成的 2 s 保持期差（阶段 4 服务器测试暴露）。条件一断（本块不成立）资格期清零；
          反相一路冻结（慢路无效、seconds ≤ 0）时资格期计时也冻结——「连续」按有效音频算，静音不算打断也不算成立。
          快路无效（它自己的门下、刚重启）只是「没有加速证据」：按慢路那一支判定，不冻结。
          触发后：phaseFloor 以 1 / attack 每秒线性升向 1（出厂 0.1 s 从 0 到 1），保持期重置为 hold（出厂 2 s）；
          未触发：先耗保持期，剩下的时间再以 1 / release 每秒线性降向 0（出厂 2 s 从 1 到 0）——同一块里跨过保持期末尾时
          只有剩下的那部分时间用于释放，这是块长无关的关键。
          phaseFloor 现在只是「提示强度」：phaseActive = phaseFloor > 0，界面「收单」灯琥珀 +「低频反相」提示。输出 monoAmount 原样
          （ADR-015，2026-09-27 Max 试听「不自然、有点假」）：收单只去掉 S，单声道回放听的是 M，收不收单都一样；原来的「反相时收满」
          救不了单声道听众，只让立体声听众在反相段丢低音（真歌 will we be okay −1 ～ −2.5 dB、l_kskdhjf 最多 −7.6 dB，单声道几乎不变）。
          从条件成立到提示亮起至少 phaseMinSeconds（出厂 0.15 s）；整链从反相音频出现起还要加分析窗、平滑过零与块粒度
          （ClayGlazeTests 按推导的上限核对）。

      - 宽度保护：允许的最大加宽 w_allow = 10·log10(k · P_M / P_S)，k = (1 − ρ_min) / (1 + ρ_min)（ρ_min 0.25 → k = 0.6），
          由「加宽后预测高频相关度 ρ' = (P_M − γ²P_S) / (P_M + γ²P_S) ≥ ρ_min」解出（γ² ≤ k · P_M / P_S）。
          下限 0：不加宽时相关度已经低于下限的歌，只是不加宽——保护从不自动收窄（负宽度只能手动，规格第 4 节）。
          P_S = 0（高频纯单声道，加宽不改变任何东西）→ 不限（clayWidthUncappedDb）。
          运动选「每秒 dB 速率」，不用一阶平滑：一阶平滑从「不限」（100 dB 的哨兵值）出发要很久才降到方案值附近，
          而且永远只是逼近；线性速率有确定的完成时间，验收「攻击 200 ms、释放 2 s」也直接。
          速率按此刻的方案加宽量 P = max(0, 方案 widthDb) 定：收回 P / attack dB/s、放开 P / release dB/s——
          整段方案加宽在 attack（出厂 0.2 s）内收完、在 release（出厂 2 s）内放回，与方案大小无关。
          上限高于 P 的部分对输出不可见（输出 = min(方案值, 上限)），所以运动只在 [0, P] 里按速率走：
            下降时从 min(上限, P) 起步（不会先花时间从 100 dB 往下爬，保护一触发输出就开始收）；
            回升一旦到达 P 就直接置成 w_allow——此后上限不限制方案，状态回到「上限 = 允许值」的静止形态，
            不留一个会在方案变大时误拦的中间值；w_allow 本身就是物理上允许的最大值，方案变大越过它时照拦不误。
          输出 widthDb = min(方案值, 上限)；方案值 ≤ 0 时原样不动（上限 ≥ 0）。

      - 冻结：seconds ≤ 0 或非有限 → 整体不动。实时视图的某一路无效 → 只冻那一路（保持期、资格期计时也不走），另一路照常：
            反相一路：慢路 liveLowCorrelation.valid = false 或数值非有限（门上没有低频内容、静音——分析器在帧门限下把两路都置
            无效，审核 V1-03 的「视图冻结与保护时钟」由此对齐）；
            宽度一路：liveHighValid = false，或 P_M / P_S 非有限、为负。
          理由：「不知道」时不改（GLAZE 通则）。例如间奏里没有低频，反相保护停在原处；低频回来时若还是反相，不必再经历一次攻击。
      - active = false（GLAZE ≤ 0）：状态直接回 ClayGuardState{}、资格期计时清零，输出 = 输入（原样返回 planned）。
        ⚠ 旧工程（画像格式 1 / 旧算法方案，没有立体声特征）保护也必须不工作（active = false，或 reset 且不调 update）——
        由编排器保证（GlazeController::updateClay 在方案源不支持 Clay 时 reset、自动值中性）。ADR-015 起反相一路不改声音，旧工程
        遇到反相低频时声音也不会变；宽度保护只会收回方案的加宽，旧方案加宽为 0，不受影响。
      - 资格期计时不存档（ClayGuardState 已定、随执行快照保存）：setState / reset 后从 0 起算。恢复后若反相还在继续，只是晚最多
        phaseMinSeconds 才重新把保持期拉满；下限与剩余保持期照快照接着走，输出不变。
      - T-038 House 档反相提示（updateHouseHint，设计规格-曲风-House学习方式 第 9 节）：与上面的反相保护分开的一条更严的提示，
          只提示、不改声音，不进 ClayGuardState、不存档。
          亮：eligible（编排器判定此刻在 House 档的满编段里）且慢路 ρ 有效、< houseHintCorrelation（出厂 0.6：左右等功率时单声道
          低频损失 10·log10((1 + ρ) / 2) 约 1 dB 起），连续满 requiredSeconds（编排器按 4 小节换成秒、拿不到拍点时 8 s）。只按有效音频
          秒数累加、满资格的判断带与反相保护相同的 1 µs 容差，所以与块长无关（只差「在哪一块的末尾看到」）。
          保持（照反相保护的写法）：亮着时条件不成立，先保持 phaseHoldSeconds（与反相保护同一个常数，出厂 2 s）再熄灭；保持期里条件
          回来就重新装满。慢路 ρ 在 0.6 上下抖动时提示不闪，熄灭后要重新连续满 requiredSeconds 才再亮。
          熄灭：保持期用完，或不 eligible（离开满编段、曲风不是 House）→ 立即熄灭、计时清零。
          冻结：seconds ≤ 0 / 非有限（静音、旁通、停播）或慢路无效（门上没有低频内容）→ 状态不动（同反相保护：「不知道」时不改）。
          单声道损失 monoLossDb：亮起那一刻 = 10·log10((1 + ρ) / 2)；之后条件成立的块按 1 s 一阶平滑跟随当时的 ρ（界面显示到 0.1 dB，
          平滑后不随相关度的小抖动来回跳；按秒数指数衰减，块长无关到舍入）；保持期里不更新（那时 ρ 已在门限之上，不代表问题段）。
          ρ 先钳到 [−0.99, 1]，完全反相时写约 −23 dB 封顶、不出 −∞。
          houseHintPendingSeconds 两种状态各一种含义：没亮时 = 条件已连续成立的秒数（资格期）；亮着时 = 剩余保持秒数。
      - 方案值非有限（不该出现）按中性处理（monoAmount 0、widthDb 0）：保护照常工作，输出也是有限值；lowCutHz、monoHz 原样透传。
      - 标志是推导量，冻结块里也按当前状态重算：phaseActive = phaseFloor > 0（反相提示，ADR-015；与方案值无关）；
        widthActive = widthCapDb < 方案值（宽度保护真在改输出时才亮）。界面标志要再与三态相与——这一步归编排器（写 Status 时）。

    resolveClayParams（纯函数）：规则见 ClayGuard.h 文件头。补充两点：
      - 合成后收单比例为 0（「自动」且 GLAZE ≤ 0（NaN 也算）、「关」，或自动方案的比例本身为 0）时，monoHz 一律取中性默认
        （ClayModule::Params{}.monoHz = 80 Hz，即 ClayPlan「monoAmount = 0 时 monoHz 保持默认 80」的约定），不取自动方案的
        monoHz：某一项不处理时，交给 ClayModule 的该项参数就是固定的中性常数——不会因为方案过渡里一个用不上的 monoHz 在变
        （或旧标定快照缺字段时决策器给出的 0 Hz 被钳成 40 Hz）而让模块离开「逐位直通」的静止状态
        （设计规格-Clay 第 4 节「逐位直通」、验收第 1 条：GLAZE 0 且抽屉默认 / 三项都关 / 旧工程未重新聆听）。
      - 钳制：非有限值按中性；低切 ≤ 0 = 关（保持 0），否则钳到 10 ～ 60 Hz；其余钳到 ClayModule 的 min / max
        （模块自己也钳；这里钳是为了让 Status、执行快照与抽屉的实际值小圆点看到的就是模块真正用的值）。

  ==============================================================================
*/

#include "ClayGuard.h"

#include <cmath>

namespace celadon::glaze
{
    namespace
    {
        // 时长常数与单块时长的防呆上限（约 11.6 天）：比任何真实设置都长，只防坏快照里的天文数字。
        constexpr double maxSeconds = 1.0e6;

        // 满资格的判断容差（远小于 192 kHz 的一个样本 5.2 µs）：反相保护与 House 提示共用（理由见 advancePhase）。
        constexpr double qualificationToleranceSeconds = 1.0e-6;

        // T-038 House 反相提示里单声道损失的显示平滑（见文件头）。只影响提示里写的数，不影响亮灭、不影响声音。
        constexpr double houseHintLossSmoothingSeconds = 1.0;

        // 左右等功率时单声道低频的功率损失：10·log10((1 + ρ) / 2)（dB，≤ 0）。ρ 钳到 [−0.99, 1]：完全反相时约 −23 dB 封顶。
        float houseMonoLossDb (float correlation) noexcept
        {
            const auto rho = juce::jlimit (-0.99, 1.0, static_cast<double> (correlation));
            return static_cast<float> (10.0 * std::log10 ((1.0 + rho) * 0.5));
        }

        float finiteOr (float value, float fallback) noexcept
        {
            return std::isfinite (value) ? value : fallback;
        }

        // 时长：非有限按 0 读（与旧快照缺字段同口径），钳到 [0, maxSeconds]。
        double cleanSeconds (double seconds) noexcept
        {
            return std::isfinite (seconds) ? juce::jlimit (0.0, maxSeconds, seconds) : 0.0;
        }
    }

    //==============================================================================
    ClayGuard::Tuning ClayGuard::makeTuning (const Constants& constants) noexcept
    {
        // 非有限的值一律按 0 读：与旧快照缺字段（GlazeConstants.h「旧快照缺这些字段按 0 读」）同一口径，
        // 时长 0 = 立即（攻击 / 释放一块到位、没有保持期），都不会产生非有限的输出。ρ_min 例外：它与决策器共用
        // clayWidthPowerScale（非有限 → 不许加宽，≤ −1 → 不限），不按 0 读。
        // ADR-012 新加的快路门与最短持续缺字段时同样读成 0 / 0（阶段 4 之前存的开发期快照；v0.1.7.0 未发布，已发布的 v0.1.6.2
        // 存档的 Clay 本来就中性）：门 0 = 快路只在慢路已低于 0 时参与，最短持续 0 = 条件一成立就攻击——比出厂更敏感、但不失控，
        // 重新聆听后换成出厂快照。
        Tuning result;
        result.phaseCorrelation    = juce::jlimit (-1.0f, 1.0f, finiteOr (constants.clayGuardPhaseCorrelation, 0.0f));
        result.phaseFastGate       = juce::jlimit (-1.0f, 1.0f, finiteOr (constants.clayGuardPhaseFastGate, 0.0f));
        result.phaseMinSeconds     = cleanSeconds (constants.clayGuardPhaseMinSeconds);
        result.phaseAttackSeconds  = cleanSeconds (constants.clayGuardPhaseAttackSeconds);
        result.phaseHoldSeconds    = cleanSeconds (constants.clayGuardPhaseHoldSeconds);
        result.phaseReleaseSeconds = cleanSeconds (constants.clayGuardPhaseReleaseSeconds);
        result.widthAttackSeconds  = cleanSeconds (constants.clayGuardWidthAttackSeconds);
        result.widthReleaseSeconds = cleanSeconds (constants.clayGuardWidthReleaseSeconds);

        // k 与决策器同一处（GlazeConstants.h clayWidthPowerScale）：ρ_min 非有限或 ≥ 1 → 0（不许加宽），≤ −1 → +∞（不限）。
        result.widthPowerScale = clayWidthPowerScale (constants.clayMinCorrelationAfterWidth);
        return result;
    }

    void ClayGuard::setConstants (const Constants& constants) noexcept
    {
        // 只换常数、不动状态：剩余保持期、资格期计时、当前下限与上限照旧，按新的速率与门槛继续走
        //（新的最短持续时间比已累计的短 → 下一块就算满资格）。
        tuning = makeTuning (constants);
        houseHintCorrelation = std::isfinite (constants.house.phaseHintCorrelation)
                                   ? juce::jlimit (-1.0f, 1.0f, constants.house.phaseHintCorrelation) : 0.6f;
    }

    void ClayGuard::reset() noexcept
    {
        state = ClayGuardState{};
        phasePendingSeconds = 0.0;
        houseHint = HouseHint{};
        houseHintPendingSeconds = 0.0;
    }

    // T-038 House 档反相提示（设计规格-曲风-House学习方式 第 9 节）：口径见文件头。
    void ClayGuard::updateHouseHint (const ShortTermView& view, double seconds, bool eligible, double requiredSeconds) noexcept
    {
        // 不在 House 档的满编段里：熄灭、计时清零（再亮要重新连续满 requiredSeconds）。
        if (! eligible)
        {
            houseHint = HouseHint{};
            houseHintPendingSeconds = 0.0;
            return;
        }

        // 冻结：静音 / 旁通 / 停播块，或慢路不知道（门上没有低频内容）。
        if (! (seconds > 0.0 && std::isfinite (seconds)))
            return;

        const auto& slow = view.liveLowCorrelation;

        if (! (slow.valid && std::isfinite (slow.value)))
            return;

        const auto dt = juce::jmin (seconds, maxSeconds);
        const auto holds = slow.value < houseHintCorrelation;

        if (! houseHint.active)
        {
            // 资格期：须连续成立，一断就从头计。
            if (! holds)
            {
                houseHintPendingSeconds = 0.0;
                return;
            }

            houseHintPendingSeconds = juce::jmin (houseHintPendingSeconds + dt, maxSeconds);

            if (! (cleanSeconds (requiredSeconds) - houseHintPendingSeconds > qualificationToleranceSeconds))
            {
                houseHint.active = true;
                houseHint.monoLossDb = houseMonoLossDb (slow.value);
                houseHintPendingSeconds = tuning.phaseHoldSeconds;   // 亮着时改记剩余保持秒数
            }

            return;
        }

        if (holds)
        {
            // 条件还在：重新装满保持期，损失按 1 s 平滑跟随此刻的 ρ。
            houseHintPendingSeconds = tuning.phaseHoldSeconds;
            const auto target = static_cast<double> (houseMonoLossDb (slow.value));
            const auto current = static_cast<double> (houseHint.monoLossDb);
            houseHint.monoLossDb = static_cast<float> (target + (current - target) * std::exp (-dt / houseHintLossSmoothingSeconds));
            return;
        }

        // 条件不成立：耗保持期，用完就熄灭（保持期为 0 = 立即熄灭，旧快照缺字段时如此）。用完的判断带同样的 1 µs 容差：
        // 秒数按块累加，恰好用完的那一块在不同块长下会剩 ±1e-16 s，不带容差就差一块。
        houseHintPendingSeconds -= dt;

        if (! (houseHintPendingSeconds > qualificationToleranceSeconds))
        {
            houseHint = HouseHint{};
            houseHintPendingSeconds = 0.0;
        }
    }

    void ClayGuard::setState (const ClayGuardState& restored) noexcept
    {
        // 执行快照来自文件，按坏数据对待：NaN 回中性，其余钳到合法范围（±∞ 由钳制落到边界）。
        ClayGuardState clean;
        clean.phaseFloor       = std::isnan (restored.phaseFloor) ? 0.0f : juce::jlimit (0.0f, 1.0f, restored.phaseFloor);
        clean.phaseHoldSeconds = static_cast<float> (cleanSeconds (static_cast<double> (restored.phaseHoldSeconds)));
        clean.widthCapDb       = std::isnan (restored.widthCapDb) ? clayWidthUncappedDb
                                                                  : juce::jlimit (0.0f, clayWidthUncappedDb, restored.widthCapDb);

        // 两个标志是推导量（下一块 update 按方案值重算）；这里只去掉与数值矛盾的：
        // 没有下限 → 反相保护不可能在介入（ADR-014：标志 = 下限高于方案值）；上限不限 → 宽度保护不可能在介入。
        clean.phaseActive = restored.phaseActive && clean.phaseFloor > 0.0f;
        clean.widthActive = restored.widthActive && clean.widthCapDb < clayWidthUncappedDb;
        state = clean;

        // 资格期计时不在快照里：恢复后从 0 起算（保存重开与沿用执行量的宿主 reset 因此同一起点，见文件头）。
        phasePendingSeconds = 0.0;
    }

    //==============================================================================
    ClayPlan ClayGuard::update (const ClayPlan& planned, const ShortTermView& view, double seconds, bool active) noexcept
    {
        if (! active)
        {
            state = ClayGuardState{};
            phasePendingSeconds = 0.0;
            return planned;
        }

        const auto plannedMono  = finiteOr (planned.monoAmount, 0.0f);
        const auto plannedWidth = finiteOr (planned.widthDb, 0.0f);

        // seconds ≤ 0 / 非有限：整体冻结（静音、宿主旁通、停播块），只按当前状态修改方案值。
        if (seconds > 0.0 && std::isfinite (seconds))
        {
            const auto dt = juce::jmin (seconds, maxSeconds);

            // 反相一路：慢路低频相关度无效（门上没有低频内容、静音）→ 这一路冻结（保持期、资格期计时也不走）。
            const auto phaseInput = phaseInputOf (view);

            if (phaseInput != PhaseInput::unknown)
                advancePhase (phaseInput == PhaseInput::inverted, dt);

            // 宽度一路：高频两项无效 → 这一路冻结。
            const auto highUsable = view.liveHighValid
                                 && std::isfinite (view.liveHighMidPower) && view.liveHighMidPower >= 0.0f
                                 && std::isfinite (view.liveHighSidePower) && view.liveHighSidePower >= 0.0f;

            if (highUsable)
                advanceWidth (allowedWidthDb (view), plannedWidth, dt);
        }

        state.phaseActive = state.phaseFloor > 0.0f;   // ADR-015：反相提示（不再改收单）
        state.widthActive = state.widthCapDb < plannedWidth;

        // 只可能「减小 widthDb」；lowCutHz、monoHz、monoAmount、costLimitedMask 原样（ADR-015：反相只提示，不改收单）。
        auto guarded = planned;
        guarded.monoAmount = plannedMono;
        guarded.widthDb    = juce::jmin (plannedWidth, state.widthCapDb);
        return guarded;
    }

    ClayGuard::PhaseInput ClayGuard::phaseInputOf (const ShortTermView& view) const noexcept
    {
        const auto& slow = view.liveLowCorrelation;

        // 慢路不知道（门上没有低频内容、静音、刚重启还没平滑满）→ 这一路冻结。
        if (! (slow.valid && std::isfinite (slow.value)))
            return PhaseInput::unknown;

        if (slow.value < tuning.phaseCorrelation)
            return PhaseInput::inverted;

        // 快路只在慢路已明显离开同相（< 门）时参与加速；快路无效 = 没有加速证据，按慢路那一支判定（不冻结）。
        const auto& fast = view.liveLowCorrelationFast;

        if (slow.value < tuning.phaseFastGate && fast.valid && std::isfinite (fast.value) && fast.value < tuning.phaseCorrelation)
            return PhaseInput::inverted;

        return PhaseInput::clear;
    }

    void ClayGuard::advancePhase (bool conditionHolds, double seconds) noexcept
    {
        // 本块的时间按先后分两段：先是「未触发」的（条件不成立，或还在资格期里），再是「已触发」的（条件已连续成立满
        // phaseMinSeconds）。一块之内视图不变，所以最多只在满资格的那一刻切开一次——与块长无关（见文件头）。
        auto untriggeredSeconds = seconds;
        auto triggeredSeconds = 0.0;
        auto qualified = false;   // 本块结束时条件已连续成立满资格期（= 已触发，哪怕本块里触发后的时间为 0）

        if (conditionHolds)
        {
            // 满资格的判断带 1 µs 容差（qualificationToleranceSeconds）：资格期按 double 秒数累加，块长不同，恰好在满资格那一刻
            // 结束的块会累加成 0.15000… 或 0.14999…；不带容差时一边算「满了」、一边还差 1e-17 s，保持期（满资格就装上 2 s）因此
            // 相差整整 2 s（T-012 阶段 4 服务器测试 ClayGuard 块长一致用例，块长 333 时暴露）。
            const auto needed = tuning.phaseMinSeconds - phasePendingSeconds;   // 还差多久满资格（≤ 容差 = 已满）

            if (! (needed > qualificationToleranceSeconds))
            {
                untriggeredSeconds = 0.0;
                triggeredSeconds = seconds;
                qualified = true;
            }
            else if (seconds >= needed - qualificationToleranceSeconds)
            {
                untriggeredSeconds = juce::jmin (needed, seconds);
                triggeredSeconds = juce::jmax (0.0, seconds - needed);
                phasePendingSeconds = tuning.phaseMinSeconds;   // 封顶：之后只看「还成立不成立」
                qualified = true;
            }
            else
            {
                phasePendingSeconds += seconds;
            }
        }
        else
        {
            phasePendingSeconds = 0.0;   // 须「连续」成立：一断就从头计
        }

        auto level = static_cast<double> (state.phaseFloor);
        auto holdLeft = static_cast<double> (state.phaseHoldSeconds);

        if (untriggeredSeconds > 0.0)
        {
            // 先耗保持期；同一段里跨过保持期末尾时，只有剩下的时间用于释放（块长无关）。
            const auto held = juce::jmin (holdLeft, untriggeredSeconds);
            holdLeft -= held;
            const auto releasing = untriggeredSeconds - held;

            if (releasing > 0.0)
                level = tuning.phaseReleaseSeconds > 0.0 ? juce::jmax (0.0, level - releasing / tuning.phaseReleaseSeconds) : 0.0;
        }

        if (qualified)
        {
            // 攻击从此刻的下限接着升（不从 0 重来）；保持期从条件最后一次成立的那一块末尾起算。满资格即装上保持期——
            // 「已连续成立满资格期」本身就是触发，不要求本块触发后的时间 > 0（否则满资格恰好落在块末时保持期晚一块才装上）。
            if (! (tuning.phaseAttackSeconds > 0.0))
                level = 1.0;                                                        // 攻击时间 0 = 立即（旧快照缺字段按 0 读）
            else if (triggeredSeconds > 0.0)
                level = juce::jmin (1.0, level + triggeredSeconds / tuning.phaseAttackSeconds);

            holdLeft = tuning.phaseHoldSeconds;
        }

        state.phaseFloor = static_cast<float> (level);
        state.phaseHoldSeconds = static_cast<float> (holdLeft);
    }

    void ClayGuard::advanceWidth (float allowedDb, float plannedWidthDb, double seconds) noexcept
    {
        // 可见区间 [0, top]：上限高于 top 的部分不影响输出（见文件头「宽度保护」）。
        const auto top = static_cast<double> (juce::jmax (0.0f, plannedWidthDb));
        const auto visible = juce::jmin (static_cast<double> (state.widthCapDb), top);
        const auto target = juce::jmin (static_cast<double> (allowedDb), top);
        auto next = visible;

        if (target < visible)          // 收回：整段方案加宽在 attack 秒内收完
            next = tuning.widthAttackSeconds > 0.0 ? juce::jmax (target, visible - top * seconds / tuning.widthAttackSeconds) : target;
        else if (target > visible)     // 放开：从 0 回到方案值用 release 秒
            next = tuning.widthReleaseSeconds > 0.0 ? juce::jmin (target, visible + top * seconds / tuning.widthReleaseSeconds) : target;

        // 仍在限制方案 → 记下这个上限；已不限制（到达 top，或方案不加宽）→ 回到静止形态「上限 = 允许值」。
        state.widthCapDb = next < top ? static_cast<float> (next) : allowedDb;
    }

    float ClayGuard::allowedWidthDb (const ShortTermView& view) const noexcept
    {
        const auto side = static_cast<double> (view.liveHighSidePower);

        // P_S = 0（高频纯单声道）：加宽不改变任何东西，不限。
        if (! (side > 0.0))
            return clayWidthUncappedDb;

        const auto ratio = tuning.widthPowerScale * static_cast<double> (view.liveHighMidPower) / side;

        // γ² ≤ k · P_M / P_S；右边 ≤ 1 → 不加宽时预测相关度已在下限或以下 → 只允许 0（保护从不自动收窄）。
        if (! (ratio > 1.0))
            return 0.0f;

        return static_cast<float> (juce::jmin (static_cast<double> (clayWidthUncappedDb), 10.0 * std::log10 (ratio)));
    }

    //==============================================================================
    ClayModule::Params resolveClayParams (const ClaySettings& user, const ClayPlan& autoValues, float glazePercent) noexcept
    {
        const ClayModule::Params neutral {};
        const auto automaticOn = glazePercent > 0.0f;   // NaN 也按 GLAZE 0：「自动」项取中性
        auto resolved = neutral;

        // 未知的枚举值（坏状态）落不进任何分支 → 该项保持中性。
        switch (user.lowCutMode)
        {
            case ClayMode::automatic:
                if (automaticOn)
                    resolved.lowCutHz = autoValues.lowCutHz;
                break;

            case ClayMode::off:
                break;

            case ClayMode::manual:
                resolved.lowCutHz = user.lowCutHz;   // GLAZE 0 也生效
                break;
        }

        switch (user.monoMode)
        {
            case ClayMode::automatic:
                if (automaticOn)
                {
                    resolved.monoHz = autoValues.monoHz;
                    resolved.monoAmount = autoValues.monoAmount;
                }
                break;

            case ClayMode::off:
                break;

            case ClayMode::manual:
                resolved.monoHz = user.monoHz;
                resolved.monoAmount = 1.0f;          // 手动收单 = 分频点以下完全收单（旋钮只调分频点）
                break;
        }

        switch (user.widthMode)
        {
            case ClayMode::automatic:
                if (automaticOn)
                    resolved.widthDb = autoValues.widthDb;
                break;

            case ClayMode::off:
                break;

            case ClayMode::manual:
                resolved.widthDb = user.widthDb;     // 负值 = 收窄，只能手动
                break;
        }

        resolved.bypass = user.bypass;

        // 钳到 ClayModule 的参数范围；非有限按中性；低切 ≤ 0 = 关（保持 0）。
        resolved.lowCutHz = std::isfinite (resolved.lowCutHz) && resolved.lowCutHz > 0.0f
                          ? juce::jlimit (ClayModule::minLowCutHz, ClayModule::maxLowCutHz, resolved.lowCutHz)
                          : neutral.lowCutHz;
        resolved.monoHz = std::isfinite (resolved.monoHz)
                        ? juce::jlimit (ClayModule::minMonoHz, ClayModule::maxMonoHz, resolved.monoHz)
                        : neutral.monoHz;
        resolved.monoAmount = std::isfinite (resolved.monoAmount)
                            ? juce::jlimit (0.0f, 1.0f, resolved.monoAmount)
                            : neutral.monoAmount;
        resolved.widthDb = std::isfinite (resolved.widthDb)
                         ? juce::jlimit (ClayModule::minWidthDb, ClayModule::maxWidthDb, resolved.widthDb)
                         : neutral.widthDb;

        // 收单比例为 0 时分频点没有意义（ClayPlan 的约定「monoAmount = 0 时 monoHz 保持默认 80」）：一律交中性值，
        // 模块的分频点就不会因为自动方案里一个用不上的 monoHz 在变而离开静止状态（见文件头）。比例离开 0 时，
        // 分频点与比例在模块里一起按 20 ms 平滑，比例从 0 起步，听不出。顺手把 −0 归成 +0（加宽同理）。
        if (resolved.monoAmount == 0.0f)
        {
            resolved.monoAmount = neutral.monoAmount;
            resolved.monoHz = neutral.monoHz;
        }

        if (resolved.widthDb == 0.0f)
            resolved.widthDb = neutral.widthDb;

        return resolved;
    }

    //==============================================================================
    bool ClayPressureMonitor::update (double pressure, double seconds) noexcept
    {
        if (! (seconds > 0.0) || ! std::isfinite (seconds))
            return false;

        const auto dt = juce::jmin (seconds, maxSeconds);
        const auto p = std::isfinite (pressure) ? juce::jlimit (0.0, maxPressureDb, pressure) : 0.0;

        smoothed += (p - smoothed) * (1.0 - std::exp (-dt / smoothingSeconds));
        sinceStep = juce::jmin (sinceStep + dt, maxSeconds);
        aboveSeconds = smoothed > threshold ? juce::jmin (aboveSeconds + dt, maxSeconds) : 0.0;

        if (aboveSeconds >= sustainSeconds && sinceStep >= cooldownSeconds)
        {
            smoothed = 0.0;
            aboveSeconds = 0.0;
            sinceStep = 0.0;
            return true;
        }

        return false;
    }

    void ClayPressureMonitor::reset() noexcept
    {
        smoothed = 0.0;
        aboveSeconds = 0.0;
        sinceStep = cooldownSeconds;
    }

    float ClayPressureMonitor::nextScale (float scale) noexcept
    {
        if (! std::isfinite (scale))
            return 0.0f;

        if (scale > 0.5f)
            return 0.5f;

        if (scale > 0.25f)
            return 0.25f;

        return 0.0f;
    }

    ClayPlan scaleClayPlan (const ClayPlan& plan, float scale, float lowCutFloorHz) noexcept
    {
        const auto s = std::isfinite (scale) ? juce::jlimit (0.0f, 1.0f, scale) : 0.0f;

        if (! (s < 1.0f))
            return plan;

        auto out = plan;
        auto mask = static_cast<std::uint8_t> (plan.costLimitedMask);

        if (plan.lowCutHz > 0.0f)
        {
            const auto floorHz = std::isfinite (lowCutFloorHz) && lowCutFloorHz > 0.0f ? lowCutFloorHz : ClayModule::minLowCutHz;

            if (! (s > 0.0f))
                out.lowCutHz = 0.0f;
            else if (plan.lowCutHz > floorHz)
                out.lowCutHz = static_cast<float> (static_cast<double> (floorHz)
                                                   * std::pow (static_cast<double> (plan.lowCutHz) / static_cast<double> (floorHz),
                                                               static_cast<double> (s)));

            mask = static_cast<std::uint8_t> (mask | clayCostLimitedLowCut);
        }

        if (plan.monoAmount > 0.0f)
        {
            out.monoAmount = plan.monoAmount * s;

            if (! (out.monoAmount > 0.0f))
            {
                out.monoAmount = 0.0f;
                out.monoHz = ClayPlan {}.monoHz;
            }

            mask = static_cast<std::uint8_t> (mask | clayCostLimitedMono);
        }

        if (plan.widthDb > 0.0f)
        {
            out.widthDb = plan.widthDb * s;
            mask = static_cast<std::uint8_t> (mask | clayCostLimitedWidth);
        }

        out.costLimitedMask = mask;
        return out;
    }
}
