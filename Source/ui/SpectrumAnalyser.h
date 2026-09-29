/*
  ==============================================================================

    SpectrumAnalyser.h
    频谱仪的计算部分（v0.1.8.0 界面改版）：在界面线程里对取样口（AnalyzerTap）送来的 IN / OUT 单声道和做 FFT，
    换成 20 Hz ～ 20 kHz 对数刻度上的 numPoints 个点（dB），并带显示用的动态：

      - 显示曲线：升得快、落得慢（像电平表），填充与轮廓线用它；
      - 平均线：约 1 s 的慢速平均，再沿频率方向平滑（设计稿里的白线）；
      - 峰值线：保持 1.5 s 后以 12 dB/s 落下；
      - 电平：最近一帧新样本的 RMS（dBFS），中层粒子的速度与亮度跟它走。

    0 dBFS 的正弦在自己那个点上读 0 dB（Hann 窗按相干增益归一）。显示时的斜率（dB/oct，绕 1 kHz 转）由画图的一方加。
    只依赖 juce_dsp：无头测试也能编（SpectrumAnalyserTests）。

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

        static constexpr int numPoints = 480;
        static constexpr float minFrequency = 20.0f;
        static constexpr float maxFrequency = 20000.0f;
        static constexpr float floorDb = -120.0f;

        SpectrumAnalyser();

        // 采样率变了（或第一次）：按它选 FFT 长度（≤ 50 kHz 用 8192 点，更高依次加倍），清空历史。
        void setSampleRate (double newSampleRate);
        double getSampleRate() const noexcept                  { return sampleRate; }

        // 追加新样本（按到达顺序）。
        void push (Stream stream, const float* samples, int numSamples);

        // 每帧调一次：两路各对最新的 FFT 长度个样本做一次变换，更新显示 / 平均 / 峰值。没有新样本时照样让曲线按动态落下。
        void update();

        // 第 i 个点的频率（Hz）与它在 [0, 1] 上的横向位置（对数刻度）。
        static float frequencyAt (int index) noexcept;
        static float positionOf (float frequency) noexcept;

        const std::vector<float>& getDisplayDb (Stream stream) const noexcept   { return channels[(size_t) stream].display; }
        const std::vector<float>& getAverageDb() const noexcept                 { return averageSmoothed; }
        const std::vector<float>& getPeakDb() const noexcept                    { return peak; }

        // 某个频率上显示曲线的值（在相邻两点之间按对数频率插值）。
        float displayDbAt (Stream stream, float frequency) const noexcept;

        // 最近一帧新样本的 RMS（dBFS），没有新样本时为 floorDb。
        float getRecentRmsDb (Stream stream) const noexcept                     { return channels[(size_t) stream].recentRmsDb; }

        // 整体清零（编辑器刚打开、采样率变了）。
        void reset();

        // OUT 换了来源（A / B 切换、试听差值开关）：OUT 的历史、显示、平均线与峰值线清掉重来——前一种信号的峰值不再保持 1.5 s、
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
            double newEnergy = 0.0;
            std::vector<float> display;       // 显示曲线（dB）
            float recentRmsDb = floorDb;
        };

        void analyse (Channel& channel, std::vector<float>& spectrumDb);

        double sampleRate = 48000.0;
        int fftOrder = 13;
        int fftSize = 1 << 13;
        std::unique_ptr<juce::dsp::FFT> fft;
        std::vector<float> window, fftBuffer, measured;
        float windowGain = 1.0f;

        std::array<Channel, 2> channels;
        std::vector<float> average, averageSmoothed, peak, scratch;
        std::vector<int> peakHoldFrames;
        int outputSamplesSinceRestart = -1;   // ≥ 0：restartOutput 之后 OUT 已到的新样本数（攒满 FFT 长度才恢复平均线与峰值线）

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrumAnalyser)
    };
}
