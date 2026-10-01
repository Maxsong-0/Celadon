/*
  ==============================================================================

    SegmentLedger.h
    T-038 House 档按小节记账（设计规格-曲风-House学习方式 第 2 节；ADR-026）。只在曲风为 House 子类时由分析器使用，通用档不碰它。

      - 一段 = 一小节（宿主 PPQ + 拍号定位）；拿不到拍点时每 Constants::house.fallbackSegmentSeconds 秒一段；
      - 段按位置存：同一小节重放时覆盖，不重复计入；跳着播放也能补齐整首；
      - 小节边界换算成整数样本位置：从第一次见到的锚点出发按恒定速度推（锚点样本 + round(n × 每小节样本数)），只在速度或拍号改变、
        或遇到不连续点时重新锚定——恒定速度下与宿主分块无关（画像逐位相同）；
      - 每段存可相加的累计和（K 加权能量、27 段功率、频谱证据、低 / 高频功率、起音、立体声、次声）与按百分位算的量的逐块值
        （真峰值、先试后用代价、3 s 短期响度）；
      - 门控：段响度 ≥ S_ref − ΔG（S_ref = 已听各段响度的第 95 百分位，按时长加权），且段 lowShare ≥ 低频参考 × 系数
        （低频参考 = 已过响度门各段 lowShare 的第 95 百分位）；
      - 容量 maxSegments，在 prepare 里分配；音频线程 0 分配、0 锁。

    T-038 阶段 1 只立接口与容量；喂数据、门控与出画像的接口由阶段 2 的 B（与 MusicAnalyzer、GlazeController 同一条线）设计并实现。

  ==============================================================================
*/

#pragma once

#include "GlazeTypes.h"

namespace celadon::glaze
{
    class SegmentLedger
    {
    public:
        static constexpr int maxSegments = 512;   // 124 BPM 约 16.5 分钟；满了不再收新段（状态行写明）

        SegmentLedger() = default;

        // 分配全部固定容量（消息线程 / prepareToPlay）。
        void prepare (double sampleRate);

        // 清空段落账（「重新聆听」、宿主复位）。不分配。
        void reset() noexcept;

        SegmentMode getMode() const noexcept       { return mode; }
        int getHeardSegments() const noexcept      { return heardSegments; }

    private:
        double sampleRate = 0.0;
        SegmentMode mode = SegmentMode::none;
        int heardSegments = 0;
    };
}
