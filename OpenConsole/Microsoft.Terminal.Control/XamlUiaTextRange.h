/*++
Copyright (c) Microsoft Corporation
Licensed under the MIT license.

Module Name:
- XamlUiaTextRange.h

Abstract:
- This module is a wrapper for the UiaTextRange
  (a text range accessibility provider). It allows
  for UiaTextRange to be used in Windows Terminal.
- Wraps the UIAutomationCore ITextRangeProvider
  (https://docs.microsoft.com/en-us/windows/win32/api/uiautomationcore/nn-uiautomationcore-itextrangeprovider)
  with a XAML ITextRangeProvider
  (https://docs.microsoft.com/en-us/uwp/api/windows.ui.xaml.automation.provider.itextrangeprovider)

Author(s):
- Carlos Zamora   (CaZamor)    2019
--*/

#pragma once

#include "TermControlAutomationPeer.h"
#include <UIAutomationCore.h>
#include "../../external/terminal/src/types/TermControlUiaTextRange.hpp"

namespace winrt::Microsoft::Terminal::Control::implementation
{
    class XamlUiaTextRange :
        public winrt::implements<XamlUiaTextRange, Microsoft::UI::Xaml::Automation::Provider::ITextRangeProvider>
    {
    public:
        XamlUiaTextRange(::ITextRangeProvider* uiaProvider, Microsoft::UI::Xaml::Automation::Provider::IRawElementProviderSimple parentProvider) :
            _parentProvider{ parentProvider }
        {
            _uiaProvider.attach(uiaProvider);
        }

#pragma region ITextRangeProvider
        Microsoft::UI::Xaml::Automation::Provider::ITextRangeProvider Clone() const;
        bool Compare(Microsoft::UI::Xaml::Automation::Provider::ITextRangeProvider pRange) const;
        int32_t CompareEndpoints(Microsoft::UI::Xaml::Automation::Text::TextPatternRangeEndpoint endpoint,
            Microsoft::UI::Xaml::Automation::Provider::ITextRangeProvider pTargetRange,
            Microsoft::UI::Xaml::Automation::Text::TextPatternRangeEndpoint targetEndpoint);
        void ExpandToEnclosingUnit(Microsoft::UI::Xaml::Automation::Text::TextUnit unit) const;
        Microsoft::UI::Xaml::Automation::Provider::ITextRangeProvider FindAttribute(int32_t textAttributeId,
            winrt::Windows::Foundation::IInspectable val,
            bool searchBackward);
        Microsoft::UI::Xaml::Automation::Provider::ITextRangeProvider FindText(winrt::hstring text,
            bool searchBackward,
            bool ignoreCase);
        winrt::Windows::Foundation::IInspectable GetAttributeValue(int32_t textAttributeId) const;
        void GetBoundingRectangles(winrt::com_array<double>& returnValue) const;
        Microsoft::UI::Xaml::Automation::Provider::IRawElementProviderSimple GetEnclosingElement();
        winrt::hstring GetText(int32_t maxLength) const;
        int32_t Move(Microsoft::UI::Xaml::Automation::Text::TextUnit unit,
            int32_t count);
        int32_t MoveEndpointByUnit(Microsoft::UI::Xaml::Automation::Text::TextPatternRangeEndpoint endpoint,
            Microsoft::UI::Xaml::Automation::Text::TextUnit unit,
            int32_t count) const;
        void MoveEndpointByRange(Microsoft::UI::Xaml::Automation::Text::TextPatternRangeEndpoint endpoint,
            Microsoft::UI::Xaml::Automation::Provider::ITextRangeProvider pTargetRange,
            Microsoft::UI::Xaml::Automation::Text::TextPatternRangeEndpoint targetEndpoint) const;
        void Select() const;
        void AddToSelection() const;
        void RemoveFromSelection() const;
        void ScrollIntoView(bool alignToTop) const;
        winrt::com_array<Microsoft::UI::Xaml::Automation::Provider::IRawElementProviderSimple> GetChildren() const;
#pragma endregion ITextRangeProvider

    private:
        wil::com_ptr<::ITextRangeProvider> _uiaProvider;
        Microsoft::UI::Xaml::Automation::Provider::IRawElementProviderSimple _parentProvider;
    };
}
