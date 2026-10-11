#include "ReferenceSpectrum.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <thread>

namespace celadon
{
    namespace
    {
        constexpr double silencePower = 1.0e-8; // 立体声均方 -80 dBFS
        constexpr float relativeFloorDb = -120.0f;
        constexpr int snapshotReadAttempts = 8;

        bool readNumber (const juce::XmlElement& xml, const char* attribute, double& result)
        {
            if (! xml.hasAttribute (attribute))
                return false;
            const auto text = xml.getStringAttribute (attribute).trim();
            const auto* start = text.toRawUTF8();
            char* end = nullptr;
            errno = 0;
            result = std::strtod (start, &end);
            return start != end && *end == '\0' && errno != ERANGE && std::isfinite (result);
        }
    }

    ReferenceSpectrum::Publication::Publication() noexcept
    {
        static_assert (std::is_trivially_copyable_v<Payload>);
        static_assert (std::atomic<std::uint32_t>::is_always_lock_free);
        static_assert (std::atomic<std::uint64_t>::is_always_lock_free);
        for (auto& word : words)
            word.store (0);
    }

    void ReferenceSpectrum::Publication::write (const Payload& payload) noexcept
    {
        std::array<std::uint32_t, wordCount> raw {};
        std::memcpy (raw.data(), &payload, sizeof (payload));
        // 顺序一致性（各原子操作都用默认的 seq_cst）同样给逐字读取与序号标记排定先后；
        // 读者从不依赖与写者竞争的普通内存访问。
        const auto before = sequence.load();
        sequence.store (before + 1);
        for (size_t i = 0; i < wordCount; ++i)
            words[i].store (raw[i]);
        sequence.store (before + 2);
    }

    bool ReferenceSpectrum::Publication::read (Payload& result) const noexcept
    {
        std::array<std::uint32_t, wordCount> raw {};
        for (int attempt = 0; attempt < snapshotReadAttempts; ++attempt)
        {
            const auto before = sequence.load();
            if ((before & 1u) != 0)
                continue;
            for (size_t i = 0; i < wordCount; ++i)
                raw[i] = words[i].load();
            if (sequence.load() == before)
            {
                std::memcpy (static_cast<void*> (&result), raw.data(), sizeof (result));
                return true;
            }
        }
        // 音频线程上不会无限自旋，也绝不交出新旧两代混在一起的曲线。
        return false;
    }

    void ReferenceSpectrum::advanceTicket (std::atomic<std::uint64_t>& destination,
                                            std::uint64_t ticket) noexcept
    {
        auto existing = destination.load();
        while (existing < ticket && ! destination.compare_exchange_weak (existing, ticket)) {}
    }

    void ReferenceSpectrum::requestLearn() noexcept
    {
        advanceTicket (learnTicket, nextTicket.fetch_add (1) + 1);
    }

    void ReferenceSpectrum::cancelLearn() noexcept
    {
        advanceTicket (cancelTicket, nextTicket.fetch_add (1) + 1);
    }

    void ReferenceSpectrum::clear() noexcept
    {
        advanceTicket (clearTicket, nextTicket.fetch_add (1) + 1);
    }

    void ReferenceSpectrum::resetLearning() noexcept
    {
        frameFill = 0;
        analysisPending = false;
        captureInterrupted = false;
        requireFullFinalFrame = false;
        validSamples = 0;
        accumulatedBands.fill (0.0);
        accumulatedTotal = 0.0;
    }

    void ReferenceSpectrum::applyCommands() noexcept
    {
        Payload restore {};
        const auto gotRestore = restored.read (restore);
        const auto clearAt = clearTicket.load();
        const auto learnAt = learnTicket.load();
        const auto cancelAt = cancelTicket.load();
        const auto restoreAt = gotRestore ? restore.referenceTicket : std::uint64_t { 0 };

        if (restoreAt > current.referenceTicket && restoreAt > clearAt)
        {
            current.snapshot.hasReference = restore.snapshot.hasReference;
            current.snapshot.relativeDb = restore.snapshot.relativeDb;
            current.referenceTicket = restoreAt;
        }
        if (clearAt > current.referenceTicket)
        {
            current.snapshot.hasReference = false;
            current.snapshot.relativeDb.fill (0.0f);
            current.referenceTicket = clearAt;
        }

        const auto newest = std::max ({ learnAt, cancelAt, clearAt, restoreAt });
        if (newest > current.controlTicket)
        {
            resetLearning();
            learning = newest == learnAt;
            current.controlTicket = newest;
        }
    }

    void ReferenceSpectrum::publish() noexcept
    {
        auto& snapshot = current.snapshot;
        snapshot.state = learning ? (snapshot.sidechainConnected ? State::learning : State::waitingForSidechain)
                                  : (snapshot.hasReference ? State::ready : State::idle);
        snapshot.seconds = learning ? (double) validSamples / sampleRate
                                    : (snapshot.hasReference ? learningSeconds : 0.0);
        published.write (current);
    }

    void ReferenceSpectrum::prepare (const juce::dsp::ProcessSpec& spec)
    {
        sampleRate = std::isfinite (spec.sampleRate) && spec.sampleRate >= 8000.0
                        && spec.sampleRate <= 768000.0 ? spec.sampleRate : 48000.0;
        int order = 13;
        while (order < 17 && (double) (1 << order) < sampleRate / 6.0)
            ++order;
        fftSize = 1 << order;
        targetSamples = (std::int64_t) std::ceil (sampleRate * learningSeconds);
        window.resize ((size_t) fftSize);
        bitReversal.resize ((size_t) fftSize);
        twiddleReal.resize ((size_t) fftSize / 2);
        twiddleImag.resize ((size_t) fftSize / 2);
        power.resize ((size_t) fftSize / 2 + 1);
        for (auto& channel : input)
            channel.assign ((size_t) fftSize * 2, 0.0f);
        for (auto& channel : analysisInput)
            channel.assign ((size_t) fftSize * 2, 0.0f);
        for (auto& channel : transform)
            channel.assign ((size_t) fftSize * 2, 0.0f);
        for (int i = 0; i < fftSize; ++i)
        {
            window[(size_t) i] = (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi
                                                              * (double) i / (double) fftSize));
            int reversed = 0, bits = i;
            for (int bit = 0; bit < order; ++bit)
            {
                reversed = (reversed << 1) | (bits & 1);
                bits >>= 1;
            }
            bitReversal[(size_t) i] = reversed;
            if (i < fftSize / 2)
            {
                const auto angle = -juce::MathConstants<double>::twoPi * (double) i / (double) fftSize;
                twiddleReal[(size_t) i] = (float) std::cos (angle);
                twiddleImag[(size_t) i] = (float) std::sin (angle);
            }
        }

        weights.clear();
        const auto binHz = sampleRate / (double) fftSize;
        const auto halfBand = std::pow (2.0, 1.0 / 6.0);
        for (int b = 0; b < numBands; ++b)
        {
            // 段边界与 BalanceModule 完全相同，都是精确的 1/3 倍频程（它对外的中心频率接口
            // getBandCentreHz 返回的是惯用的取整显示标签，即 IEC 61260 标称值）。
            const auto centre = 1000.0 * std::pow (2.0, (double) (b - 14) / 3.0);
            const auto low = centre / halfBand, high = centre * halfBand;
            auto& band = bands[(size_t) b];
            band.first = std::max (1, (int) std::floor (low / binHz - 0.5) + 1);
            const auto last = std::min (fftSize / 2, (int) std::ceil (high / binHz + 0.5) - 1);
            band.count = std::max (0, last - band.first + 1);
            band.offset = (int) weights.size();
            for (int k = band.first; k <= last; ++k)
                weights.push_back (std::max (0.0, std::min (high, ((double) k + 0.5) * binHz)
                                                   - std::max (low, ((double) k - 0.5) * binHz)) / binHz);
        }
        // 结束已经在跑的学习，但保留还没到达 process 的新「学习参考」请求
        // （例如刚点完按钮，宿主紧接着就 prepare）。
        applyCommands();
        learning = learning && current.controlTicket > lastAudioControlTicket;
        resetLearning();
        current.snapshot.sidechainConnected = false;
        publish();
    }

    void ReferenceSpectrum::startAnalysisIfReady() noexcept
    {
        if (analysisPending || ! learning) return;
        const auto needed = requireFullFinalFrame ? fftSize : (int) std::min ((std::int64_t) fftSize, targetSamples - validSamples);
        if (needed <= 0) return;
        if (frameFill < needed)
        {
            // 中间断开过时，不能把断开前后的两截拼进同一个 FFT 窗。
            // 不过已经采齐的结尾前缀（缓冲开头、够本帧所需样本数的那一段）照样保留。
            if (captureInterrupted) { frameFill = 0; captureInterrupted = false; }
            return;
        }
        input.swap (analysisInput); // 只交换 vector：不分配、不拷贝、不加锁。
        analysisLength = needed;
        analysisSuffix = frameFill - needed;
        frameFill = analysisSuffix; // 先预留后缀的位置；后缀由下文加窗阶段的工作逐步拷回。
        captureInterrupted = false;
        analysisPending = true;
        analysisStage = AnalysisStage::window;
        analysisIndex = 0;
        analysisEnergy = analysisWindowEnergy = analysisTotal = analysisInBand = 0.0;
        analysisBands.fill (0.0);
    }

    void ReferenceSpectrum::finishAnalysis (bool valid) noexcept
    {
        if (valid) validSamples += analysisLength;
        else if (analysisLength < fftSize)
            // 被拒的极短结尾前缀不能反复排出整份 FFT，排得比算得还快。
            // 保留它后面已采到的后缀，回到按整窗分析；
            // 这样最多比 10 秒下限多出一个有效分析窗，
            // 而且不丢参考样本。
            requireFullFinalFrame = true;
        analysisPending = false;
        if (validSamples >= targetSamples) finishLearning();
    }

    void ReferenceSpectrum::advanceAnalysis (int workUnits) noexcept
    {
        // 工作量一律按「音频样本」分摊，不按回调边界。因此数值运算顺序
        // 与已学参考曲线（committed）的形状都与块长无关。
        while (analysisPending && workUnits-- > 0)
        {
            switch (analysisStage)
            {
                case AnalysisStage::window:
                {
                    const auto i = analysisIndex++;
                    if (i < analysisSuffix)
                        for (int ch = 0; ch < 2; ++ch)
                            input[(size_t) ch][(size_t) i] = analysisInput[(size_t) ch][(size_t) (analysisLength + i)];
                    if (i < fftSize)
                    {
                        const auto destination = (size_t) (2 * bitReversal[(size_t) i]);
                        const auto w = (double) window[(size_t) i];
                        if (i < analysisLength) analysisWindowEnergy += w * w;
                        for (int ch = 0; ch < 2; ++ch)
                        {
                            const auto value = i < analysisLength ? analysisInput[(size_t) ch][(size_t) i] : 0.0f;
                            analysisEnergy += (double) value * value;
                            transform[(size_t) ch][destination] = value * window[(size_t) i];
                            transform[(size_t) ch][destination + 1] = 0.0f;
                        }
                    }
                    if (analysisIndex == std::max (fftSize, analysisSuffix))
                    {
                        if (analysisEnergy / (2.0 * analysisLength) < silencePower || analysisWindowEnergy <= 0.0)
                            finishAnalysis (false);
                        else
                        {
                            analysisStage = AnalysisStage::fft;
                            fftWidth = 2; fftGroup = fftJ = 0;
                        }
                    }
                    break;
                }
                case AnalysisStage::fft:
                {
                    const auto half = fftWidth / 2;
                    const auto a = (size_t) (2 * (fftGroup + fftJ));
                    const auto b = (size_t) (2 * (fftGroup + fftJ + half));
                    const auto twiddle = (size_t) (fftJ * (fftSize / fftWidth));
                    for (int ch = 0; ch < 2; ++ch)
                    {
                        auto& buffer = transform[(size_t) ch];
                        const auto re = buffer[b] * twiddleReal[twiddle] - buffer[b + 1] * twiddleImag[twiddle];
                        const auto im = buffer[b] * twiddleImag[twiddle] + buffer[b + 1] * twiddleReal[twiddle];
                        const auto ar = buffer[a], ai = buffer[a + 1];
                        buffer[a] = ar + re; buffer[a + 1] = ai + im;
                        buffer[b] = ar - re; buffer[b + 1] = ai - im;
                    }
                    if (++fftJ == half)
                    {
                        fftJ = 0;
                        fftGroup += fftWidth;
                        if (fftGroup == fftSize)
                        {
                            fftGroup = 0;
                            fftWidth *= 2;
                            if (fftWidth > fftSize)
                            {
                                analysisStage = AnalysisStage::power;
                                analysisIndex = 1;
                            }
                        }
                    }
                    break;
                }
                case AnalysisStage::power:
                {
                    const auto k = analysisIndex++;
                    double value = 0.0;
                    for (int ch = 0; ch < 2; ++ch)
                    {
                        const auto& buffer = transform[(size_t) ch];
                        const auto re = (double) buffer[(size_t) (2 * k)], im = (double) buffer[(size_t) (2 * k + 1)];
                        value += (re * re + im * im) * (k == fftSize / 2 ? 0.5 : 1.0);
                    }
                    power[(size_t) k] = value;
                    analysisTotal += value;
                    if (analysisIndex > fftSize / 2)
                    {
                        if (! std::isfinite (analysisTotal) || analysisTotal <= 0.0) finishAnalysis (false);
                        else { analysisStage = AnalysisStage::bands; analysisBand = analysisBandBin = 0; }
                    }
                    break;
                }
                case AnalysisStage::bands:
                {
                    const auto& band = bands[(size_t) analysisBand];
                    if (analysisBandBin < band.count)
                    {
                        analysisBands[(size_t) analysisBand] += power[(size_t) (band.first + analysisBandBin)]
                                                               * weights[(size_t) (band.offset + analysisBandBin)];
                        ++analysisBandBin;
                    }
                    else
                    {
                        analysisInBand += analysisBands[(size_t) analysisBand];
                        analysisBandBin = 0;
                        if (++analysisBand == numBands)
                        {
                            if (analysisInBand / ((double) fftSize * analysisWindowEnergy) < silencePower
                                || analysisInBand / analysisTotal < 1.0e-6) finishAnalysis (false);
                            else { analysisStage = AnalysisStage::accumulate; analysisIndex = 0; }
                        }
                    }
                    break;
                }
                case AnalysisStage::accumulate:
                {
                    const auto weight = (double) analysisLength / analysisWindowEnergy;
                    if (analysisIndex < numBands)
                    {
                        const auto b = (size_t) analysisIndex++;
                        accumulatedBands[b] += analysisBands[b] * weight;
                    }
                    else
                    {
                        accumulatedTotal += analysisTotal * weight;
                        finishAnalysis (true);
                    }
                    break;
                }
            }
        }
    }

    void ReferenceSpectrum::finishLearning() noexcept
    {
        if (accumulatedTotal > 0.0 && std::isfinite (accumulatedTotal))
        {
            for (size_t b = 0; b < accumulatedBands.size(); ++b)
                current.snapshot.relativeDb[b] = (float) juce::jlimit ((double) relativeFloorDb, 0.0,
                    10.0 * std::log10 (std::max (1.0e-12, accumulatedBands[b] / accumulatedTotal)));
            current.snapshot.hasReference = true;
            current.referenceTicket = current.controlTicket;
        }
        learning = false;
    }

    void ReferenceSpectrum::process (const juce::AudioBuffer<float>& sidechain, bool connected) noexcept
    {
        applyCommands();
        lastAudioControlTicket = current.controlTicket;
        current.snapshot.sidechainConnected = connected && sidechain.getNumChannels() > 0;
        if (! learning || fftSize == 0) { publish(); return; }
        if (! current.snapshot.sidechainConnected) captureInterrupted = true;
        const auto* left = current.snapshot.sidechainConnected ? sidechain.getReadPointer (0) : nullptr;
        const auto* right = current.snapshot.sidechainConnected ? sidechain.getReadPointer (std::min (1, sidechain.getNumChannels() - 1)) : nullptr;
        for (int i = 0; i < sidechain.getNumSamples() && learning; ++i)
        {
            startAnalysisIfReady();
            // 两窗容量（2 × fftSize）还有一个作用：结尾的短前缀被拒时，多采出来的
            // 后缀也留得住。后缀的搬运同样在那份固定工作预算（每样本 16 个单元）里完成；
            // 之后的新样本接在预留的后缀后面。
            if (left != nullptr && ! captureInterrupted && frameFill < 2 * fftSize)
            {
                const auto sanitize = [] (float value) noexcept
                {
                    return std::isfinite (value) && std::abs (value) <= 1.0e6f ? value : 0.0f;
                };
                input[0][(size_t) frameFill] = sanitize (left[i]);
                input[1][(size_t) frameFill] = sanitize (right[i]);
                ++frameFill;
            }
            startAnalysisIfReady();
            advanceAnalysis (16);
            startAnalysisIfReady();
        }
        publish();
    }

    ReferenceSpectrum::Snapshot ReferenceSpectrum::getSnapshot() const noexcept
    {
        Snapshot snapshot {};
        readSnapshot (snapshot);
        return snapshot;
    }

    bool ReferenceSpectrum::readSnapshot (Snapshot& snapshot) const noexcept
    {
        Payload result {}, restore {};
        if (! published.read (result))
            return false; // 自洽、安全的退路：绝不交出读撕裂的已学参考曲线。
        // 并发的 XML 写者可能还在发布下一份参考。在那份恢复数据读得到之前，
        // 先沿用 published 里自洽的已学参考曲线；哪怕只是一瞬间的空快照，
        // 也会把目标频谱修正（target EQ）关掉。
        const auto restoreAt = restored.read (restore) ? restore.referenceTicket : std::uint64_t { 0 };
        const auto clearAt = clearTicket.load(), learnAt = learnTicket.load(), cancelAt = cancelTicket.load();
        if (restoreAt > result.referenceTicket && restoreAt > clearAt)
        {
            result.snapshot.hasReference = restore.snapshot.hasReference;
            result.snapshot.relativeDb = restore.snapshot.relativeDb;
            result.referenceTicket = restoreAt;
        }
        if (clearAt > result.referenceTicket)
        {
            result.snapshot.hasReference = false;
            result.snapshot.relativeDb.fill (0.0f);
        }
        const auto newest = std::max ({ learnAt, cancelAt, clearAt, restoreAt });
        if (newest > result.controlTicket)
        {
            const bool pendingLearn = newest == learnAt;
            result.snapshot.state = pendingLearn
                ? (result.snapshot.sidechainConnected ? State::learning : State::waitingForSidechain)
                : (result.snapshot.hasReference ? State::ready : State::idle);
            result.snapshot.seconds = pendingLearn ? 0.0 : (result.snapshot.hasReference ? learningSeconds : 0.0);
        }
        snapshot = result.snapshot;
        return true;
    }

    std::unique_ptr<juce::XmlElement> ReferenceSpectrum::createStateXml() const
    {
        // 保存本来就要分配 XML，是非音频线程的操作。与尝试次数有上限的实时读取
        // getSnapshot 不同，这里遇到争用就一直重试，而不是把那个故意保守的
        // 「无参考」退路存进工程。
        Snapshot snapshot {};
        while (! readSnapshot (snapshot))
            std::this_thread::yield();
        auto xml = std::make_unique<juce::XmlElement> ("REFERENCE_SPECTRUM");
        xml->setAttribute ("format", 1);
        xml->setAttribute ("seconds", snapshot.hasReference ? learningSeconds : 0.0);
        if (snapshot.hasReference)
            for (int b = 0; b < numBands; ++b)
            {
                auto* band = xml->createNewChildElement ("BAND");
                band->setAttribute ("index", b);
                band->setAttribute ("relativeDb", (double) snapshot.relativeDb[(size_t) b]);
            }
        return xml;
    }

    void ReferenceSpectrum::restoreStateXml (const juce::XmlElement* xml)
    {
        Payload payload {};
        double seconds = 0.0;
        bool valid = xml != nullptr && xml->hasTagName ("REFERENCE_SPECTRUM")
            && xml->getStringAttribute ("format") == "1" && readNumber (*xml, "seconds", seconds)
            && seconds == learningSeconds && xml->getNumChildElements() == numBands;
        std::array<bool, numBands> seen {};
        double relativePower = 0.0;
        if (valid)
            for (const auto* band : xml->getChildIterator())
            {
                double index = 0.0, db = 0.0;
                if (! band->hasTagName ("BAND") || ! readNumber (*band, "index", index)
                    || index < 0.0 || index >= numBands || index != std::floor (index)
                    || ! readNumber (*band, "relativeDb", db) || db < relativeFloorDb || db > 0.0
                    || seen[(size_t) index] || band->getNumChildElements() != 0)
                {
                    valid = false;
                    break;
                }
                seen[(size_t) index] = true;
                payload.snapshot.relativeDb[(size_t) index] = (float) db;
                relativePower += std::pow (10.0, db / 10.0);
            }
        // 各段互不重叠，加起来不可能超过总 RMS 功率。
        // 允许 float / XML 的舍入误差，但拒收全部贴在下限、凭空捏造的参考。
        valid = valid && relativePower >= 1.0e-6 && relativePower <= 1.0001;
        if (valid)
        {
            payload.snapshot.hasReference = true;
            payload.snapshot.state = State::ready;
            payload.snapshot.seconds = learningSeconds;
        }
        else
            payload = {};
        const std::lock_guard<std::mutex> lock (restoreMutex);
        payload.referenceTicket = payload.controlTicket = nextTicket.fetch_add (1) + 1;
        restored.write (payload);
    }
}
