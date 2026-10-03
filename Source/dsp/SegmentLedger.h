/*
  ==============================================================================

    SegmentLedger.h
    T-038 House 档按小节记账（设计规格-曲风-House学习方式 第 2、3、5 节；ADR-026）。只在曲风为 House 子类时由分析器使用，
    通用档一个函数都不调（分析器在通用档不碰它，逐位同改前）。分析器是唯一的使用者，接口按它的需要定。

    一、分段（beginBlock：分析器每块 processInput 开头调，先于这一块的任何数据）
      - 一段 = 一小节：按宿主的 PPQ、速度、拍号定位（小节起点优先用 ppqLastBarStart，拿不到就按 PPQ ÷ 每小节拍数推）。
        拿不到拍点（没有 PPQ、没在播放、BPM 无效，或小节长不在 minimumBarSeconds ～ maximumBarSeconds 之内）时每
        Constants::house.fallbackSegmentSeconds 秒一段：宿主给了位置就按位置分格（重放同一位置照样覆盖），没有位置就按分析器时间轴顺排（不去重）。
      - 段边界是整数样本：从锚点出发按恒定速度推，第 n 段起点 = 第 0 段起点 + round(n × 每段样本数)。只在速度 / 拍号 / 分段方式变了、
        宿主的 PPQ（或位置）与锚点推算的差出 positionToleranceSeconds（同编排器判跳播的 50 ms）、或遇到不连续点时重新锚定——
        恒定速度下与宿主怎么分块无关（逐位相同）；变速时每次重新锚定，允许偏差。
      - 段的身份 = (分段方式, 是否按位置, 位置键)。小节的键 = round(小节起点 PPQ × keyScale)（与拍号历史无关，换过拍号也不撞）；
        按位置的 2 秒段的键 = 宿主位置 ÷ 段长（向下取整）。
      - 播放从段中间开始时，这一次实例从开始播放的那个样本起（start 晚于段的起点）；「听到一半」按整段的标称长度算。

    二、记账（openSegmentAt：分析器三路数据各按自己的时间戳找所在的段，跨段边界的按起点归属）
      - 还没定稿的段（最多 maxOpenSegments 个，在固定池里）直接由分析器往 Segment 里累加：可以相加的量存累计和
        （K 加权能量与子块数、27 段功率、频谱证据、低频起伏直方图、谱通量计数、立体声功率与互相关、起音有效时长与事件、次声能量），
        按百分位算的量存逐块值（400 ms 门块功率、3 s 短期响度的直方图格号、先试后用每一路的最大 8 个块峰值）。真峰值取最大，按段存最大值。
      - 找不到段（数据早于账本的起点、所在的段已经定稿）就不收：分析器照常累计它自己的通用画像。

    三、定稿（finalizeBefore：分析器在三路数据都越过某个时刻之后调）
      - 段尾早于这个时刻的段，数据已经交齐：算段的有效时长、段响度、段的低频占比，判「已听」（有效音频 ≥ 标称长度的 heardFraction），
        再按身份存进账本（容量 maxSegments，prepare 里分配；满了以后不再收新段，isStoreFull 报告；没有一个有效音频子块的段不存：
        它对门控与画像都没有贡献，宿主停着时插件照常收的静音也就不会把账本塞满）。同一位置再听一遍：新实例「已听」
        或有效音频不比旧的少就覆盖旧值，不重复计入（与「代表性」按 100 ms 格记听过的位置是同一思路）；不按位置的段从不去重。
      - 不连续点（endEpoch）：账本没定稿的段全部就地定稿（分析器在不连续点丢掉跨越它的窗口，之后不会再有这些段的数据），下一块重新锚定。
      - 每定稿一段就重算一次门控（按段的先后，一段一算：同一时刻定稿几段与分几次定稿结果相同，所以与宿主分块无关）。

    四、门控（设计规格第 2.3 节）
      - 段响度 = 段内过了现有「有效音频」门的 100 ms 子块的 K 加权能量平均（LUFS，与 IN 表同一测点、同一口径）；
        段的低频占比 = 段内帧门之上各帧 100 Hz 段及以下的功率 ÷ 全频功率（与画像 lowShare 同一口径）。
      - 响度门：段响度 ≥ S_ref − ΔG；S_ref = 已听各段段响度的第 95 百分位，按段的有效时长加权。
        低频门：段 lowShare ≥ 低频参考 × 系数；低频参考 = 已过响度门各段 lowShare 的第 95 百分位（同样按有效时长加权：同一口径，
        听了半小节的段不与整小节等权）。两个百分位都用定长直方图取格中心（响度 0.1 LU 一格，同分析器 I / LRA 的直方图；占比 0.001 一格）。
      - 两道门都过 = 满编段（Segment::full）。门是相对的：drop 一来 S_ref 与低频参考上移，前奏小节自动掉出门外，不用清空重听。
      - 新段（定稿时）比「这一段并入之前」的 S_ref 高 ΔG 以上、且过了那时的低频门 = 新响段（surge）：编排器据此用快过渡。

    五、给编排器的读数
      - getSummary：门控结果（画像格式 3 的 houseGate 就是它；reopenCount 由编排器填）；
      - readEvents：每定稿一段一条事件（段响度、低频占比、是否已听 / 满编 / 新响段、所属的连续时间段），环形缓冲按序号读，
        编排器据此判「锁定后连续 4 小节更响更满」；
      - isCurrentSegmentFull / getCurrentSegmentSeconds：此刻正在播的这一段（还没定稿）按当前的门暂判是不是满编段、它的标称时长
        （House 反相提示的资格与「持续 4 小节」换成的秒数）；
      - getCurrentReading：同一段的暂判读数（段响度、低频占比），或「安静」——播够了 minimumProvisionalSeconds、有效音频却不够
        （过不了分析器「有效音频」门的 breakdown、静音）。安静的段绝不是满编段，也绝不沿用上一段的结果（T-038 评审 F1 返修）；
        只有刚开始、自己还读不出的段才退回紧挨着的上一段（还开着的前一段；没有就是最近定稿的那一段，存没存进账本都算）。

    实时约束：全部状态定长；prepare 分配账本（约 1.8 MB），音频线程只拷贝、清零，不分配、不加锁。reset 只把计数归零（账本里旧段的内容
    下次用到那一格时整体覆盖），不清零 1.8 MB。

  ==============================================================================
*/

#pragma once

#include "GlazeConstants.h"
#include "GlazeTypes.h"

#include <array>
#include <cstdint>
#include <vector>

namespace celadon::glaze
{
    class SegmentSink;   // T-048：段定稿的接收者，定义在本文件末尾

    class SegmentLedger
    {
    public:
        static constexpr int maxSegments = 512;           // 124 BPM 约 16.5 分钟；满了不再收新段（isStoreFull）
        static constexpr int maxOpenSegments = 8;         // 还没定稿的段（等三路数据都越过段尾；平常 1 ～ 2 个）
        static constexpr int maxSubBlocks = 64;           // 每段最多记的 100 ms 子块 / 先试后用块（段长上限 6.4 s）
        static constexpr int maxEvents = 128;             // 每段最多记的起音事件强度（50 ms 不应期：6.4 s 最多 128 个）
        static constexpr int trialSeries = 7;             // 先试后用：原始 + 6 个候选（低切、收单 ×4、加宽）
        static constexpr int trialTopCount = 8;           // 每一路留最大的 8 个块峰值（同分析器：≤ 512 块时 P99 只用到前 7 个）
        static constexpr int trialCapacity = 512;         // 合并时最多用这么多块（整段整段地取，同分析器的 512 块上限）
        static constexpr int blendSeries = 2;             // 守护混合损失候选（T-043，clayBlendCandidateHz 的 10 / 20 Hz；同分析器）
        static constexpr int lowRangeBins = 280;          // 低频逐帧电平直方图（−120 … +20 dB，0.5 dB 一格；同分析器）
        static constexpr int lowShareLastBand = 4;        // 低频 = 100 Hz 段及以下（段 0 ～ 4；同分析器 lowShare）
        static constexpr int eventCapacity = 32;          // 定稿事件的环形缓冲

        static constexpr double minimumBarSeconds = 0.5;  // 小节长在这个范围外按「拿不到拍点」处理（4/4 约 37.5 ～ 480 BPM）
        static constexpr double maximumBarSeconds = 6.4;  // = maxSubBlocks × 100 ms
        static constexpr double heardFraction = 0.5;      // 有效音频 ≥ 标称长度的一半才算「已听」这一段
        static constexpr double minimumLowShareSeconds = 0.5;   // 段的低频占比至少要 0.5 s 帧门之上的帧
        static constexpr double minimumProvisionalSeconds = 0.5;// 正在播的段至少有这么多有效音频，才按它自己的读数暂判满编
        static constexpr double positionToleranceSeconds = 0.05;// 宿主 PPQ / 位置与锚点推算的差在这之内不重新锚定（同编排器）
        static constexpr double tempoTolerance = 1.0e-6;  // 速度相对变化在这之内算没变（宿主的速度读数偶有末位抖动）
        static constexpr double keyScale = 16.0;          // 小节键 = round(小节起点 PPQ × 16)
        static constexpr double gatePercentile = 0.95;

        // 门控直方图：段响度 −70 … +10 LUFS、0.1 LU 一格（同分析器 I / LRA）；低频占比 0 … 1、0.001 一格。
        static constexpr double loudnessHistogramMinLufs = -70.0;
        static constexpr int loudnessBinsPerLu = 10;
        static constexpr int loudnessBins = 80 * loudnessBinsPerLu;
        static constexpr int lowShareBins = 1000;

        //==============================================================================
        // 一段的账（分析器直接往还没定稿的段里累加；定稿后只读）。各项的口径见分析器 MusicAnalyzer.cpp「House 档」一节。
        struct Segment
        {
            // ---- 身份与时间（分段时定）----
            SegmentMode mode = SegmentMode::none;
            bool positioned = false;              // 按宿主位置定的段（小节、带位置的 2 秒段）：同一位置再听覆盖旧值
            std::int64_t key = 0;                 // 位置键（见文件头第一节）；不按位置的段不用
            std::int64_t start = 0, end = 0;      // 这一次实例在分析器时间轴上的 [start, end)
            std::int64_t nominalLength = 0;       // 整段的标称长度（样本）：「已听」按它算
            std::uint32_t epoch = 0;              // 所属的连续时间段（不连续点、换曲风之后 + 1）

            // ---- 子块流（100 ms，按子块起点归属）----
            int subBlocks = 0;
            int effectiveSubBlocks = 0;           // 过了现有「有效音频」门的子块
            double effectiveEnergy = 0.0;         // 这些子块的 K 加权均方之和
            float truePeak = 0.0f;                // 归到本段的子块真峰值的最大值（线性）
            int gatingCount = 0;
            std::array<float, (size_t) maxSubBlocks> gatingPower {};      // 400 ms 门块功率（绝对门之上；相对门在合并时算）
            int rangeCount = 0;
            double rangeEnergy = 0.0;
            std::array<std::uint16_t, (size_t) maxSubBlocks> rangeBin {}; // 3 s 短期响度（绝对门之上）的直方图格号

            // ---- 帧流（Balance 帧，按分析窗起点归属）----
            std::int64_t frames = 0;              // 帧门之上的帧
            double frameSeconds = 0.0;            // 这些帧的时长（帧数 × hop）
            std::array<double, (size_t) numBands> powerSum {}, tameSum {}, recoverSum {}, deficitWeight {}, validWeight {};
            std::array<double, (size_t) numBands> stereoPowerSum {}, stereoCrossSum {}, stereoWeight {};
            double lowStereoWeight = 0.0, highStereoWeight = 0.0;
            std::array<std::uint16_t, (size_t) lowRangeBins> lowRangeCount {};
            int lowRangeFirst = lowRangeBins, lowRangeLast = -1;          // 有计数的格的范围（合并时只扫这一段）
            std::int64_t fluxFrames = 0, musicFluxFrames = 0, fluxEvents = 0;
            double fluxSum = 0.0;

            // ---- 样本流（起音组，按组起点归属；起音事件按触发的那一组归属）----
            std::int64_t onsetEffectiveSamples = 0;
            double subEnergy = 0.0, midEnergy = 0.0;
            int onsetEvents = 0;
            int strengthCount = 0;
            std::array<std::uint8_t, (size_t) maxEvents> strengthBin {};

            // ---- 先试后用（100 ms 块，按块起点归属）----
            int trialBlocks = 0;
            std::array<std::array<double, (size_t) trialTopCount>, (size_t) trialSeries> trialTop {};   // 每一路最大的 8 个（降序）
            std::array<double, (size_t) blendSeries> blendIntended {}, blendOutput {};   // 混合损失候选：各有效块两份 K 加权能量之和

            // ---- T-048 实时试算（口径同上面三项，只差「什么时候记」；只给实时画像 LiveProfile 读）----
            // 分析器有两份账（MusicAnalyzer.cpp 第「七」节）：
            //   - House 门控那一份：上面三项与下面三项都只在学习期间记、数值相同（House 门控画像只用学习区间的试算；实时模式开不开都一样，
            //     所以 House 方案逐位不变）；
            //   - 实时段落账（实时模式开着才有）：下面三项在候选真跑的每一块都记（候选在实时模式下一直跑），上面三项仍只在学习期间记。
            //     它的三道相对门（帧门、子块有效门、起音组有效门）参考它自己的长期参考，不参考学习区间（同一节 ①～⑤），所以除上面三项外，
            //     段与学不学习、宿主块长无关。
            int liveTrialBlocks = 0;
            std::array<std::array<double, (size_t) trialTopCount>, (size_t) trialSeries> liveTrialTop {};
            std::array<double, (size_t) blendSeries> liveBlendIntended {}, liveBlendOutput {};

            // ---- 定稿时算出 ----
            double effectiveSeconds = 0.0;
            bool heard = false;
            bool loudnessValid = false;
            float loudnessLufs = 0.0f;
            bool lowShareValid = false;
            float lowShare = 0.0f;
            bool full = false;                    // 满编段（每次门控重算都更新）
        };

        // 每定稿一段一条（readEvents）。
        struct Event
        {
            std::uint64_t sequence = 0;           // 从 0 起的序号（跨 reset 单调）
            std::uint32_t epoch = 0;
            bool stored = false;                  // 存进了账本（没有有效音频的段、账本满了 = false）
            bool heard = false;
            bool full = false;                    // 这一段并入后的门控结果
            bool surge = false;                   // 新响段（见文件头第四节）
            bool loudnessValid = false;
            float loudnessLufs = 0.0f;
            bool lowShareValid = false;
            float lowShare = 0.0f;
        };

        SegmentLedger() = default;

        // 分配账本（消息线程 / prepareToPlay），并清空。subBlockLength = IN 表子块长（样本）。
        void prepare (double sampleRate, int subBlockLength);

        // 子块长随 IN 表（分析器发现表的子块长变了时调；不分配）。
        void setSubBlockLength (int subBlockLength) noexcept;

        // 门控与分段用的常数（编排器经分析器交来；只拷贝、清洗几个数）。
        void setConstants (const HouseConstants& constants) noexcept;

        // T-048：段定稿的接收者（nullptr = 不交）。只存指针，不拥有；在开始处理音频之前设，处理中不换。
        void setSink (SegmentSink* newSink) noexcept   { sink = newSink; }
        SegmentSink* getSink() const noexcept          { return sink; }

        // 清空段落账（「重新聆听」、宿主复位、读取工程）。acceptFrom = 门块 / 短期响度窗口最早的样本不早于它才收（同分析器学习区间）。
        // 不分配、不清零账本的内容（只把计数归零）。事件序号与新响段计数跨 reset 单调。
        void reset (std::int64_t acceptFrom = 0) noexcept;

        // ---- 分段（音频线程）----
        void beginBlock (std::int64_t blockStart, int numSamples, const HostInfo& host) noexcept;

        // 不连续点 / 换曲风：没定稿的段就地定稿，下一块重新锚定。acceptFrom 同 reset。
        void endEpoch (std::int64_t acceptFrom) noexcept;

        // T-048：同 endEpoch，但先把开着的段截到 dataEnd（数据只送到这里；之后的样本属于下一条时间轴的新段）再就地定稿——
        // 交出去的段 [start, end) 首尾相接（分析器实时模式用；House 门控那一份照旧用 endEpoch，不截）。start 晚于 dataEnd 的段截成空段。
        void endEpochAt (std::int64_t dataEnd, std::int64_t acceptFrom) noexcept;

        // ---- 记账（音频线程）----
        // time 所在的、还没定稿的段；没有就是 nullptr。hint = 调用方自己的缓存（每路一个，初值 −1），连续查同一段时不扫池。
        Segment* openSegmentAt (std::int64_t time, int& hint) noexcept;
        std::int64_t getAcceptFrom() const noexcept   { return acceptFrom; }

        // ---- 定稿（音频线程）----
        // 段尾 ≤ completeBefore 的段全部定稿（按先后一段一段来）。返回有没有定稿。
        bool finalizeBefore (std::int64_t completeBefore) noexcept;

        // ---- 读数 ----
        const HouseGateSummary& getSummary() const noexcept   { return summary; }
        int getNumStored() const noexcept                     { return numStored; }
        const Segment& getStored (int index) const noexcept   { return store[(size_t) index]; }
        bool isStoreFull() const noexcept                     { return storeFull; }
        SegmentMode getMode() const noexcept;
        int getHeardSegments() const noexcept                 { return summary.heardSegments; }
        std::uint32_t getEpoch() const noexcept               { return epoch; }
        std::uint32_t getSurgeCount() const noexcept          { return surgeCount; }
        std::uint32_t getFinalizedCount() const noexcept      { return finalizedCount; }
        // 账本内容真变了的次数（新存进一段或覆盖了旧段；没有有效音频的段、账本满了、重放不如旧值的不算）。T-038 评审 F3：
        // 分析器只在它变了时重算画像，静音段与账本满后的段不再白算、编排器也不再为没变的画像发暂定过渡。
        std::uint32_t getStoredRevision() const noexcept      { return storedRevision; }
        bool isCurrentSegmentFull() const noexcept;
        double getCurrentSegmentSeconds() const noexcept;
        // T-038 集成（评审 F1）：此刻正在播的这一段的暂判（文件头第五节）。measured = 读数可比（段响度 LUFS、低频占比，口径同定稿，
        // 写进两个输出）；quiet = 安静的段（有效音频不够），输出不改；none = 读不出（刚开始且同一段连续时间里没有定稿过的段、宿主停着），
        // 输出不改。编排器拿 measured 的读数与**已提交画像**比，判「此刻是否在主段落」（House 档只在主段落校准），quiet 就是不在——
        // 不用段落账自己的相对门：读档后段落账从空开始，门控结果要很久才有。isCurrentSegmentFull = measured 且过此刻的门。
        enum class Reading { none, quiet, measured };
        Reading getCurrentReading (float& loudnessLufs, float& lowShare) const noexcept;
        int getOpenSegments() const noexcept                  { return openCount; }

        // 读 cursor 之后的事件（最多 maxCount 条），cursor 推进到读完的位置。读者落后超过环形容量：跳到能读到的最早一条，返回负的条数。
        int readEvents (std::uint64_t& cursor, Event* out, int maxCount) const noexcept;
        std::uint64_t getEventCount() const noexcept          { return eventTotal; }

    private:
        // 分段方式与锚点（见文件头第一节）。
        struct Regime
        {
            SegmentMode mode = SegmentMode::none;
            bool positioned = false;
            double samplesPerSegment = 0.0;       // 小节：每小节样本数；2 秒段：段长
            std::int64_t segmentSamples = 0;      // 2 秒段的整数段长
            double bpm = 0.0, samplesPerQuarter = 0.0, quartersPerBar = 0.0;
            int numerator = 4, denominator = 4;
            std::int64_t anchorTime = 0;          // 锚定时的分析器时间（块起点）
            double anchorPpq = 0.0;               // 锚定时宿主的 PPQ
            std::int64_t anchorPosition = 0;      // 锚定时宿主的样本位置
            std::int64_t originSample = 0;        // 第 0 段的起点（可早于 anchorTime）
            double originPpq = 0.0;               // 第 0 段（小节）起点的 PPQ
            std::int64_t originCell = 0;          // 第 0 段（带位置的 2 秒段）的格号
        };

        Regime regimeFor (const HostInfo& host) const noexcept;
        bool consistent (const Regime& wanted, std::int64_t blockStart, const HostInfo& host) const noexcept;
        void anchor (const Regime& wanted, std::int64_t blockStart, const HostInfo& host) noexcept;
        std::int64_t boundary (std::int64_t index) const noexcept;
        std::int64_t keyOf (std::int64_t index) const noexcept;
        std::int64_t indexAt (std::int64_t time) const noexcept;
        void openSegment (std::int64_t index, std::int64_t start) noexcept;
        Segment& newest() noexcept               { return pool[(size_t) order[(size_t) ((openHead + openCount - 1) % maxOpenSegments)]]; }
        const Segment& newest() const noexcept   { return pool[(size_t) order[(size_t) ((openHead + openCount - 1) % maxOpenSegments)]]; }
        void finalizeOldest() noexcept;
        void deriveSegment (Segment& segment) const noexcept;
        int commit (const Segment& segment) noexcept;   // 返回存进的格（−1 = 没存）
        void recomputeGate() noexcept;
        bool passesGate (bool heard, bool loudnessValid, float loudnessLufs, bool lowShareValid, float lowShare) const noexcept;
        Reading readOpen (const Segment& open, float& loudnessLufs, float& lowShare) const noexcept;
        Reading readLastFinal (float& loudnessLufs, float& lowShare) const noexcept;

        double sampleRate = 48000.0;
        int subBlockLength = 4800;
        std::int64_t positionTolerance = 2400;
        std::int64_t fallbackSamples = 96000;
        float gateWidthLu = 3.0f, lowGateFactor = 0.6f;
        HouseConstants constants;

        Regime regime;
        bool anchored = false;
        std::int64_t newestIndex = 0;             // 最新那个开着的段在当前锚点下的序号
        std::int64_t acceptFrom = 0;
        std::uint32_t epoch = 0;

        std::array<Segment, (size_t) maxOpenSegments> pool {};
        std::array<bool, (size_t) maxOpenSegments> poolUsed {};
        std::array<int, (size_t) maxOpenSegments> order {};   // 开着的段按时间先后（环形：openHead 起 openCount 个）
        int openHead = 0, openCount = 0;

        std::vector<Segment> store;               // prepare 分配 maxSegments 格；前 numStored 格有效（格序 = 第一次存进的先后）
        int numStored = 0;
        bool storeFull = false;
        int lastStoredSlot = -1;                  // 最近存进账本的那一格（getMode 在没有开着的段时用它的分段方式）

        // 最近定稿的那一段（存没存进账本都记）：此刻这一段刚开始、自己还读不出时的退路。不用「最近存进的那一格」——有效音频为 0 的段
        // 不进账本，安静的 breakdown 整段都会继承前面 drop 最后一小节的结果（T-038 评审 F1 返修）。上一段还在等定稿时这里是再前一段，
        // 所以退路先看还开着的前几段，最后才看这里。
        struct LastFinal
        {
            bool valid = false;
            std::uint32_t epoch = 0;
            bool heard = false;
            bool loudnessValid = false;
            float loudnessLufs = 0.0f;
            bool lowShareValid = false;
            float lowShare = 0.0f;
        };
        LastFinal lastFinal;

        HouseGateSummary summary;
        bool lowReferenceValid = false;
        std::array<double, (size_t) loudnessBins> loudnessWeights {};
        std::array<double, (size_t) lowShareBins> lowShareWeights {};

        std::array<Event, (size_t) eventCapacity> events {};
        std::uint64_t eventTotal = 0;
        std::uint32_t surgeCount = 0;
        std::uint32_t finalizedCount = 0;
        std::uint32_t storedRevision = 0;

        SegmentSink* sink = nullptr;              // T-048：见 setSink
    };

    // T-048 段定稿的接收者（实时画像 LiveProfile 实现它；设计规格-实时GLAZE-主段与增益 第 1 节）。
    // SegmentLedger 每定稿一段调一次 segmentFinalized：不论这一段存没存进账本（没有有效音频、账本满了、重放不如旧值都照样交），
    // 交的是 deriveSegment 之后的段（effectiveSeconds / loudnessLufs / lowShare / heard 已算好；full 是账本自己的门控结果，接收者不用）。
    // 不连续点（endEpoch）就地定稿的段同样交。音频线程调用：实现必须 0 分配、0 锁、不抛异常。
    class SegmentSink
    {
    public:
        virtual ~SegmentSink() = default;
        virtual void segmentFinalized (const SegmentLedger::Segment& segment) noexcept = 0;

        // T-048 阶段 1 修订（Max 2026-10-02 选「回看重判」）：分析器每处理完一块（processInput、processSpectrum 末尾各一次）报告实时段落账
        // **交齐的位置** T（样本，与 Segment 的 start / end 同一条实时时间轴；分析器 / 表 reset、prepare 之后接着往后数，单调不减）：
        // 报 T 时 end ≤ T 的段都已交出，之后交来的段 end > T。T 比此刻的输入位置晚（段要等四路数据都越过段尾才定稿，约 0.1～0.3 s 再加一块）；
        // 正因为报交齐位置而不是此刻，接收者在 ≤ T 的**绝对样本网格**上安排的工作（实时画像的回看重算分片、按时刻发布）只取决于段序列与 T，
        // 不取决于被调了几次、宿主块多长（MusicAnalyzer.h「时间轴与 advanceTo」）。默认什么都不做。音频线程：0 分配、0 锁、不抛异常。
        virtual void advanceTo (std::int64_t analyzerTime) noexcept   { (void) analyzerTime; }
    };
}
