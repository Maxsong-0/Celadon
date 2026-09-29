/*
  ==============================================================================

    UiSettings.cpp

  ==============================================================================
*/

#include "UiSettings.h"

#include <cmath>

namespace celadon::ui
{
    namespace
    {
        constexpr const char* languageKey     = "language";
        constexpr const char* reduceMotionKey = "reduceMotion";
        constexpr const char* zoomKey         = "defaultZoom";

        juce::PropertiesFile::Options fileOptions()
        {
            juce::PropertiesFile::Options options;
            options.applicationName     = "Celadon";
            options.filenameSuffix      = "settings";
            options.folderName          = "Celadon";
            options.osxLibrarySubFolder = "Application Support";
            options.commonToAllUsers    = false;
            options.storageFormat       = juce::PropertiesFile::storeAsXML;
            options.millisecondsBeforeSaving = 500;  // 默认大小在拖动时每帧都变：攒 0.5 s 再写盘；语言与动效立刻写（save）
            return options;
        }

        double clampZoom (double zoom) noexcept
        {
            return std::isfinite (zoom) ? juce::jlimit (UiSettings::minZoom, UiSettings::maxZoom, zoom) : 1.0;
        }
    }

    UiSettings::UiSettings()
        : file (std::make_unique<juce::PropertiesFile> (fileOptions()))
    {
        // 没存过语言：按系统语言（中文系统用中文，其余英文）。
        language = file->containsKey (languageKey) ? languageFromCode (file->getValue (languageKey))
                                                   : defaultLanguageForSystem();
        reduceMotion = file->getBoolValue (reduceMotionKey, false);
        defaultZoom  = clampZoom (file->getDoubleValue (zoomKey, 1.0));
    }

    UiSettings::~UiSettings()
    {
        if (file != nullptr)
            file->saveIfNeeded();
    }

    void UiSettings::setLanguage (Language newLanguage)
    {
        if (newLanguage == language)
            return;

        language = newLanguage;
        file->setValue (languageKey, juce::String (languageCode (language)));
        save();
    }

    void UiSettings::setReduceMotion (bool shouldReduceMotion)
    {
        if (shouldReduceMotion == reduceMotion)
            return;

        reduceMotion = shouldReduceMotion;
        file->setValue (reduceMotionKey, reduceMotion);
        save();
    }

    void UiSettings::setDefaultZoom (double zoom)
    {
        const auto clamped = clampZoom (zoom);

        if (std::abs (clamped - defaultZoom) < 1.0e-6)
            return;

        defaultZoom = clamped;

        // 缩放拖动时每一帧都会来：只记下、不广播（别的窗口不跟着变大小）；写盘由 PropertiesFile 的 0.5 s 定时器合并。
        file->setValue (zoomKey, defaultZoom);
    }

    void UiSettings::save()
    {
        file->saveIfNeeded();
        sendChangeMessage();
    }
}
