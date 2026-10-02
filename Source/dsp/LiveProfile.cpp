/*
  ==============================================================================

    LiveProfile.cpp
    T-048 阶段 1 脚手架：接口占位（编得过、行为是「永远冷启动」）。实现由阶段 1 代理 A 按 LiveProfile.h 与
    设计规格-实时GLAZE-主段与增益 写，替换本文件全部内容。

  ==============================================================================
*/

#include "LiveProfile.h"

namespace celadon::glaze
{
    MainSectionModel mainSectionModelFor (Genre genre) noexcept
    {
        return isHouse (genre) ? MainSectionModel::fourOnFloor : MainSectionModel::loudness;
    }

    struct LiveProfile::Impl
    {
        LiveConstants constants;
        MainSectionModel model = MainSectionModel::loudness;
        MusicProfile profile;
        LiveDiagnostics diagnostics;
    };

    LiveProfile::LiveProfile() : impl (std::make_unique<Impl>()) {}
    LiveProfile::~LiveProfile() = default;

    void LiveProfile::prepare (double) {}
    void LiveProfile::reset() noexcept {}

    void LiveProfile::setConstants (const LiveConstants& constants, MainSectionModel model) noexcept
    {
        impl->constants = constants;
        impl->model = model;
    }

    void LiveProfile::segmentFinalized (const SegmentLedger::Segment&) noexcept {}
    void LiveProfile::commitWithMainness (const SegmentLedger::Segment&, float) noexcept {}

    float LiveProfile::mainnessFor (bool, float, bool, float) const noexcept   { return 0.0f; }

    const MusicProfile& LiveProfile::getProfile() const noexcept   { return impl->profile; }
    LiveDiagnostics LiveProfile::getDiagnostics() const noexcept   { return impl->diagnostics; }

    int LiveProfile::readCommits (std::uint64_t&, LiveCommitRecord*, int) const noexcept   { return 0; }

    std::unique_ptr<juce::XmlElement> LiveProfile::createStateXml() const
    {
        return std::make_unique<juce::XmlElement> ("LIVE_PROFILE");
    }

    bool LiveProfile::restoreStateXml (const juce::XmlElement&)   { return false; }
}
