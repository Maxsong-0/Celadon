#include "ContrastGuard.h"

#include <cmath>

namespace celadon::glaze
{
    namespace
    {
        // T-048 存档里的 128 笔环。只为 0.1.8.10 能读回 T-097 写的存档：写空值、读时不看。
        constexpr int legacyRingCapacity = 128;
        const std::array<float, (size_t) legacyRingCapacity> legacyEmptyRing {};

        // 遗忘后小到这里的格直接归零：只是不让一格无限地缩下去（浮点精度上它对分位早就没有影响）。
        constexpr float negligibleWeight = 1.0e-9f;
    }

    void ContrastGuard::prepare (double newStepSeconds) noexcept
    {
        if (newStepSeconds > 0.0 && std::isfinite (newStepSeconds))
            stepSeconds = newStepSeconds;

        decay = std::exp (-stepSeconds / forgetSeconds);
        clear();
    }

    void ContrastGuard::clear() noexcept
    {
        reliefDb = 0.0f;
        engaged = false;
        overSeconds = 0.0;
        underSeconds = 0.0;
        histogramSeconds = 0.0;
        inputHistogram.fill (0.0f);
        outputHistogram.fill (0.0f);
        lastReading = {};
        clearWindows();
    }

    void ContrastGuard::clearWindows() noexcept
    {
        inputWindow.clear();
        outputWindow.clear();
    }

    void ContrastGuard::setFloorDb (float newFloorDb) noexcept
    {
        floorDb = std::isfinite (newFloorDb) ? juce::jmin (0.0f, newFloorDb) : -3.0f;
        reliefDb = juce::jmax (floorDb, reliefDb);
    }

    void ContrastGuard::snapRelief() noexcept
    {
        reliefDb = 0.0f;
        engaged = false;
        overSeconds = 0.0;
        underSeconds = 0.0;
    }

    double ContrastGuard::percentileLufs (const Histogram& histogram, double total, double fraction) noexcept
    {
        // 累计到 fraction × 总量的那一格，格内按线性插值（不落在格中心：0.1 LU 的格差会让门槛比较在边界上翻来翻去）。
        const auto target = fraction * total;
        double cumulative = 0.0;

        for (int bin = 0; bin < histogramBins; ++bin)
        {
            const auto weight = (double) histogram[(size_t) bin];

            if (weight <= 0.0)
                continue;

            if (cumulative + weight >= target)
            {
                const auto within = juce::jlimit (0.0, 1.0, (target - cumulative) / weight);
                return histogramMinLufs + ((double) bin + within) / (double) histogramBinsPerLu;
            }

            cumulative += weight;
        }

        return histogramMinLufs + (double) histogramBins / (double) histogramBinsPerLu;
    }

    double ContrastGuard::spreadLu (const Histogram& histogram) noexcept
    {
        double total = 0.0;

        for (const auto weight : histogram)
            total += (double) weight;

        if (! (total > 0.0))
            return 0.0;

        return percentileLufs (histogram, total, highPercentile) - percentileLufs (histogram, total, lowPercentile);
    }

    void ContrastGuard::addToHistograms (double inputLufs, double outputLufs) noexcept
    {
        const auto shrink = (float) decay;

        for (auto* histogram : { &inputHistogram, &outputHistogram })
            for (auto& weight : *histogram)
            {
                const auto shrunk = weight * shrink;
                weight = shrunk < negligibleWeight ? 0.0f : shrunk;
            }

        const auto add = (float) stepSeconds;
        inputHistogram[(size_t) histogramBinOf (inputLufs)] += add;
        outputHistogram[(size_t) histogramBinOf (outputLufs)] += add;
        histogramSeconds = histogramSeconds * decay + stepSeconds;
    }

    ContrastGuard::Reading ContrastGuard::judge() const noexcept
    {
        Reading reading;
        reading.histogramSeconds = histogramSeconds;
        reading.inputSpreadLu = (float) spreadLu (inputHistogram);
        reading.outputSpreadLu = (float) spreadLu (outputHistogram);
        reading.lossLu = reading.inputSpreadLu - reading.outputSpreadLu;
        reading.judged = reached (histogramSeconds, fullSeconds) && reading.inputSpreadLu >= minimumInputSpreadLu;
        return reading;
    }

    GuardOutput ContrastGuard::process (const LoudnessMeter::SubBlock& input, const LoudnessMeter::SubBlock& output,
                                        bool musicActive, bool enabled, bool clubDelivery) noexcept
    {
        inputWindow.push (input.meanSquare, input.truePeakLinear);
        outputWindow.push (output.meanSquare, output.truePeakLinear);

        const auto before = reliefDb;
        const auto dt = (float) stepSeconds;
        auto sustainedOver = false;
        double inputLufs = 0.0, outputLufs = 0.0;
        const auto measured = musicActive && inputWindow.shortTermLufs (inputLufs) && outputWindow.shortTermLufs (outputLufs);

        if (measured)
        {
            addToHistograms (inputLufs, outputLufs);
            lastReading = judge();
        }

        if (musicActive && ! enabled)
        {
            overSeconds = 0.0;
            underSeconds = 0.0;
            reliefDb = juce::jmin (0.0f, reliefDb + releaseDbPerSecond * dt);
        }
        else if (measured)
        {
            const auto threshold = clubDelivery ? clubLossLu : standardLossLu;

            if (lastReading.judged && lastReading.lossLu > threshold)
            {
                overSeconds += stepSeconds;
                underSeconds = 0.0;
                sustainedOver = reached (overSeconds, triggerSeconds);

                if (sustainedOver && reliefDb > retreatBoundDb)   // Q15 A：已比界深就保持，不回弹
                    reliefDb = juce::jmax (juce::jmax (floorDb, retreatBoundDb), reliefDb - retreatDbPerSecond * dt);
            }
            else if (! lastReading.judged || lastReading.lossLu < threshold - releaseMarginLu)
            {
                underSeconds += stepSeconds;
                overSeconds = 0.0;

                if (reached (underSeconds, releaseAfterSeconds))
                    reliefDb = juce::jmin (0.0f, reliefDb + releaseDbPerSecond * dt);
            }
            else
            {
                overSeconds = 0.0;
                underSeconds = 0.0;
            }
        }
        // 其余（非有效音乐，或两侧 S 还不全有效）：计时与退让都保持。

        engaged = reliefDb < before || (reliefDb <= floorDb && sustainedOver);
        return current();
    }

    GuardOutput ContrastGuard::current() const noexcept
    {
        GuardOutput out;
        out.reliefDb = reliefDb;
        out.cause = reliefDb < 0.0f ? GuardCause::contrast : GuardCause::none;
        out.engaged = engaged;
        return out;
    }

    std::unique_ptr<juce::XmlElement> ContrastGuard::createStateXml() const
    {
        auto xml = std::make_unique<juce::XmlElement> ("CONTRAST_GUARD");
        xmlio::Writer writer (*xml);
        writer.field ("reliefDb", reliefDb);
        writer.field ("floorDb", floorDb);
        writer.field ("engaged", engaged);
        writer.field ("overSeconds", overSeconds);
        writer.field ("underSeconds", underSeconds);
        writer.field ("histogramSeconds", histogramSeconds);
        writer.field ("inputHistogram", inputHistogram);
        writer.field ("outputHistogram", outputHistogram);

        // 0.1.8.10 的读法要这几项（严格读、缺一项就整份 LIVE 冷启动）。空环 = 它从头攒，退让照旧。
        const int legacyZero = 0;
        const int legacyPushLength = 4800;
        writer.field ("ringCount", legacyZero);
        writer.field ("ringWrite", legacyZero);
        writer.field ("samplesUntilPush", legacyPushLength);
        writer.field ("inputRing", legacyEmptyRing);
        writer.field ("outputRing", legacyEmptyRing);
        return xml;
    }

    bool ContrastGuard::restoreStateXml (const juce::XmlElement& xml)
    {
        if (xml.getTagName() != "CONTRAST_GUARD")
            return false;

        auto relief = reliefDb;
        auto floor = floorDb;
        auto wasEngaged = engaged;
        xmlio::Reader reader (xml);
        reader.field ("reliefDb", relief);
        reader.field ("floorDb", floor);
        reader.field ("engaged", wasEngaged);

        // T-097 之前的节点（128 笔环）没有直方图：退让照旧，计时与直方图从空开始。
        const auto hasHistograms = xml.hasAttribute ("inputHistogram");
        auto over = 0.0, under = 0.0, seconds = 0.0;
        Histogram restoredInput {}, restoredOutput {};

        if (hasHistograms)
        {
            reader.field ("overSeconds", over);
            reader.field ("underSeconds", under);
            reader.field ("histogramSeconds", seconds);
            reader.field ("inputHistogram", restoredInput);
            reader.field ("outputHistogram", restoredOutput);
        }

        if (! reader.ok || ! std::isfinite (relief) || ! std::isfinite (floor) || ! std::isfinite (over) || ! std::isfinite (under)
            || ! std::isfinite (seconds) || over < 0.0 || under < 0.0 || seconds < 0.0)
            return false;

        for (const auto* histogram : { &restoredInput, &restoredOutput })
            for (const auto weight : *histogram)
                if (! std::isfinite (weight) || weight < 0.0f)
                    return false;

        reliefDb = relief;
        floorDb = floor;
        engaged = wasEngaged;
        overSeconds = over;
        underSeconds = under;
        histogramSeconds = seconds;
        inputHistogram = restoredInput;
        outputHistogram = restoredOutput;
        lastReading = {};
        clearWindows();
        return true;
    }
}
