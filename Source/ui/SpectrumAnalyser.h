/*
  ==============================================================================

    SpectrumAnalyser.h
    频谱仪的计算部分（v0.1.8.0 界面改版）：在界面线程里对取样口（AnalyzerTap）送来的 IN / OUT 单声道和做 FFT，
    换成 20 Hz ～ 20 kHz 对数刻度上的 numPoints 个点（dB），并带显示用的动态：

      - 显示曲线：升得快、落得慢（像电平表），填充与轮廓线用它；
      - 平均线：约 0.8 s 的慢速平均，再沿频率方向平滑（设计稿里的白线）；
      - 峰值线：保持 1.5 s 后以 12 dB/s 落下；
      - 电平：上次取走以来推进来的样本的 RMS（dBFS），中层粒子的速度与亮度跟它走。

    0 dBFS 的正弦在自己那个点上读 0 dB（Hann 窗按相干增益归一）。显示时的斜率（dB/oct，绕 1 kHz 转）由画图的一方加。
    只依赖 juce_dsp：无头测试也能编（SpectrumAnalyserTests）。

    T-032（2026-09-30）：FFT 长度封顶 16384（R9-11）；每个点在 bin 上怎么取只随采样率变，setSampleRate 算一次存表；
    IN 虚线关着时界面让这里跳过 IN 那路的加窗 + FFT（setInputSpectrumEnabled，R7-05）。

    T-036（2026-10-01，Max「更顺滑、响应更快，可以模仿 MiniMeters」）：
      - 动态按时间算：update (秒)，升 / 落 / 平均 / 峰值保持都是时间常数，界面按屏幕刷新（60 / 120 帧）推，手感与帧率无关；
        升的时间常数 10 ms（几乎一帧就到）、落 120 ms（原来 30 帧时约 36 ms / 205 ms）；
      - 「这一帧没有新样本」不再当作静音：宿主的块比一帧长时（120 帧、512 样本块），两块之间的帧继续逼近上一次的测量值；
        250 ms 都没有新样本（宿主停了）才按 24 dB/s 落下。原来那一帧就落 0.8 dB、下一块到了再跳回去，曲线「先落后跳」；
      - 每个逻辑像素一个点（480 → 1033）；点上的值 = max（bin 之间在线性幅度上的三次插值，点区间里完整落入的 bin 的最大值），
        再换成 dB：低频点比 bin 密，曲线圆滑地穿过各 bin——局部最高的 bin 用 Catmull-Rom 斜率，夹在两个 bin 之间的峰画成圆包、
        顶接近真实电平（正好落在 bin 上的正弦不过冲）；谷底与单调段保形，每段不低于两端较小者（强峰旁不出假缺口）；
        高频一个点覆盖好几个 bin，取其中最大的（保住谐波尖峰）；两段之间连续，没有原来功率线性插值的棱角；
      - 电平改成「上次取走以来」的 RMS（takeRecentRmsDb）：频谱一秒更新 60 / 120 次，粒子仍在 30 Hz 定时器里取，取到的仍是
        这 33 ms 里的全部样本，与原来同口径。

  ==============================================================================
*/

#pragma once

#include <juce_dsp/juce_dsp.h>

#include <array>
#include <vector>

namespace celadon::ui
{
    class SpectrumAnalyser
    {
    public:
        enum class Stream : int { input = 0, output = 1 };

        // 每个逻辑像素一个点：频谱仪面板宽 1032（SpectrumView::width），第 i 个点画在 x = i 上，两端落在面板左右边。
        static constexpr int numPoints = 1033;
        static constexpr float minFrequency = 20.0f;
        static constexpr float maxFrequency = 20000.0f;
        static constexpr float floorDb = -120.0f;

        // 显示动态（时间常数与速率都按秒，见 update）。
        static constexpr double riseSeconds = 0.010;          // 显示曲线升：时间常数
        static constexpr double fallSeconds = 0.120;          // ……落
        static constexpr double averageSeconds = 0.8;         // 平均线：时间常数（原来 30 帧 × 0.04 / 帧 ≈ 0.82 s）
        static constexpr double peakHoldSeconds = 1.5;        // 峰值线：保持
        static constexpr float peakFallDbPerSecond = 12.0f;   // ……之后的下落速度
        static constexpr double stallSeconds = 0.25;          // 这么久没有新样本才算宿主停了
        static constexpr float idleFallDbPerSecond = 24.0f;   // 停了之后显示曲线的下落速度
        static constexpr int averageSmoothRadius = 13;        // 平均线沿频率平滑的半径（点）：两遍，约 ±1/8 倍频程（原来 480 点时 6 点）

        SpectrumAnalyser();

        // 采样率变了（或第一次）：按它选 FFT 长度（≤ 50 kHz 用 8192 点，更高一律 16384），重算每个点的取法表，清空历史。
        void setSampleRate (double newSampleRate);
        double getSampleRate() const noexcept                  { return sampleRate; }
        int getFftSize() const noexcept                        { return fftSize; }

        // 追加新样本（按到达顺序）。
        void push (Stream stream, const float* samples, int numSamples);

        // 每帧调一次，elapsedSeconds = 距上一次 update 的时间（界面按屏幕刷新推：60 帧时约 1 / 60 s；负数、非有限当 0，
        // 超过 0.25 s 按 0.25 s）。有新样本的那一路对最新的 FFT 长度个样本做一次变换，显示曲线按升 / 落向它靠近；
        // 没有新样本时继续靠近上一次的测量值，停够 stallSeconds 才往下落。平均线、峰值线跟着 OUT 的显示曲线走。
        void update (double elapsedSeconds);

        // 第 i 个点的频率（Hz）与它在 [0, 1] 上的横向位置（对数刻度）。
        static float frequencyAt (int index) noexcept;
        static float positionOf (float frequency) noexcept;

        const std::vector<float>& getDisplayDb (Stream stream) const noexcept   { return channels[(size_t) stream].display; }
        const std::vector<float>& getAverageDb() const noexcept                 { return averageSmoothed; }
        const std::vector<float>& getPeakDb() const noexcept                    { return peak; }

        // 最近一次测量（没有动态）：显示曲线正在靠近的值。测试与诊断用。
        const std::vector<float>& getMeasuredDb (Stream stream) const noexcept  { return channels[(size_t) stream].measured; }

        // 某个频率上显示曲线的值（在相邻两点之间按对数频率插值）。
        float displayDbAt (Stream stream, float frequency) const noexcept;

        // 上次取走以来推进来的样本的 RMS（dBFS），取完清零；这段时间没有样本时为 floorDb。
        float takeRecentRmsDb (Stream stream) noexcept;

        // 整体清零（编辑器刚打开、采样率变了）。
        void reset();

        // IN 那一路做不做频谱（加窗 + FFT）。界面上 IN 虚线关着时不做：样本照收（环形缓冲、RMS 照常），只省掉加窗与 FFT；
        // 关的一刻 IN 的显示曲线落到底线。重新打开后，下一次有 IN 新样本的 update() 直接取那一帧的测量值（不从底线按「升」慢慢爬），
        // 在那之前 hasInputSpectrum() 为假（界面据此先不画 IN）。默认开。
        void setInputSpectrumEnabled (bool shouldAnalyse) noexcept;
        bool isInputSpectrumEnabled() const noexcept            { return inputSpectrumEnabled; }
        bool hasInputSpectrum() const noexcept                  { return inputSpectrumEnabled && ! inputSeedPending; }

        // OUT 换了来源（A / B 切换、试听差值开关）：OUT 的历史、测量、显示、平均线与峰值线清掉重来——前一种信号的峰值不再保持 1.5 s、
        // 平均线不再慢慢退，曲线只描述此刻听到的。平均线与峰值线等 OUT 攒满一整个 FFT 窗口的新样本后，从那一刻的显示曲线起步。
        // IN 不动。
        void restartOutput();
        bool isRestartingOutput() const noexcept                { return outputSamplesSinceRestart >= 0; }

    private:
        struct Channel
        {
            std::vector<float> history;       // 环形缓冲，长度 = FFT 长度
            int writeIndex = 0;
            int newSamples = 0;               // 上次 update 以来新到的样本数
            double secondsWithoutSamples = 0.0;
            double rmsEnergy = 0.0;           // 上次 takeRecentRmsDb 以来的平方和与样本数
            int rmsCount = 0;
            std::vector<float> measured;      // 最近一次测量（dB）
            std::vector<float> display;       // 显示曲线（dB）
        };

        // 一个显示点在 bin 上的取法（setSampleRate 里算好）：中心频率落在 bin、bin + 1 之间，按 Hermite 基函数的四个权重
        // 做三次插值；[firstInside, lastInside] 是完整落在点区间里的 bin（可能为空：first > last），取其中最大的与插值比大。
        struct PointRange
        {
            int bin = 0;
            float h00 = 1.0f, h10 = 0.0f, h01 = 0.0f, h11 = 0.0f;
            int firstInside = 1, lastInside = 0;
        };

        void updatePointRanges();
        void analyse (Channel& channel);
        void approach (Channel& channel, float rise, float fall) noexcept;
        void smoothAverage();

        double sampleRate = 48000.0;
        int fftOrder = 13;
        int fftSize = 1 << 13;
        std::unique_ptr<juce::dsp::FFT> fft;
        std::vector<float> window, fftBuffer;
        float windowGain = 1.0f;

        std::vector<PointRange> pointRanges;
        int lastBinUsed = 0;                  // 取法表用到的最高 bin（插值要再往上看一个）
        std::vector<float> binMagnitude, binSlope;   // 每帧：各 bin 的线性幅度与三次插值的斜率（幅度 / bin）

        std::array<Channel, 2> channels;
        std::vector<float> average, averageSmoothed, peak, scratch;
        std::vector<double> prefix;           // 平均线平滑用的前缀和
        std::vector<double> peakHoldLeft;     // 每点峰值还要保持的秒数
        int outputSamplesSinceRestart = -1;   // ≥ 0：restartOutput 之后 OUT 已到的新样本数（攒满 FFT 长度才恢复平均线与峰值线）
        bool inputSpectrumEnabled = true;
        bool inputSeedPending = false;        // IN 刚重新打开：下一次分析直接取测量值

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrumAnalyser)
    };
}
