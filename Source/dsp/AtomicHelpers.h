/*
  ==============================================================================

    AtomicHelpers.h
    音频线程与界面线程之间传表的读数：音频线程取最大值写入，界面线程读完清零。无锁。

  ==============================================================================
*/

#pragma once

#include <atomic>

namespace celadon
{
    // 把 destination 更新为 max(destination, value)；多线程安全、无锁。
    inline void atomicMax (std::atomic<float>& destination, float value) noexcept
    {
        auto current = destination.load (std::memory_order_relaxed);

        while (value > current
               && ! destination.compare_exchange_weak (current, value, std::memory_order_relaxed))
        {
        }
    }
}
