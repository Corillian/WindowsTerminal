/*++
Copyright (c) Microsoft Corporation
Licensed under the MIT license.

Module Name:
- InteractivityAutomationPeer.h

Abstract:
- This module provides UI Automation access to the ControlInteractivity,
  to support both automation tests and accessibility (screen
  reading) applications.
- See TermControlAutomationPeer for more details on how UIA is implemented.
- This is the primary implementation of the ITextProvider interface, for the
  TermControlAutomationPeer. The TermControlAutomationPeer will be attached to
  the actual UI tree, via FrameworkElementAutomationPeer. However, the
  ControlInteractivity is totally oblivious to the UI tree that might be hosting
  it. So this class implements the actual text pattern for the buffer, because
  it has access to the buffer. TermControlAutomationPeer can then call the
  methods on this class to expose the implementation in the actual UI tree.

Author(s):
- Mike Griese (migrie), May 2021

--*/

#pragma once

#include "ControlInteractivity.h"
#include "InteractivityAutomationPeer.g.h"
#include "winrt/Microsoft.UI.Xaml.Automation.h"
#include "winrt/Microsoft.UI.Xaml.Automation.Peers.h"
#include "../../external/terminal/src/types/TermControlUiaProvider.hpp"
#include "../../external/terminal/src/types/IUiaEventDispatcher.h"
#include "../../external/terminal/src/types/IControlAccessibilityInfo.h"

namespace winrt::Microsoft::Terminal::Control::implementation
{
    struct InteractivityAutomationPeer :
        public InteractivityAutomationPeerT<InteractivityAutomationPeer>,
        ::Microsoft::Console::Types::IUiaEventDispatcher,
        ::Microsoft::Console::Types::IControlAccessibilityInfo
    {
    public:
        InteractivityAutomationPeer(Microsoft::Terminal::Control::implementation::ControlInteractivity* owner);

        void SetControlBounds(const Windows::Foundation::Rect bounds);
        void SetControlPadding(const Core::Padding padding);
        void ParentProvider(Microsoft::UI::Xaml::Automation::Peers::AutomationPeer parentProvider);

#pragma region IUiaEventDispatcher
        void SignalSelectionChanged() override;
        void SignalTextChanged() override;
        void SignalCursorChanged() override;
        void NotifyNewOutput(std::wstring_view newOutput) override;
#pragma endregion

#pragma region ITextProvider Pattern
        Microsoft::UI::Xaml::Automation::Provider::ITextRangeProvider RangeFromPoint(Windows::Foundation::Point screenLocation);
        Microsoft::UI::Xaml::Automation::Provider::ITextRangeProvider RangeFromChild(Microsoft::UI::Xaml::Automation::Provider::IRawElementProviderSimple childElement);
        com_array<Microsoft::UI::Xaml::Automation::Provider::ITextRangeProvider> GetVisibleRanges();
        com_array<Microsoft::UI::Xaml::Automation::Provider::ITextRangeProvider> GetSelection();
        Microsoft::UI::Xaml::Automation::SupportedTextSelection SupportedTextSelection();
        Microsoft::UI::Xaml::Automation::Provider::ITextRangeProvider DocumentRange();
#pragma endregion

#pragma region IControlAccessibilityInfo Pattern
        // Inherited via IControlAccessibilityInfo
        virtual COORD GetFontSize() const noexcept override;
        virtual RECT GetBounds() const noexcept override;
        virtual RECT GetPadding() const noexcept override;
        virtual double GetScaleFactor() const noexcept override;
        virtual void ChangeViewport(SMALL_RECT NewWindow) override;
        virtual HRESULT GetHostUiaProvider(IRawElementProviderSimple** provider) override;
#pragma endregion

        TYPED_EVENT(SelectionChanged, IInspectable, IInspectable);
        TYPED_EVENT(TextChanged, IInspectable, IInspectable);
        TYPED_EVENT(CursorChanged, IInspectable, IInspectable);
        TYPED_EVENT(NewOutput, IInspectable, hstring);

    private:
        Microsoft::UI::Xaml::Automation::Provider::ITextRangeProvider _CreateXamlUiaTextRange(::ITextRangeProvider* returnVal) const;

        ::Microsoft::WRL::ComPtr<::Microsoft::Terminal::TermControlUiaProvider> _uiaProvider;
        winrt::Microsoft::Terminal::Control::implementation::ControlInteractivity* _interactivity;
        weak_ref<Microsoft::UI::Xaml::Automation::Peers::AutomationPeer> _parentProvider;

        til::rect _controlBounds{};
        til::rect _controlPadding{};

        winrt::com_array<Microsoft::UI::Xaml::Automation::Provider::ITextRangeProvider> WrapArrayOfTextRangeProviders(SAFEARRAY* textRanges);
    };
}
