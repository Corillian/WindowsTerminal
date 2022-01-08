#include "pch.h"
#include <UIAutomationCore.h>
#include <LibraryResources.h>
#include "InteractivityAutomationPeer.h"
#include "InteractivityAutomationPeer.g.cpp"

#include "XamlUiaTextRange.h"
#include "../../external/terminal/src/types/UiaTracing.h"

using namespace Microsoft::Console::Types;
using namespace winrt::Microsoft::UI::Xaml::Automation::Peers;

namespace UIA
{
    using ::ITextRangeProvider;
    using ::SupportedTextSelection;
}

namespace XamlAutomation
{
    using winrt::Microsoft::UI::Xaml::Automation::SupportedTextSelection;
    using winrt::Microsoft::UI::Xaml::Automation::Provider::IRawElementProviderSimple;
    using winrt::Microsoft::UI::Xaml::Automation::Provider::ITextRangeProvider;
}

namespace winrt::Microsoft::Terminal::Control::implementation
{
    InteractivityAutomationPeer::InteractivityAutomationPeer(Control::implementation::ControlInteractivity* owner) :
        _interactivity{ owner }
    {
        THROW_IF_FAILED(::Microsoft::WRL::MakeAndInitialize<::Microsoft::Terminal::TermControlUiaProvider>(&_uiaProvider, _interactivity->GetUiaData(), this));
    };

    void InteractivityAutomationPeer::SetControlBounds(const Windows::Foundation::Rect bounds)
    {
        _controlBounds = til::rectangle{ til::math::rounding, bounds };
    }
    void InteractivityAutomationPeer::SetControlPadding(const Core::Padding padding)
    {
        _controlPadding = til::rectangle(
            til::point(static_cast<ptrdiff_t>(padding.Left), static_cast<ptrdiff_t>(padding.Top)),
            til::point(static_cast<ptrdiff_t>(padding.Right), static_cast<ptrdiff_t>(padding.Bottom)));
    }
    void InteractivityAutomationPeer::ParentProvider(AutomationPeer parentProvider)
    {
        _parentProvider = parentProvider;
    }

    // Method Description:
    // - Signals the ui automation client that the terminal's selection has
    //   changed and should be updated
    // - We will raise a new event, for out embedding control to be able to
    //   raise the event. AutomationPeer by itself doesn't hook up to the
    //   eventing mechanism, we need the FrameworkAutomationPeer to do that.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void InteractivityAutomationPeer::SignalSelectionChanged()
    {
        _SelectionChangedHandlers(*this, nullptr);
    }

    // Method Description:
    // - Signals the ui automation client that the terminal's output has changed
    //   and should be updated
    // - We will raise a new event, for out embedding control to be able to
    //   raise the event. AutomationPeer by itself doesn't hook up to the
    //   eventing mechanism, we need the FrameworkAutomationPeer to do that.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void InteractivityAutomationPeer::SignalTextChanged()
    {
        _TextChangedHandlers(*this, nullptr);
    }

    // Method Description:
    // - Signals the ui automation client that the cursor's state has changed
    //   and should be updated
    // - We will raise a new event, for out embedding control to be able to
    //   raise the event. AutomationPeer by itself doesn't hook up to the
    //   eventing mechanism, we need the FrameworkAutomationPeer to do that.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void InteractivityAutomationPeer::SignalCursorChanged()
    {
        _CursorChangedHandlers(*this, nullptr);
    }

#pragma region ITextProvider
    com_array<XamlAutomation::ITextRangeProvider> InteractivityAutomationPeer::GetSelection()
    {
        SAFEARRAY* pReturnVal;
        THROW_IF_FAILED(_uiaProvider->GetSelection(&pReturnVal));
        return WrapArrayOfTextRangeProviders(pReturnVal);
    }

    com_array<XamlAutomation::ITextRangeProvider> InteractivityAutomationPeer::GetVisibleRanges()
    {
        SAFEARRAY* pReturnVal;
        THROW_IF_FAILED(_uiaProvider->GetVisibleRanges(&pReturnVal));
        return WrapArrayOfTextRangeProviders(pReturnVal);
    }

    XamlAutomation::ITextRangeProvider InteractivityAutomationPeer::RangeFromChild(XamlAutomation::IRawElementProviderSimple childElement)
    {
        UIA::ITextRangeProvider* returnVal;
        // ScreenInfoUiaProvider doesn't actually use parameter, so just pass in nullptr
        THROW_IF_FAILED(_uiaProvider->RangeFromChild(/* IRawElementProviderSimple */ nullptr,
            &returnVal));
        return _CreateXamlUiaTextRange(returnVal);
    }

    XamlAutomation::ITextRangeProvider InteractivityAutomationPeer::RangeFromPoint(Windows::Foundation::Point screenLocation)
    {
        UIA::ITextRangeProvider* returnVal;
        THROW_IF_FAILED(_uiaProvider->RangeFromPoint({ screenLocation.X, screenLocation.Y }, &returnVal));
        return _CreateXamlUiaTextRange(returnVal);
    }

    XamlAutomation::ITextRangeProvider InteractivityAutomationPeer::DocumentRange()
    {
        UIA::ITextRangeProvider* returnVal;
        THROW_IF_FAILED(_uiaProvider->get_DocumentRange(&returnVal));
        return _CreateXamlUiaTextRange(returnVal);
    }

    XamlAutomation::SupportedTextSelection InteractivityAutomationPeer::SupportedTextSelection()
    {
        UIA::SupportedTextSelection returnVal;
        THROW_IF_FAILED(_uiaProvider->get_SupportedTextSelection(&returnVal));
        return static_cast<XamlAutomation::SupportedTextSelection>(returnVal);
    }

#pragma endregion

#pragma region IControlAccessibilityInfo
    COORD InteractivityAutomationPeer::GetFontSize() const
    {
        return til::size{ til::math::rounding, _interactivity->Core().FontSize() };
    }

    RECT InteractivityAutomationPeer::GetBounds() const
    {
        return _controlBounds;
    }

    HRESULT InteractivityAutomationPeer::GetHostUiaProvider(IRawElementProviderSimple** provider)
    {
        RETURN_HR_IF(E_INVALIDARG, provider == nullptr);
        *provider = nullptr;

        return S_OK;
    }

    RECT InteractivityAutomationPeer::GetPadding() const
    {
        return _controlPadding;
    }

    double InteractivityAutomationPeer::GetScaleFactor() const
    {
        //return DisplayInformation::GetForCurrentView().RawPixelsPerViewPixel();

        winrt::throw_hresult(CO_E_NOT_SUPPORTED);

        return 1.0;
    }

    void InteractivityAutomationPeer::ChangeViewport(const SMALL_RECT NewWindow)
    {
        _interactivity->UpdateScrollbar(NewWindow.Top);
    }
#pragma endregion

    XamlAutomation::ITextRangeProvider InteractivityAutomationPeer::_CreateXamlUiaTextRange(UIA::ITextRangeProvider* returnVal) const
    {
        // LOAD-BEARING: use _parentProvider->ProviderFromPeer(_parentProvider) instead of this->ProviderFromPeer(*this).
        // Since we split the automation peer into TermControlAutomationPeer and InteractivityAutomationPeer,
        // using "this" returns null. This can cause issues with some UIA Client scenarios like any navigation in Narrator.
        const auto parent{ _parentProvider.get() };
        if (!parent)
        {
            return nullptr;
        }
        const auto xutr = winrt::make_self<XamlUiaTextRange>(returnVal, parent.ProviderFromPeer(parent));
        return xutr.as<XamlAutomation::ITextRangeProvider>();
    };

    // Method Description:
    // - extracts the UiaTextRanges from the SAFEARRAY and converts them to Xaml ITextRangeProviders
    // Arguments:
    // - SAFEARRAY of UIA::UiaTextRange (ITextRangeProviders)
    // Return Value:
    // - com_array of Xaml Wrapped UiaTextRange (ITextRangeProviders)
    com_array<XamlAutomation::ITextRangeProvider> InteractivityAutomationPeer::WrapArrayOfTextRangeProviders(SAFEARRAY* textRanges)
    {
        // transfer ownership of UiaTextRanges to this new vector
        auto providers = SafeArrayToOwningVector<::Microsoft::Terminal::TermControlUiaTextRange>(textRanges);
        int count = gsl::narrow<int>(providers.size());

        std::vector<XamlAutomation::ITextRangeProvider> vec;
        vec.reserve(count);
        for (int i = 0; i < count; i++)
        {
            if (auto xutr = _CreateXamlUiaTextRange(providers[i].detach()))
            {
                vec.emplace_back(std::move(xutr));
            }
        }

        com_array<XamlAutomation::ITextRangeProvider> result{ vec };

        return result;
    }
}
