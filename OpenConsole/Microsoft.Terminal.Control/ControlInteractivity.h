// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Module Name:
// - ControlInteractivity.h
//
// Abstract:
// - This is a wrapper for the `ControlCore`. It holds the logic for things like
//   double-click, right click copy/paste, selection, etc. This is intended to
//   be a UI framework-independent abstraction. The methods this layer exposes
//   can be called the same from both the WinUI `TermControl` and the WPF
//   control.
//
// Author:
// - Mike Griese (zadjii-msft) 01-Apr-2021

#pragma once

#include "ControlInteractivity.g.h"
#include "EventArgs.h"
#include "ControlCore.h"

namespace ControlUnitTests
{
    class ControlCoreTests;
    class ControlInteractivityTests;
};

namespace winrt::Microsoft::Terminal::Control::implementation
{
    struct ControlInteractivity : ControlInteractivityT<ControlInteractivity>
    {
    public:
        ControlInteractivity(IControlSettings settings,
            TerminalConnection::ITerminalConnection connection);

        void GotFocus();
        void LostFocus();
        void UpdateSettings();
        void Initialize();
        Control::ControlCore Core();

        Control::InteractivityAutomationPeer OnCreateAutomationPeer();
        ::Microsoft::Console::Types::IUiaData* GetUiaData() const;

#pragma region Input Methods
        void PointerPressed(Control::MouseButtonState buttonState,
            const unsigned int pointerUpdateKind,
            const uint64_t timestamp,
            const ::Microsoft::Terminal::Core::ControlKeyStates modifiers,
            const til::point pixelPosition);
        void TouchPressed(const til::point contactPoint);

        void PointerMoved(Control::MouseButtonState buttonState,
            const unsigned int pointerUpdateKind,
            const ::Microsoft::Terminal::Core::ControlKeyStates modifiers,
            const bool focused,
            const til::point pixelPosition,
            const bool pointerPressedInBounds);
        void TouchMoved(const til::point newTouchPoint,
            const bool focused);

        void PointerReleased(Control::MouseButtonState buttonState,
            const unsigned int pointerUpdateKind,
            const ::Microsoft::Terminal::Core::ControlKeyStates modifiers,
            const til::point pixelPosition);
        void TouchReleased();

        bool MouseWheel(const ::Microsoft::Terminal::Core::ControlKeyStates modifiers,
            const int32_t delta,
            const til::point pixelPosition,
            const Control::MouseButtonState state);

        void UpdateScrollbar(const double newValue);

#pragma endregion

        bool CopySelectionToClipboard(bool singleLine,
            const Windows::Foundation::IReference<CopyFormat>& formats);
        void RequestPasteTextFromClipboard();
        void SetEndSelectionPoint(const til::point pixelPosition);

        TYPED_EVENT(OpenHyperlink, IInspectable, Control::OpenHyperlinkEventArgs);
        TYPED_EVENT(PasteFromClipboard, IInspectable, Control::PasteFromClipboardEventArgs);
        TYPED_EVENT(ScrollPositionChanged, IInspectable, Control::ScrollPositionChangedArgs);

    private:
        // NOTE: _uiaEngine must be ordered before _core.
        //
        // ControlCore::AttachUiaEngine receives a IRenderEngine as a raw pointer, which we own.
        // We must ensure that we first destroy the ControlCore before the UiaEngine instance
        // in order to safely resolve this unsafe pointer dependency. Otherwise a deallocated
        // IRenderEngine is accessed when ControlCore calls Renderer::TriggerTeardown.
        // (C++ class members are destroyed in reverse order.)
        std::unique_ptr<::Microsoft::Console::Render::UiaEngine> _uiaEngine;

        winrt::com_ptr<ControlCore> _core{ nullptr };
        unsigned int _rowsToScroll;
        double _internalScrollbarPosition{ 0.0 };

        // If this is set, then we assume we are in the middle of panning the
        //      viewport via touch input.
        std::optional<til::point> _touchAnchor;

        using Timestamp = uint64_t;

        // imported from WinUser
        // Used for PointerPoint.Timestamp Property (https://docs.microsoft.com/en-us/uwp/api/windows.ui.input.pointerpoint.timestamp#Windows_UI_Input_PointerPoint_Timestamp)
        Timestamp _multiClickTimer;
        unsigned int _multiClickCounter;
        Timestamp _lastMouseClickTimestamp;
        std::optional<til::point> _lastMouseClickPos;
        std::optional<til::point> _singleClickTouchdownPos;
        std::optional<til::point> _lastMouseClickPosNoSelection;
        // This field tracks whether the selection has changed meaningfully
        // since it was last copied. It's generally used to prevent copyOnSelect
        // from firing when the pointer _just happens_ to be released over the
        // terminal.
        bool _selectionNeedsToBeCopied;

        std::optional<COORD> _lastHoveredCell{ std::nullopt };
        // Track the last hyperlink ID we hovered over
        uint16_t _lastHoveredId{ 0 };

        std::optional<interval_tree::IntervalTree<til::point, size_t>::interval> _lastHoveredInterval{ std::nullopt };

        unsigned int _numberOfClicks(til::point clickPos, Timestamp clickTime);
        void _updateSystemParameterSettings() noexcept;

        void _mouseTransparencyHandler(const double mouseDelta);
        void _mouseZoomHandler(const double mouseDelta);
        void _mouseScrollHandler(const double mouseDelta,
            const til::point terminalPosition,
            const bool isLeftButtonPressed);

        void _hyperlinkHandler(const std::wstring_view uri);
        bool _canSendVTMouseInput(const ::Microsoft::Terminal::Core::ControlKeyStates modifiers);

        void _sendPastedTextToConnection(std::wstring_view wstr);
        til::point _getTerminalPosition(const til::point& pixelPosition);

        bool _sendMouseEventHelper(const til::point terminalPosition,
            const unsigned int pointerUpdateKind,
            const ::Microsoft::Terminal::Core::ControlKeyStates modifiers,
            const SHORT wheelDelta,
            Control::MouseButtonState buttonState);

        friend class ControlUnitTests::ControlCoreTests;
        friend class ControlUnitTests::ControlInteractivityTests;
    };
}

namespace winrt::Microsoft::Terminal::Control::factory_implementation
{
    BASIC_FACTORY(ControlInteractivity);
}
