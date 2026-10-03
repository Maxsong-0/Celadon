/*
  ==============================================================================

    LiveProfile.h
    T-048 实时画像（设计规格-实时GLAZE 第 3 节、设计规格-实时GLAZE-主段与增益 第 1～3、5 节；ADR-030）。
    T-048 阶段 1 修订（Max 2026-10-02 选「回看重判」）：实时画像 = 带遗忘的离线门控。

    纯逻辑：只吃分析器定稿的段（SegmentLedger::Segment，经 SegmentSink 交来），不碰音频、不碰模块。阶段 1 插件不用它（只在渲染工具与
    测试里）；阶段 2 编排器把它喂给决策器。

    一、回看重判（每一份画像怎么算）
      最近的段放在定长的回看窗口里（windowCapacity 段，124 BPM 约 16.5 分钟）。每来一段，就按「此刻」的参考把窗口里每一段重判一遍、
      重新合并出画像——口径逐项同 MusicAnalyzer.cpp「六」节 House 门控画像那一族函数（computeHouseProfile …），只是每段的权重从
      「满编 1 / 否则 0」换成带遗忘的软权重：
        - 参考：S_ref = 窗口里各段段响度的 referencePercentile 分位（权重 = 有效时长 × 参考遗忘）；lowRef = 「按此刻 S_ref 算响度因子 ≥ 0.5」
          的那些段的 lowShare 的同分位（同样的权重）。不再由「当时过没过响度门」决定。
        - 主段度 m′ = 同一套软斜坡（规格第 2 节）按此刻的参考重算：4/4 型 ramp(L − (S_ref − ΔG)) × ramp(lowShare ÷ lowRef − g_low)，
          响度型只有响度因子，被动型有效音频一律 1。冷启动不用特例：第一段的参考就是它自己。
        - 段 i 的主段权重 f_i = m′_i × exp(−Σ_{j 比 i 新} m′_j × e_j ÷ W_mem,j) × 全局因子_i（e = 有效时长）：主段时钟只在主段进账时走，
          而且是按此刻的 m′ 走——breakdown（m′ = 0）不推时钟，画像、M、c 逐位不动；被重判成非主段的前奏也不再推时钟。
          参考权重 = e_i × exp(−Σ_{j 比 i 新} e_j ÷ referenceMemorySeconds) × 全局因子_i（按有效音频）；响度型 / 被动型的 I 用的「全部段」
          权重 = exp(−Σ_{j 比 i 新} e_j ÷ W_mem,j) × 全局因子_i。
        - 全局因子：换歌软遗忘（× newProgramKeep，主段、参考、全部段一起）与长时间无主段的时间衰减（只主段）是「事件」——在一段进账时
          判定，乘在当时窗口里全部更老的段上，之后不因重判撤销。
        - 主段质量 M = Σ f_i × e_i；c = 1 − exp(−M ÷ M0)；representativeness = c，effectiveSeconds = M；各特征 confidence =
          min(c, 观测充分度)。LRA、I 按曲风分口径：4/4 型主段加权，响度型 / 被动型全部段（参考权重 / 全部段权重）。
      进账判断（只用于事件与提交记录）：一段进账时先按上一份已发布画像的参考算一次 m（冷启动以自己为参考），长时间无主段只看它；
      换歌看「段响度与上一份画像的 I 之差」与它的低频因子，「轻」的那一支还要最近 newProgramSeconds 里段响度起伏 ≤ newProgramRangeLu。
      窗口满了以后，最老的段直接丢弃（不进「老账」）：它此刻的权重份额见 LiveDiagnostics::oldestMainShare / oldestReferenceShare，
      误差估计写在 .cpp 文件头。

    二、时间：回看重算按绝对样本网格分片，按确定的时刻发布
      - 分析器每处理完一块调 advanceTo，报实时段落账**交齐的位置** T：end ≤ T 的段都已交出、之后交来的段 end > T（SegmentLedger.h
        SegmentSink::advanceTo、MusicAnalyzer.h「时间轴与 advanceTo」）。渲染工具与测试台不另报。
      - 网格点 = sliceGridSeconds 的整数倍（绝对样本，与宿主块长无关）；每个网格点最多做一片重算（估计 ≤ sliceBudgetNanoseconds）。
      - 第 k 段的画像在 D_k = max(段尾 + publishDelaySeconds, D_{k−1}) 这一刻发布：advanceTo 报告的时间 ≥ D_k 时，先把第 k 段那一遍
        重算做完（来不及的就在这次调用里补完——大块、离线、成批交段），再发布。getProfile / getDiagnostics / readCommits / readJudgements
        只读已发布的那一份。
      - 所以已发布的画像只取决于段序列与 advanceTo 报告的时间：内容 = 前 k 段的纯函数（与分片怎么切、advanceTo 调了几次、宿主块多长
        都无关）；时刻 = D_k。前提：段在 D_k 之前交来——分析器报的是交齐位置，T ≥ D_k > 段尾时这一段必已交来，所以由分析器驱动时
        前提恒成立；别的调用方报得比交齐位置早到 D_k 之后，晚于 D_k 才交来的段在它交来后的第一次 advanceTo 发布（内容不变，只是看得见得晚）。
      - advanceTo 的时间只增不减：比上一次早或相同的调用什么都不做（分析器 processInput 与 processSpectrum 末尾各调一次）。
      - 交来的段超过 queueCapacity 段还没发布（调用方一直不调 advanceTo）：segmentFinalized 就地把最早那一段算完并立刻发布（内容不变，
        时刻提前）——只是防误用的退路，正常使用碰不到。

    三、实时约束
      - 全部容量在构造时一次分配（回看窗口约 1.7 MB、试算直方图约 0.58 MB，其余几十 KB）；prepare / reset / 音频线程接口不分配、不加锁。
      - segmentFinalized 只把段拷进队列（定长拷贝，O(1)）；重算全在 advanceTo 的分片里。
      - setConstants / createStateXml / restoreStateXml 在消息线程，且不与音频线程接口同时调用（setConstants 与 restore 同步重算一遍，
        约零点几毫秒）。

    四、等价性（测试锚点）
      遗忘关掉（W_mem、参考遗忘都取无穷，事件关掉）、参考冻结为 House 账本最终的 S_ref / lowRef（setFrozenReferencesForTesting）、
      斜坡宽取极小（硬门）时，回看重判的画像与分析器 House 档按同一批段算出的候选画像一致（容差逐项写在测试里）。

  ==============================================================================
*/

#pragma once

#include "GlazeConstants.h"
#include "GlazeTypes.h"
#include "SegmentLedger.h"

#include <juce_core/juce_core.h>

#include <cstdint>
#include <limits>
#include <memory>

namespace celadon::glaze
{
    // 主段模型（设计规格-实时GLAZE-主段与增益 第 2 节）。
    enum class MainSectionModel : std::uint8_t
    {
        fourOnFloor = 0,   // 4/4 型：m = 响度因子 × 低频因子（T-038 两道门的软与）
        loudness = 1,      // 响度型：m = 响度因子
        passive = 2        // 被动型：有效音频一律 m = 1
    };

    // 曲风 → 主段模型。House 三类 = 4/4 型；通用 = 响度型（以后的新曲风在这里加）。
    MainSectionModel mainSectionModelFor (Genre genre) noexcept;

    //==============================================================================
    // LiveConstants 每个字段的合法取值：LiveProfile 的清洗（坏值换出厂值）与渲染工具 --live-constants 的校验（坏值报错）共用这一张表
    // （T-048 REPORT-C 第 7 节第 3 条：原来工具照抄了一份）。+∞ 只在注明「含 ∞」的范围里合法（「不遗忘 / 永不触发」，等价性测试这么关）。
    enum class LiveConstantDomain : std::uint8_t
    {
        finite,              // 有限的数
        positiveFinite,      // > 0 的有限数
        percentile,          // (0, 1]
        notNan,              // 任意数，含 ±∞（如 memoryBars ≤ 0 = 不按小节、peakTailSeconds ≤ 0 = 只取最大值）
        positive,            // > 0，含 +∞
        unit,                // [0, 1]
        nonNegative,         // ≥ 0，含 +∞
        nonNegativeFinite    // ≥ 0 的有限数
    };

    bool isInLiveConstantDomain (double value, LiveConstantDomain domain) noexcept;

    // 两份常数逐字段并排访问：visit (模型名（"fourOnFloor" / "loudness" / "passive"；不属于主段模型的字段为 nullptr）, 字段名,
    // a 的字段, b 的字段, 取值范围)。字段是 float& 或 double&（a、b 可以是 const）。LiveConstants 加字段时只在这里加一行。
    template <typename A, typename B, typename Visitor>
    void visitLiveConstantPairs (A& a, B& b, Visitor&& visit)
    {
        const auto model = [&visit] (const char* prefix, auto& ma, auto& mb)
        {
            visit (prefix, "gateWidthLu", ma.gateWidthLu, mb.gateWidthLu, LiveConstantDomain::finite);
            visit (prefix, "loudnessRampLu", ma.loudnessRampLu, mb.loudnessRampLu, LiveConstantDomain::positiveFinite);
            visit (prefix, "lowGateFactor", ma.lowGateFactor, mb.lowGateFactor, LiveConstantDomain::finite);
            visit (prefix, "lowRampWidth", ma.lowRampWidth, mb.lowRampWidth, LiveConstantDomain::positiveFinite);
            visit (prefix, "referencePercentile", ma.referencePercentile, mb.referencePercentile, LiveConstantDomain::percentile);
            visit (prefix, "memoryBars", ma.memoryBars, mb.memoryBars, LiveConstantDomain::notNan);
            visit (prefix, "memorySeconds", ma.memorySeconds, mb.memorySeconds, LiveConstantDomain::positive);
            visit (prefix, "confidenceBars", ma.confidenceBars, mb.confidenceBars, LiveConstantDomain::notNan);
            visit (prefix, "confidenceSeconds", ma.confidenceSeconds, mb.confidenceSeconds, LiveConstantDomain::positive);
        };

        model ("fourOnFloor", a.fourOnFloor, b.fourOnFloor);
        model ("loudness", a.loudness, b.loudness);
        model ("passive", a.passive, b.passive);

        const char* const none = nullptr;
        visit (none, "referenceMemorySeconds", a.referenceMemorySeconds, b.referenceMemorySeconds, LiveConstantDomain::positive);
        visit (none, "mainThreshold", a.mainThreshold, b.mainThreshold, LiveConstantDomain::unit);
        visit (none, "noMainSeconds", a.noMainSeconds, b.noMainSeconds, LiveConstantDomain::nonNegative);
        visit (none, "noMainDecaySeconds", a.noMainDecaySeconds, b.noMainDecaySeconds, LiveConstantDomain::positive);
        visit (none, "newProgramLu", a.newProgramLu, b.newProgramLu, LiveConstantDomain::nonNegative);
        visit (none, "newProgramSeconds", a.newProgramSeconds, b.newProgramSeconds, LiveConstantDomain::positive);
        visit (none, "newProgramKeep", a.newProgramKeep, b.newProgramKeep, LiveConstantDomain::unit);
        visit (none, "newProgramRangeLu", a.newProgramRangeLu, b.newProgramRangeLu, LiveConstantDomain::nonNegative);
        visit (none, "peakTailSeconds", a.peakTailSeconds, b.peakTailSeconds, LiveConstantDomain::notNan);
        visit (none, "publishDelaySeconds", a.publishDelaySeconds, b.publishDelaySeconds, LiveConstantDomain::nonNegativeFinite);
    }

    // 一份常数逐字段访问：visit (模型名或 nullptr, 字段名, 字段, 取值范围)。
    template <typename Constants, typename Visitor>
    void visitLiveConstants (Constants& constants, Visitor&& visit)
    {
        visitLiveConstantPairs (constants, constants,
                                [&visit] (const char* model, const char* name, auto& field, auto&, LiveConstantDomain domain)
                                { visit (model, name, field, domain); });
    }

    // 不在取值范围里的字段换成出厂值（LiveConstants 的默认值），其余原样。
    LiveConstants sanitiseLiveConstants (const LiveConstants& constants) noexcept;

    //==============================================================================
    // 每发布一份画像记一条（readCommits）；渲染工具的 --live-trace 与测试读。
    struct LiveCommitRecord
    {
        std::uint64_t sequence = 0;          // 第几次发布（从 0 起，跨 reset 单调）；新实例里 = 段的到达序号
        std::int64_t start = 0, end = 0;     // 这一段在分析器时间轴上的 [start, end)（样本）
        std::int64_t publishTime = 0;        // 这一份画像的发布时刻 D（分析器时间轴；文件头第二节）
        double nominalSeconds = 0.0;         // 段的标称长度（秒）
        SegmentMode mode = SegmentMode::none;
        double effectiveSeconds = 0.0;       // 坏段记 0
        bool loudnessValid = false;
        float loudnessLufs = 0.0f;
        bool lowShareValid = false;
        float lowShare = 0.0f;
        // 进账判断：按上一份已发布画像的参考（冷启动以自己为参考）。长时间无主段与换歌只看它。
        float admissionLoudnessFactor = 0.0f;
        float admissionLowFactor = 0.0f;
        float admissionMainness = 0.0f;
        // 这一份画像对这一段的判断：按这一份画像的参考（之后每一份画像都会按当时的参考回看重判它，见 readJudgements）。
        float loudnessFactor = 0.0f;
        float lowFactor = 0.0f;
        float mainness = 0.0f;               // m′
        double weightSeconds = 0.0;          // w′ = m′ × effectiveSeconds（最新一段还没有遗忘）
        bool referenceValid = false;         // 这一份画像的 S_ref（判窗口里全部段用的）
        float referenceLufs = 0.0f;
        bool lowReferenceValid = false;      // 这一份画像的 lowRef
        float lowReference = 0.0f;
        bool mainLevelValid = false;
        float mainLevelLufs = 0.0f;          // 这一份画像的 integratedLufs（4/4 型 = 主段电平）
        double mainMassSeconds = 0.0;        // 这一份画像的主段质量 M
        float confidence = 0.0f;             // 这一份画像的 c
        bool timeDecay = false;              // 这一段进账时「长时间无主段」的时间衰减在生效
        bool newProgram = false;             // 这一段进账时触发了换歌软遗忘
        std::uint8_t newProgramVote = 0;     // 0 不像换歌（清零重计）、1 响 newProgramLu 以上、2 轻 newProgramLu 以上且低音满、3 读不出（不计也不清零）
        bool newProgramRangeValid = false;   // 这一段进账时那一串已累计满 newProgramSeconds，量了起伏
        float newProgramRangeLu = 0.0f;      // 那一串最近 newProgramSeconds 的段响度起伏（最大 − 最小）
    };

    // 已发布那一份画像的汇总读数（诊断、测试、以后的状态行）。可平凡拷贝。
    struct LiveDiagnostics
    {
        std::uint64_t commits = 0;           // 这一份画像含的段数（冷启动以来）
        bool referenceValid = false;
        float referenceLufs = 0.0f;          // S_ref
        bool lowReferenceValid = false;
        float lowReference = 0.0f;           // lowRef
        double mainMassSeconds = 0.0;        // M
        float confidence = 0.0f;             // c = 1 − exp(−M ÷ M0)
        double activeSeconds = 0.0;          // 进账过的有效音频总时长（不遗忘）
        double secondsSinceMain = 0.0;       // 最近一次进账 m ≥ mainThreshold 之后的有效音频
        bool timeDecayActive = false;
        std::uint32_t newProgramCount = 0;
        int windowSegments = 0;              // 回看窗口里的段
        std::uint64_t droppedSegments = 0;   // 挤出窗口丢掉的老段（冷启动以来）
        float oldestMainShare = 0.0f;        // 窗口里最老一段在 M 里的份额（窗口满时下一次就丢掉它）
        float oldestReferenceShare = 0.0f;   // 同上，在参考权重里的份额
        std::int64_t publishTime = 0;        // 这一份画像的发布时刻（冷启动 0）
        // ---- 工作量统计：不属于这一份画像，随交段与 advanceTo 的先后、宿主块长变（逐位比较时不比）----
        int pendingSegments = 0;             // 读数那一刻已交来、还没发布的段
        int lastPassSlices = 0;              // 这一份画像那一遍用了几片（同步补完的不算片）
        std::uint64_t catchUps = 0;          // 冷启动以来，到了发布时刻还没做完、在那次调用里补完的遍数
        std::uint64_t forcedPublications = 0;   // 冷启动以来，队列满时就地发布的份数
    };

    // 已发布那一份画像里窗口每一段的判断（readJudgements；渲染工具写「最终判断」、测试核对等价性）。
    struct LiveSegmentJudgement
    {
        std::uint64_t sequence = 0;          // 段的到达序号（reset 以来，从 0 起）
        std::int64_t start = 0, end = 0;
        float mainness = 0.0f;               // m′（按这一份画像的参考）
        double weightSeconds = 0.0;          // f × 有效时长（含主段遗忘与全局因子）
    };

    class LiveProfile final : public SegmentSink
    {
    public:
        static constexpr int commitCapacity = 64;            // readCommits 的环形缓冲
        static constexpr int windowCapacity = 512;           // 回看窗口（124 BPM 约 16.5 分钟；2 秒段约 17 分钟）
        static constexpr int queueCapacity = 32;             // 已交来、还没发布的段（文件头第二节最后一条）
        static constexpr double sliceGridSeconds = 0.005;    // 网格点间隔（每个网格点最多一片）
        static constexpr double sliceBudgetNanoseconds = 8000.0;   // 每片按 .cpp 的工作量估计做到这么多为止（107 实测见 REPORT-A2）
        static constexpr std::int64_t noPublication = std::numeric_limits<std::int64_t>::max();

        LiveProfile();
        ~LiveProfile() override;

        // 消息线程：按采样率换算（网格、发布延迟、试算块长）并冷启动。不分配（容量在构造时已分配）。
        void prepare (double sampleRate);

        // 冷启动：清空窗口、队列、事件与已发布的画像，时间轴回到「不知道」（下一次 advanceTo 重新起算）。不分配。提交序号跨 reset 单调。
        void reset() noexcept;

        // 常数与主段模型（处理之前交；换曲风时再交一次）。消息线程，不与音频线程接口同时调：已有的段保留，已发布的那一份按新常数
        // 同步重算并就地替换（不另发提交记录），在途的那一遍从重算开头重来（进账判断不重做）。
        void setConstants (const LiveConstants& constants, MainSectionModel model) noexcept;

        // ---- 音频线程 ----

        // SegmentSink：分析器每定稿一段调一次。只入队（O(1)）；进账与重算在 advanceTo 的分片里。
        void segmentFinalized (const SegmentLedger::Segment& segment) noexcept override;

        // SegmentSink：实时段落账交齐到了 analyzerTime（样本，end 不晚于它的段都已交来）。做到期的网格点上的分片、发布到期的画像（文件头第二节）。
        void advanceTo (std::int64_t analyzerTime) noexcept override;

        // 流结束（离线渲染的歌尾、测试）：把已交来、还没发布的段全部算完并按序发布（提交记录照记各自的发布时刻 D；时间轴不动）。
        void flush() noexcept;

        // 每发布一份画像回调一次（在 advanceTo / flush / 队列满的 segmentFinalized 里，音频线程；实现必须 0 分配、0 锁、不抛异常）。
        // 回调时 getProfile / getDiagnostics 已是这一份。渲染工具用它逐份取快照（一次 advanceTo 可能发布好几份）。
        class PublicationListener
        {
        public:
            virtual ~PublicationListener() = default;
            virtual void livePublished (const LiveCommitRecord& record) noexcept = 0;
        };

        // 只存指针（nullptr = 不回调）；处理之前设。
        void setPublicationListener (PublicationListener* listener) noexcept;

        // 同 segmentFinalized，但这一段的主段度钉死为 mainness（0 ～ 1，越界钳位、非有限当 0）：每次重判都用它，参考照常算。诊断与测试用。
        void commitWithMainness (const SegmentLedger::Segment& segment, float mainness) noexcept;

        // 按已发布那一份画像的参考给一份读数算 m（不入队）：以后编排器给「正在播的这一段」暂判用。没有参考时以这份读数自己为参考。
        float mainnessFor (bool loudnessValid, float loudnessLufs, bool lowShareValid, float lowShare) const noexcept;

        // 下一份要发布的画像的发布时刻（还没有交来的段 = noPublication）。渲染工具逐份取快照用：advanceTo 到它、读、再往后。
        std::int64_t getNextPublicationTime() const noexcept;

        // ---- 读数（音频线程；只读已发布的那一份，返回的引用在下一次发布 / reset / setConstants 之前有效）----

        const MusicProfile& getProfile() const noexcept;
        LiveDiagnostics getDiagnostics() const noexcept;

        // 读 cursor 之后的提交记录（最多 maxCount 条），cursor 推进。落后超过环形容量：跳到能读到的最早一条，返回负的条数。
        int readCommits (std::uint64_t& cursor, LiveCommitRecord* out, int maxCount) const noexcept;

        // 已发布那一份画像的窗口，从旧到新最多 maxCount 段（out = nullptr 只数个数）。返回写出的段数。
        int readJudgements (LiveSegmentJudgement* out, int maxCount) const noexcept;

        // ---- 状态（消息线程，不与音频线程接口同时调；阶段 2 的 LIVE 存档节点里画像这一部分）----

        // 回看窗口与还没发布的段（逐段的原始量与事件印记）、事件时钟、换歌与无主段的计时、计数。不含提交记录与派生量（读回后重算）。
        std::unique_ptr<juce::XmlElement> createStateXml() const;
        // 读回：窗口里的段重算出已发布的那一份（与保存时逐位相同）；保存时还没发布的段按序进账、重算、立刻发布（不另发提交记录）；
        // 时间轴回到「不知道」（存档读回后宿主的时间轴从头算）。格式版本不认识、数据坏（长度不对、非有限值、计数越界）→ 冷启动并返回 false。
        bool restoreStateXml (const juce::XmlElement& xml);

        // ---- 只给测试用 ----

        // 冻结参考：enabled 时进账判断、重判、mainnessFor 都用给定的 S_ref / lowRef（等价性锚点：House 账本最终的参考）。
        // 消息线程，与 setConstants 一样同步重算已发布的那一份。
        void setFrozenReferencesForTesting (bool enabled, float referenceLufs, float lowReference) noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LiveProfile)
    };
}
