// T-051 量具（证据，不进构建）：IN 虚线每次重建的堆分配次数，改前（juce::PathStrokeType::createDashedStroke 写进成员路径）
// 对改后（celadon::ui::createDashedStrokeReusing，中线段与虚线都用成员路径）。链接时用 -Wl,--wrap=malloc/realloc/calloc
// 数 JUCE 与本项目静态链接部分的 malloc 族调用（JUCE 的 HeapBlock / Array / Path 都走它们）。
// 编法（2026-10-04 DGX Spark 上这样编的）：取 celadon_ui_snapshot 的编译命令（ninja -t commands celadon_ui_snapshot），把源文件
// SnapshotMain.cpp 换成本文件；链接命令里把 SnapshotMain.cpp.o 换成本文件的 .o、输出改名，再加
// -Wl,--wrap=malloc -Wl,--wrap=realloc -Wl,--wrap=calloc。结果：改前每次 18.00 次，改后 2.00 次（300 帧平均，两条路径每帧逐元素相同）。
#include <JuceHeader.h>
#include "../../../Source/ui/SpectrumView.h"

#include <cstdio>

extern "C"
{
    void* __real_malloc (size_t);
    void* __real_realloc (void*, size_t);
    void* __real_calloc (size_t, size_t);

    static bool counting = false;
    static long allocations = 0;

    void* __wrap_malloc (size_t n)            { if (counting) ++allocations; return __real_malloc (n); }
    void* __wrap_realloc (void* p, size_t n)  { if (counting) ++allocations; return __real_realloc (p, n); }
    void* __wrap_calloc (size_t a, size_t b)  { if (counting) ++allocations; return __real_calloc (a, b); }
}

// 频谱仪那样的折线：每逻辑像素一个点，形状每帧变一点（模拟播放中 IN 曲线每帧在动）。
static void buildLine (juce::Path& line, int frame)
{
    line.clear();

    for (int i = 0; i < celadon::ui::SpectrumAnalyser::numPoints; ++i)
    {
        const auto x = (float) i / (float) (celadon::ui::SpectrumAnalyser::numPoints - 1) * (float) celadon::ui::SpectrumView::width;
        const auto y = 120.0f + 90.0f * std::sin ((float) i * 0.031f + 0.2f * (float) frame) * std::cos ((float) i * 0.0071f);

        if (i == 0)
            line.startNewSubPath (x, y);
        else
            line.lineTo (x, y);
    }
}

int main()
{
    const float dashes[] = { 3.0f, 2.0f };
    const juce::PathStrokeType stroke (1.1f);
    juce::Path line, oldDashes, centre, newDashes;
    constexpr int warmUp = 30, frames = 300;
    long oldCount = 0, newCount = 0;

    for (int frame = 0; frame < warmUp + frames; ++frame)
    {
        buildLine (line, frame);

        counting = frame >= warmUp;
        allocations = 0;
        oldDashes.clear();                                                        // 改前 SpectrumView::paint 的写法
        stroke.createDashedStroke (oldDashes, line, dashes, 2);
        oldCount += allocations;

        allocations = 0;
        celadon::ui::createDashedStrokeReusing (stroke, line, dashes, 2, centre, newDashes);   // 改后
        newCount += allocations;
        counting = false;

        if (oldDashes != newDashes)
        {
            std::printf ("frame %d: paths differ\n", frame);
            return 1;
        }
    }

    std::printf ("每次重建的 malloc / realloc / calloc 次数（%d 帧平均，先暖机 %d 帧）：改前 %.2f，改后 %.2f\n",
                 frames, warmUp, (double) oldCount / frames, (double) newCount / frames);
    return 0;
}
