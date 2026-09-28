/*
  ==============================================================================

    BalanceModule.h
    Balance 模块：Gullfoss 式动态频谱平衡（设计规格-Balance，方案 B1；细节定法见
    04-协调/2026-09-24-v0.3-Balance实现计划「规格没写死、写码前定下的细节」表）。

    每 hop（约 10 ms）一次分析：
      两声道功率谱取平均 → 27 个 1/3 倍频程段能量 L[b]（dB）
      → 快跟踪 Lfast[b]（按 Speed 的攻击 / 释放）
      → 参考轮廓 R[b] = 形状 S[b]（Lfast 频率方向 [1,2,3,4,3,2,1]/16 抹平 + 按 Speed 的慢时间平滑）
                       + 全段共同电平 C（各段 60 ms 快电平的稳健位置估计，整体变响 / 变轻时立刻跟上）
      → 偏差 D[b] = Lfast − R → 死区 ±1 dB → Tame 压 / Recover 提
        Tame 增强只作用于同帧频谱中的局部峰，共同电平变化保留原有基础力度
        自动 Tame 份额另需两侧宽带支撑与持续局部峰，原始帧起音触发短暂保护；同一声道清晰的 1:2:3 谱线关系减轻自动削减
        自动新增削减最多 2.5 dB；谐波关系只作保守保护证据，不识别人声或乐器
        Recover 与 Tame 同时打开时，另按上行掩蔽把「已经被压下的主导段」分一部分给更高、仍有内容的被掩蔽邻段，
        再用 A 计权把这部分的净增益拉回 0 附近。只有一侧打开、或谱本身已经平，这条路径不改目标。
        这是公开心理声学里的上行扩展，不是 Gullfoss 的听觉模型，也不声称与 Gullfoss Master 等价。
      → 独立叠加 Brighten 倾斜（每十倍频程 3 dB × Brighten / 100）
      → 每段合计限制 −8 ～ +6 dB、−60 dBFS 静音门、每 hop 最多变 1 dB
      → 修正量 G[b]（dB）。
    执行：27 个 RBJ 峰值滤波器（Q ≈ 4.318，double 精度），两声道同一组增益；增益在两次 hop 之间按样本线性插值，
    每 64 样本重算一次系数。零延迟；Recover = Tame = Brighten = 0 时 G 恒为 0 → 逐位直通。
    只依赖 juce_dsp，可无头编译做自动测试。

    v0.5 性能优化（2026-09-25，研究 P4 R3 / R4 / R5），输出与 v0.4 的关系见各条：
      - 滤波：样本在外层、段在内层；AArch64 上两声道打包成 float64x2 一起算（每步运算与标量版相同 → 逐位相同），
        其他平台与单声道走标量（同一个 processBiquad）。
      - 增益精确为 0 的段，状态低于 1e-10 就归零（修 v0.4「Recover / Tame 拧回 0 后几段一直停不下、不再逐位直通」）。
        这一条与 v0.4 不逐位相同：只在归零那一刻差 ≤ 1e-10 的状态，输出差在 1 个 float LSB 以内。
      - 每 hop 的分析拆成三片、摊到 hop 后第 0 / 32 / 64 个样本上做（新目标最早在第 64 个样本那次系数重算才用到），
        削掉小块时的最坏块尖峰；G 与输出逐位不变，界面读到的 G 晚 64 个样本发布。

    v0.6（GLAZE，设计规格-GLAZE-分析器第 4 节）：每帧原始段功率快照放进固定容量的环形缓冲（readFramesSince），给分析器读
    频谱证据。只读 measureBandPowers 的结果、只多存一份，G 与输出逐位不变（改动前后摘要对照见 v0.6 A1 报告）。

    v0.1.7.0（修坯 Clay，设计规格-Clay-分析决策与验收 1.1）：片 2 在两声道都变换完之后，按与 bandPower 相同的段边界、bin 权重
    （含 spectrumScale）与段内求和顺序，多累加一份左右交叉功率 X[b] = Σ w·Re(X_L · conj(X_R))，随同一帧快照发布
    （FrameSnapshot::bandCross；分析声道数为 1 时 = bandPower）。交叉功率由 const 函数从已有的 FFT 结果读出、只写进新数组，
    G 与输出逐位不变；不新增 FFT，每 hop 约 800 次复数乘加（27 段只覆盖约 35 Hz ～ 18 kHz，与采样率无关）。
    精确运算下 |X| ≤ bandPower（柯西-施瓦茨），发布前钳进 [−bandPower, bandPower]，只去掉舍入越界：读者由快照算出的
    P_M = (bandPower + X) / 2、P_S = (bandPower − X) / 2 永不为负，ρ = X / bandPower 永在 [−1, 1]。

  ==============================================================================
*/

#pragma once

#include <juce_dsp/juce_dsp.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace celadon
{
    class BalanceModule
    {
    public:
        static constexpr int numBands = 27;

        enum class Speed { slow = 0, mid = 1, fast = 2 };

        struct Params
        {
            float recover  = 0.0f;       // 0 ～ 100
            float tame     = 0.0f;       // 0 ～ 100
            float brighten = 0.0f;       // −100 ～ +100
            Speed speed    = Speed::mid;
            bool  bypass   = false;
            float automaticRecover = 0.0f; // 0 ～ recover：Recover 中由自动编排加入的份额；仅这部分保护滚降/短暂缺口
            float automaticTame = 0.0f;    // 0 ～ tame：Tame 中的自动份额；逐段持续许可/起音保护，新增削减最多 2.5 dB
        };

        using BandGains = std::array<float, (size_t) numBands>;

        BalanceModule() = default;

        void prepare (const juce::dsp::ProcessSpec& spec);

        // 清空全部状态（分析历史、滤波器、修正量归零）；平滑器直接落到当前目标。
        void reset();

        // 音频线程每块调一次。
        void setParams (const Params& newParams);

        // 原地处理；样本数可以超过 prepare 时的块大小（内部切块）。
        void process (juce::AudioBuffer<float>& io);

        // 零延迟（分析比音频晚 10 ～ 40 ms 是「反应滞后」，不是上报延迟）。
        int getLatencySamples() const noexcept   { return 0; }

        // 各段当前修正量 G[b]（dB，正 = 提升、负 = 压下）。界面线程调用，无锁（每段一个原子变量，每 hop 更新）。
        void getBandGainsDb (BandGains& out) const noexcept;

        // 第 band 段的 1/3 倍频程中心频率（40, 50, 63, 80 … 12500, 16000 Hz）。
        static float getBandCentreHz (int band) noexcept;

        // 每帧原始段功率快照（v0.6 GLAZE 分析器读频谱证据用；设计规格-GLAZE-分析器第 4 节）。
        // bandPowerDb = 本帧 27 段的段内总功率（两声道功率平均，dB），在 Balance 自己的输入端测，未经任何时间平均；
        // endSample = 本帧最后一个样本的时间戳（自上次 reset 以来处理过的样本数，与两块表同一时间轴）：本帧的分析窗是
        // [endSample − getAnalysisWindowSamples(), endSample)。
        // 旁通时分析照跑，快照照常发布（GLAZE 依赖这一点）。
        // 发布时刻：本帧分析的最后一片（拆片时在 hop 后第 64 个样本，关掉拆片时在 hop 边界上）；两种做法发布的内容逐位相同，
        // 只是拆片时晚 64 个样本才读得到。
        struct FrameSnapshot
        {
            std::array<float, (size_t) numBands> bandPowerDb {};
            std::int64_t endSample = 0;
            std::array<float, (size_t) numBands> bandPower {};   // v0.6 A1 追加：同一帧的段功率（线性均方，与 bandPowerDb 同源，
                                                                  // 0 = 数字静音），读者做功率域统计时不必再算 27 次指数
            // v0.1.7.0 追加（修坯 Clay 的立体声分析，设计规格-Clay-分析决策与验收 1.1）：同一帧、同一段边界与 spectrumScale 下的
            // 左右交叉功率 X[b] = Σ w·Re(X_L · conj(X_R))（bandPower = Σ w·(|X_L|² + |X_R|²) / 2 的同一口径），于是
            // P_M = (bandPower + X) / 2、P_S = (bandPower − X) / 2、ρ = X / bandPower。单声道时 bandCross = bandPower。
            // 只读、多存一份：修正量 G 与音频输出逐位不变。发布前钳在 [−bandPower, bandPower]（P_M、P_S 永不为负）；本帧分析声道数
            // 为 1（含立体声 prepare 但宿主只送 1 声道的块）时等于 bandPower。
            std::array<float, (size_t) numBands> bandCross {};
        };

        static constexpr int frameRingCapacity = 64;   // 每 hop 约 10 ms，≥ 0.6 s，远大于任何宿主块

        // 读 cursor 之后新发布的帧，最多 maxCount 个，并把 cursor 推进到已读位置。读者落后超过容量时跳到最旧可用的一帧，
        // 返回值取负（-读出的个数）表示中间丢过帧。cursor 从 0 起（cursor 是帧序号，不是样本数）；只在调用 process 的线程上调。
        // cursor 比已发布的帧数还大（模块被 reset 过、读者没跟着清）时从头读，也按「丢过」报告。
        int readFramesSince (std::int64_t& cursor, FrameSnapshot* out, int maxCount) const noexcept;

        // ---- v0.6 A1 追加（读者据此对齐时间轴、识别 reset；只在调用 process 的线程上调）----
        // 自上次 reset 以来送进分析的样本数（0 声道的块不处理，也不计数）。
        std::int64_t getProcessedSamples() const noexcept   { return processedSamples; }
        // 分析窗长（FFT 点数）与 hop（样本）：44.1 / 48 k 为 2048 / 512，采样率每翻一倍都翻一倍。prepare 之前为 0。
        int getAnalysisWindowSamples() const noexcept       { return fftSize; }
        int getAnalysisHopSamples() const noexcept          { return hopSize; }

        // 此刻在工作的峰值滤波器段数（0 ～ 27）：增益不为 0、或状态还没衰减到零的段。诊断与测试用
        // （例如「Recover / Tame 拧回 0 之后所有段都停下」）；读的是音频线程的状态，只能在调用 process 的线程上调。
        int getActiveFilterCount() const noexcept;

        // 只给测试用：false = 每 hop 的分析全部在 hop 边界那一个样本上做完（v0.4 的做法）；true（默认）= 拆成三片
        // 摊到 hop 后第 0 / 32 / 64 个样本上。两种做法的 G 与输出逐位相同（BalanceTests 对照），拆片只为削掉最坏块尖峰；
        // 只是界面读到的 G 晚 64 个样本发布（最后一片在第 64 个样本）。
        // 在 prepare 之后、处理之前调。
        void setAnalysisSlicingForTesting (bool shouldSlice) noexcept   { sliceAnalysis = shouldSlice; }

    private:
        //==============================================================================
        // 转置直接 II 型双二阶（double 精度）：系数按 a0 归一，状态每段每声道一份。
        struct BiquadCoefficients
        {
            double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
        };

        // 两个 double 紧挨着：同一段相邻两个声道的状态 {s1, s2, s1, s2} 正好能被 NEON 的 vld2q_f64 拆成
        // {s1 左, s1 右} 与 {s2 左, s2 右} 两个打包寄存器（filterStates 按「段 × 声道」排列）。
        struct BiquadState
        {
            double s1 = 0.0, s2 = 0.0;
        };

        static constexpr int numSpeeds = 3;

        template <typename T>
        using PerBand = std::array<T, (size_t) numBands>;

        static double processBiquad (double x, const BiquadCoefficients& c, BiquadState& s) noexcept;
        static void snapToZero (double& value, double threshold) noexcept;
        static double exactBandCentreHz (int band) noexcept;

        //==============================================================================
        void processChunk (juce::AudioBuffer<float>& io, int startSample, int numSamples);
        void processSegment (juce::AudioBuffer<float>& io, int startSample, int numSamples, int numChannelsToUse) noexcept;
        void filterChannelScalar (float* samples, int numSamples, int channel, const int* bands, int numActive) noexcept;
        void filterChannelPairPacked (float* first, float* second, int numSamples, int channel, const int* bands, int numActive) noexcept;   // 只在 AArch64 上定义与调用
        void pushIntoAnalysisRing (const juce::AudioBuffer<float>& io, int startSample, int numSamples, int numChannelsToUse);

        // 每 hop 的分析分三片，落在 hop 后第 0 / 32 / 64 个样本上（见 BalanceModule.cpp「分析拆片」）：
        //   片 0：提交插值起点、快照参数、各声道加窗拷出、前一半声道的 FFT；
        //   片 1：前一半声道的功率谱累加；
        //   片 2：后一半声道的 FFT 与功率谱累加、27 段求和（v0.1.7.0 起连同左右交叉功率）、跟踪、判断、发布。
        void beginAnalysis();
        void runNextAnalysisSlice();
        int firstChannelGroupSize() const noexcept;
        float* frameBuffer (int channel) noexcept;
        const float* frameBuffer (int channel) const noexcept;   // 只读版：交叉功率只经它读 FFT 结果
        void transformFrame (int channel);
        void accumulatePower (int channel) noexcept;
        void measureBandPowers() noexcept;
        void measureBandCross() noexcept;                         // 只写 bandCross（v0.1.7.0）
        double crossPowerOfBand (size_t band) const noexcept;     // 声道 0 / 1 的 FFT 结果 → 第 band 段交叉功率（只读）
        void publishFrameSnapshot() noexcept;
        void updateTrackers();
        void updateHarmonicProtection() noexcept;
        void updateCorrectionTargets();
        void publishGains() noexcept;
        void updateCoefficients();
        void designPeak (int band, double gainDb);
        void applyBypassTarget (bool immediately);

        //==============================================================================
        Params params;
        double sampleRate = 0.0;
        int maxBlockSize = 0;
        int numChannels = 0;

        // ---- 分析：FFT、Hann 窗、输入 FIFO（环形）----
        std::unique_ptr<juce::dsp::FFT> fft;
        int fftSize = 0;
        int hopSize = 0;
        double hopSeconds = 0.0;
        std::vector<float> window;              // Hann，fftSize 点
        double spectrumScale = 0.0;             // 2 / (N · Σw²)：单个 bin 的 |X|² → 该 bin 频率格内的均方功率
        std::vector<float> analysisRing;        // numChannels × fftSize，每声道一段环形缓冲
        int ringWriteIndex = 0;
        int analysedChannels = 0;               // 最近一次写入 FIFO 的声道数（功率谱按它取平均）
        std::vector<float> frameBuffers;        // numChannels × 2 × fftSize：各声道加窗后的帧，performRealOnlyForwardTransform 原地用
        std::vector<double> powerSpectrum;      // fftSize / 2 + 1，各声道功率谱之和（求段能量时再按声道数取平均）
        int samplesUntilAnalysis = 0;           // 到下一次 hop 分析还有几个样本（跨块延续，与块大小无关）
        int samplesUntilCoefficientUpdate = 0;  // 到下一次系数重算还有几个样本
        int samplesSinceHop = 0;                // 本 hop 内已过的样本数，增益线性插值用

        // ---- 分析拆片：本帧的快照与进度 ----
        bool sliceAnalysis = true;              // false：全部片在 hop 边界上一口气做完（只给测试用）
        int nextAnalysisSlice = -1;             // 下一片的编号（1 或 2）；-1 = 没有做到一半的帧
        int samplesUntilNextSlice = 0;          // 到下一片还有几个样本
        int analysisSliceSpacing = 0;           // 本帧相邻两片的间隔（样本）：拆片时 32，关掉时 0
        int frameChannels = 0;                  // 本帧分析的声道数（片 0 时的 analysedChannels）
        Params frameParams;                     // 片 0 时的参数：后面几片的跟踪 / 判断都用它，与一口气做完时逐位相同
        float frameAppliedAmount = 1.0f;        // 片 0 时「修正真正作用到输出上」的比例（1 − 旁通混合），发布给界面用

        // ---- 段 → bin：每个 bin 的功率视为均匀铺在它的频率格里，段能量 = 段范围内各格的积分（边界格按重叠比例计权）----
        PerBand<int> bandFirstBin {};           // 与段有重叠的第一个 bin
        PerBand<int> bandNumBins {};            // 连续几个 bin
        PerBand<int> bandWeightOffset {};       // 该段的权重在 binWeights 里的起点
        std::vector<double> binWeights;         // 每段每 bin 的权重 = 重叠宽度 / bin 间距（段内整格为 1）
        std::array<PerBand<double>, (size_t) numSpeeds> preAverageCoeff {};   // 每段功率域时间平均的 hop 一阶系数，按 Speed
        PerBand<double> tiltPerBrighten {};     // Brighten = +100 时各段的倾斜量（dB），从过了死区的偏差里减去

        // ---- 每 hop 的分析状态（全部 dB 或功率，按段）----
        PerBand<double> bandPower {};           // 本帧 P[b]
        PerBand<double> bandCross {};           // v0.1.7.0：本帧左右交叉功率 X[b]（与 P[b] 同口径，钳在 ±P[b] 内；分析声道数为 1
                                                // 时 = P[b]）。只进每帧快照，跟踪 / 判断 / G 的任何计算都不读它
        PerBand<double> averagedPower {};       // Welch 式时间平均后的 P[b]
        PerBand<int>    hopsAlive {};           // 段「活着」以来经过的 hop 数（运行均值的分母）
        PerBand<bool>   bandAlive {};           // 段电平在静音门之上
        PerBand<bool>   bandRevived {};         // 本 hop 刚从门下回来（或第一帧）
        PerBand<int>    hopsSinceRevival {};    // 复活以来的 hop 数：前几 hop 参考锁在偏差 0，之后参考的运行均值用它做分母
        PerBand<double> levelDb {};             // L[b]
        PerBand<double> fastLevelDb {};         // Lfast[b]
        PerBand<double> referenceShapeDb {};    // 参考轮廓的形状 S[b]（慢时间平滑）：目标参考 = S[b] + commonLevelDb
        PerBand<double> referencePower {};      // 目标参考经本段同样的功率域时间平均（与测量同步滞后）
        PerBand<double> referenceDb {};         // 再经同样的攻击 / 释放跟踪后的参考 R[b]（未加 Brighten 倾斜）
        PerBand<double> smoothedLevelDb {};     // Lfast 频率方向抹平后的值（每 hop 的临时量）
        PerBand<double> quickPower {};          // 只给共同电平 C 用的快电平：所有段同一个 60 ms 对称功率平均（评审 R1-01）
        PerBand<double> quickLevelDb {};        // quickPower 的 dB
        PerBand<double> locationValues {};      // 估计全段共同电平用的临时数组（每 hop）：各段 quickLevel − S
        PerBand<double> locationWeights {};     // 对应的静音门权重
        PerBand<double> gateWeight {};          // 静音门软权重 0 ～ 1
        PerBand<double> targetGainDb {};        // 本 hop 决定的 G[b]，下一个 hop 到达时滤波器正好等于它
        PerBand<double> automaticDeficitSeconds {}; // 自动 Recover 的连续缺口时长；仅利用现有频谱，每 hop 更新
        PerBand<double> automaticPeakSeconds {};    // 自动 Tame 的连续局部峰时长（起音保护期间不累计）
        PerBand<double> automaticOnsetHoldSeconds {};
        PerBand<double> maskingBoostDb {};         // 上行掩蔽提升的平滑状态；Recover/Tame 不同时打开时保持 0
        PerBand<double> harmonicProtection {};     // 0 ～ 1，清晰谐波组的平滑保护证据；仅缩小自动 Tame 份额
        PerBand<double> previousBandPower {};       // 起音检测用原始帧，避免慢测量的余尾继续获得自动许可
        PerBand<double> hopStartGainDb {};      // 本 hop 开始时的 G[b]，插值起点
        double commonLevelDb = 0.0;             // 全段共同的电平 C：各段 (quickLevel − S) 的稳健位置估计，每 hop 直接重估
        double quickCoeff = 0.0;                // quickPower 的 hop 一阶系数
        PerBand<double> laggedCommonPower {};   // C 经本段测量同样的功率域时间平均（形状 S 的目标用它，评审 R1-01）
        PerBand<double> laggedCommonDb {};      // 再经同样的攻击 / 释放：本段「看到的」共同电平

        // ---- 执行：27 个 RBJ 峰值滤波器 ----
        PerBand<double> cosW0 {};
        PerBand<double> alpha {};               // sin(w0) / (2Q)
        PerBand<BiquadCoefficients> coefficients {};
        PerBand<double> gainForCoefficients {}; // 当前系数对应的增益（dB），没变就不重算
        PerBand<int>    activeBands {};         // 在工作的段（系数不是直通或状态非零），按段号从低到高；其余段整段跳过（逐位直通且省 CPU）
        int numActiveBands = 0;                 // activeBands 里的有效项数；每次系数重算时重排
        std::vector<BiquadState> filterStates;  // [段 × numChannels + 声道]：同一段的各声道状态相邻，打包路径一次读两个声道

        // ---- 时间常数（hop 一阶系数，按 Speed）----
        std::array<double, (size_t) numSpeeds> attackCoeff {}, releaseCoeff {}, referenceCoeff {};

        // ---- 旁通：零延迟，与处理后信号 20 ms 交叉淡化 ----
        juce::SmoothedValue<float> bypassMixSmoothed;
        juce::AudioBuffer<float> dryScratch;

        std::array<std::atomic<float>, (size_t) numBands> publishedGainsDb {};

        // ---- v0.6 GLAZE 每帧快照（音频线程写、音频线程读，不跨线程）----
        std::vector<FrameSnapshot> frameRing;   // frameRingCapacity 帧，prepare 时分配
        std::int64_t framesWritten = 0;         // 自 reset 以来发布的帧数（= 下一帧的序号）
        std::int64_t processedSamples = 0;      // 自 reset 以来送进分析环形缓冲的样本数
        std::int64_t frameEndSample = 0;        // 本帧（片 0 时）的 endSample：分析窗最后一个样本之后的时间戳

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BalanceModule)
    };
}
