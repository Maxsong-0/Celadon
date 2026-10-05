/*
  ==============================================================================

    GlazeProfile.h
    GLAZE 随工程保存的内容（设计规格-GLAZE-时间线与保存 第 5 节）：已提交版本 = 画像 + 锁定的风格权重 + 方案 +
    校准 + 标定快照。保存 / 读取用 juce::XmlElement（juce_core，无头测试可用），处理器再挂进 APVTS 状态树。

      - 已提交版本不混入候选；另有独立执行快照记录当前听到的方案、临时画像和执行偏差，不冒充学习/校准完成；
      - 不保存音频延迟线/检测器内部历史；从同一快照和同一起点重新渲染可复现；
      - 画像 formatVersion 与方案 algorithmVersion、常数 version 各自独立；读到未知的更高 formatVersion 按读取失败处理；
      - 读取失败时 out 不变、返回 false（编排器按「未学习」处理），不抛异常；
      - 浮点一律按 IEEE 位写成十六进制（GlazeConstants.h 的 xmlio）：保存再读取逐位相同，与宿主区域设置无关。
      - 方案的 algorithmVersion 与本版本不同也照样读进来（时间线与保存第 5 节：照用保存的自动增量数值，由编排器决定何时重算）。
      - v0.1.7.0 修坯 Clay（设计规格-Clay 第 7 节，A6）：画像格式 2 多存立体声 / 超低频特征；画像按它自己的 formatVersion 写
        （格式 1 的旧画像重存仍是格式 1），读格式 1 时新字段一律 valid = false、数组 0；computeProfileId 只在格式 ≥ 2 时
        把新字段接在原有字段之后哈希——格式 1 的画像号与 v0.1.6.2 逐位相同，旧工程校准参考条件里的画像号不失效。
        方案多存 Clay 四项、执行快照多存 liveClay 与 ClayGuard 状态：旧存档缺这些属性 = 中性，属性在但损坏照样拒收。
      - ADR-012（设计规格-Clay-根治修订第 3 节，v0.1.7.0 发布前追加，格式号不变）：画像格式 2 多存三类峰值代价
        （lowCutPeakCostDb、monoPeakCostDb[4]、widthPeakCostDb），与格式 2 其他字段同样严格、同样接在画像号哈希的最后；
        方案与执行快照的 ClayPlan 多存 costLimitedMask（缺属性 = 0）。
      - T-032 R6-02（2026-09-29，格式号不变）：方案多存 automaticLevelWithdrawn（缺属性 = 该方案的 limitedByManual，旧语义保守）；
        标定快照多存三个风格字段 warm_ / punchy_ / clean_automaticHeatFloor 与 heatUsefulCrestDb（GlazeConstants.cpp visitHeatFloor：
        旧的全局 automaticHeatFloor 照写、照读作三者的缺省）。

  ==============================================================================
*/

#pragma once

#include "GlazeConstants.h"
#include "GlazeTypes.h"

#include <juce_core/juce_core.h>

#include <memory>

namespace celadon::glaze
{
    // 已提交版本：编排器里真正生效的那一份（候选版本整体替换它，不逐项混合）。平凡可拷贝，音频线程拷贝不分配。
    struct CommittedState
    {
        bool hasProfile = false;         // false = 未学习（其余字段无意义）
        MusicProfile profile;
        std::uint32_t profileId = 0;     // 画像内容的哈希，校准参考条件用
        StyleWeights lockedWeights;      // 「自动」风格锁定的权重（随画像一起定，之后不重新判断）
        Plan plan;                       // 保存时的方案（算法版本不同时照用这里的自动增量数值）
        Calibration calibration;         // 校准值及其有效性与参考条件
        Constants constants;             // 标定快照（旧工程按它保存时的常数生成方案）
    };

    // 最近一块实际执行的参数依据，独立于正式画像。平凡拷贝；XML 只在消息线程生成。
    struct ExecutionState
    {
        bool valid = false;
        bool hasProfile = false;
        bool provisional = false;       // 临时画像，绝不等同 CommittedState::hasProfile
        bool requiresCalibration = false; // 保护/计划改写后的执行量待验证；跨再次保存保留，不改正式校准记录
        MusicProfile profile;
        Constants constants;
        Plan plan;                      // 当时过渡混合后的计划；保护退让在恢复时吸收为更小的 Gain
        CalibrationReference reference;
        float gainBiasDb = 0.0f;         // 已执行校准偏差，包括未完成轮次
        float protectionDb = 0.0f;       // 已执行安全退让
        float liveToneOffsetDb = 0.0f;
        float liveTame = 0.0f;           // 分析窗口重新稳定前保持的自动 Tame 份额
        float liveHeatReduction = 0.0f;  // 已规划自动份额的回收比例；旧存档缺失 = 中性
        float liveGlueReduction = 0.0f;
        // v0.1.7.0 修坯 Clay：最近一块实际执行的「自动」态 Clay 值（过渡混合 + 保护之后、三态合成之前）与保护状态。
        // 旧快照缺字段 = 中性（ClayPlan{}、ClayGuardState{}）。
        ClayPlan liveClay;
        ClayGuardState clayGuard;
    };

    inline constexpr const char* executionTag = "EXECUTION";
    std::unique_ptr<juce::XmlElement> toXml (const ExecutionState&);
    bool fromXml (const juce::XmlElement&, ExecutionState& out);

    // 画像内容的哈希（逐字段量化后 FNV-1a；同画像同值）。
    std::uint32_t computeProfileId (const MusicProfile&) noexcept;

    std::unique_ptr<juce::XmlElement> toXml (const MusicProfile&);
    bool fromXml (const juce::XmlElement&, MusicProfile& out);

    std::unique_ptr<juce::XmlElement> toXml (const CommittedState&);
    bool fromXml (const juce::XmlElement&, CommittedState& out);

    // 状态树里的节点名（处理器挂在 APVTS 状态的子节点下）。
    inline constexpr const char* stateTag = "GLAZE_STATE";

    // ---- v0.6 A2 追加 ----

    // 本版本能读写的数据格式版本：画像（MusicProfile::formatVersion 的上限）与已提交版本（GLAZE_STATE 的 formatVersion）。
    // 读到比它高的版本 → 读取失败（按未学习处理）；低于 1 同样失败。
    inline constexpr int profileFormatVersion = 3;   // 2 = v0.1.7.0 加立体声 / 超低频字段；仍读 1（新字段 valid = false、数组 0）
                                                     // 3 = T-038 House 档门控结果（只有 House 档学出的画像写成格式 3；仍读 1 / 2）
    inline constexpr int stateFormatVersion = 1;

    // T-048 阶段 2：编排器写出的 GLAZE_STATE。只认带 LIVE 的这一版；更高版本整份按冷启动。
    // 上面的 stateFormatVersion 仍是旧「已提交版本」辅助函数 toXml(CommittedState) 的格式，编排器不再用它存档。
    // 名字不叫 liveStateFormatVersion：LiveProfile.cpp 里匿名命名空间已经用了这个标识符。
    inline constexpr int liveArchiveFormatVersion = 2;

    // ---- v0.1.7.0 修坯 Clay 追加（A6）----

    // 画像带 Clay 所需立体声 / 超低频特征（GlazeTypes.h MusicProfile「格式版本 2」那几项）的最低格式版本。与上面的
    // profileFormatVersion（本版本能读的上限）分开：以后格式再升，Clay 特征仍从 2 起就有。
    // 用它的地方：画像 XML 写 / 读新字段、computeProfileId 是否把新字段并进哈希、编排器判断「旧画像 → 自动 Clay 中性、提示重新聆听」。
    inline constexpr int clayProfileFormatVersion = 2;

    // ---- T-038 曲风追加 ----

    // 画像带 House 门控结果（MusicProfile::houseGate）的最低格式版本。分析器只在 House 档学出的画像上把 formatVersion 设成它；
    // 通用档的画像仍是 clayProfileFormatVersion（与 main 逐字节相同、画像号不变）。写 / 读 houseGate、computeProfileId 是否并进它都看这里。
    inline constexpr int houseProfileFormatVersion = 3;

    inline constexpr const char* profileTag = "GLAZE_PROFILE";

    // 逐位相同（浮点按位比较，NaN、±0 都区分；按保存格式逐项比较，所以「存得下的东西」全部覆盖）。
    // 测试与编排器（消息线程）用；会分配，别在音频线程调。
    bool bitwiseEqual (const MusicProfile& a, const MusicProfile& b);
    bool bitwiseEqual (const CommittedState& a, const CommittedState& b);
}
