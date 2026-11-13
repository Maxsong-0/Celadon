#include "OutputLevelTracker.h"

#include "GlazeConstants.h"

#include <cmath>
#include <cstring>

namespace celadon::glaze
{
    namespace
    {
        // 读档时一段样本数的上界：一段最长 8 s（prepare 的 maxOpenSamples），采样率上界 768 kHz（同 LiveProfile.cpp 的
        // stateMaximumSampleRate）。别的采样率下存的档读进来也在这个界以内。
        constexpr std::int64_t stateMaximumSegmentSamples = 8 * 768000;

        // 十进制非负整数，严格：至少一位、只有数字、不超过 maximum。juce::String::getLargeIntValue 把读不懂的文本当 0，
        // 坏档会被当成合法的 0 收下。规矩同 LiveProfile.cpp 的 parseUnsigned（那一个在它的匿名命名空间里，没有共用的头）。
        bool parseCount (const juce::String& text, std::int64_t maximum, std::int64_t& out) noexcept
        {
            auto p = text.getCharPointer();

            if (p.isEmpty())
                return false;

            std::int64_t value = 0;

            while (! p.isEmpty())
            {
                const auto c = p.getAndAdvance();

                if (c < '0' || c > '9')
                    return false;

                // 两边先各自转成有符号 64 位再减。juce_wchar 带不带符号看平台（x86_64 Linux、macOS 带，aarch64 Linux、Windows 不带）：
                // 直接相减的话，不带符号的平台上 '0' 以下的字符减出来是个四十多亿的数，碰巧被下面的上界检查拒掉，上面那道
                // 下界检查成了摆设、测试也测不到它；带符号的平台上却是负数，全靠它。这样写，各平台都只由上面那一行把关。
                const auto digit = (std::int64_t) c - (std::int64_t) '0';

                if (digit > maximum || value > (maximum - digit) / 10)   // value × 10 + digit ≤ maximum
                    return false;

                value = value * 10 + digit;
            }

            out = value;
            return true;
        }
    }

    void OutputLevelTracker::prepare (double newSampleRate) noexcept
    {
        if (newSampleRate > 0.0 && std::isfinite (newSampleRate))
            sampleRate = newSampleRate;

        LoudnessMeter::designKWeighting (sampleRate, shelf, highpass);
        maxOpenSamples = std::max<std::int64_t> (1, (std::int64_t) std::llround (sampleRate * 8.0));
        clear();
    }

    void OutputLevelTracker::clear() noexcept
    {
        resetFilters();
        clearSegments();
        legacyState = false;
        blockMeasured = false;
        blockInputLufs = blockLowShare = 0.0f;
    }

    // 待入账的入口能量作废（这一片的出口样本若还进账，所在的段就算没配齐）。
    void OutputLevelTracker::discardInput() noexcept
    {
        pendingInputEnergy = 0.0;
        pendingInputSamples = 0;
    }

    void OutputLevelTracker::clearSegments() noexcept
    {
        open = {};
        openExtra = {};
        closed.fill (Segment {});
        closedExtra.fill (Extra {});
        closedNow.fill (0.0f);
        closedCount = 0;
        closedWrite = 0;
        haveOpenMainness = false;
        openWasMain = false;
        discardInput();
    }

    void OutputLevelTracker::resetFilters() noexcept
    {
        shelfS1.fill (0.0);
        shelfS2.fill (0.0);
        highpassS1.fill (0.0);
        highpassS2.fill (0.0);
        inputShelfS1.fill (0.0);
        inputShelfS2.fill (0.0);
        inputHighpassS1.fill (0.0);
        inputHighpassS2.fill (0.0);
        discardInput();   // 复位前那一片的入口能量与复位后的出口不是一回事
    }

    void OutputLevelTracker::pushClosed (const Segment& segment, const Extra& extra) noexcept
    {
        if (segment.samples <= 0 || ! (segment.mainness >= mainnessGate))
            return;

        const auto slot = (std::size_t) closedWrite;
        closed[slot] = segment;
        closedExtra[slot] = extra;
        closedNow[slot] = segment.mainness;   // 下一次 rejudge() 之前按当时的判定
        closedWrite = (closedWrite + 1) % closedCapacity;
        closedCount = juce::jmin (closedCapacity, closedCount + 1);
    }

    void OutputLevelTracker::noteMainness (float mainness, bool measured, float inputLufs, float lowShare) noexcept
    {
        if (! std::isfinite (mainness))
            mainness = 0.0f;

        blockMeasured = measured && std::isfinite (inputLufs) && std::isfinite (lowShare) && lowShare >= 0.0f;
        blockInputLufs = blockMeasured ? inputLufs : 0.0f;
        blockLowShare = blockMeasured ? lowShare : 0.0f;

        const auto main = mainness >= mainnessGate;

        if (! haveOpenMainness)
        {
            haveOpenMainness = true;
            openWasMain = main;
            open.mainness = mainness;
            return;
        }

        if (main != openWasMain)
        {
            if (openWasMain)
                pushClosed (open, openExtra);

            open = {};
            openExtra = {};
            openWasMain = main;
        }

        open.mainness = mainness;
    }

    void OutputLevelTracker::closeSegment() noexcept
    {
        if (openWasMain)
            pushClosed (open, openExtra);

        open = {};
        openExtra = {};
        haveOpenMainness = false;
        openWasMain = false;
    }

    void OutputLevelTracker::resetChannel (int channel) noexcept
    {
        shelfS1[(std::size_t) channel] = shelfS2[(std::size_t) channel] = 0.0;
        highpassS1[(std::size_t) channel] = highpassS2[(std::size_t) channel] = 0.0;
    }

    void OutputLevelTracker::resetInputChannel (int channel) noexcept
    {
        inputShelfS1[(std::size_t) channel] = inputShelfS2[(std::size_t) channel] = 0.0;
        inputHighpassS1[(std::size_t) channel] = inputHighpassS2[(std::size_t) channel] = 0.0;
    }

    void OutputLevelTracker::processInput (const juce::AudioBuffer<float>& input) noexcept
    {
        discardInput();

        const auto channels = juce::jmin (2, input.getNumChannels());
        const auto numSamples = input.getNumSamples();

        if (channels <= 0 || numSamples <= 0)
            return;

        // 滤波器一直跑（不看有没有 m′、冻没冻结）：状态连续，哪一片开始入账都不用重新建立。
        const auto weight = channels == 1 ? 2.0 : 1.0;
        auto energy = 0.0;
        auto allFinite = true;

        for (int channel = 0; channel < channels; ++channel)
        {
            const auto* samples = input.getReadPointer (channel);
            auto& s1 = inputShelfS1[(std::size_t) channel];
            auto& s2 = inputShelfS2[(std::size_t) channel];
            auto& h1 = inputHighpassS1[(std::size_t) channel];
            auto& h2 = inputHighpassS2[(std::size_t) channel];

            for (int i = 0; i < numSamples; ++i)
            {
                const auto x = (double) samples[i];

                if (! std::isfinite (x))
                {
                    resetInputChannel (channel);
                    allFinite = false;
                    continue;
                }

                auto y = LoudnessMeter::kWeight (x, shelf, s1, s2);
                y = LoudnessMeter::kWeight (y, highpass, h1, h2);

                if (! std::isfinite (y))
                {
                    resetInputChannel (channel);
                    allFinite = false;
                    continue;
                }

                energy += weight * y * y;
            }
        }

        // 有非有限样本的片不配对：量到的入口能量缺了那几个样本，和出口不是同一批。不交待入账的量，这一片的出口进账时所在的段
        // 就算没配齐。
        if (allFinite && std::isfinite (energy))
        {
            pendingInputEnergy = energy;
            pendingInputSamples = numSamples;
        }
    }

    void OutputLevelTracker::processOutput (const juce::AudioBuffer<float>& output, float limitGainDb) noexcept
    {
        // 这一片的入口能量只给这一次调用用：先取走，后面不管从哪里返回都不会留到下一片。
        const auto inputEnergy = pendingInputEnergy;
        const auto inputSamples = pendingInputSamples;
        discardInput();

        if (! haveOpenMainness)
            return;

        const auto channels = juce::jmin (2, output.getNumChannels());
        const auto numSamples = output.getNumSamples();

        if (channels <= 0 || numSamples <= 0)
            return;

        const auto paired = inputSamples == numSamples;
        const auto inputPerSample = paired ? inputEnergy / (double) numSamples : 0.0;
        const auto weight = channels == 1 ? 2.0 : 1.0;
        // T-106：这一块的能量按当块执行的 Limit Gain 折回 0 dB。一块一个系数（增益是逐块给模块的）。
        const auto unityFactor = std::isfinite (limitGainDb) ? std::pow (10.0, -0.1 * (double) limitGainDb) : 1.0;

        for (int i = 0; i < numSamples; ++i)
        {
            auto sampleFinite = true;

            for (int channel = 0; channel < channels; ++channel)
            {
                const auto x = (double) output.getReadPointer (channel)[i];

                if (! std::isfinite (x))
                {
                    resetChannel (channel);
                    sampleFinite = false;
                    continue;
                }

                auto y = LoudnessMeter::kWeight (x, shelf, shelfS1[(std::size_t) channel], shelfS2[(std::size_t) channel]);
                y = LoudnessMeter::kWeight (y, highpass, highpassS1[(std::size_t) channel], highpassS2[(std::size_t) channel]);

                if (! std::isfinite (y))
                {
                    resetChannel (channel);
                    sampleFinite = false;
                    continue;
                }

                const auto energy = weight * y * y;
                open.energy += energy;
                openExtra.unityEnergy += energy * unityFactor;
            }

            ++open.samples;

            // 非有限的出口样本照旧计入段样本数（T-106 之前就这样），但不算配上了入口：它的出口能量没量到，所在的段不当证据。
            if (paired && sampleFinite)
            {
                openExtra.inputEnergy += inputPerSample;
                ++openExtra.pairedSamples;
            }

            if (blockMeasured)
            {
                openExtra.inputLufsSum += (double) blockInputLufs;
                openExtra.lowShareSum += (double) blockLowShare;
                ++openExtra.inputSamples;
            }

            if (open.samples >= maxOpenSamples)
            {
                const auto mainness = open.mainness;
                const auto wasMain = openWasMain;
                if (wasMain)
                    pushClosed (open, openExtra);

                open = {};
                openExtra = {};
                open.mainness = mainness;
                openWasMain = wasMain;
            }
        }
    }

    template <typename Visit>
    void OutputLevelTracker::forEachMainSegment (Visit&& visit) const noexcept
    {
        const auto stored = juce::jmin (closedCount, closedCapacity);

        for (int age = 0; age < stored; ++age)
        {
            const auto index = (std::size_t) ((closedWrite - 1 - age + closedCapacity * 2) % closedCapacity);

            // closedNow：重判后的 m′（没重判过就是当时的判定，进账时已 ≥ 0.5）。
            if (closed[index].samples > 0 && closedNow[index] >= mainnessGate)
                visit (closed[index], closedExtra[index]);
        }

        if (openWasMain && open.samples > 0 && open.mainness >= mainnessGate)
            visit (open, openExtra);
    }

    OutputLevelTracker::Evidence OutputLevelTracker::evidenceAt (float limitGainDb, float fullWithinDb, float zeroBeyondDb) const noexcept
    {
        Evidence result;

        if (! std::isfinite (limitGainDb))
            return result;

        const auto full = std::isfinite (fullWithinDb) ? (double) juce::jmax (0.0f, fullWithinDb) : 0.0;
        const auto zero = std::isfinite (zeroBeyondDb) ? juce::jmax (full, (double) zeroBeyondDb) : full;
        auto unity = 0.0, input = 0.0, samples = 0.0;

        forEachMainSegment ([&] (const Segment& segment, const Extra& extra) noexcept
        {
            // 入口没配齐的段：两侧不是同一批样本，不当证据。
            if (extra.pairedSamples != segment.samples)
                return;

            // 没有出口能量的段（数字静音）：量不出它是在什么增益下录的，折算不了。不当证据，也不占证据时长——不然几秒静音就能
            // 凑满「至少 3 s 证据」。
            if (! (segment.energy > 0.0) || ! (extra.unityEnergy > 0.0) || ! std::isfinite (segment.energy) || ! std::isfinite (extra.unityEnergy))
                return;

            const auto recordedGainDb = 10.0 * std::log10 (segment.energy / extra.unityEnergy);
            const auto distance = std::abs (recordedGainDb - (double) limitGainDb);
            auto weight = 1.0;

            if (distance > full)
                weight = distance < zero ? (zero - distance) / (zero - full) : 0.0;

            unity += weight * extra.unityEnergy;
            input += weight * extra.inputEnergy;
            samples += weight * (double) segment.samples;
        });

        if (! (samples > 0.0) || ! std::isfinite (samples))
            return result;

        result.seconds = samples / sampleRate;

        const auto lufsOf = [samples] (double energy) noexcept
        {
            if (! (energy > 0.0) || ! std::isfinite (energy))
                return -100.0f;

            const auto meanSquare = energy / samples;
            const auto lufs = meanSquare > 0.0 && std::isfinite (meanSquare) ? -0.691f + (float) (10.0 * std::log10 (meanSquare)) : -100.0f;
            return std::isfinite (lufs) ? lufs : -100.0f;
        };

        // 折回 0 dB 的电平本身低到 −100 就不算读数（不让「−100 + 增益」冒充有效值）。
        if (const auto atUnity = lufsOf (unity); atUnity > -100.0f)
            result.levelLufs = juce::jmax (-100.0f, atUnity + limitGainDb);

        result.inputLufs = juce::jmax (-100.0f, lufsOf (input));
        return result;
    }

    bool OutputLevelTracker::openSegmentIsMain() const noexcept
    {
        return haveOpenMainness && openWasMain && open.mainness >= mainnessGate;
    }

    std::int64_t OutputLevelTracker::mainSamples() const noexcept
    {
        std::int64_t samples = 0;
        forEachMainSegment ([&samples] (const Segment& segment, const Extra&) noexcept { samples += segment.samples; });
        return samples;
    }

    bool OutputLevelTracker::validSegment (const Segment& segment) noexcept
    {
        return std::isfinite (segment.energy) && segment.energy >= 0.0
            && segment.samples >= 0 && segment.samples <= stateMaximumSegmentSamples
            && std::isfinite (segment.mainness);
    }

    bool OutputLevelTracker::validExtra (const Extra& extra, std::int64_t segmentSamples) noexcept
    {
        const auto limit = juce::jmax ((std::int64_t) 0, segmentSamples);
        return std::isfinite (extra.unityEnergy) && extra.unityEnergy >= 0.0
            && std::isfinite (extra.inputEnergy) && extra.inputEnergy >= 0.0
            && std::isfinite (extra.inputLufsSum)
            && std::isfinite (extra.lowShareSum) && extra.lowShareSum >= 0.0
            && extra.inputSamples >= 0 && extra.inputSamples <= limit
            && extra.pairedSamples >= 0 && extra.pairedSamples <= limit;
    }

    std::unique_ptr<juce::XmlElement> OutputLevelTracker::createStateXml() const
    {
        auto xml = std::make_unique<juce::XmlElement> ("OUTPUT_LEVEL");
        xmlio::Writer writer (*xml);
        writer.field ("openEnergy", open.energy);
        writer.field ("openMainness", open.mainness);
        writer.field ("closedCount", closedCount);
        writer.field ("closedWrite", closedWrite);
        writer.field ("haveOpenMainness", haveOpenMainness);
        writer.field ("openWasMain", openWasMain);
        xml->setAttribute ("openSamples", juce::String (open.samples));

        juce::MemoryBlock packed ((size_t) closedCapacity * sizeof (Segment));
        std::memcpy (packed.getData(), closed.data(), packed.getSize());
        xml->setAttribute ("closed", juce::Base64::toBase64 (packed.getData(), packed.getSize()));

        // T-106：每段另记的量另写七个属性。上面那些原样不动，0.1.8.10 不认识新属性、照旧读得了这一节。
        writer.field ("openUnityEnergy", openExtra.unityEnergy);
        writer.field ("openInputEnergy", openExtra.inputEnergy);
        writer.field ("openInputLufsSum", openExtra.inputLufsSum);
        writer.field ("openLowShareSum", openExtra.lowShareSum);
        xml->setAttribute ("openInputSamples", juce::String (openExtra.inputSamples));
        xml->setAttribute ("openPairedSamples", juce::String (openExtra.pairedSamples));

        juce::MemoryBlock extraPacked ((size_t) closedCapacity * sizeof (Extra));
        std::memcpy (extraPacked.getData(), closedExtra.data(), extraPacked.getSize());
        xml->setAttribute ("closedExtra", juce::Base64::toBase64 (extraPacked.getData(), extraPacked.getSize()));
        return xml;
    }

    bool OutputLevelTracker::restoreStateXml (const juce::XmlElement& xml)
    {
        if (xml.getTagName() != "OUTPUT_LEVEL")
            return false;

        Segment restoredOpen;
        auto count = closedCount;
        auto write = closedWrite;
        auto have = haveOpenMainness;
        auto wasMain = openWasMain;
        xmlio::Reader reader (xml);
        reader.field ("openEnergy", restoredOpen.energy);
        reader.field ("openMainness", restoredOpen.mainness);
        reader.field ("closedCount", count);
        reader.field ("closedWrite", write);
        reader.field ("haveOpenMainness", have);
        reader.field ("openWasMain", wasMain);

        juce::MemoryOutputStream packed;
        if (! reader.ok || count < 0 || count > closedCapacity || write < 0 || write >= closedCapacity
            || ! juce::Base64::convertFromBase64 (packed, xml.getStringAttribute ("closed"))
            || packed.getDataSize() != (size_t) closedCapacity * sizeof (Segment))
            return false;

        static constexpr const char* extraAttributes[] = { "openUnityEnergy", "openInputEnergy", "openInputLufsSum", "openLowShareSum",
                                                           "openInputSamples", "openPairedSamples", "closedExtra" };
        int present = 0;

        for (const auto* name : extraAttributes)
            present += xml.hasAttribute (name) ? 1 : 0;

        if (present == 0)
        {
            // 旧存档：不知道每段是在什么增益下录的，折算不了。段累计清空、从头攒；滤波器照旧不动。
            // 上面那几项的校验同 0.1.8.10；段的内容（closed 里各段、openSamples）反正不用，不看。
            clearSegments();
            legacyState = true;
            return true;
        }

        if (present != (int) std::size (extraAttributes))
            return false;

        // 新格式：这里起读到的每一个量都要用，逐个校验，有一个不合规矩就整节拒收（同别的节点，坏数据不猜）。
        // 三个样本数是十进制文本，严格解析；各段（开着的一段 + 已收的 32 段）连同它另记的量都验。
        Extra restoredOpenExtra;
        xmlio::Reader extraReader (xml);
        extraReader.field ("openUnityEnergy", restoredOpenExtra.unityEnergy);
        extraReader.field ("openInputEnergy", restoredOpenExtra.inputEnergy);
        extraReader.field ("openInputLufsSum", restoredOpenExtra.inputLufsSum);
        extraReader.field ("openLowShareSum", restoredOpenExtra.lowShareSum);

        juce::MemoryOutputStream extraPacked;
        std::array<Segment, (std::size_t) closedCapacity> restoredClosed {};
        std::array<Extra, (std::size_t) closedCapacity> restoredExtra {};

        if (! extraReader.ok
            || ! parseCount (xml.getStringAttribute ("openSamples"), stateMaximumSegmentSamples, restoredOpen.samples)
            || ! parseCount (xml.getStringAttribute ("openInputSamples"), stateMaximumSegmentSamples, restoredOpenExtra.inputSamples)
            || ! parseCount (xml.getStringAttribute ("openPairedSamples"), stateMaximumSegmentSamples, restoredOpenExtra.pairedSamples)
            || ! validSegment (restoredOpen) || ! validExtra (restoredOpenExtra, restoredOpen.samples)
            || ! juce::Base64::convertFromBase64 (extraPacked, xml.getStringAttribute ("closedExtra"))
            || extraPacked.getDataSize() != sizeof (restoredExtra))
            return false;

        std::memcpy (restoredClosed.data(), packed.getData(), packed.getDataSize());
        std::memcpy (restoredExtra.data(), extraPacked.getData(), extraPacked.getDataSize());

        for (std::size_t i = 0; i < restoredExtra.size(); ++i)
            if (! validSegment (restoredClosed[i]) || ! validExtra (restoredExtra[i], restoredClosed[i].samples))
                return false;

        closed = restoredClosed;
        closedExtra = restoredExtra;

        for (std::size_t i = 0; i < closed.size(); ++i)
            closedNow[i] = closed[i].mainness;   // 下一次 rejudge() 起按此刻的参考

        open = restoredOpen;
        openExtra = restoredOpenExtra;
        closedCount = count;
        closedWrite = write;
        haveOpenMainness = have;
        openWasMain = wasMain;
        legacyState = false;
        discardInput();
        return true;
    }
}
