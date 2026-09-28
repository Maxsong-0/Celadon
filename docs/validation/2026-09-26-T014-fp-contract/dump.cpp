// T-014 对照程序：三个 DSP 部件在固定输入上的输出按位写出（.bin），再由 compare.py 在不同编译选项之间逐位比较。
// 同一份源码按不同选项编多次（默认 SSE2 / -march=x86-64-v3 / 再加 -ffp-contract=off / 强制标量回退 / clang），
// JUCE 静态库始终是基线编译，所以 HalfBand 的系数（JUCE FilterDesign 算的）各配置相同，差异只来自被测文件本身。
#include "../Source/dsp/HalfBandOversampler.h"
#include "../Source/dsp/Saturator.h"
#include "../Source/dsp/TruePeak.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace celadon;

namespace
{
    std::vector<float> noise (int n, uint32_t seed, float amplitude)
    {
        std::vector<float> v ((size_t) n);
        uint32_t s = seed;

        for (auto& x : v)
        {
            s = s * 1664525u + 1013904223u;
            x = amplitude * ((float) (s >> 8) * (1.0f / 8388608.0f) - 1.0f);
        }

        return v;
    }

    void writeFile (const std::string& dir, const char* name, const std::vector<float>& data)
    {
        const auto path = dir + "/" + name;
        auto* f = std::fopen (path.c_str(), "wb");
        std::fwrite (data.data(), sizeof (float), data.size(), f);
        std::fclose (f);
    }

    uint32_t bitsOf (float x) { uint32_t u; std::memcpy (&u, &x, 4); return u; }

    // ---- HalfBand：Color Heat 的三级（8×），双声道白噪声，块长 512，收集上采样与下采样输出 ----
    void runHalfBand (const std::string& dir)
    {
        constexpr int channels = 2, total = 48000, blockSize = 512;
        const std::vector<float> input[2] = { noise (total, 1, 0.99f), noise (total, 2, 0.99f) };

        HalfBandOversampler oversampler ((size_t) channels);
        oversampler.addOversamplingStage (0.05f, -90.0f, 0.06f, -75.0f);
        oversampler.addOversamplingStage (0.10f, -80.0f, 0.12f, -70.0f);
        oversampler.addOversamplingStage (0.10f, -65.0f, 0.15f, -55.0f);
        oversampler.initProcessing ((size_t) blockSize);

        std::vector<float> up, down;
        juce::AudioBuffer<float> buffer (channels, blockSize);

        for (int start = 0; start + blockSize <= total; start += blockSize)
        {
            for (int ch = 0; ch < channels; ++ch)
                buffer.copyFrom (ch, 0, input[ch].data() + start, blockSize);

            juce::dsp::AudioBlock<float> block (buffer);
            auto upsampled = oversampler.processSamplesUp (block);

            for (int ch = 0; ch < channels; ++ch)
            {
                auto* data = upsampled.getChannelPointer ((size_t) ch);
                up.insert (up.end(), data, data + upsampled.getNumSamples());
            }

            oversampler.processSamplesDown (block);

            for (int ch = 0; ch < channels; ++ch)
                down.insert (down.end(), buffer.getReadPointer (ch), buffer.getReadPointer (ch) + blockSize);
        }

        writeFile (dir, "halfband-up.bin", up);
        writeFile (dir, "halfband-down.bin", down);
        std::printf ("halfband: %zu up, %zu down samples\n", up.size(), down.size());
    }

    // ---- Saturator：fastTanh 取样扫描 + 穷举 [0, 8] 全部非负 float（只出散列与界）+ Tube 整段 ----
    void runSaturator (const std::string& dir)
    {
        std::vector<float> xs;
        constexpr int sweep = 1 << 20;

        for (int i = 0; i <= sweep; ++i)
            xs.push_back (-10.0f + 20.0f * (float) i / (float) sweep);

        const float specials[] = { 0.0f, -0.0f, 1.0e-4f, 4.0e-4f, 7.90531110763549805f, -7.90531110763549805f,
                                   std::nextafter (7.90531110763549805f, 0.0f), 7.99881172180175781f, 10.0f,
                                   std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity() };
        xs.insert (xs.end(), std::begin (specials), std::end (specials));

        auto ys = xs;
        Saturator::fastTanhBlock (ys.data(), (int) ys.size());
        writeFile (dir, "fasttanh-sweep.bin", ys);

        std::printf ("fastTanh(+Inf) = %.9g (0x%08x)  fastTanh(7.9053111) = %.9g (0x%08x)  fastTanh(10) = %.9g\n",
                     (double) Saturator::fastTanh (std::numeric_limits<float>::infinity()), bitsOf (Saturator::fastTanh (std::numeric_limits<float>::infinity())),
                     (double) Saturator::fastTanh (7.90531110763549805f), bitsOf (Saturator::fastTanh (7.90531110763549805f)),
                     (double) Saturator::fastTanh (10.0f));

        // 穷举 0 … 8.0f 的全部 float 位型：FNV-1a 散列（配置间逐位比较用）、|y| > 1 的个数、最大 y、奇对称。
        uint64_t hash = 1469598103934665603ull;
        uint64_t aboveOne = 0, oddFailures = 0;
        float maxY = 0.0f;
        constexpr uint32_t last = 0x41000000u;   // 8.0f
        constexpr int chunk = 4096;
        std::vector<float> pos (chunk), neg (chunk);

        for (uint32_t base = 0; base <= last; base += chunk)
        {
            const auto count = (int) std::min<uint64_t> (chunk, (uint64_t) last - base + 1);

            for (int i = 0; i < count; ++i)
            {
                const uint32_t u = base + (uint32_t) i;
                std::memcpy (&pos[(size_t) i], &u, 4);
                neg[(size_t) i] = -pos[(size_t) i];
            }

            Saturator::fastTanhBlock (pos.data(), count);
            Saturator::fastTanhBlock (neg.data(), count);

            for (int i = 0; i < count; ++i)
            {
                const auto y = pos[(size_t) i];
                const auto b = bitsOf (y);

                for (int k = 0; k < 4; ++k)
                {
                    hash ^= (b >> (8 * k)) & 0xffu;
                    hash *= 1099511628211ull;
                }

                if (std::abs (y) > 1.0f)
                    ++aboveOne;

                if (y > maxY)
                    maxY = y;

                if (bitsOf (neg[(size_t) i]) != (b ^ 0x80000000u))
                    ++oddFailures;
            }
        }

        std::printf ("fastTanh exhaustive [0, 8]: hash %016llx, |y| > 1: %llu, max y %.9g (0x%08x), odd failures %llu\n",
                     (unsigned long long) hash, (unsigned long long) aboveOne, (double) maxY, bitsOf (maxY),
                     (unsigned long long) oddFailures);

        Saturator tube;
        tube.setCurve (Saturator::Curve::Tube);
        tube.setDriveAndAsymmetry (6.4f, 0.09f);
        auto block = noise (48000, 7, 0.99f);
        tube.processTubeBlock (block.data(), (int) block.size());
        writeFile (dir, "tube-block.bin", block);
    }

    // ---- TruePeak：本构建内 processBlock 与 processSample 是否逐位相同；输出写出供配置间比较 ----
    void runTruePeak (const std::string& dir)
    {
        constexpr int total = 48000, blockSize = 512;
        const auto input = noise (total, 3, 1.2f);

        TruePeakDetector blockDetector, sampleDetector;
        blockDetector.prepare (1);
        sampleDetector.prepare (1);

        std::vector<float> byBlock ((size_t) total), bySample ((size_t) total);

        for (int start = 0; start < total; start += blockSize)
            blockDetector.processBlock (0, input.data() + start, byBlock.data() + start, std::min (blockSize, total - start));

        for (int i = 0; i < total; ++i)
            bySample[(size_t) i] = sampleDetector.processSample (0, input[(size_t) i]);

        int mismatches = 0;

        for (int i = 0; i < total; ++i)
            if (bitsOf (byBlock[(size_t) i]) != bitsOf (bySample[(size_t) i]))
                ++mismatches;

        writeFile (dir, "truepeak-block.bin", byBlock);
        writeFile (dir, "truepeak-sample.bin", bySample);
        std::printf ("truepeak: kernel %s, processBlock vs processSample in this build: %d / %d samples differ\n",
                     TruePeakDetector::getBlockKernelName(), mismatches, total);
    }
}

int main (int argc, char* argv[])
{
    const std::string dir = argc > 1 ? argv[1] : ".";
    runHalfBand (dir);
    runSaturator (dir);
    runTruePeak (dir);
    return 0;
}
