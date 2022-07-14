/*++
Copyright (c) Microsoft Corporation
Licensed under the MIT license.

Module Name:
- SearchBoxControl.cpp

Abstract:
- the search dialog component used in Terminal Search

Author(s):
- Kaiyu Wang (kawa) 11-27-2019

--*/

#pragma once

#include "winrt/Microsoft.UI.Xaml.h"
#include "winrt/Microsoft.UI.Xaml.Markup.h"
#include "winrt/Microsoft.UI.Xaml.Controls.Primitives.h"
#include "winrt/Microsoft.UI.Xaml.Controls.h"
#include "winrt/Microsoft.UI.Xaml.Input.h"
#include "SearchBoxControl.g.h"
#include <cppwinrt_utils.h>

namespace winrt::Microsoft::Terminal::Control::implementation
{
    struct SearchBoxControl : SearchBoxControlT<SearchBoxControl>
    {
        SearchBoxControl();

        void TextBoxKeyDown(const winrt::Windows::Foundation::IInspectable& /*sender*/, const winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs& e);

        void SetFocusOnTextbox();
        void PopulateTextbox(const winrt::hstring& text);
        bool ContainsFocus();

        void GoBackwardClicked(const winrt::Windows::Foundation::IInspectable& /*sender*/, const winrt::Microsoft::UI::Xaml::RoutedEventArgs& /*e*/);
        void GoForwardClicked(const winrt::Windows::Foundation::IInspectable& /*sender*/, const winrt::Microsoft::UI::Xaml::RoutedEventArgs& /*e*/);
        void CloseClick(const winrt::Windows::Foundation::IInspectable& /*sender*/, const winrt::Microsoft::UI::Xaml::RoutedEventArgs& e);

        WINRT_CALLBACK(Search, SearchHandler);
        TYPED_EVENT(Closed, Control::SearchBoxControl, Microsoft::UI::Xaml::RoutedEventArgs);

    private:
        std::unordered_set<winrt::Windows::Foundation::IInspectable> _focusableElements;

        bool _GoForward();
        bool _CaseSensitive();
        void _KeyDownHandler(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs& e);
        void _CharacterHandler(const winrt::Windows::Foundation::IInspectable& /*sender*/, const winrt::Microsoft::UI::Xaml::Input::CharacterReceivedRoutedEventArgs& e);
    };
}

namespace winrt::Microsoft::Terminal::Control::factory_implementation
{
    BASIC_FACTORY(SearchBoxControl);
}
