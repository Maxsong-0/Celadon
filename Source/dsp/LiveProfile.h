/*
  ==============================================================================

    LiveProfile.h
    T-048 实时画像（设计规格-实时GLAZE 第 3 节、设计规格-实时GLAZE-主段与增益 第 1～3、5 节；ADR-030）。

    纯逻辑：只吃分析器定稿的段（SegmentLedger::Segment，经 SegmentSink 交来），不碰音频、不碰模块。每定稿一段：
      1. 按此刻的参考（S_ref、lowRef）与曲风的主段模型算主段度 m ∈ [0, 1]（第 2 节）；
      2. 把这一段以权重 w = m × 段有效时长并进各特征的累计，并进之前先把旧累计按「进账」遗忘：× exp(−w ÷ W_mem)（第 3 节）；
         S_ref / lowRef / LRA 的直方图另按有效音频时长遗忘（全部段都进，不看 m）；
      3. 重算 MusicProfile 形状的实时画像（字段与口径同分析器的 House 门控画像，MusicAnalyzer.cpp「六」节 computeHouseProfile 一族），
         每个特征的 confidence = min(置信度 c, 该特征自己的观测充分度)，representativeness = c，effectiveSeconds = 主段质量 M。
    阶段 1 插件不用它（只在渲染工具与测试里）；阶段 2 编排器把它喂给决策器。

    等价性（测试锚点）：遗忘关掉（W_mem 与参考遗忘都取无穷）、逐段用 commitWithMainness 交账本门控的 full（1 / 0）时，
    画像与分析器 House 档按同一批段算出的候选画像一致（直方图取格中心造成的差在容差内，容差逐项写在测试里）。

    实时约束：prepare 里分配全部固定容量；segmentFinalized / commitWithMainness / 读数在音频线程，0 分配、0 锁；
    结果只取决于交来的段序列（与宿主分块无关，因为段本身与分块无关）。

  ==============================================================================
*/

#pragma once

#include "GlazeConstants.h"
#include "GlazeTypes.h"
#include "SegmentLedger.h"

#include <juce_core/juce_core.h>

#include <cstdint>
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

    // 每并入一段记一条（readCommits）；渲染工具的 --live-trace 与测试读。
    struct LiveCommitRecord
    {
        std::uint64_t sequence = 0;          // 从 0 起，跨 reset 单调
        std::int64_t start = 0, end = 0;     // 段在分析器时间轴上的 [start, end)（样本）
        double nominalSeconds = 0.0;         // 段的标称长度（秒）
        SegmentMode mode = SegmentMode::none;
        double effectiveSeconds = 0.0;
        bool loudnessValid = false;
        float loudnessLufs = 0.0f;
        bool lowShareValid = false;
        float lowShare = 0.0f;
        float loudnessFactor = 0.0f;         // m 的两个因子（被动型都是 1；没有参考时见 .cpp「冷启动」）
        float lowFactor = 0.0f;
        float mainness = 0.0f;               // m
        double weightSeconds = 0.0;          // w = m × effectiveSeconds
        bool referenceValid = false;         // 并入之前的 S_ref 有效
        float referenceLufs = 0.0f;          // 并入之前的 S_ref
        bool lowReferenceValid = false;
        float lowReference = 0.0f;           // 并入之前的 lowRef
        bool mainLevelValid = false;
        float mainLevelLufs = 0.0f;          // 并入之后画像的 integratedLufs（4/4 型 = 主段电平）
        double mainMassSeconds = 0.0;        // 并入之后的主段质量 M
        float confidence = 0.0f;             // 并入之后的 c
        bool timeDecay = false;              // 这一段并入时「长时间无主段」的时间衰减在生效
        bool newProgram = false;             // 这一段触发了换歌软遗忘
    };

    // 此刻的汇总读数（诊断、测试、以后的状态行）。
    struct LiveDiagnostics
    {
        std::uint64_t commits = 0;
        bool referenceValid = false;
        float referenceLufs = 0.0f;          // S_ref
        bool lowReferenceValid = false;
        float lowReference = 0.0f;           // lowRef
        double mainMassSeconds = 0.0;        // M
        float confidence = 0.0f;             // c = 1 − exp(−M ÷ M0)
        double activeSeconds = 0.0;          // 并入过的有效音频总时长（不遗忘）
        double secondsSinceMain = 0.0;       // 最近一次 m ≥ mainThreshold 之后的有效音频
        bool timeDecayActive = false;
        std::uint32_t newProgramCount = 0;
    };

    class LiveProfile final : public SegmentSink
    {
    public:
        static constexpr int commitCapacity = 64;   // readCommits 的环形缓冲

        LiveProfile();
        ~LiveProfile() override;

        // 消息线程：分配固定容量并冷启动。sampleRate 用来把段的样本数换成秒。
        void prepare (double sampleRate);

        // 冷启动：清空全部累计与参考（不分配）。提交序号跨 reset 单调。
        void reset() noexcept;

        // 常数与此刻的主段模型（编排器 / 工具在处理之前交；换曲风时再交一次：已有累计保留，之后的段按新模型算 m）。只拷贝。
        void setConstants (const LiveConstants& constants, MainSectionModel model) noexcept;

        // ---- 音频线程 ----

        // SegmentSink：分析器每定稿一段调一次。按此刻的参考算 m，再并入。
        void segmentFinalized (const SegmentLedger::Segment& segment) noexcept override;

        // 跳过 m 的计算、用给定的主段度（0 ～ 1，越界钳位、非有限当 0）并入；参考直方图照常更新。等价性测试与诊断用。
        void commitWithMainness (const SegmentLedger::Segment& segment, float mainness) noexcept;

        // 用此刻的参考给一份读数算 m（不并入）：以后编排器给「正在播的这一段」暂判（棘轮）用。读数无效的项按 .cpp 的口径处理。
        float mainnessFor (bool loudnessValid, float loudnessLufs, bool lowShareValid, float lowShare) const noexcept;

        // ---- 读数（音频线程；返回的引用在下一次并入 / reset 之前有效）----

        const MusicProfile& getProfile() const noexcept;
        LiveDiagnostics getDiagnostics() const noexcept;

        // 读 cursor 之后的提交记录（最多 maxCount 条），cursor 推进。落后超过环形容量：跳到能读到的最早一条，返回负的条数。
        int readCommits (std::uint64_t& cursor, LiveCommitRecord* out, int maxCount) const noexcept;

        // ---- 状态（消息线程；阶段 2 的 LIVE 存档节点里画像这一部分）----

        // 全部累计、参考直方图、遗忘质量、计数（不含 commit 环形缓冲）。读回后 getProfile / getDiagnostics 逐位等于保存时。
        std::unique_ptr<juce::XmlElement> createStateXml() const;
        // 格式版本不认识、数据坏（长度不对、非有限值）→ 冷启动并返回 false。
        bool restoreStateXml (const juce::XmlElement& xml);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LiveProfile)
    };
}
