/*
  ==============================================================================

    ProcessorTestMain.cpp
    celadon_processor_tests 入口（T-099）：把整个插件（处理器 + 它链接要的编辑器与界面源码）编进来，跑「CeladonProcessor」
    类别的 juce::UnitTest。测试里不建编辑器、不跑消息循环；JUCE 的消息管理器照插件宿主的样子先建好（处理器与 APVTS 构造时
    都起计时器）。

    与 celadon_tests 分开：那边只编无头的 DSP 与界面计算、不依赖 JUCE 界面模块；这边要 juce_audio_processors 与界面模块
    （Linux 上要 freetype2 / fontconfig / x11 的开发头，见 scripts/fetch-gui-dev.sh）。也不替换全局 operator new
    （分配计数是 celadon_tests 的事，这里还要在 ThreadSanitizer 下跑）。

  ==============================================================================
*/

#include <JuceHeader.h>

#include <iostream>

namespace
{
    constexpr const char* testCategory = "CeladonProcessor";
    constexpr const char* benchCategory = "CeladonProcessorBench";

    class ConsoleRunner final : public juce::UnitTestRunner
    {
        void logMessage (const juce::String& message) override
        {
            std::cout << message << std::endl;
        }
    };

    int usage (const char* unknown)
    {
        std::cout << "未知参数：" << unknown << "\n"
                  << "用法：celadon_processor_tests                 跑 " << testCategory << " 类别的全部用例\n"
                  << "      celadon_processor_tests --only A,B      只跑名字为 A、B 的用例类（如 ProcessorConcurrency）\n"
                  << "      celadon_processor_tests --bench         跑基准类别：存读档持锁时间（ProcessorStateLockBench，约 1 分钟，要独占机器）\n"
                  << "                                              与 T-099 拆锁前后的存档摘要（ProcessorStateDigest）；配 --only 只跑其一" << std::endl;
        return 2;
    }
}

int main (int argc, char* argv[])
{
    const juce::ScopedJuceInitialiser_GUI juceGui;

    ConsoleRunner runner;
    runner.setAssertOnFailure (false);

    juce::StringArray only;
    auto bench = false;

    for (int i = 1; i < argc; ++i)
    {
        const juce::String argument (argv[i]);

        if (argument == "--only" && i + 1 < argc)
            only.addTokens (juce::String (argv[++i]), ",", {});
        else if (argument == "--bench")
            bench = true;
        else
            return usage (argv[i]);
    }

    only.trim();
    only.removeEmptyStrings();

    const auto* category = bench ? benchCategory : testCategory;
    juce::Array<juce::UnitTest*> selected;

    for (auto* test : juce::UnitTest::getTestsInCategory (category))
        if (only.isEmpty() || only.contains (test->getName()))
            selected.add (test);

    // 名字一个都对不上、或有对不上的，报错退出，不当作通过。
    if (selected.isEmpty() || (! only.isEmpty() && selected.size() != only.size()))
    {
        std::cout << "--only 里有对不上的用例类名。" << category << " 类别里有：";

        for (auto* test : juce::UnitTest::getTestsInCategory (category))
            std::cout << " " << test->getName();

        std::cout << std::endl;
        return 2;
    }

    runner.runTests (selected);

    int passes = 0, failures = 0;

    for (int i = 0; i < runner.getNumResults(); ++i)
    {
        passes += runner.getResult (i)->passes;
        failures += runner.getResult (i)->failures;
    }

    std::cout << "\n==== celadon_processor_tests: " << passes << " passed, " << failures << " failed ====" << std::endl;
    return failures == 0 ? 0 : 1;
}
