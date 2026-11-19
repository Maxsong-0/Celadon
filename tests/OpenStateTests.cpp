/*
  ==============================================================================

    OpenStateTests.cpp
    V5「一釉」展开状态的纯逻辑（T-123，Source/ui/OpenState.h）的自动测试：每个模块几页、点模块行的一块之后的新状态、
    切页签、三个收起入口得到同一个状态、随工程存读的两个数怎么钳位（旧工程没有、值不合法都按全部收起）。

  ==============================================================================
*/

#include "TestHelpers.h"

#include "../Source/ParameterIds.h"
#include "../Source/ui/OpenState.h"

#include <cstring>
#include <limits>
#include <string>

namespace celadon::test
{
    namespace
    {
        using namespace celadon::ui;
        using openstate::State;

        // 这些规则在编译期就能算：编辑器、截图工具、测试用的是同一份。
        static_assert (openstate::pageCount (openstate::balance) == 3 && openstate::pageCount (openstate::limit) == 2);
        static_assert (openstate::afterModuleClick ({}, openstate::clay) == State { openstate::clay, 0 });
        static_assert (! openstate::collapsed().isOpen());
        static_assert (openstate::fromStored ("4", "1") == State { openstate::limit, 1 } && openstate::fromStored ("9", "0") == State {});

        juce::String describe (State s)
        {
            return "{" + juce::String (s.module) + ", " + juce::String (s.page) + "}";
        }
    }

    class OpenStateTests  : public juce::UnitTest
    {
    public:
        OpenStateTests() : juce::UnitTest ("OpenState", "Celadon") {}

        void runTest() override
        {
            beginTest ("module numbers follow the processing order and each module has its pages");
            {
                expectEquals (openstate::none, 0);
                expectEquals (openstate::balance, 1);
                expectEquals (openstate::clay, 2);
                expectEquals (openstate::color, 3);
                expectEquals (openstate::limit, 4);
                expectEquals (openstate::moduleCount, 4);

                expectEquals (openstate::pageCount (openstate::balance), 3, "BALANCE: controls, loud-not-harsh, target spectrum");
                expectEquals (openstate::pageCount (openstate::clay), 1);
                expectEquals (openstate::pageCount (openstate::color), 1);
                expectEquals (openstate::pageCount (openstate::limit), 2, "LIMIT: controls, release safety");

                for (const auto module : { openstate::none, -1, 5, 9, std::numeric_limits<int>::min(), std::numeric_limits<int>::max() })
                    expectEquals (openstate::pageCount (module), 0, "no pages for module " + juce::String (module));

                // 7 个面板页的清单与 pageCount 一致、不重不漏。
                int listed = 0;

                for (int module = 1; module <= openstate::moduleCount; ++module)
                    for (int page = 0; page < openstate::pageCount (module); ++page)
                    {
                        expect (listed < openstate::totalPages);

                        if (listed < openstate::totalPages)
                            expect (openstate::allPages[listed] == State { module, page }, "allPages[" + juce::String (listed) + "]");

                        ++listed;
                    }

                expectEquals (listed, openstate::totalPages);
                expectEquals (openstate::totalPages, 7);
            }

            beginTest ("a new project starts with everything collapsed");
            {
                const State fresh;
                expect (! fresh.isOpen());
                expect (fresh == openstate::collapsed());
                expectEquals (fresh.module, 0);
                expectEquals (fresh.page, 0);
                expect (openstate::isValid (fresh));
            }

            beginTest ("clicking a block: opens it, collapses it when it is the open one, switches straight over from another one");
            {
                for (int clicked = 1; clicked <= openstate::moduleCount; ++clicked)
                {
                    const auto opened = openstate::afterModuleClick ({}, clicked);
                    expect (opened == State { clicked, 0 }, "collapsed + click " + juce::String (clicked) + " -> " + describe (opened));
                    expect (openstate::afterModuleClick (opened, clicked) == openstate::collapsed(),
                            "clicking the open block " + juce::String (clicked) + " again collapses it");

                    for (int other = 1; other <= openstate::moduleCount; ++other)
                        if (other != clicked)
                            expect (openstate::afterModuleClick (opened, other) == State { other, 0 },
                                    juce::String (clicked) + " open, click " + juce::String (other) + " switches without closing first");
                }
            }

            beginTest ("switching to another module lands on its first page; the page is only remembered for the open module");
            {
                const auto balanceTarget = openstate::afterPageClick (openstate::afterModuleClick ({}, openstate::balance), 2);
                expect (balanceTarget == State { openstate::balance, 2 });

                const auto limit = openstate::afterModuleClick (balanceTarget, openstate::limit);
                expect (limit == State { openstate::limit, 0 }, "BALANCE page 3 -> LIMIT lands on " + describe (limit));

                // 收起再展开同一块，也回到第一页（收起后页号不留）。
                const auto reopened = openstate::afterModuleClick (openstate::afterModuleClick (balanceTarget, openstate::balance), openstate::balance);
                expect (reopened == State { openstate::balance, 0 }, "collapse and reopen lands on " + describe (reopened));
            }

            beginTest ("clicks on something that is not a block change nothing");
            {
                const State open { openstate::color, 0 };

                for (const auto clicked : { 0, -1, 5, 9, std::numeric_limits<int>::min(), std::numeric_limits<int>::max() })
                {
                    expect (openstate::afterModuleClick (open, clicked) == open, "open, click " + juce::String (clicked));
                    expect (openstate::afterModuleClick ({}, clicked) == openstate::collapsed(), "collapsed, click " + juce::String (clicked));
                }
            }

            beginTest ("page tabs: only pages the open module has; nothing happens while collapsed");
            {
                const State balance { openstate::balance, 0 };
                expect (openstate::afterPageClick (balance, 1) == State { openstate::balance, 1 });
                expect (openstate::afterPageClick (balance, 2) == State { openstate::balance, 2 });
                expect (openstate::afterPageClick (State { openstate::balance, 2 }, 0) == balance);
                expect (openstate::afterPageClick (balance, 3) == balance, "BALANCE has no fourth page");
                expect (openstate::afterPageClick (balance, -1) == balance);

                const State limit { openstate::limit, 0 };
                expect (openstate::afterPageClick (limit, 1) == State { openstate::limit, 1 });
                expect (openstate::afterPageClick (limit, 2) == limit, "LIMIT has no third page");

                expect (openstate::afterPageClick (State { openstate::clay, 0 }, 1) == State { openstate::clay, 0 }, "CLAY has one page");
                expect (openstate::afterPageClick (State { openstate::color, 0 }, 1) == State { openstate::color, 0 }, "COLOR has one page");
                expect (openstate::afterPageClick ({}, 1) == openstate::collapsed(), "no tab to click while collapsed");
            }

            beginTest ("the three ways to collapse give the same state, and at most one module is ever open");
            {
                // 再点同一块 / 面板头的收起键 / 频谱上的「放大频谱」：后两个就是 collapsed()。
                for (const auto& open : openstate::allPages)
                {
                    expect (openstate::afterModuleClick (open, open.module) == openstate::collapsed(), "click the open block " + describe (open));
                    expect (openstate::collapsed() == State {}, "collapse key / enlarge spectrum");
                }

                // 任意点一串：状态始终合法（合法状态里只有一个模块号，所以同一时刻最多展开一个）。
                State state;
                std::uint32_t random = 0x1234abcd;

                for (int step = 0; step < 2000; ++step)
                {
                    random = random * 1664525u + 1013904223u;
                    const auto pick = (int) ((random >> 16) % 12u) - 2;   // −2 ～ 9：含不存在的模块与页

                    state = (step % 3 == 2) ? openstate::afterPageClick (state, pick) : openstate::afterModuleClick (state, pick);
                    expect (openstate::isValid (state), "step " + juce::String (step) + " left " + describe (state));
                }
            }

            beginTest ("stored values: every legal pair reads back as itself");
            {
                for (const auto& page : openstate::allPages)
                    expect (openstate::fromStored (std::to_string (page.module), std::to_string (page.page)) == page, "stored " + describe (page));

                expect (openstate::fromStored ("0", "0") == openstate::collapsed(), "module 0 is all collapsed");
                expect (openstate::fromStored ("04", "01") == State { openstate::limit, 1 }, "leading zeros are still that number");
            }

            beginTest ("stored values: an old project (no properties) and every illegal value open collapsed");
            {
                // 旧工程：两个属性都没有，状态树给的是空值，转成文字是空串。
                expect (openstate::fromStored ("", "") == openstate::collapsed(), "old project");

                // 模块号不合法：超出范围、负数、小数、科学计数、不是数、前后带空白或符号、全角数字。
                for (const char* module : { "9", "-1", "5", "2.5", "0.5", "1000000000", "1e9", "1e300", "nan", "inf", "-inf", "abc", "2abc", " 2", "2 ",
                                            "+2", "", "0x2", "\xef\xbc\x92" })
                    expect (openstate::fromStored (module, "0") == openstate::collapsed(), juce::String ("module \"") + module + "\"");

                // 模块号合法、页号不合法：整个状态不可信，也按全部收起（不猜成第一页）。
                for (const char* page : { "-1", "3", "9", "1.5", "1e9", "nan", "inf", "x", "", " 1", "1 ", "+1", "1000" })
                    expect (openstate::fromStored ("1", page) == openstate::collapsed(), juce::String ("BALANCE page \"") + page + "\"");

                expect (openstate::fromStored ("4", "2") == openstate::collapsed(), "LIMIT has no third page");
                expect (openstate::fromStored ("2", "1") == openstate::collapsed(), "CLAY has one page");
                expect (openstate::fromStored ("3", "1") == openstate::collapsed(), "COLOR has one page");

                // 都收起时页号没有意义：存着别的页号也只是全部收起。
                expect (openstate::fromStored ("0", "2") == openstate::collapsed());
                expect (openstate::fromStored ("-0", "-0") == openstate::collapsed());
            }

            beginTest ("isValid rejects what fromStored would never produce");
            {
                expect (! openstate::isValid (State { 0, 1 }), "collapsed with a page");
                expect (! openstate::isValid (State { 5, 0 }));
                expect (! openstate::isValid (State { -1, 0 }));
                expect (! openstate::isValid (State { openstate::balance, 3 }));
                expect (! openstate::isValid (State { openstate::limit, -1 }));

                for (const auto& page : openstate::allPages)
                    expect (openstate::isValid (page) && page.isOpen());
            }

            beginTest ("the two project properties have their own names next to uiZoom");
            {
                expect (std::strcmp (uiid::uiOpenModuleProperty, "uiOpenModule") == 0);
                expect (std::strcmp (uiid::uiOpenPageProperty, "uiOpenPage") == 0);
                expect (std::strcmp (uiid::uiOpenModuleProperty, uiid::uiZoomProperty) != 0
                            && std::strcmp (uiid::uiOpenPageProperty, uiid::uiZoomProperty) != 0
                            && std::strcmp (uiid::uiOpenModuleProperty, uiid::uiOpenPageProperty) != 0);
            }
        }
    };

    static OpenStateTests openStateTests;
}
