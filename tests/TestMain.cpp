/*
  ==============================================================================

    TestMain.cpp
    celadon_tests 入口：跑「Celadon」分类下的全部 juce::UnitTest，失败则返回 1。

    另外在这里替换全局 operator new / delete（整个测试程序只能有一处定义）：每次分配给「当前线程」的
    计数器加一，测试据此断言音频路径（process / setParams / restartWithFadeIn）不分配内存
    （v0.3 计划 B6；2026-09-25 评审 R2-03）。计数只覆盖经 operator new 的分配——std::vector、make_unique、
    juce::String 等；JUCE HeapBlock（AudioBuffer 的存储）直接调 std::malloc，不在计数内，那一类靠评审核对。
    计数器是 thread_local 的普通整数：没有动态初始化、不加锁，替换后的 new 自身也不会再分配。

  ==============================================================================
*/

#include <juce_core/juce_core.h>

#include "TestHelpers.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <new>

#if defined (_WIN32)
 #include <malloc.h>   // _aligned_malloc / _aligned_free：Windows 没有 posix_memalign，对齐分配要配对释放
// 控制台按 UTF-8 输出（中文日志）；只声明这一个函数，不把 windows.h 带进来。
extern "C" __declspec (dllimport) int __stdcall SetConsoleOutputCP (unsigned int codePage);
#endif

namespace
{
    thread_local std::uint64_t allocationsOnThisThread = 0;

    void* allocate (std::size_t size)
    {
        ++allocationsOnThisThread;

        // 大小为 0 也要返回唯一的非空指针（标准要求）。
        if (auto* memory = std::malloc (size == 0 ? 1 : size))
            return memory;

        throw std::bad_alloc();
    }

    void* allocateAligned (std::size_t size, std::align_val_t alignment)
    {
        ++allocationsOnThisThread;

        // posix_memalign 要求对齐至少是 sizeof (void*) 且为 2 的幂；operator new 的对齐参数本身保证是 2 的幂。
        const auto align = std::max (static_cast<std::size_t> (alignment), sizeof (void*));
        void* memory = nullptr;

       #if defined (_WIN32)
        memory = _aligned_malloc (size == 0 ? 1 : size, align);

        if (memory != nullptr)
            return memory;
       #else
        if (posix_memalign (&memory, align, size == 0 ? 1 : size) == 0)
            return memory;
       #endif

        throw std::bad_alloc();
    }

    // 对齐版 new 分出来的内存：POSIX 用 free，Windows 必须用 _aligned_free。
    void freeAligned (void* memory) noexcept
    {
       #if defined (_WIN32)
        _aligned_free (memory);
       #else
        std::free (memory);
       #endif
    }
}

std::uint64_t celadon::test::allocationCountOnThisThread() noexcept
{
    return allocationsOnThisThread;
}

// nothrow 版本由标准库按「调用下面的普通版本并吞掉异常」实现（[new.delete.single]），不必另写，计数也不会重复。
void* operator new (std::size_t size)                                   { return allocate (size); }
void* operator new[] (std::size_t size)                                 { return allocate (size); }
void* operator new (std::size_t size, std::align_val_t alignment)       { return allocateAligned (size, alignment); }
void* operator new[] (std::size_t size, std::align_val_t alignment)     { return allocateAligned (size, alignment); }

void operator delete (void* memory) noexcept                                        { std::free (memory); }
void operator delete[] (void* memory) noexcept                                      { std::free (memory); }
void operator delete (void* memory, std::size_t) noexcept                           { std::free (memory); }
void operator delete[] (void* memory, std::size_t) noexcept                         { std::free (memory); }
void operator delete (void* memory, std::align_val_t) noexcept                      { freeAligned (memory); }
void operator delete[] (void* memory, std::align_val_t) noexcept                    { freeAligned (memory); }
void operator delete (void* memory, std::size_t, std::align_val_t) noexcept         { freeAligned (memory); }
void operator delete[] (void* memory, std::size_t, std::align_val_t) noexcept       { freeAligned (memory); }

namespace
{
    // 把 JUCE 单元测试的日志直接打到标准输出，服务器上跑时能看到。
    class ConsoleRunner  : public juce::UnitTestRunner
    {
        void logMessage (const juce::String& message) override
        {
            std::cout << message << std::endl;
        }
    };
}

// 用法：celadon_tests                       全部测试类
//       celadon_tests --only A,B             只跑名字为 A、B 的测试类（UnitTest 构造时给的名字，如 MusicAnalyzer）
// 多条线并行开发时各自只跑自己的类，全套由主进程集成时跑（v0.6 起）。名字一个都对不上时报错退出，不当作通过。
int main (int argc, char* argv[])
{
   #if defined (_WIN32)
    SetConsoleOutputCP (65001);   // CP_UTF8
   #endif

    ConsoleRunner runner;
    runner.setAssertOnFailure (false);

    juce::StringArray only;

    for (int i = 1; i < argc; ++i)
    {
        if (juce::String (argv[i]) == "--only" && i + 1 < argc)
            only.addTokens (juce::String (argv[++i]), ",", {});
        else
        {
            std::cout << "未知参数：" << argv[i] << "（用法：celadon_tests [--only 测试类名,测试类名]）" << std::endl;
            return 2;
        }
    }

    only.trim();
    only.removeEmptyStrings();

    if (only.isEmpty())
    {
        runner.runTestsInCategory ("Celadon");
    }
    else
    {
        juce::Array<juce::UnitTest*> selected;

        for (auto* test : juce::UnitTest::getTestsInCategory ("Celadon"))
            if (only.contains (test->getName()))
                selected.add (test);

        if (selected.size() != only.size())
        {
            std::cout << "--only 里有对不上的测试类名。已有的类：";

            for (auto* test : juce::UnitTest::getTestsInCategory ("Celadon"))
                std::cout << " " << test->getName();

            std::cout << std::endl;
            return 2;
        }

        runner.runTests (selected);
    }

    int passes = 0, failures = 0;

    for (int i = 0; i < runner.getNumResults(); ++i)
    {
        passes += runner.getResult (i)->passes;
        failures += runner.getResult (i)->failures;
    }

    std::cout << "\n==== celadon_tests: " << passes << " passed, " << failures << " failed ====" << std::endl;
    return failures == 0 ? 0 : 1;
}
