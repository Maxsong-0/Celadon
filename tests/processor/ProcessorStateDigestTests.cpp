/*
  ==============================================================================

    ProcessorStateDigestTests.cpp
    T-099 拆锁（ADR-040）前后存档逐字节相同的证据：几种画像大小下打印整份存档与其中 GLAZE 节点的摘要（state-digest 行），
    并核对同一场景跑两次摘要相同（存档对同一段输入是确定的，两棵树的摘要才有得比）。

    只用拆锁之前（集成分支 f4741f5）就有的公开接口：ProcessorHarness.h 的台架、getStateInformation / setStateInformation、
    glaze::stateTag。同一份文件原样放进拆锁之前的树（CMakeLists.txt 的 celadon_processor_tests 源清单加这一行）再跑一遍，
    两边的 state-digest 行逐行相同 = 存出来的字节没变（含「读档 → 再存」这条路：读档拆成暂存 + 换入之后，读回的状态也没变）。

    场景：
      empty       刚 prepare、没放过音频（实时画像是空的）；
      partial     工程 A 的参数，放 60 s 音乐（窗口没满，通常还有没发布的段）；
      full        GLAZE 100，放 512 × 2 + 60 s 音乐把回看窗口填满（512 段，最老的段开始被挤出）——最坏的存档；
      partial-reloaded / full-reloaded
                  一台新实例读上面那份存档（第一次读档：落位那条路）、prepare、马上再存；
      partial-reloaded-played
                  读 partial 的存档之后再放 10 s 音乐再存（读回的状态接着跑也与改前相同）。
    full 要放约 18 分钟的音频（两遍），整组约一两分钟，所以挂在基准类别里（--bench --only ProcessorStateDigest），不进 CI、
    不在 TSan 下跑。入口：docs/validation/2026-11-13-T099-state-lock/run-processor-tests.sh digest。

  ==============================================================================
*/

#include "ProcessorHarness.h"

#include <cstdint>
#include <functional>

namespace celadon::processortest
{
    namespace
    {
        // FNV-1a 64：只用来比「两边是不是同一串字节」，不防碰撞攻击。
        std::uint64_t fnv1a (const void* data, size_t size) noexcept
        {
            auto h = 14695981039346656037ULL;
            const auto* bytes = static_cast<const unsigned char*> (data);

            for (size_t i = 0; i < size; ++i)
            {
                h ^= (std::uint64_t) bytes[i];
                h *= 1099511628211ULL;
            }

            return h;
        }

        juce::String hexOf (std::uint64_t value)
        {
            return juce::String::toHexString ((juce::int64) value).paddedLeft ('0', 16);
        }

        // 一份存档的摘要：整份二进制、GLAZE 节点原文；另带画像的形状（窗口段数、没发布的段、字节数）给人看。
        struct Digest
        {
            juce::String whole, glaze, shape;

            bool operator== (const Digest& other) const   { return whole == other.whole && glaze == other.glaze; }
        };

        Digest digestOf (const juce::MemoryBlock& block)
        {
            const auto summary = summarise (block);
            const auto glazeText = summary.glazeXml.toStdString();
            Digest d;
            d.whole = hexOf (fnv1a (block.getData(), block.getSize()));
            d.glaze = hexOf (fnv1a (glazeText.data(), glazeText.size()));
            d.shape = describe (summary) + ", state blob " + juce::String ((juce::int64) block.getSize()) + " bytes";
            return d;
        }

        juce::MemoryBlock saveEmpty()
        {
            Rig rig;
            rig.prepare();
            return rig.save();
        }

        juce::MemoryBlock savePartial()
        {
            Rig rig;
            applyProjectA (rig);
            rig.prepare();
            rig.playMusic (60.0);
            return rig.save();
        }

        juce::MemoryBlock saveFull()
        {
            Rig rig;
            rig.setParameter (param::glazeAmount, 100.0f);
            rig.prepare();
            rig.playMusic (glaze::LiveProfile::windowCapacity * 2.0 + 60.0);
            return rig.save();
        }

        // 第一次读档（新实例，读档先于 prepare，同宿主打开工程），之后放 playSeconds 秒音乐再存。
        juce::MemoryBlock reloadAndSave (const juce::MemoryBlock& source, double playSeconds)
        {
            Rig rig;
            rig.load (source);
            rig.prepare();

            if (playSeconds > 0.0)
                rig.playMusic (playSeconds);

            return rig.save();
        }
    }

    class ProcessorStateDigest final : public juce::UnitTest
    {
    public:
        ProcessorStateDigest() : juce::UnitTest ("ProcessorStateDigest", "CeladonProcessorBench") {}

        void runTest() override
        {
            beginTest ("state digests (compare line by line with the same file built in the tree before the T-099 lock split)");

            const auto empty = scenario ("empty", saveEmpty);
            const auto partial = scenario ("partial", savePartial);
            const auto full = scenario ("full", saveFull);

            scenario ("partial-reloaded", [&partial] { return reloadAndSave (partial, 0.0); });
            scenario ("full-reloaded", [&full] { return reloadAndSave (full, 0.0); });
            scenario ("partial-reloaded-played", [&partial] { return reloadAndSave (partial, 10.0); });

            expectEquals (summarise (full).window, glaze::LiveProfile::windowCapacity, "the full scenario fills the look-back window");
            expect (summarise (empty).hasGlazeNode, "even an empty profile writes a GLAZE node");
        }

    private:
        // 跑两遍（两台独立的实例），摘要必须相同；打印第一遍的摘要。返回第一遍的存档给后面的场景用。
        juce::MemoryBlock scenario (const char* name, const std::function<juce::MemoryBlock()>& make)
        {
            const auto first = make();
            const auto second = make();
            const auto d1 = digestOf (first), d2 = digestOf (second);

            logMessage ("state-digest " + juce::String (name) + ": " + d1.whole);
            logMessage ("state-digest " + juce::String (name) + "/glaze: " + d1.glaze);
            logMessage ("  (" + juce::String (name) + ": " + d1.shape + ")");
            expect (d1 == d2, juce::String (name) + ": the same scenario saves the same bytes twice ("
                                  + d1.whole + " vs " + d2.whole + ")");
            return first;
        }
    };

    static ProcessorStateDigest processorStateDigest;
}
