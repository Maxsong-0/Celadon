/*
  ==============================================================================

    OpenState.h
    V5「一釉」展开状态的纯逻辑（T-123，设计规格-界面-V5一釉 第 3 节）：模块行四块里此刻展开的是哪一个、它的面板停在哪一页。
    只用标准库，不依赖 JUCE、不碰界面：编辑器把鼠标动作交给这里算出新状态，再去摆组件；随工程存读的两个数也在这里钳位。
    自动测试在 tests/OpenStateTests.cpp。

    规则：
      - 同一时刻最多展开一个模块（状态里只有一个模块号，结构上就摆不出两个）；新工程默认全部收起；
      - 点模块行的一块：没展开就展开它；它正展开着就收起；展开着别的就直接换成它（不先关再开），并回到它的第一页；
      - 收起有三个入口：再点同一块（afterModuleClick）、面板头的收起键、频谱上的「放大频谱」（后两个都是 collapsed()）；
      - 页号只记当前展开的那个模块的：收起、换模块之后都回到第一页；
      - 随工程存两个数（状态树根属性，名字在 ParameterIds.h）：uiOpenModule（0 = 都收起，1 ～ 4）与 uiOpenPage（从 0 起）。
        旧工程没有这两个属性；读进来的值不合法（模块号不在 1 ～ 4、页号超出该模块的页数、不是整数、不是数）时整个状态不可信，
        一律按全部收起，不提示、不猜成第一页（fromStored）。

  ==============================================================================
*/

#pragma once

#include <string_view>

namespace celadon::ui::openstate
{
    // 模块号 = 处理顺序 BALANCE → CLAY → COLOR → LIMIT。存进工程的就是这个数，发布后不改。
    inline constexpr int none = 0, balance = 1, clay = 2, color = 3, limit = 4;
    inline constexpr int moduleCount = 4;

    // 每个模块的面板有几页：BALANCE 3 页（控制 / 推响不刺 / 目标频谱）、CLAY 与 COLOR 各 1 页、LIMIT 2 页（控制 / 发行安全）。
    // 不是模块的号（含 0 = 都收起）没有页。
    constexpr int pageCount (int module) noexcept
    {
        switch (module)
        {
            case balance: return 3;
            case clay:    return 1;
            case color:   return 1;
            case limit:   return 2;
            default:      return 0;
        }
    }

    struct State
    {
        int module = none;   // 0 = 都收起
        int page = 0;        // 展开的那个模块的页；都收起时恒为 0

        constexpr bool isOpen() const noexcept                          { return module != none; }
        constexpr bool operator== (const State& other) const noexcept   { return module == other.module && page == other.page; }
        constexpr bool operator!= (const State& other) const noexcept   { return ! (*this == other); }
    };

    // 全部 7 个面板页，按模块行从左到右、页签从左到右（单行文字检查、截图场景按它遍历）。
    inline constexpr int totalPages = 7;
    inline constexpr State allPages[totalPages] =
    {
        { balance, 0 }, { balance, 1 }, { balance, 2 },
        { clay, 0 },
        { color, 0 },
        { limit, 0 }, { limit, 1 }
    };

    // 合法状态：都收起（页号 0），或展开着一个真有的模块、停在它真有的一页。
    constexpr bool isValid (State state) noexcept
    {
        if (state.module == none)
            return state.page == 0;

        return state.page >= 0 && state.page < pageCount (state.module);   // 不是模块的号 pageCount 为 0，这里恒假
    }

    constexpr State collapsed() noexcept   { return {}; }

    // 点了模块行的第 clicked 块（1 ～ 4）之后的状态。clicked 不是模块号：不变。
    constexpr State afterModuleClick (State current, int clicked) noexcept
    {
        if (pageCount (clicked) == 0)
            return current;

        return current.module == clicked ? collapsed() : State { clicked, 0 };
    }

    // 点了面板头第 page 个页签（从 0 起）之后的状态。都收起时、或展开的模块没有这一页：不变。
    constexpr State afterPageClick (State current, int page) noexcept
    {
        if (page < 0 || page >= pageCount (current.module))
            return current;

        return { current.module, page };
    }

    // 从工程读回来的两个属性的文字。调用方把状态树属性转成字符串交进来：编辑器写进去的是整数，读工程读回来的是 XML 属性的文字，
    // 没有这个属性是空串。只认不带符号的十进制整数（最多 3 位）：空串、负号、小数点、字母、空白、nan / inf 都不算数；
    // 两个里有一个不算数、或合起来不是一个展开着的合法状态，就是全部收起。
    constexpr State fromStored (std::string_view module, std::string_view page) noexcept
    {
        const auto parse = [] (std::string_view text) -> int
        {
            if (text.empty() || text.size() > 3)
                return -1;

            int value = 0;

            for (const auto character : text)
            {
                if (character < '0' || character > '9')
                    return -1;

                value = value * 10 + (character - '0');
            }

            return value;
        };

        const State state { parse (module), parse (page) };
        return state.isOpen() && isValid (state) ? state : collapsed();
    }
}
