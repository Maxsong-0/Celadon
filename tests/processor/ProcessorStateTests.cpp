/*
  ==============================================================================

    ProcessorStateTests.cpp
    处理器层的存读档语义（T-099，ADR-040；单线程，确定性）：

      - 往返一致：存 → 新实例读 → 再存，参数逐个相同，GLAZE 存档读一遍之后就不再变（不动点）；
      - 运行中的实例读档（换宿主预设、撤销）：参数照换，GLAZE 存档整份不读，存出来的 GLAZE 节点逐字不变（Max 2026-11-12 拍板第 4 项）；
      - 第一次读档照常读：刚建的实例、第一块音频之前连读两份、读档前只送过静音的实例，都读回存档里的实时画像；
      - 旧工程照常读：0.1.8.10 的工程（带曲风 / 交付两个已删参数）、≤ 0.1.8.5 的工程（没有格式号、缺高级母带与 Clay 参数、
        GLAZE 是格式 1 的旧聆听结果）、v0.4 之前的工程（没有 GLAZE 参数）；
      - Lookahead 不进宿主自动化，其余参数照旧可自动化；旧工程里存的 Lookahead 值照读（拍板第 11 项）。

    0.1.8.x 的旧工程不另存夹具：按 addMissingParameterNodes 与 LegacyState.h 认的差别（缺哪些参数、根元素有没有格式号、
    GLAZE 节点的格式号、两个已删参数节点）从当前版本存出来的档改出来。GLAZE 格式 1 的真实存档夹具在 tests/LegacyFixtureTests.cpp
    （链层，这里不复制第二份）；处理器层只关心「格式 1 → 整份丢弃、冷启动、提示一次」这条路，与节点里的内容无关。

  ==============================================================================
*/

#include "ProcessorHarness.h"

#include "../../Source/ParameterIds.h"

#include <iterator>

namespace celadon::processortest
{
    class ProcessorStateTests final : public juce::UnitTest
    {
    public:
        ProcessorStateTests() : juce::UnitTest ("ProcessorState", "CeladonProcessor") {}

        void runTest() override
        {
            const auto projectA = makeProjectA();
            const auto presetD = makePresetD();
            const auto summaryA = summarise (projectA);
            const auto summaryD = summarise (presetD);

            beginTest ("rig precondition: project A carries a live profile, preset D an empty one");
            expect (summaryA.decoded && summaryA.window > 0, describe (summaryA));
            expect (summaryD.decoded && summaryD.window == 0 && summaryD.pending == 0, describe (summaryD));

            testLookaheadNotAutomatable();
            testRoundTrip (projectA, summaryA);
            testRunningInstanceKeepsLiveState (projectA, presetD);
            testFirstLoadRestores (projectA, presetD, summaryA);
            testLegacyProjects (projectA);
            testRunningLoadClearsLegacyNotice (projectA);
        }

    private:
        // ≤ 0.1.8.5 的工程：根元素没有格式号与版本，没有高级母带（master_*）与 Clay（clay_*）参数，GLAZE 节点是格式 1 的旧聆听结果
        // （内容清空：处理器层只看「格式 1 → 整份丢弃、冷启动、提示一次」这条路，见文件头）。
        static juce::MemoryBlock makeFormatOneProject (const juce::MemoryBlock& projectA)
        {
            auto xml = stateXml (projectA);
            xml->removeAttribute ("celadonStateFormat");
            xml->removeAttribute ("celadonVersion");

            for (auto* node = xml->getFirstChildElement(); node != nullptr;)
            {
                auto* next = node->getNextElement();
                const auto id = node->getStringAttribute ("id");

                if (node->hasTagName ("PARAM") && (id.startsWith ("master_") || id.startsWith ("clay_")))
                    xml->removeChildElement (node, true);

                node = next;
            }

            if (auto* glazeNode = xml->getChildByName (glaze::stateTag))
            {
                glazeNode->deleteAllChildElements();
                glazeNode->setAttribute ("formatVersion", 1);
            }

            return toBlock (*xml);
        }

        void testLookaheadNotAutomatable()
        {
            beginTest ("Lookahead is not host-automatable, every other parameter still is");

            Rig rig;
            int checked = 0;

            // 本类的 getParameters() 返回 APVTS，遮住了基类同名函数：显式调基类版本拿全部参数。
            for (auto* parameter : static_cast<juce::AudioProcessor&> (*rig.processor).getParameters())
                if (auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*> (parameter))
                {
                    const auto shouldAutomate = withId->paramID != param::limitLookahead;
                    expect (parameter->isAutomatable() == shouldAutomate, withId->paramID);
                    ++checked;
                }

            expectEquals (checked, (int) std::size (param::allParameterIds));

            beginTest ("a stored Lookahead value still loads, into a fresh and into a running instance");

            Rig donor;
            donor.setParameter (param::limitLookahead, 5.0f);
            const auto withLookahead = donor.save();

            Rig fresh;
            fresh.load (withLookahead);
            expectWithinAbsoluteError (fresh.parameter (param::limitLookahead), 5.0f, 1.0e-4f);

            Rig running;
            running.prepare();
            running.playMusic (6.0);
            running.load (withLookahead);
            expectWithinAbsoluteError (running.parameter (param::limitLookahead), 5.0f, 1.0e-4f);
        }

        void testRoundTrip (const juce::MemoryBlock& projectA, const GlazeSummary& summaryA)
        {
            beginTest ("save / load round trip: parameters identical, GLAZE state is a fixed point after one load");

            // 宿主打开工程的顺序：新实例先读档、再 prepare。
            Rig second;
            second.load (projectA);
            expect (parameterMismatches (*second.processor, projectA).isEmpty(),
                    "loaded parameters differ from the state: " + parameterMismatches (*second.processor, projectA).joinIntoString (", "));
            second.prepare();
            const auto savedTwice = second.save();

            Rig third;
            third.load (savedTwice);
            third.prepare();
            const auto savedThrice = third.save();

            const auto summaryTwice = summarise (savedTwice);
            const auto summaryThrice = summarise (savedThrice);

            // 读档时存档里还没发布的段按序进账并立刻发布（LiveProfile::restoreStateXml），段一个不丢。
            expectEquals (summaryTwice.window + summaryTwice.pending, summaryA.window + summaryA.pending,
                          "segments after reload: " + describe (summaryTwice) + ", original " + describe (summaryA));
            expect (summaryTwice.glazeXml == summaryThrice.glazeXml, "GLAZE node differs between the second and the third save");
            expect (parameterMismatches (*third.processor, projectA).isEmpty(),
                    "parameters after two hops differ from the original: " + parameterMismatches (*third.processor, projectA).joinIntoString (", "));

            // 存档格式号照写（T-032 R10-02）。
            const auto xml = stateXml (savedThrice);
            expect (xml != nullptr && xml->getIntAttribute ("celadonStateFormat", 0) == 1);
        }

        void testRunningInstanceKeepsLiveState (const juce::MemoryBlock& projectA, const juce::MemoryBlock& presetD)
        {
            beginTest ("load into a running instance: parameters swap, the stored GLAZE state is not read");

            Rig running;
            running.setParameter (param::glazeAmount, 80.0f);
            running.prepare();
            running.playMusic (24.0);

            const auto before = summarise (running.save());
            expect (before.window > 0, "rig precondition: the running instance has a live profile (" + describe (before) + ")");

            // 换一个空画像的预设、再换一个带别的画像的工程：两次都只换参数。中间不处理音频，GLAZE 节点应逐字不变。
            for (const auto* blob : { &presetD, &projectA })
            {
                running.load (*blob);
                expect (parameterMismatches (*running.processor, *blob).isEmpty(),
                        "parameters not swapped: " + parameterMismatches (*running.processor, *blob).joinIntoString (", "));

                const auto after = summarise (running.save());
                expect (after.glazeXml == before.glazeXml, "the load changed the running instance's GLAZE state (" + describe (after) + ")");
            }

            // 接着播：画像照旧在（编排器的状态快照每块按已发布的画像报置信度）。
            running.playMusic (1.0);
            expectGreaterThan (running.processor->getGlazeStatus().confidence, 0.0f);
        }

        void testFirstLoadRestores (const juce::MemoryBlock& projectA, const juce::MemoryBlock& presetD, const GlazeSummary& summaryA)
        {
            const auto expectRestored = [this, &projectA, &summaryA] (Rig& rig, const juce::String& what)
            {
                const auto restored = summarise (rig.save());
                expectEquals (restored.window + restored.pending, summaryA.window + summaryA.pending,
                              what + ": project A's live profile not restored (" + describe (restored) + ")");
                expect (parameterMismatches (*rig.processor, projectA).isEmpty(), what + ": parameters not swapped");
            };

            beginTest ("first load restores: a fresh instance loading two states before any audio keeps the second");
            {
                Rig fresh;
                fresh.load (presetD);
                fresh.load (projectA);
                fresh.prepare();
                expectRestored (fresh, "two loads before audio");
            }

            beginTest ("first load restores: load after prepare, before the first block");
            {
                Rig prepared;
                prepared.prepare();
                prepared.load (projectA);
                expectRestored (prepared, "load after prepare");
            }

            beginTest ("first load restores: an instance that has only processed silence");
            {
                Rig silent;
                silent.prepare();
                silent.playSilence (3.0);
                silent.load (projectA);
                expectRestored (silent, "silence only");

                silent.playSilence (0.1);
                expectGreaterThan (silent.processor->getGlazeStatus().confidence, 0.0f);
            }
        }

        void testLegacyProjects (const juce::MemoryBlock& projectA)
        {
            namespace legacy = param::legacy;

            beginTest ("0.1.8.10 project (default genre / delivery nodes) loads, its live profile restores, re-save drops the two nodes");
            {
                auto xml = stateXml (projectA);

                for (const auto* id : { legacy::glazeGenre, legacy::glazeDelivery })
                {
                    auto* node = xml->createNewChildElement ("PARAM");
                    node->setAttribute ("id", id);
                    node->setAttribute ("value", 0.0);
                }

                Rig fresh;
                fresh.load (toBlock (*xml));
                fresh.prepare();
                const auto resaved = fresh.save();

                expect (parameterMismatches (*fresh.processor, projectA).isEmpty());
                expectGreaterThan (summarise (resaved).window, 0);
                expect (! stateXml (resaved)->toString().contains (legacy::glazeGenre));
            }

            beginTest ("<= 0.1.8.5 project (no format, no mastering / Clay parameters, GLAZE format 1): new processing neutral, old listening result discarded with a notice");
            {
                // 读之前先把这几个新参数拧离旧版的值：旧版预设读进参数改过的实例时，新参数也要回到旧版的声音（不沿用读之前的值）。
                Rig fresh;
                fresh.setParameter (param::masterHarshMode, 2.0f);
                fresh.setParameter (param::masterTargetMode, 4.0f);
                fresh.setParameter (param::masterCodecMarginDb, 1.0f);
                fresh.setParameter (param::masterDither, 1.0f);
                fresh.setParameter (param::clayLowCutMode, 2.0f);
                fresh.load (makeFormatOneProject (projectA));

                expectEquals (fresh.parameter (param::masterHarshMode), 1.0f);    // 关
                expectEquals (fresh.parameter (param::masterTargetMode), 0.0f);
                expectEquals (fresh.parameter (param::masterCodecMarginDb), 0.0f);
                expectEquals (fresh.parameter (param::masterDither), 0.0f);
                expectEquals (fresh.parameter (param::clayLowCutMode), 0.0f);     // 自动（默认）
                expectWithinAbsoluteError (fresh.parameter (param::glazeAmount), 62.0f, 1.0e-3f);

                fresh.prepare();
                fresh.playSilence (0.1);
                expect (fresh.processor->getGlazeStatus().legacySessionDiscarded, "a format-1 listening result must be discarded and noticed once");
                expectEquals (summarise (fresh.save()).window, 0);
            }

            beginTest ("pre-v0.4 project (no GLAZE parameter): GLAZE loads as 0, not the fresh default 35");
            {
                auto xml = stateXml (projectA);

                for (auto* node = xml->getFirstChildElement(); node != nullptr;)
                {
                    auto* next = node->getNextElement();

                    if (node->hasTagName ("PARAM") && node->getStringAttribute ("id") == param::glazeAmount)
                        xml->removeChildElement (node, true);

                    node = next;
                }

                Rig fresh;
                fresh.load (toBlock (*xml));
                expectEquals (fresh.parameter (param::glazeAmount), 0.0f);
            }
        }

        void testRunningLoadClearsLegacyNotice (const juce::MemoryBlock& projectA)
        {
            // 运行中读档不读 GLAZE、不冷启动（ADR-040），上一次读档点亮的旧工程提示（legacySessionDiscarded）跟着清（GlazeController::keepStateOnLoad）：
            // 不清的话，读进干净的工程之后提示还一直亮着。另一侧（刚建的实例读格式 1 → 提示）见 testLegacyProjects。
            const auto formatOne = makeFormatOneProject (projectA);

            beginTest ("a running load clears the legacy notice an earlier load raised");
            {
                Rig rig;
                rig.load (formatOne);
                rig.prepare();
                rig.playMusic (24.0);
                expect (rig.processor->getGlazeStatus().legacySessionDiscarded, "rig precondition: the format-1 load raised the notice");
                expect (summarise (rig.save()).window > 0, "rig precondition: the instance is running with a live profile");

                rig.load (projectA);
                expect (! rig.processor->getGlazeStatus().legacySessionDiscarded, "the notice survived a running load of a current project");
            }

            beginTest ("a running load of a format-1 project raises no notice (nothing cold-started)");
            {
                Rig rig;
                rig.setParameter (param::glazeAmount, 80.0f);
                rig.prepare();
                rig.playMusic (24.0);
                const auto before = summarise (rig.save());
                expect (before.window > 0, "rig precondition: the instance is running with a live profile (" + describe (before) + ")");

                rig.load (formatOne);
                expect (! rig.processor->getGlazeStatus().legacySessionDiscarded, "a running load lit the legacy notice");
                expectWithinAbsoluteError (rig.parameter (param::glazeAmount), 62.0f, 1.0e-3f);   // 参数照换
                expect (summarise (rig.save()).glazeXml == before.glazeXml, "a running load of a format-1 project touched the GLAZE state");
            }
        }
    };

    static ProcessorStateTests processorStateTests;
}
