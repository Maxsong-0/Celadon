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
        // 坏档会被当成合法的 0 收下。规矩同 LiveProfile.cpp 的 parseUnsigned，但没有并成一个：那一个按无符号 64 位读、上界可以
        // 到 UINT64_MAX（LiveProfile 的计数器）；这里的数字要按有符号相减（见下），并过去要么改它认的范围，要么丢掉下面那条理由。
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

        // 一个样本过两级 K 加权（同 LoudnessMeter）。入口、出口两路共用。样本或结果不是有限值时返回 false：调用方复位这一路的
        // 滤波器，这个样本不算能量。
        bool kWeightSample (double x, const LoudnessMeter::Biquad& shelf, const LoudnessMeter::Biquad& highpass, double& shelfS1,
                            double& shelfS2, double& highpassS1, double& highpassS2, double& y) noexcept
        {
            if (! std::isfinite (x))
                return false;

            y = LoudnessMeter::kWeight (x, shelf, shelfS1, shelfS2);
            y = LoudnessMeter::kWeight (y, highpass, highpassS1, highpassS2);
            return std::isfinite (y);
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
                auto y = 0.0;

                if (! kWeightSample ((double) samples[i], shelf, highpass, s1, s2, h1, h2, y))
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

        // T-106：这一块的能量按当块执行的 Limit Gain 折回 0 dB。一块一个系数（增益是逐块给模块的）。
        // 「执行的」按限幅器自己的规矩算（外部第二意见 F4）：有限值钳进 0 ～ 24 dB，非有限值回它的默认 0 dB。直接拿交来的值折算的话，
        // 范围外的有限值记的增益和执行的对不上，极端的负值还会让系数上溢成 ∞、折回 0 dB 的能量变 ∞，存档时自己拒收自己。
        const auto executedGainDb = executedLimitGainDb (limitGainDb);
        const auto unityFactor = std::pow (10.0, -0.1 * (double) executedGainDb);
        const auto paired = inputSamples == numSamples;
        const auto inputPerSample = paired ? inputEnergy / (double) numSamples : 0.0;
        const auto weight = channels == 1 ? 2.0 : 1.0;

        for (int i = 0; i < numSamples; ++i)
        {
            auto sampleFinite = true;

            for (int channel = 0; channel < channels; ++channel)
            {
                const auto c = (std::size_t) channel;
                auto y = 0.0;

                if (! kWeightSample ((double) output.getReadPointer (channel)[i], shelf, highpass, shelfS1[c], shelfS2[c], highpassS1[c],
                                     highpassS2[c], y))
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
        // 已收的段从旧到新排成一列（从环里最旧的那一格起）。
        const auto stored = juce::jmin (closedCount, closedCapacity);
        std::array<Segment, (std::size_t) closedCapacity> history {};
        std::array<Extra, (std::size_t) closedCapacity> historyExtra {};

        for (int i = 0; i < stored; ++i)
        {
            const auto index = (std::size_t) ((closedWrite - stored + i + closedCapacity) % closedCapacity);
            history[(std::size_t) i] = closed[index];
            historyExtra[(std::size_t) i] = closedExtra[index];
        }

        // 给 0.1.8.10 的那一份：最新的至多 32 段，从旧到新放在第 0 格起，写入位置是最新一段的下一格。0.1.8.10 从写入位置往前
        // 数 closedCount 段，读到的正是这几段。
        const auto legacyCount = juce::jmin (stored, legacyClosedCapacity);
        std::array<Segment, (std::size_t) legacyClosedCapacity> legacy {};

        for (int i = 0; i < legacyCount; ++i)
            legacy[(std::size_t) i] = history[(std::size_t) (stored - legacyCount + i)];

        auto xml = std::make_unique<juce::XmlElement> ("OUTPUT_LEVEL");
        xmlio::Writer writer (*xml);
        writer.field ("openEnergy", open.energy);
        writer.field ("openMainness", open.mainness);
        writer.field ("closedCount", legacyCount);
        writer.field ("closedWrite", legacyCount % legacyClosedCapacity);
        writer.field ("haveOpenMainness", haveOpenMainness);
        writer.field ("openWasMain", openWasMain);
        xml->setAttribute ("openSamples", juce::String (open.samples));
        xml->setAttribute ("closed", juce::Base64::toBase64 (legacy.data(), sizeof (legacy)));

        // T-106：每段另记的量、存档时的采样率、全部已收的段另写十个属性。上面那些照 0.1.8.10 的格式写，0.1.8.10 不认识新属性、
        // 照旧读得了这一节；这一版读新格式时只用下面这些，closed 只验大小（见 restoreStateXml）。
        writer.field ("openUnityEnergy", openExtra.unityEnergy);
        writer.field ("openInputEnergy", openExtra.inputEnergy);
        writer.field ("openInputLufsSum", openExtra.inputLufsSum);
        writer.field ("openLowShareSum", openExtra.lowShareSum);
        writer.field ("sampleRate", sampleRate);   // 段里的样本数按它记；读回时采样率不同就不用这批段（见 restoreStateXml）
        xml->setAttribute ("openInputSamples", juce::String (openExtra.inputSamples));
        xml->setAttribute ("openPairedSamples", juce::String (openExtra.pairedSamples));
        writer.field ("historyCount", stored);
        xml->setAttribute ("history", juce::Base64::toBase64 (history.data(), (size_t) stored * sizeof (Segment)));
        xml->setAttribute ("historyExtra", juce::Base64::toBase64 (historyExtra.data(), (size_t) stored * sizeof (Extra)));
        return xml;
    }

    bool OutputLevelTracker::restoreStateXml (const juce::XmlElement& xml)
    {
        if (xml.getTagName() != "OUTPUT_LEVEL")
            return false;

        Segment restoredOpen;
        auto count = 0;   // closedCount、closedWrite 只验（给 0.1.8.10 的那一份），这一版不用
        auto write = 0;
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
        if (! reader.ok || count < 0 || count > legacyClosedCapacity || write < 0 || write >= legacyClosedCapacity
            || ! juce::Base64::convertFromBase64 (packed, xml.getStringAttribute ("closed"))
            || packed.getDataSize() != (size_t) legacyClosedCapacity * sizeof (Segment))
            return false;

        static constexpr const char* extraAttributes[] = { "openUnityEnergy", "openInputEnergy", "openInputLufsSum", "openLowShareSum",
                                                           "openInputSamples", "openPairedSamples", "sampleRate",
                                                           "historyCount", "history", "historyExtra" };
        int present = 0;

        for (const auto* name : extraAttributes)
            present += xml.hasAttribute (name) ? 1 : 0;

        if (present == 0)
        {
            // 旧存档：不知道每段是在什么增益下录的，折算不了。段累计清空、从头攒；滤波器照旧不动。
            // 上面那几项的校验同 0.1.8.10（环 32 格）；段的内容（closed 里各段、openSamples）反正不用，不看。
            clearSegments();
            legacyState = true;
            return true;
        }

        if (present != (int) std::size (extraAttributes))
            return false;

        // 新格式：这里起读到的每一个量都要用，逐个校验，有一个不合规矩就整节拒收（同别的节点，坏数据不猜）。
        // 三个样本数是十进制文本，严格解析；各段（开着的一段 + history 里的 historyCount 段）连同它另记的量都验。
        // 已收的段只从 history 读：closed 是给 0.1.8.10 的那一份（最新的至多 32 段），上面验过大小，内容不用。
        Extra restoredOpenExtra;
        xmlio::Reader extraReader (xml);
        extraReader.field ("openUnityEnergy", restoredOpenExtra.unityEnergy);
        extraReader.field ("openInputEnergy", restoredOpenExtra.inputEnergy);
        extraReader.field ("openInputLufsSum", restoredOpenExtra.inputLufsSum);
        extraReader.field ("openLowShareSum", restoredOpenExtra.lowShareSum);
        auto savedRate = 0.0;
        extraReader.field ("sampleRate", savedRate);
        auto historyCount = -1;
        extraReader.field ("historyCount", historyCount);

        juce::MemoryOutputStream historyPacked, historyExtraPacked;
        std::array<Segment, (std::size_t) closedCapacity> restoredClosed {};
        std::array<Extra, (std::size_t) closedCapacity> restoredExtra {};

        if (! extraReader.ok || ! std::isfinite (savedRate) || savedRate <= 0.0 || historyCount < 0 || historyCount > closedCapacity
            || ! parseCount (xml.getStringAttribute ("openSamples"), stateMaximumSegmentSamples, restoredOpen.samples)
            || ! parseCount (xml.getStringAttribute ("openInputSamples"), stateMaximumSegmentSamples, restoredOpenExtra.inputSamples)
            || ! parseCount (xml.getStringAttribute ("openPairedSamples"), stateMaximumSegmentSamples, restoredOpenExtra.pairedSamples)
            || ! validSegment (restoredOpen) || ! validExtra (restoredOpenExtra, restoredOpen.samples)
            || ! juce::Base64::convertFromBase64 (historyPacked, xml.getStringAttribute ("history"))
            || historyPacked.getDataSize() != (size_t) historyCount * sizeof (Segment)
            || ! juce::Base64::convertFromBase64 (historyExtraPacked, xml.getStringAttribute ("historyExtra"))
            || historyExtraPacked.getDataSize() != (size_t) historyCount * sizeof (Extra))
            return false;

        if (historyCount > 0)
        {
            std::memcpy (restoredClosed.data(), historyPacked.getData(), historyPacked.getDataSize());
            std::memcpy (restoredExtra.data(), historyExtraPacked.getData(), historyExtraPacked.getDataSize());
        }

        // T-109：reserved 读进来是什么都清成 0（T-109 之前 MSVC 版存的这 4 字节是栈上的旧值），之后再存出来的字节才确定。
        for (std::size_t i = 0; i < (std::size_t) historyCount; ++i)
            restoredClosed[i].reserved = 0;

        for (std::size_t i = 0; i < (std::size_t) historyCount; ++i)
            if (! validSegment (restoredClosed[i]) || ! validExtra (restoredExtra[i], restoredClosed[i].samples))
                return false;

        // 从旧到新放在第 0 格起，写入位置是最新一段的下一格（满 128 段时回到第 0 格，正是最旧的那一段）。
        closed = restoredClosed;
        closedExtra = restoredExtra;

        for (std::size_t i = 0; i < closed.size(); ++i)
            closedNow[i] = closed[i].mainness;   // 下一次 rejudge() 起按此刻的参考

        open = restoredOpen;
        openExtra = restoredOpenExtra;
        closedCount = historyCount;
        closedWrite = historyCount % closedCapacity;
        haveOpenMainness = have;
        openWasMain = wasMain;
        legacyState = false;
        discardInput();

        // T-106（外部第二意见 F5）：存档时的采样率和此刻不同，段里的样本数是按存档时的采样率记的——时长（「至少 3 s 证据」那道门）
        // 和各段的权重都对不上。这批段不用：段历史清空、从头攒，其余照旧（不打旧存档标记，G_trim 不归零：它和采样率无关）。
        // 只在读档晚于 prepare 时有用：prepare 本来就清段历史。
        if (std::abs (savedRate - sampleRate) > 1.0e-6 * sampleRate)
            clearSegments();

        return true;
    }
}
