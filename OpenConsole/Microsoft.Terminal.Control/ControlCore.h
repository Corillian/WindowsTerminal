// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Module Name:
// - ControlCore.h
//
// Abstract:
// - This encapsulates a `Terminal` instance, a `DxEngine` and `Renderer`, and
//   an `ITerminalConnection`. This is intended to be everything that someone
//   might need to stand up a terminal instance in a control, but without any
//   regard for how the UX works.
//
// Author:
// - Mike Griese (zadjii-msft) 01-Apr-2021

#pragma once

#include "EventArgs.h"
#include "ControlCore.g.h"
#include "../../external/terminal/src/renderer/base/Renderer.hpp"
#include "../../external/terminal/src/renderer/dx/DxRenderer.hpp"
#include "../../external/terminal/src/renderer/uia/UiaRenderer.hpp"
#include "../TerminalCore/ControlKeyStates.hpp"
#include "../TerminalCore/Terminal.hpp"
#include "../../external/terminal/src/buffer/out/search.h"
#include <cppwinrt_utils.h>

#include <winrt/Microsoft.Terminal.TerminalConnection.h>
#include <winrt/Microsoft.Terminal.Core.h>

namespace ControlUnitTests
{
    class ControlCoreTests;
    class ControlInteractivityTests;
};

namespace winrt::Microsoft::Terminal::Control::implementation
{
    struct ControlCore : ControlCoreT<ControlCore>
    {
    public:
        ControlCore(IControlSettings settings,
                    TerminalConnection::ITerminalConnection connection);
        ~ControlCore();

        bool Initialize(const double actualWidth,
                        const double actualHeight,
                        const double compositionScale);
        void EnablePainting();

        void UpdateSettings(const IControlSettings& settings);
        void UpdateAppearance(const IControlAppearance& newAppearance);
        void SizeChanged(const double width, const double height);
        void ScaleChanged(const double scale);
        uint64_t SwapChainHandle() const;

        void AdjustFontSize(int fontSizeDelta);
        void ResetFontSize();
        FontInfo GetFont() const;
        til::size FontSizeInDips() const;

        winrt::Windows::Foundation::Size FontSize() const noexcept;
        winrt::hstring FontFaceName() const noexcept;
        uint16_t FontWeight() const noexcept;

        winrt::Microsoft::Terminal::Core::Color BackgroundColor() const;
        void SetBackgroundOpacity(const double opacity);

        void SendInput(const winrt::hstring& wstr);
        void PasteText(const winrt::hstring& hstr);
        bool CopySelectionToClipboard(bool singleLine, const Windows::Foundation::IReference<CopyFormat>& formats);

        void ToggleShaderEffects();
        void AdjustOpacity(const double adjustment);
        void ResumeRendering();

        void UpdatePatternLocations();
        void SetHoveredCell(Core::Point terminalPosition);
        void ClearHoveredCell();
        winrt::hstring GetHyperlink(const til::point position) const;
        winrt::hstring HoveredUriText() const;
        Windows::Foundation::IReference<Core::Point> HoveredCell() const;

        ::Microsoft::Console::Types::IUiaData* GetUiaData() const;

        void Close();

#pragma region ICoreState
        const size_t TaskbarState() const noexcept;
        const size_t TaskbarProgress() const noexcept;

        hstring Title();
        Windows::Foundation::IReference<winrt::Windows::UI::Color> TabColor() noexcept;
        hstring WorkingDirectory() const;

        TerminalConnection::ConnectionState ConnectionState() const;

        int ScrollOffset();
        int ViewHeight() const;
        int BufferHeight() const;

        bool BracketedPasteEnabled() const noexcept;
#pragma endregion

#pragma region ITerminalInput
        bool TrySendKeyEvent(const WORD vkey,
                             const WORD scanCode,
                             const Core::ControlKeyStates modifiers,
                             const bool keyDown);
        bool SendCharEvent(const wchar_t ch,
                           const WORD scanCode,
                           const Core::ControlKeyStates modifiers);
        bool SendMouseEvent(const til::point viewportPos,
                            const unsigned int uiButton,
                            const Core::ControlKeyStates states,
                            const short wheelDelta,
                            const ::Microsoft::Console::VirtualTerminal::TerminalInput::MouseButtonState state);
        void UserScrollViewport(const int viewTop);
#pragma endregion

        void BlinkAttributeTick();
        void BlinkCursor();
        bool CursorOn() const;
        void CursorOn(const bool isCursorOn);

        bool IsVtMouseModeEnabled() const;
        Core::Point CursorPosition() const;

        bool HasSelection() const;
        bool CopyOnSelect() const;
        Windows::Foundation::Collections::IVector<winrt::hstring> SelectedText(bool trimTrailingWhitespace) const;
        void SetSelectionAnchor(til::point const& position);
        void SetEndSelectionPoint(til::point const& position);

        void Search(const winrt::hstring& text,
                    const bool goForward,
                    const bool caseSensitive);

        void LeftClickOnTerminal(const til::point terminalPosition,
                                 const int numberOfClicks,
                                 const bool altEnabled,
                                 const bool shiftEnabled,
                                 const bool isOnOriginalPosition,
                                 bool& selectionNeedsToBeCopied);

        void AttachUiaEngine(::Microsoft::Console::Render::IRenderEngine* const pEngine);

        bool IsInReadOnlyMode() const;
        void ToggleReadOnlyMode();

        // -------------------------------- WinRT Events ---------------------------------
        // clang-format off
        WINRT_CALLBACK(FontSizeChanged, Control::FontSizeChangedEventArgs);

        TYPED_EVENT(CopyToClipboard,           IInspectable, Control::CopyToClipboardEventArgs);
        TYPED_EVENT(TitleChanged,              IInspectable, Control::TitleChangedEventArgs);
        TYPED_EVENT(WarningBell,               IInspectable, IInspectable);
        TYPED_EVENT(TabColorChanged,           IInspectable, IInspectable);
        TYPED_EVENT(BackgroundColorChanged,    IInspectable, IInspectable);
        TYPED_EVENT(ScrollPositionChanged,     IInspectable, Control::ScrollPositionChangedArgs);
        TYPED_EVENT(CursorPositionChanged,     IInspectable, IInspectable);
        TYPED_EVENT(TaskbarProgressChanged,    IInspectable, IInspectable);
        TYPED_EVENT(ConnectionStateChanged,    IInspectable, IInspectable);
        TYPED_EVENT(HoveredHyperlinkChanged,   IInspectable, IInspectable);
        TYPED_EVENT(RendererEnteredErrorState, IInspectable, IInspectable);
        TYPED_EVENT(SwapChainChanged,          IInspectable, IInspectable);
        TYPED_EVENT(RendererWarning,           IInspectable, Control::RendererWarningArgs);
        TYPED_EVENT(RaiseNotice,               IInspectable, Control::NoticeEventArgs);
        TYPED_EVENT(TransparencyChanged,       IInspectable, Control::TransparencyChangedEventArgs);
        TYPED_EVENT(ReceivedOutput,            IInspectable, IInspectable);
        // clang-format on

    private:
        bool _initializedTerminal{ false };
        bool _closing{ false };

        TerminalConnection::ITerminalConnection _connection{ nullptr };
        event_token _connectionOutputEventToken;
        TerminalConnection::ITerminalConnection::StateChanged_revoker _connectionStateChangedRevoker;

        std::unique_ptr<::Microsoft::Terminal::Core::Terminal> _terminal{ nullptr };

        // NOTE: _renderEngine must be ordered before _renderer.
        //
        // As _renderer has a dependency on _renderEngine (through a raw pointer)
        // we must ensure the _renderer is deallocated first.
        // (C++ class members are destroyed in reverse order.)
        std::unique_ptr<::Microsoft::Console::Render::DxEngine> _renderEngine{ nullptr };
        std::unique_ptr<::Microsoft::Console::Render::Renderer> _renderer{ nullptr };

        IControlSettings _settings{ nullptr };

        FontInfoDesired _desiredFont;
        FontInfo _actualFont;

        // storage location for the leading surrogate of a utf-16 surrogate pair
        std::optional<wchar_t> _leadingSurrogate{ std::nullopt };

        std::optional<til::point> _lastHoveredCell{ std::nullopt };
        // Track the last hyperlink ID we hovered over
        uint16_t _lastHoveredId{ 0 };

        bool _isReadOnly{ false };

        std::optional<interval_tree::IntervalTree<til::point, size_t>::interval> _lastHoveredInterval{ std::nullopt };

        // These members represent the size of the surface that we should be
        // rendering to.
        double _panelWidth{ 0 };
        double _panelHeight{ 0 };
        double _compositionScale{ 0 };

        winrt::Microsoft::UI::Dispatching::DispatcherQueue _dispatcher{ nullptr };
        std::shared_ptr<ThrottledFuncTrailing<>> _tsfTryRedrawCanvas;
        std::shared_ptr<ThrottledFuncTrailing<>> _updatePatternLocations;
        std::shared_ptr<ThrottledFuncTrailing<Control::ScrollPositionChangedArgs>> _updateScrollBar;

        winrt::fire_and_forget _asyncCloseConnection();

        void _setFontSize(int fontSize);
        void _updateFont(const bool initialUpdate = false);
        void _refreshSizeUnderLock();
        void _doResizeUnderLock(const double newWidth,
                                const double newHeight);

        void _sendInputToConnection(std::wstring_view wstr);

#pragma region TerminalCoreCallbacks
        void _terminalCopyToClipboard(std::wstring_view wstr);
        void _terminalWarningBell();
        void _terminalTitleChanged(std::wstring_view wstr);
        void _terminalTabColorChanged(const std::optional<til::color> color);
        void _terminalBackgroundColorChanged(const COLORREF color);
        void _terminalScrollPositionChanged(const int viewTop,
                                            const int viewHeight,
                                            const int bufferSize);
        void _terminalCursorPositionChanged();
        void _terminalTaskbarProgressChanged();
#pragma endregion

#pragma region RendererCallbacks
        void _rendererWarning(const HRESULT hr);
        void _renderEngineSwapChainChanged();
#pragma endregion

        void _raiseReadOnlyWarning();
        void _updateAntiAliasingMode(::Microsoft::Console::Render::DxEngine* const dxEngine);
        void _connectionOutputHandler(const hstring& hstr);
        void _updateHoveredCell(const std::optional<til::point> terminalPosition);

        inline bool _IsClosing() const noexcept
        {
#ifndef NDEBUG
            if (_dispatcher)
            {
                // _closing isn't atomic and may only be accessed from the main thread.
                //
                // Though, the unit tests don't actually run in TAEF's main
                // thread, so we don't care when we're running in tests.
                assert(_inUnitTests || _dispatcher.HasThreadAccess());
            }
#endif
            return _closing;
        }

        friend class ControlUnitTests::ControlCoreTests;
        friend class ControlUnitTests::ControlInteractivityTests;
        bool _inUnitTests{ false };
    };
}

namespace winrt::Microsoft::Terminal::Control::factory_implementation
{
    BASIC_FACTORY(ControlCore);
}
