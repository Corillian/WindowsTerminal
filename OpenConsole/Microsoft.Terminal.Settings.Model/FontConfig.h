/*++
Copyright (c) Microsoft Corporation
Licensed under the MIT license.

Module Name:
- FontConfig

Abstract:
- The implementation of the FontConfig winrt class. Provides settings related
  to the font settings of the terminal, for the terminal control.

Author(s):
- Pankaj Bhojwani - June 2021

--*/

#pragma once

#include "pch.h"
#include "FontConfig.g.h"
#include "JsonUtils.h"
#include "../inc/cppwinrt_utils.h"
#include "IInheritable.h"
#include <DefaultSettings.h>

using IFontAxesMap = winrt::Windows::Foundation::Collections::IMap<winrt::hstring, float>;
using IFontFeatureMap = winrt::Windows::Foundation::Collections::IMap<winrt::hstring, uint32_t>;

namespace winrt::Microsoft::Terminal::Settings::Model::implementation
{
    struct FontConfig : FontConfigT<FontConfig>, IInheritable<FontConfig>
    {
    public:
        FontConfig(winrt::weak_ref<Profile> sourceProfile);
        static winrt::com_ptr<FontConfig> CopyFontInfo(const winrt::com_ptr<FontConfig> source, winrt::weak_ref<Profile> sourceProfile);
        Json::Value ToJson() const;
        void LayerJson(const Json::Value& json);
        bool HasAnyOptionSet() const;

        Model::Profile SourceProfile();

        INHERITABLE_SETTING(Model::FontConfig, hstring, FontFace, DEFAULT_FONT_FACE);
        INHERITABLE_SETTING(Model::FontConfig, int32_t, FontSize, DEFAULT_FONT_SIZE);
        INHERITABLE_SETTING(Model::FontConfig, Windows::UI::Text::FontWeight, FontWeight, DEFAULT_FONT_WEIGHT);
        INHERITABLE_SETTING(Model::FontConfig, IFontAxesMap, FontAxes);
        INHERITABLE_SETTING(Model::FontConfig, IFontFeatureMap, FontFeatures);

    private:
        winrt::weak_ref<Profile> _sourceProfile;
    };
}
