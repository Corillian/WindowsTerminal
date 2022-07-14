// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "ControlCore.h"

#include <DefaultSettings.h>
#include <unicode.hpp>
#include <Utf16Parser.hpp>
#include <WinUser.h>
#include <LibraryResources.h>

#include "EventArgs.h"
#include "../../external/terminal/src/types/inc/GlyphWidth.hpp"
#include "../../external/terminal/src/types/inc/Utils.hpp"
#include "../../external/terminal/src/buffer/out/search.h"
#include "../../renderer/atlas/AtlasEngine.h"
#include "../../renderer/dx/DxRenderer.hpp"

#include "ControlCore.g.cpp"

using namespace ::Microsoft::Console::Types;
using namespace ::Microsoft::Console::VirtualTerminal;
using namespace ::Microsoft::Terminal::Core;
using namespace winrt::Windows::System;
using namespace winrt::Windows::ApplicationModel::DataTransfer;

// The minimum delay between updates to the scroll bar's values.
// The updates are throttled to limit power usage.
constexpr const auto ScrollBarUpdateInterval = std::chrono::milliseconds(8);

// The minimum delay between updating the TSF input control.
constexpr const auto TsfRedrawInterval = std::chrono::milliseconds(100);

// The minimum delay between updating the locations of regex patterns
constexpr const auto UpdatePatternLocationsInterval = std::chrono::milliseconds(500);

namespace winrt::Microsoft::Terminal::Control::implementation
{
    // Helper static function to ensure that all ambiguous-width glyphs are reported as narrow.
    // See microsoft/terminal#2066 for more info.
    static bool _IsGlyphWideForceNarrowFallback(const std::wstring_view /* glyph */)
    {
        return false; // glyph is not wide.
    }

    static bool _EnsureStaticInitialization()
    {
        // use C++11 magic statics to make sure we only do this once.
        static auto initialized = []() {
            // *** THIS IS A SINGLETON ***
            SetGlyphWidthFallback(_IsGlyphWideForceNarrowFallback);

            return true;
        }();
        return initialized;
    }

    ControlCore::ControlCore(Control::IControlSettings settings,
                             Control::IControlAppearance unfocusedAppearance,
                             TerminalConnection::ITerminalConnection connection) :
        _connection{ connection },
        _desiredFont{ DEFAULT_FONT_FACE, 0, DEFAULT_FONT_WEIGHT, { 0, DEFAULT_FONT_SIZE }, CP_UTF8 },
        _actualFont{ DEFAULT_FONT_FACE, 0, DEFAULT_FONT_WEIGHT, { 0, DEFAULT_FONT_SIZE }, CP_UTF8, false }
    {
        _EnsureStaticInitialization();

        _settings = winrt::make_self<implementation::ControlSettings>(settings, unfocusedAppearance);

        _terminal = std::make_unique<::Microsoft::Terminal::Core::Terminal>();

        // Subscribe to the connection's disconnected event and call our connection closed handlers.
        _connectionStateChangedRevoker = _connection.StateChanged(winrt::auto_revoke, [this](auto&& /*s*/, auto&& /*v*/) {
            _ConnectionStateChangedHandlers(*this, nullptr);
        });

        // This event is explicitly revoked in the destructor: does not need weak_ref
        _connectionOutputEventToken = _connection.TerminalOutput({ this, &ControlCore::_connectionOutputHandler });

        _terminal->SetWriteInputCallback([this](std::wstring_view wstr) {
            _sendInputToConnection(wstr);
        });

        // GH#8969: pre-seed working directory to prevent potential races
        _terminal->SetWorkingDirectory(_settings->StartingDirectory());

        auto pfnCopyToClipboard = std::bind(&ControlCore::_terminalCopyToClipboard, this, std::placeholders::_1);
        _terminal->SetCopyToClipboardCallback(pfnCopyToClipboard);

        auto pfnWarningBell = std::bind(&ControlCore::_terminalWarningBell, this);
        _terminal->SetWarningBellCallback(pfnWarningBell);

        auto pfnTitleChanged = std::bind(&ControlCore::_terminalTitleChanged, this, std::placeholders::_1);
        _terminal->SetTitleChangedCallback(pfnTitleChanged);

        auto pfnTabColorChanged = std::bind(&ControlCore::_terminalTabColorChanged, this, std::placeholders::_1);
        _terminal->SetTabColorChangedCallback(pfnTabColorChanged);

        auto pfnScrollPositionChanged = std::bind(&ControlCore::_terminalScrollPositionChanged, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);
        _terminal->SetScrollPositionChangedCallback(pfnScrollPositionChanged);

        auto pfnTerminalCursorPositionChanged = std::bind(&ControlCore::_terminalCursorPositionChanged, this);
        _terminal->SetCursorPositionChangedCallback(pfnTerminalCursorPositionChanged);

        auto pfnTerminalTaskbarProgressChanged = std::bind(&ControlCore::_terminalTaskbarProgressChanged, this);
        _terminal->TaskbarProgressChangedCallback(pfnTerminalTaskbarProgressChanged);

        auto pfnShowWindowChanged = std::bind(&ControlCore::_terminalShowWindowChanged, this, std::placeholders::_1);
        _terminal->SetShowWindowCallback(pfnShowWindowChanged);

        // MSFT 33353327: Initialize the renderer in the ctor instead of Initialize().
        // We need the renderer to be ready to accept new engines before the SwapChainPanel is ready to go.
        // If we wait, a screen reader may try to get the AutomationPeer (aka the UIA Engine), and we won't be able to attach
        // the UIA Engine to the renderer. This prevents us from signaling changes to the cursor or buffer.
        {
            // First create the render thread.
            // Then stash a local pointer to the render thread so we can initialize it and enable it
            // to paint itself *after* we hand off its ownership to the renderer.
            // We split up construction and initialization of the render thread object this way
            // because the renderer and render thread have circular references to each other.
            auto renderThread = std::make_unique<::Microsoft::Console::Render::RenderThread>();
            auto* const localPointerToThread = renderThread.get();

            // Now create the renderer and initialize the render thread.
            const auto& renderSettings = _terminal->GetRenderSettings();
            _renderer = std::make_unique<::Microsoft::Console::Render::Renderer>(renderSettings, _terminal.get(), nullptr, 0, std::move(renderThread));

            _renderer->SetBackgroundColorChangedCallback([this]() { _rendererBackgroundColorChanged(); });

            _renderer->SetRendererEnteredErrorStateCallback([weakThis = get_weak()]() {
                if (auto strongThis{ weakThis.get() })
                {
                    strongThis->_RendererEnteredErrorStateHandlers(*strongThis, nullptr);
                }
            });

            THROW_IF_FAILED(localPointerToThread->Initialize(_renderer.get()));
        }

        // Get our dispatcher. If we're hosted in-proc with XAML, this will get
        // us the same dispatcher as TermControl::Dispatcher(). If we're out of
        // proc, this'll return null. We'll need to instead make a new
        // DispatcherQueue (on a new thread), so we can use that for throttled
        // functions.
        _dispatcher = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
        if (!_dispatcher)
        {
            auto controller{ winrt::Microsoft::UI::Dispatching::DispatcherQueueController::CreateOnDedicatedThread() };
            _dispatcher = controller.DispatcherQueue();
        }

        // A few different events should be throttled, so they don't fire absolutely all the time:
        // * _tsfTryRedrawCanvas: When the cursor position moves, we need to
        //   inform TSF, so it can move the canvas for the composition. We
        //   throttle this so that we're not hopping across the process boundary
        //   every time that the cursor moves.
        // * _updatePatternLocations: When there's new output, or we scroll the
        //   viewport, we should re-check if there are any visible hyperlinks.
        //   But we don't really need to do this every single time text is
        //   output, we can limit this update to once every 500ms.
        // * _updateScrollBar: Same idea as the TSF update - we don't _really_
        //   need to hop across the process boundary every time text is output.
        //   We can throttle this to once every 8ms, which will get us out of
        //   the way of the main output & rendering threads.
        _tsfTryRedrawCanvas = std::make_shared<ThrottledFuncTrailing<>>(
            _dispatcher,
            TsfRedrawInterval,
            [weakThis = get_weak()]() {
                if (auto core{ weakThis.get() }; !core->_IsClosing())
                {
                    core->_CursorPositionChangedHandlers(*core, nullptr);
                }
            });

        _updatePatternLocations = std::make_shared<ThrottledFuncTrailing<>>(
            _dispatcher,
            UpdatePatternLocationsInterval,
            [weakThis = get_weak()]() {
                if (auto core{ weakThis.get() }; !core->_IsClosing())
                {
                    core->UpdatePatternLocations();
                }
            });

        _updateScrollBar = std::make_shared<ThrottledFuncTrailing<Control::ScrollPositionChangedArgs>>(
            _dispatcher,
            ScrollBarUpdateInterval,
            [weakThis = get_weak()](const auto& update) {
                if (auto core{ weakThis.get() }; !core->_IsClosing())
                {
                    core->_ScrollPositionChangedHandlers(*core, update);
                }
            });

        UpdateSettings(settings, unfocusedAppearance);
    }

    ControlCore::~ControlCore()
    {
        Close();

        if (_renderer)
        {
            _renderer->TriggerTeardown();
        }
    }

    bool ControlCore::Initialize(const double actualWidth,
                                 const double actualHeight,
                                 const double compositionScale)
    {
        assert(_settings);

        _panelWidth = actualWidth;
        _panelHeight = actualHeight;
        _compositionScale = compositionScale;

        { // scope for terminalLock
            auto terminalLock = _terminal->LockForWriting();

            if (_initializedTerminal)
            {
                return false;
            }

            const auto windowWidth = actualWidth * compositionScale;
            const auto windowHeight = actualHeight * compositionScale;

            if (windowWidth == 0 || windowHeight == 0)
            {
                return false;
            }

            if (/*Feature_AtlasEngine::IsEnabled() &&*/ _settings->UseAtlasEngine())
            {
                _renderEngine = std::make_unique<::Microsoft::Console::Render::AtlasEngine>();
            }
            else
            {
                _renderEngine = std::make_unique<::Microsoft::Console::Render::DxEngine>();
            }

            _renderer->AddRenderEngine(_renderEngine.get());

            // Initialize our font with the renderer
            // We don't have to care about DPI. We'll get a change message immediately if it's not 96
            // and react accordingly.
            _updateFont(true);

            const COORD windowSize{ static_cast<short>(windowWidth),
                                    static_cast<short>(windowHeight) };

            // First set up the dx engine with the window size in pixels.
            // Then, using the font, get the number of characters that can fit.
            // Resize our terminal connection to match that size, and initialize the terminal with that size.
            const auto viewInPixels = Viewport::FromDimensions({ 0, 0 }, windowSize);
            LOG_IF_FAILED(_renderEngine->SetWindowSize({ viewInPixels.Width(), viewInPixels.Height() }));

            // Update DxEngine's SelectionBackground
            _renderEngine->SetSelectionBackground(til::color{ _settings->SelectionBackground() });

            const auto vp = _renderEngine->GetViewportInCharacters(viewInPixels);
            const auto width = vp.Width();
            const auto height = vp.Height();
            _connection.Resize(height, width);

            if (_OwningHwnd != 0)
            {
                if (auto conpty{ _connection.try_as<TerminalConnection::ConptyConnection>() })
                {
                    conpty.ReparentWindow(_OwningHwnd);
                }
            }

            // Override the default width and height to match the size of the swapChainPanel
            _settings->InitialCols(width);
            _settings->InitialRows(height);

            _terminal->CreateFromSettings(*_settings, *_renderer);

            // IMPORTANT! Set this callback up sooner than later. If we do it
            // after Enable, then it'll be possible to paint the frame once
            // _before_ the warning handler is set up, and then warnings from
            // the first paint will be ignored!
            _renderEngine->SetWarningCallback(std::bind(&ControlCore::_rendererWarning, this, std::placeholders::_1));

            // Tell the DX Engine to notify us when the swap chain changes.
            // We do this after we initially set the swapchain so as to avoid unnecessary callbacks (and locking problems)
            _renderEngine->SetCallback(std::bind(&ControlCore::_renderEngineSwapChainChanged, this));

            _renderEngine->SetRetroTerminalEffect(_settings->RetroTerminalEffect());
            _renderEngine->SetPixelShaderPath(_settings->PixelShaderPath());
            _renderEngine->SetForceFullRepaintRendering(_settings->ForceFullRepaintRendering());
            _renderEngine->SetSoftwareRendering(_settings->SoftwareRendering());

            _updateAntiAliasingMode();

            // GH#5098: Inform the engine of the opacity of the default text background.
            // GH#11315: Always do this, even if they don't have acrylic on.
            _renderEngine->EnableTransparentBackground(_isBackgroundTransparent());

            THROW_IF_FAILED(_renderEngine->Enable());

            _initializedTerminal = true;
        } // scope for TerminalLock

        // Start the connection outside of lock, because it could
        // start writing output immediately.
        _connection.Start();

        return true;
    }

    // Method Description:
    // - Tell the renderer to start painting.
    // - !! IMPORTANT !! Make sure that we've attached our swap chain to an
    //   actual target before calling this.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void ControlCore::EnablePainting()
    {
        if (_initializedTerminal)
        {
            _renderer->EnablePainting();
        }
    }

    // Method Description:
    // - Writes the given sequence as input to the active terminal connection.
    // - This method has been overloaded to allow zero-copy winrt::param::hstring optimizations.
    // Arguments:
    // - wstr: the string of characters to write to the terminal connection.
    // Return Value:
    // - <none>
    void ControlCore::_sendInputToConnection(std::wstring_view wstr)
    {
        if (_isReadOnly)
        {
            _raiseReadOnlyWarning();
        }
        else
        {
            _connection.WriteInput(wstr);
        }
    }

    // Method Description:
    // - Writes the given sequence as input to the active terminal connection,
    // Arguments:
    // - wstr: the string of characters to write to the terminal connection.
    // Return Value:
    // - <none>
    void ControlCore::SendInput(const winrt::hstring& wstr)
    {
        _sendInputToConnection(wstr);
    }

    bool ControlCore::SendCharEvent(const wchar_t ch,
                                    const WORD scanCode,
                                    const ::Microsoft::Terminal::Core::ControlKeyStates modifiers)
    {
        return _terminal->SendCharEvent(ch, scanCode, modifiers);
    }

    // Method Description:
    // - Send this particular key event to the terminal.
    //   See Terminal::SendKeyEvent for more information.
    // - Clears the current selection.
    // - Makes the cursor briefly visible during typing.
    // Arguments:
    // - vkey: The vkey of the key pressed.
    // - scanCode: The scan code of the key pressed.
    // - modifiers: The Microsoft::Terminal::Core::ControlKeyStates representing the modifier key states.
    // - keyDown: If true, the key was pressed, otherwise the key was released.
    bool ControlCore::TrySendKeyEvent(const WORD vkey,
                                      const WORD scanCode,
                                      const ControlKeyStates modifiers,
                                      const bool keyDown)
    {
        // Update the selection, if it's present
        // GH#6423 - don't dismiss selection if the key that was pressed was a
        // modifier key. We'll wait for a real keystroke to dismiss the
        // GH #7395 - don't dismiss selection when taking PrintScreen
        // selection.
        // GH#8522, GH#3758 - Only modify the selection on key _down_. If we
        // modify on key up, then there's chance that we'll immediately dismiss
        // a selection created by an action bound to a keydown.
        if (HasSelection() &&
            !KeyEvent::IsModifierKey(vkey) &&
            vkey != VK_SNAPSHOT &&
            keyDown)
        {
            // try to update the selection
            if (const auto updateSlnParams{ ::Terminal::ConvertKeyEventToUpdateSelectionParams(modifiers, vkey) })
            {
                auto lock = _terminal->LockForWriting();
                _terminal->UpdateSelection(updateSlnParams->first, updateSlnParams->second);
                _renderer->TriggerSelection();
                return true;
            }

            // GH#8791 - don't dismiss selection if Windows key was also pressed as a key-combination.
            if (!modifiers.IsWinPressed())
            {
                _terminal->ClearSelection();
                _renderer->TriggerSelection();
            }

            // When there is a selection active, escape should clear it and NOT flow through
            // to the terminal. With any other keypress, it should clear the selection AND
            // flow through to the terminal.
            if (vkey == VK_ESCAPE)
            {
                return true;
            }
        }

        // If the terminal translated the key, mark the event as handled.
        // This will prevent the system from trying to get the character out
        // of it and sending us a CharacterReceived event.
        return vkey ? _terminal->SendKeyEvent(vkey,
                                              scanCode,
                                              *reinterpret_cast<::Microsoft::Terminal::Core::ControlKeyStates const*>(&modifiers),
                                              keyDown) :
                      true;
    }

    bool ControlCore::SendMouseEvent(const til::point viewportPos,
                                     const unsigned int uiButton,
                                     const ControlKeyStates states,
                                     const short wheelDelta,
                                     const TerminalInput::MouseButtonState state)
    {
        return _terminal->SendMouseEvent(viewportPos.to_win32_coord(), uiButton, *reinterpret_cast<::Microsoft::Terminal::Core::ControlKeyStates const*>(&states), wheelDelta, state);
    }

    void ControlCore::UserScrollViewport(const int viewTop)
    {
        // Clear the regex pattern tree so the renderer does not try to render them while scrolling
        _terminal->ClearPatternTree();

        // This is a scroll event that wasn't initiated by the terminal
        //      itself - it was initiated by the mouse wheel, or the scrollbar.
        _terminal->UserScrollViewport(viewTop);

        _updatePatternLocations->Run();
    }

    void ControlCore::AdjustOpacity(const double adjustment)
    {
        if (adjustment == 0)
        {
            return;
        }

        _setOpacity(Opacity() + adjustment);
    }

    void ControlCore::_setOpacity(const double opacity)
    {
        const auto newOpacity = std::clamp(opacity,
                                     0.0,
                                     1.0);

        if (newOpacity == Opacity())
                {
            return;
                }

        // Update our runtime opacity value
        Opacity(newOpacity);

        // GH#11285 - If the user is on Windows 10, and they changed the
        // transparency of the control s.t. it should be partially opaque, then
        // opt them in to acrylic. It's the only way to have transparency on
        // Windows 10.
        // We'll also turn the acrylic back off when they're fully opaque, which
        // is what the Terminal did prior to 1.12.
        if (!IsVintageOpacityAvailable())
                {
            _runtimeUseAcrylic = newOpacity < 1.0;
                }

        // Update the renderer as well. It might need to fall back from
        // cleartype -> grayscale if the BG is transparent / acrylic.
        if (_renderEngine)
        {
            _renderEngine->EnableTransparentBackground(_isBackgroundTransparent());
            _renderer->NotifyPaintFrame();
        }

            auto eventArgs = winrt::make_self<TransparencyChangedEventArgs>(newOpacity);
            _TransparencyChangedHandlers(*this, *eventArgs);
        }

    void ControlCore::ToggleShaderEffects()
    {
        auto lock = _terminal->LockForWriting();
        // Originally, this action could be used to enable the retro effects
        // even when they're set to `false` in the settings. If the user didn't
        // specify a custom pixel shader, manually enable the legacy retro
        // effect first. This will ensure that a toggle off->on will still work,
        // even if they currently have retro effect off.
        if (_settings->PixelShaderPath().empty() && !_renderEngine->GetRetroTerminalEffect())
        {
            // SetRetroTerminalEffect to true will enable the effect. In this
            // case, the shader effect will already be disabled (because neither
            // a pixel shader nor the retro effects were originally requested).
            // So we _don't_ want to toggle it again below, because that would
            // toggle it back off.
            _renderEngine->SetRetroTerminalEffect(true);
        }
        else
        {
            _renderEngine->ToggleShaderEffects();
        }
        // Always redraw after toggling effects. This way even if the control
        // does not have focus it will update immediately.
        _renderer->TriggerRedrawAll();
    }

    // Method Description:
    // - Tell TerminalCore to update its knowledge about the locations of visible regex patterns
    // - We should call this (through the throttled function) when something causes the visible
    //   region to change, such as when new text enters the buffer or the viewport is scrolled
    void ControlCore::UpdatePatternLocations()
    {
        auto lock = _terminal->LockForWriting();
        _terminal->UpdatePatternsUnderLock();
    }

    // Method description:
    // - Updates last hovered cell, renders / removes rendering of hyper-link if required
    // Arguments:
    // - terminalPosition: The terminal position of the pointer
    void ControlCore::SetHoveredCell(Core::Point pos)
    {
        _updateHoveredCell(std::optional<til::point>{ pos });
    }
    void ControlCore::ClearHoveredCell()
    {
        _updateHoveredCell(std::nullopt);
    }

    void ControlCore::_updateHoveredCell(const std::optional<til::point> terminalPosition)
    {
        if (terminalPosition == _lastHoveredCell)
        {
            return;
        }

        // GH#9618 - lock while we're reading from the terminal, and if we need
        // to update something, then lock again to write the terminal.

        _lastHoveredCell = terminalPosition;
        uint16_t newId{ 0u };
        // we can't use auto here because we're pre-declaring newInterval.
        decltype(_terminal->GetHyperlinkIntervalFromPosition(COORD{})) newInterval{ std::nullopt };
        if (terminalPosition.has_value())
        {
            auto lock = _terminal->LockForReading(); // Lock for the duration of our reads.
            newId = _terminal->GetHyperlinkIdAtPosition(terminalPosition->to_win32_coord());
            newInterval = _terminal->GetHyperlinkIntervalFromPosition(terminalPosition->to_win32_coord());
        }

        // If the hyperlink ID changed or the interval changed, trigger a redraw all
        // (so this will happen both when we move onto a link and when we move off a link)
        if (newId != _lastHoveredId ||
            (newInterval != _lastHoveredInterval))
        {
            // Introduce scope for lock - we don't want to raise the
            // HoveredHyperlinkChanged event under lock, because then handlers
            // wouldn't be able to ask us about the hyperlink text/position
            // without deadlocking us.
            {
                auto lock = _terminal->LockForWriting();

                _lastHoveredId = newId;
                _lastHoveredInterval = newInterval;
                _renderEngine->UpdateHyperlinkHoveredId(newId);
                _renderer->UpdateLastHoveredInterval(newInterval);
                _renderer->TriggerRedrawAll();
            }

            _HoveredHyperlinkChangedHandlers(*this, nullptr);
        }
    }

    winrt::hstring ControlCore::GetHyperlink(const Core::Point pos) const
    {
        // Lock for the duration of our reads.
        auto lock = _terminal->LockForReading();
        return winrt::hstring{ _terminal->GetHyperlinkAtPosition(til::point{ pos }.to_win32_coord()) };
    }

    winrt::hstring ControlCore::HoveredUriText() const
    {
        auto lock = _terminal->LockForReading(); // Lock for the duration of our reads.
        if (_lastHoveredCell.has_value())
        {
            return winrt::hstring{ _terminal->GetHyperlinkAtPosition(_lastHoveredCell->to_win32_coord()) };
        }
        return {};
    }

    Windows::Foundation::IReference<Core::Point> ControlCore::HoveredCell() const
    {
        return _lastHoveredCell.has_value() ? Windows::Foundation::IReference<Core::Point>{ _lastHoveredCell.value().to_core_point() } : nullptr;
    }

    // Method Description:
    // - Updates the settings of the current terminal.
    // - INVARIANT: This method can only be called if the caller DOES NOT HAVE writing lock on the terminal.
    void ControlCore::UpdateSettings(const IControlSettings& settings, const IControlAppearance& newAppearance)
    {
        _settings = winrt::make_self<implementation::ControlSettings>(settings, newAppearance);

        auto lock = _terminal->LockForWriting();

        _runtimeOpacity = std::nullopt;
        _runtimeUseAcrylic = std::nullopt;

        // GH#11285 - If the user is on Windows 10, and they wanted opacity, but
        // didn't explicitly request acrylic, then opt them in to acrylic.
        // On Windows 11+, this isn't needed, because we can have vintage opacity.
        if (!IsVintageOpacityAvailable() && _settings->Opacity() < 1.0 && !_settings->UseAcrylic())
        {
            _runtimeUseAcrylic = true;
        }

        const auto sizeChanged = _setFontSizeUnderLock(_settings->FontSize());

        // Update the terminal core with its new Core settings
        _terminal->UpdateSettings(*_settings);

        if (!_initializedTerminal)
        {
            // If we haven't initialized, there's no point in continuing.
            // Initialization will handle the renderer settings.
            return;
        }

        _renderEngine->SetForceFullRepaintRendering(_settings->ForceFullRepaintRendering());
        _renderEngine->SetSoftwareRendering(_settings->SoftwareRendering());
        // Inform the renderer of our opacity
        _renderEngine->EnableTransparentBackground(_isBackgroundTransparent());

        _updateAntiAliasingMode();

        if (sizeChanged)
        {
            _refreshSizeUnderLock();
        }
    }

    // Method Description:
    // - Updates the appearance of the current terminal.
    // - INVARIANT: This method can only be called if the caller DOES NOT HAVE writing lock on the terminal.
    void ControlCore::ApplyAppearance(const bool& focused)
    {
        auto lock = _terminal->LockForWriting();
        const auto& newAppearance{ focused ? _settings->FocusedAppearance() : _settings->UnfocusedAppearance() };
        // Update the terminal core with its new Core settings
        _terminal->UpdateAppearance(*newAppearance);

        // Update DxEngine settings under the lock
        if (_renderEngine)
        {
            // Update DxEngine settings under the lock
            _renderEngine->SetSelectionBackground(til::color{ newAppearance->SelectionBackground() });
            _renderEngine->SetRetroTerminalEffect(newAppearance->RetroTerminalEffect());
            _renderEngine->SetPixelShaderPath(newAppearance->PixelShaderPath());
            _renderer->TriggerRedrawAll();
        }
    }

    void ControlCore::_updateAntiAliasingMode()
    {
        D2D1_TEXT_ANTIALIAS_MODE mode;
        // Update DxEngine's AntialiasingMode
        switch (_settings->AntialiasingMode())
        {
        case TextAntialiasingMode::Cleartype:
            mode = D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE;
            break;
        case TextAntialiasingMode::Aliased:
            mode = D2D1_TEXT_ANTIALIAS_MODE_ALIASED;
            break;
        default:
            mode = D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE;
            break;
        }

        _renderEngine->SetAntialiasingMode(mode);
    }

    // Method Description:
    // - Update the font with the renderer. This will be called either when the
    //      font changes or the DPI changes, as DPI changes will necessitate a
    //      font change. This method will *not* change the buffer/viewport size
    //      to account for the new glyph dimensions. Callers should make sure to
    //      appropriately call _doResizeUnderLock after this method is called.
    // - The write lock should be held when calling this method.
    // Arguments:
    // - initialUpdate: whether this font update should be considered as being
    //   concerned with initialization process. Value forwarded to event handler.
    void ControlCore::_updateFont(const bool initialUpdate)
    {
        const auto newDpi = static_cast<int>(static_cast<double>(USER_DEFAULT_SCREEN_DPI) *
                                            _compositionScale);

        _terminal->SetFontInfo(_actualFont);

        if (_renderEngine)
        {
            std::unordered_map<std::wstring_view, uint32_t> featureMap;
            if (const auto fontFeatures = _settings->FontFeatures())
            {
                featureMap.reserve(fontFeatures.Size());

                for (const auto& [tag, param] : fontFeatures)
                {
                    featureMap.emplace(tag, param);
                }
            }
            std::unordered_map<std::wstring_view, float> axesMap;
            if (const auto fontAxes = _settings->FontAxes())
            {
                axesMap.reserve(fontAxes.Size());

                for (const auto& [axis, value] : fontAxes)
                {
                    axesMap.emplace(axis, value);
                }
            }

            // TODO: MSFT:20895307 If the font doesn't exist, this doesn't
            //      actually fail. We need a way to gracefully fallback.
            LOG_IF_FAILED(_renderEngine->UpdateDpi(newDpi));
            LOG_IF_FAILED(_renderEngine->UpdateFont(_desiredFont, _actualFont, featureMap, axesMap));
        }

        // If the actual font isn't what was requested...
        if (_actualFont.GetFaceName() != _desiredFont.GetFaceName())
        {
            // Then warn the user that we picked something because we couldn't find their font.
            // Format message with user's choice of font and the font that was chosen instead.
            const winrt::hstring message{ fmt::format(std::wstring_view{ RS_(L"NoticeFontNotFound") },
                                                      _desiredFont.GetFaceName(),
                                                      _actualFont.GetFaceName()) };
            auto noticeArgs = winrt::make<NoticeEventArgs>(NoticeLevel::Warning, message);
            _RaiseNoticeHandlers(*this, std::move(noticeArgs));
        }

        const auto actualNewSize = _actualFont.GetSize();
        _FontSizeChangedHandlers(actualNewSize.X, actualNewSize.Y, initialUpdate);
    }

    // Method Description:
    // - Set the font size of the terminal control.
    // Arguments:
    // - fontSize: The size of the font.
    // Return Value:
    // - Returns true if you need to call _refreshSizeUnderLock().
    bool ControlCore::_setFontSizeUnderLock(int fontSize)
        {
            // Make sure we have a non-zero font size
            const auto newSize = std::max<short>(gsl::narrow_cast<short>(fontSize), 1);
        const auto fontFace = _settings->FontFace();
        const auto fontWeight = _settings->FontWeight();
            _actualFont = { fontFace, 0, fontWeight.Weight, { 0, newSize }, CP_UTF8, false };
        _actualFontFaceName = { fontFace };
            _desiredFont = { _actualFont };

        const auto before = _actualFont.GetSize();
            _updateFont();
        const auto after = _actualFont.GetSize();
        return before != after;
    }

    // Method Description:
    // - Reset the font size of the terminal to its default size.
    // Arguments:
    // - none
    void ControlCore::ResetFontSize()
    {
        const auto lock = _terminal->LockForWriting();

        if (_setFontSizeUnderLock(_settings->FontSize()))
        {
            _refreshSizeUnderLock();
        }
    }

    // Method Description:
    // - Adjust the font size of the terminal control.
    // Arguments:
    // - fontSizeDelta: The amount to increase or decrease the font size by.
    void ControlCore::AdjustFontSize(int fontSizeDelta)
    {
        const auto lock = _terminal->LockForWriting();

        if (_setFontSizeUnderLock(_desiredFont.GetEngineSize().Y + fontSizeDelta))
        {
            _refreshSizeUnderLock();
        }
    }

    // Method Description:
    // - Process a resize event that was initiated by the user. This can either
    //   be due to the user resizing the window (causing the swapchain to
    //   resize) or due to the DPI changing (causing us to need to resize the
    //   buffer to match)
    // - Note that a DPI change will also trigger a font size change, and will
    //   call into here.
    // - The write lock should be held when calling this method, we might be
    //   changing the buffer size in _refreshSizeUnderLock.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void ControlCore::_refreshSizeUnderLock()
    {
        auto cx = gsl::narrow_cast<short>(_panelWidth * _compositionScale);
        auto cy = gsl::narrow_cast<short>(_panelHeight * _compositionScale);

        // Don't actually resize so small that a single character wouldn't fit
        // in either dimension. The buffer really doesn't like being size 0.
        cx = std::max(cx, _actualFont.GetSize().X);
        cy = std::max(cy, _actualFont.GetSize().Y);

        // Convert our new dimensions to characters
        const auto viewInPixels = Viewport::FromDimensions({ 0, 0 }, { cx, cy });
        const auto vp = _renderEngine->GetViewportInCharacters(viewInPixels);
        const auto currentVP = _terminal->GetViewport();

        _terminal->ClearSelection();

        // Tell the dx engine that our window is now the new size.
        THROW_IF_FAILED(_renderEngine->SetWindowSize({ cx, cy }));

        // Invalidate everything
        _renderer->TriggerRedrawAll();

        // If this function succeeds with S_FALSE, then the terminal didn't
        // actually change size. No need to notify the connection of this no-op.
        const auto hr = _terminal->UserResize({ vp.Width(), vp.Height() });
        if (SUCCEEDED(hr) && hr != S_FALSE)
        {
            _connection.Resize(vp.Height(), vp.Width());
        }
    }

    void ControlCore::SizeChanged(const double width,
                                  const double height)
    {
        // _refreshSizeUnderLock redraws the entire terminal.
        // Don't call it if we don't have to.
        if (_panelWidth == width && _panelHeight == height)
        {
            return;
        }

        _panelWidth = width;
        _panelHeight = height;

        auto lock = _terminal->LockForWriting();
        _refreshSizeUnderLock();
    }

    void ControlCore::ScaleChanged(const double scale)
    {
        if (!_renderEngine)
        {
            return;
        }

        // _refreshSizeUnderLock redraws the entire terminal.
        // Don't call it if we don't have to.
        if (_compositionScale == scale)
        {
            return;
        }

        _compositionScale = scale;

        auto lock = _terminal->LockForWriting();
        // _updateFont relies on the new _compositionScale set above
        _updateFont();
            _refreshSizeUnderLock();
        }

    void ControlCore::SetSelectionAnchor(const til::point& position)
    {
        auto lock = _terminal->LockForWriting();
        _terminal->SetSelectionAnchor(position.to_win32_coord());
    }

    // Method Description:
    // - Sets selection's end position to match supplied cursor position, e.g. while mouse dragging.
    // Arguments:
    // - position: the point in terminal coordinates (in cells, not pixels)
    void ControlCore::SetEndSelectionPoint(const til::point& position)
    {
        if (!_terminal->IsSelectionActive())
        {
            return;
        }

        // Have to take the lock because the renderer will not draw correctly if
        // you move its endpoints while it is generating a frame.
        auto lock = _terminal->LockForWriting();

        til::point terminalPosition{
            std::clamp(position.x, 0, _terminal->GetViewport().Width() - 1),
            std::clamp(position.y, 0, _terminal->GetViewport().Height() - 1)
        };

        // save location (for rendering) + render
        _terminal->SetSelectionEnd(terminalPosition.to_win32_coord());
        _renderer->TriggerSelection();
    }

    // Called when the Terminal wants to set something to the clipboard, i.e.
    // when an OSC 52 is emitted.
    void ControlCore::_terminalCopyToClipboard(std::wstring_view wstr)
    {
        _CopyToClipboardHandlers(*this, winrt::make<implementation::CopyToClipboardEventArgs>(winrt::hstring{ wstr }));
    }

    // Method Description:
    // - Given a copy-able selection, get the selected text from the buffer and send it to the
    //     Windows Clipboard (CascadiaWin32:main.cpp).
    // - CopyOnSelect does NOT clear the selection
    // Arguments:
    // - singleLine: collapse all of the text to one line
    // - formats: which formats to copy (defined by action's CopyFormatting arg). nullptr
    //             if we should defer which formats are copied to the global setting
    bool ControlCore::CopySelectionToClipboard(bool singleLine,
                                               const Windows::Foundation::IReference<CopyFormat>& formats)
    {
        // no selection --> nothing to copy
        if (!_terminal->IsSelectionActive())
        {
            return false;
        }

        // extract text from buffer
        // RetrieveSelectedTextFromBuffer will lock while it's reading
        const auto bufferData = _terminal->RetrieveSelectedTextFromBuffer(singleLine);

        // convert text: vector<string> --> string
        std::wstring textData;
        for (const auto& text : bufferData.text)
        {
            textData += text;
        }

        const auto bgColor = _terminal->GetAttributeColors({}).second;

        // convert text to HTML format
        // GH#5347 - Don't provide a title for the generated HTML, as many
        // web applications will paste the title first, followed by the HTML
        // content, which is unexpected.
        const auto htmlData = formats == nullptr || WI_IsFlagSet(formats.Value(), CopyFormat::HTML) ?
                                  TextBuffer::GenHTML(bufferData,
                                                      _actualFont.GetUnscaledSize().Y,
                                                      _actualFont.GetFaceName(),
                                                      bgColor) :
                                  "";

        // convert to RTF format
        const auto rtfData = formats == nullptr || WI_IsFlagSet(formats.Value(), CopyFormat::RTF) ?
                                 TextBuffer::GenRTF(bufferData,
                                                    _actualFont.GetUnscaledSize().Y,
                                                    _actualFont.GetFaceName(),
                                                    bgColor) :
                                 "";

        if (!_settings->CopyOnSelect())
        {
            _terminal->ClearSelection();
            _renderer->TriggerSelection();
        }

        // send data up for clipboard
        _CopyToClipboardHandlers(*this,
                                 winrt::make<CopyToClipboardEventArgs>(winrt::hstring{ textData },
                                                                       winrt::to_hstring(htmlData),
                                                                       winrt::to_hstring(rtfData),
                                                                       formats));
        return true;
    }

    void ControlCore::SelectAll()
    {
        auto lock = _terminal->LockForWriting();
        _terminal->SelectAll();
        _renderer->TriggerSelection();
    }

    // Method Description:
    // - Pre-process text pasted (presumably from the clipboard)
    //   before sending it over the terminal's connection.
    void ControlCore::PasteText(const winrt::hstring& hstr)
    {
        _terminal->WritePastedText(hstr);
        _terminal->ClearSelection();
        _renderer->TriggerSelection();
        _terminal->TrySnapOnInput();
    }

    FontInfo ControlCore::GetFont() const
    {
        return _actualFont;
    }

    winrt::Windows::Foundation::Size ControlCore::FontSize() const noexcept
    {
        const auto fontSize = _actualFont.GetSize();
        return {
            ::base::saturated_cast<float>(fontSize.X),
            ::base::saturated_cast<float>(fontSize.Y)
        };
    }
    winrt::hstring ControlCore::FontFaceName() const noexcept
    {
        // This getter used to return _actualFont.GetFaceName(), however GetFaceName() returns a STL
        // string and we need to return a WinRT string. This would require an additional allocation.
        // This method is called 10/s by TSFInputControl at the time of writing.
        return _actualFontFaceName;
    }

    uint16_t ControlCore::FontWeight() const noexcept
    {
        return static_cast<uint16_t>(_actualFont.GetWeight());
    }

    til::size ControlCore::FontSizeInDips() const
    {
        const til::size fontSize{ _actualFont.GetSize() };
        return fontSize.scale(til::math::rounding, 1.0f / ::base::saturated_cast<float>(_compositionScale));
    }

    TerminalConnection::ConnectionState ControlCore::ConnectionState() const
    {
        return _connection ? _connection.State() : TerminalConnection::ConnectionState::Closed;
    }

    hstring ControlCore::Title()
    {
        return hstring{ _terminal->GetConsoleTitle() };
    }

    hstring ControlCore::WorkingDirectory() const
    {
        return hstring{ _terminal->GetWorkingDirectory() };
    }

    bool ControlCore::BracketedPasteEnabled() const noexcept
    {
        return _terminal->IsXtermBracketedPasteModeEnabled();
    }

    Windows::Foundation::IReference<winrt::Windows::UI::Color> ControlCore::TabColor() noexcept
    {
        auto coreColor = _terminal->GetTabColor();
        return coreColor.has_value() ? Windows::Foundation::IReference<winrt::Windows::UI::Color>(toWinUIColor(coreColor.value())) :
                                       nullptr;
    }

    til::color ControlCore::BackgroundColor() const
    {
        return _terminal->GetRenderSettings().GetColorAlias(ColorAlias::DefaultBackground);
    }

    // Method Description:
    // - Gets the internal taskbar state value
    // Return Value:
    // - The taskbar state of this control
    const size_t ControlCore::TaskbarState() const noexcept
    {
        return _terminal->GetTaskbarState();
    }

    // Method Description:
    // - Gets the internal taskbar progress value
    // Return Value:
    // - The taskbar progress of this control
    const size_t ControlCore::TaskbarProgress() const noexcept
    {
        return _terminal->GetTaskbarProgress();
    }

    int ControlCore::ScrollOffset()
    {
        return _terminal->GetScrollOffset();
    }

    // Function Description:
    // - Gets the height of the terminal in lines of text. This is just the
    //   height of the viewport.
    // Return Value:
    // - The height of the terminal in lines of text
    int ControlCore::ViewHeight() const
    {
        return _terminal->GetViewport().Height();
    }

    // Function Description:
    // - Gets the height of the terminal in lines of text. This includes the
    //   history AND the viewport.
    // Return Value:
    // - The height of the terminal in lines of text
    int ControlCore::BufferHeight() const
    {
        return _terminal->GetBufferHeight();
    }

    void ControlCore::_terminalWarningBell()
    {
        // Since this can only ever be triggered by output from the connection,
        // then the Terminal already has the write lock when calling this
        // callback.
        _WarningBellHandlers(*this, nullptr);
    }

    // Method Description:
    // - Called for the Terminal's TitleChanged callback. This will re-raise
    //   a new winrt TypedEvent that can be listened to.
    // - The listeners to this event will re-query the control for the current
    //   value of Title().
    // Arguments:
    // - wstr: the new title of this terminal.
    // Return Value:
    // - <none>
    void ControlCore::_terminalTitleChanged(std::wstring_view wstr)
    {
        // Since this can only ever be triggered by output from the connection,
        // then the Terminal already has the write lock when calling this
        // callback.
        _TitleChangedHandlers(*this, winrt::make<TitleChangedEventArgs>(winrt::hstring{ wstr }));
    }

    // Method Description:
    // - Called for the Terminal's TabColorChanged callback. This will re-raise
    //   a new winrt TypedEvent that can be listened to.
    // - The listeners to this event will re-query the control for the current
    //   value of TabColor().
    // Arguments:
    // - <unused>
    // Return Value:
    // - <none>
    void ControlCore::_terminalTabColorChanged(const std::optional<til::color> /*color*/)
    {
        // Raise a TabColorChanged event
        _TabColorChangedHandlers(*this, nullptr);
    }

    // Method Description:
    // - Update the position and size of the scrollbar to match the given
    //      viewport top, viewport height, and buffer size.
    //   Additionally fires a ScrollPositionChanged event for anyone who's
    //      registered an event handler for us.
    // Arguments:
    // - viewTop: the top of the visible viewport, in rows. 0 indicates the top
    //      of the buffer.
    // - viewHeight: the height of the viewport in rows.
    // - bufferSize: the length of the buffer, in rows
    void ControlCore::_terminalScrollPositionChanged(const int viewTop,
                                                     const int viewHeight,
                                                     const int bufferSize)
    {
        // Clear the regex pattern tree so the renderer does not try to render them while scrolling
        // We're **NOT** taking the lock here unlike _scrollbarChangeHandler because
        // we are already under lock (since this usually happens as a result of writing).
        // TODO GH#9617: refine locking around pattern tree
        _terminal->ClearPatternTree();

        // Start the throttled update of our scrollbar.
        auto update{ winrt::make<ScrollPositionChangedArgs>(viewTop,
                                                            viewHeight,
                                                            bufferSize) };
        if (!_inUnitTests)
        {
            _updateScrollBar->Run(update);
        }
        else
        {
            _ScrollPositionChangedHandlers(*this, update);
        }

        // Additionally, start the throttled update of where our links are.
        _updatePatternLocations->Run();
    }

    void ControlCore::_terminalCursorPositionChanged()
    {
        // When the buffer's cursor moves, start the throttled func to
        // eventually dispatch a CursorPositionChanged event.
        _tsfTryRedrawCanvas->Run();
    }

    void ControlCore::_terminalTaskbarProgressChanged()
    {
        _TaskbarProgressChangedHandlers(*this, nullptr);
    }

    void ControlCore::_terminalShowWindowChanged(bool showOrHide)
    {
        if (_initializedTerminal)
        {
            auto showWindow = winrt::make_self<implementation::ShowWindowArgs>(showOrHide);
            _ShowWindowChangedHandlers(*this, *showWindow);
        }
    }

    bool ControlCore::HasSelection() const
    {
        return _terminal->IsSelectionActive();
    }

    bool ControlCore::CopyOnSelect() const
    {
        return _settings->CopyOnSelect();
    }

    Windows::Foundation::Collections::IVector<winrt::hstring> ControlCore::SelectedText(bool trimTrailingWhitespace) const
    {
        // RetrieveSelectedTextFromBuffer will lock while it's reading
        const auto internalResult{ _terminal->RetrieveSelectedTextFromBuffer(trimTrailingWhitespace).text };

        auto result = winrt::single_threaded_vector<winrt::hstring>();

        for (const auto& row : internalResult)
        {
            result.Append(winrt::hstring{ row });
        }
        return result;
    }

    ::Microsoft::Console::Types::IUiaData* ControlCore::GetUiaData() const
    {
        return _terminal.get();
    }

    // Method Description:
    // - Search text in text buffer. This is triggered if the user click
    //   search button or press enter.
    // Arguments:
    // - text: the text to search
    // - goForward: boolean that represents if the current search direction is forward
    // - caseSensitive: boolean that represents if the current search is case sensitive
    // Return Value:
    // - <none>
    void ControlCore::Search(const winrt::hstring& text,
                             const bool goForward,
                             const bool caseSensitive)
    {
        if (text.size() == 0)
        {
            return;
        }

        const auto direction = goForward ?
                                                Search::Direction::Forward :
                                                Search::Direction::Backward;

        const auto sensitivity = caseSensitive ?
                                                    Search::Sensitivity::CaseSensitive :
                                                    Search::Sensitivity::CaseInsensitive;

        ::Search search(*GetUiaData(), text.c_str(), direction, sensitivity);
        auto lock = _terminal->LockForWriting();
        const auto foundMatch{ search.FindNext() };
        if (foundMatch)
        {
            _terminal->SetBlockSelection(false);
            search.Select();
            _renderer->TriggerSelection();
        }

        // Raise a FoundMatch event, which the control will use to notify
        // narrator if there was any results in the buffer
        auto foundResults = winrt::make_self<implementation::FoundResultsArgs>(foundMatch);
        _FoundMatchHandlers(*this, *foundResults);
    }

    // Method Description:
    // - Asynchronously close our connection. The Connection will likely wait
    //   until the attached process terminates before Close returns. If that's
    //   the case, we don't want to block the UI thread waiting on that process
    //   handle.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    winrt::fire_and_forget ControlCore::_asyncCloseConnection()
    {
        if (auto localConnection{ std::exchange(_connection, nullptr) })
        {
            // Close the connection on the background thread.
            co_await winrt::resume_background(); // ** DO NOT INTERACT WITH THE CONTROL CORE AFTER THIS LINE **

            // Here, the ControlCore very well might be gone.
            // _asyncCloseConnection is called on the dtor, so it's entirely
            // possible that the background thread is resuming after we've been
            // cleaned up.

            localConnection.Close();
            // connection is destroyed.
        }
    }

    void ControlCore::Close()
    {
        if (!_IsClosing())
        {
            _closing = true;

            // Stop accepting new output and state changes before we disconnect everything.
            _connection.TerminalOutput(_connectionOutputEventToken);
            _connectionStateChangedRevoker.revoke();

            // GH#1996 - Close the connection asynchronously on a background
            // thread.
            // Since TermControl::Close is only ever triggered by the UI, we
            // don't really care to wait for the connection to be completely
            // closed. We can just do it whenever.
            _asyncCloseConnection();
        }
    }

    uint64_t ControlCore::SwapChainHandle() const
    {
        // This is called by:
        // * TermControl::RenderEngineSwapChainChanged, who is only registered
        //   after Core::Initialize() is called.
        // * TermControl::_InitializeTerminal, after the call to Initialize, for
        //   _AttachDxgiSwapChainToXaml.
        // In both cases, we'll have a _renderEngine by then.
        return reinterpret_cast<uint64_t>(_renderEngine->GetSwapChainHandle());
    }

    void ControlCore::_rendererWarning(const HRESULT hr)
    {
        _RendererWarningHandlers(*this, winrt::make<RendererWarningArgs>(hr));
    }

    void ControlCore::_renderEngineSwapChainChanged()
    {
        _SwapChainChangedHandlers(*this, nullptr);
    }

    void ControlCore::_rendererBackgroundColorChanged()
    {
        _BackgroundColorChangedHandlers(*this, nullptr);
    }

    void ControlCore::BlinkAttributeTick()
    {
        auto lock = _terminal->LockForWriting();

        auto& renderSettings = _terminal->GetRenderSettings();
        renderSettings.ToggleBlinkRendition(*_renderer);
    }

    void ControlCore::BlinkCursor()
    {
        if (!_terminal->IsCursorBlinkingAllowed() &&
            _terminal->IsCursorVisible())
        {
            return;
        }
        // SetCursorOn will take the write lock for you.
        _terminal->SetCursorOn(!_terminal->IsCursorOn());
    }

    bool ControlCore::CursorOn() const
    {
        return _terminal->IsCursorOn();
    }

    void ControlCore::CursorOn(const bool isCursorOn)
    {
        _terminal->SetCursorOn(isCursorOn);
    }

    void ControlCore::ResumeRendering()
    {
        _renderer->ResetErrorStateAndResume();
    }

    bool ControlCore::IsVtMouseModeEnabled() const
    {
        return _terminal != nullptr && _terminal->IsTrackingMouseInput();
    }
    bool ControlCore::ShouldSendAlternateScroll(const unsigned int uiButton,
                                                const int32_t delta) const
    {
        return _terminal != nullptr && _terminal->ShouldSendAlternateScroll(uiButton, delta);
    }

    Core::Point ControlCore::CursorPosition() const
    {
        // If we haven't been initialized yet, then fake it.
        if (!_initializedTerminal)
        {
            return { 0, 0 };
        }

        auto lock = _terminal->LockForReading();
        return til::point{ _terminal->GetCursorPosition() }.to_core_point();
    }

    // This one's really pushing the boundary of what counts as "encapsulation".
    // It really belongs in the "Interactivity" layer, which doesn't yet exist.
    // There's so many accesses to the selection in the Core though, that I just
    // put this here. The Control shouldn't be futzing that much with the
    // selection itself.
    void ControlCore::LeftClickOnTerminal(const til::point terminalPosition,
                                          const int numberOfClicks,
                                          const bool altEnabled,
                                          const bool shiftEnabled,
                                          const bool isOnOriginalPosition,
                                          bool& selectionNeedsToBeCopied)
    {
        auto lock = _terminal->LockForWriting();
        // handle ALT key
        _terminal->SetBlockSelection(altEnabled);

        auto mode = ::Terminal::SelectionExpansion::Char;
        if (numberOfClicks == 1)
        {
            mode = ::Terminal::SelectionExpansion::Char;
        }
        else if (numberOfClicks == 2)
        {
            mode = ::Terminal::SelectionExpansion::Word;
        }
        else if (numberOfClicks == 3)
        {
            mode = ::Terminal::SelectionExpansion::Line;
        }

        // Update the selection appropriately

        // We reset the active selection if one of the conditions apply:
        // - shift is not held
        // - GH#9384: the position is the same as of the first click starting
        //   the selection (we need to reset selection on double-click or
        //   triple-click, so it captures the word or the line, rather than
        //   extending the selection)
        if (HasSelection() && (!shiftEnabled || isOnOriginalPosition))
        {
            // Reset the selection
            _terminal->ClearSelection();
            selectionNeedsToBeCopied = false; // there's no selection, so there's nothing to update
        }

        if (shiftEnabled && HasSelection())
        {
            // If shift is pressed and there is a selection we extend it using
            // the selection mode (expand the "end" selection point)
            _terminal->SetSelectionEnd(terminalPosition.to_win32_coord(), mode);
            selectionNeedsToBeCopied = true;
        }
        else if (mode != ::Terminal::SelectionExpansion::Char || shiftEnabled)
        {
            // If we are handling a double / triple-click or shift+single click
            // we establish selection using the selected mode
            // (expand both "start" and "end" selection points)
            _terminal->MultiClickSelection(terminalPosition.to_win32_coord(), mode);
            selectionNeedsToBeCopied = true;
        }

        _renderer->TriggerSelection();
    }

    void ControlCore::AttachUiaEngine(::Microsoft::Console::Render::IRenderEngine* const pEngine)
    {
        // _renderer will always exist since it's introduced in the ctor
        _renderer->AddRenderEngine(pEngine);
    }

    bool ControlCore::IsInReadOnlyMode() const
    {
        return _isReadOnly;
    }

    void ControlCore::ToggleReadOnlyMode()
    {
        _isReadOnly = !_isReadOnly;
    }

    void ControlCore::_raiseReadOnlyWarning()
    {
        auto noticeArgs = winrt::make<NoticeEventArgs>(NoticeLevel::Info, RS_(L"TermControlReadOnly"));
        _RaiseNoticeHandlers(*this, std::move(noticeArgs));
    }
    void ControlCore::_connectionOutputHandler(const hstring& hstr)
    {
        _terminal->Write(hstr);

        // Start the throttled update of where our hyperlinks are.
        _updatePatternLocations->Run();
    }

    // Method Description:
    // - Clear the contents of the buffer. The region cleared is given by
    //   clearType:
    //   * Screen: Clear only the contents of the visible viewport, leaving the
    //     cursor row at the top of the viewport.
    //   * Scrollback: Clear the contents of the scrollback.
    //   * All: Do both - clear the visible viewport and the scrollback, leaving
    //     only the cursor row at the top of the viewport.
    // Arguments:
    // - clearType: The type of clear to perform.
    // Return Value:
    // - <none>
    void ControlCore::ClearBuffer(Control::ClearBufferType clearType)
    {
        if (clearType == Control::ClearBufferType::Scrollback || clearType == Control::ClearBufferType::All)
        {
            _terminal->EraseScrollback();
        }

        if (clearType == Control::ClearBufferType::Screen || clearType == Control::ClearBufferType::All)
        {
            // Send a signal to conpty to clear the buffer.
            if (auto conpty{ _connection.try_as<TerminalConnection::ConptyConnection>() })
            {
                // ConPTY will emit sequences to sync up our buffer with its new
                // contents.
                conpty.ClearBuffer();
            }
        }
    }

    hstring ControlCore::ReadEntireBuffer() const
    {
        auto terminalLock = _terminal->LockForWriting();

        const auto& textBuffer = _terminal->GetTextBuffer();

        std::wstringstream ss;
        const auto lastRow = textBuffer.GetLastNonSpaceCharacter().Y;
        for (auto rowIndex = 0; rowIndex <= lastRow; rowIndex++)
        {
            const auto& row = textBuffer.GetRowByOffset(rowIndex);
            auto rowText = row.GetText();
            const auto strEnd = rowText.find_last_not_of(UNICODE_SPACE);
            if (strEnd != std::string::npos)
            {
                rowText.erase(strEnd + 1);
                ss << rowText;
            }

            if (!row.WasWrapForced())
            {
                ss << UNICODE_CARRIAGERETURN << UNICODE_LINEFEED;
            }
        }

        return hstring(ss.str());
    }

    // Helper to check if we're on Windows 11 or not. This is used to check if
    // we need to use acrylic to achieve transparency, because vintage opacity
    // doesn't work in islands on win10.
    // Remove when we can remove the rest of GH#11285
    bool ControlCore::IsVintageOpacityAvailable() noexcept
    {
        OSVERSIONINFOEXW osver{};
        osver.dwOSVersionInfoSize = sizeof(osver);
        osver.dwBuildNumber = 22000;

        DWORDLONG dwlConditionMask = 0;
        VER_SET_CONDITION(dwlConditionMask, VER_BUILDNUMBER, VER_GREATER_EQUAL);

        return VerifyVersionInfoW(&osver, VER_BUILDNUMBER, dwlConditionMask) != FALSE;
    }

    Core::Scheme ControlCore::ColorScheme() const noexcept
    {
        Core::Scheme s;

        // This part is definitely a hack.
        //
        // This function is usually used by the "Preview Color Scheme"
        // functionality in TerminalPage. If we've got an unfocused appearance,
        // then we've applied that appearance before this is even getting called
        // (because the command palette is open with focus on top of us). If we
        // return the _current_ colors now, we'll return out the _unfocused_
        // colors. If we do that, and the user dismisses the command palette,
        // then the scheme that will get restored is the _unfocused_ one, which
        // is not what we want.
        //
        // So if that's the case, then let's grab the colors from the focused
        // appearance as the scheme instead. We'll lose any current runtime
        // changes to the color table, but those were already blown away when we
        // switched to an unfocused appearance.
        //
        // IF WE DON'T HAVE AN UNFOCUSED APPEARANCE: then just ask the Terminal
        // for it's current color table. That way, we can restore those colors
        // back.
        if (HasUnfocusedAppearance())
        {
            s.Foreground = _settings->FocusedAppearance()->DefaultForeground();
            s.Background = _settings->FocusedAppearance()->DefaultBackground();

            s.CursorColor = _settings->FocusedAppearance()->CursorColor();

            s.Black = _settings->FocusedAppearance()->GetColorTableEntry(0);
            s.Red = _settings->FocusedAppearance()->GetColorTableEntry(1);
            s.Green = _settings->FocusedAppearance()->GetColorTableEntry(2);
            s.Yellow = _settings->FocusedAppearance()->GetColorTableEntry(3);
            s.Blue = _settings->FocusedAppearance()->GetColorTableEntry(4);
            s.Purple = _settings->FocusedAppearance()->GetColorTableEntry(5);
            s.Cyan = _settings->FocusedAppearance()->GetColorTableEntry(6);
            s.White = _settings->FocusedAppearance()->GetColorTableEntry(7);
            s.BrightBlack = _settings->FocusedAppearance()->GetColorTableEntry(8);
            s.BrightRed = _settings->FocusedAppearance()->GetColorTableEntry(9);
            s.BrightGreen = _settings->FocusedAppearance()->GetColorTableEntry(10);
            s.BrightYellow = _settings->FocusedAppearance()->GetColorTableEntry(11);
            s.BrightBlue = _settings->FocusedAppearance()->GetColorTableEntry(12);
            s.BrightPurple = _settings->FocusedAppearance()->GetColorTableEntry(13);
            s.BrightCyan = _settings->FocusedAppearance()->GetColorTableEntry(14);
            s.BrightWhite = _settings->FocusedAppearance()->GetColorTableEntry(15);
        }
        else
        {
            s = _terminal->GetColorScheme();
        }

        // This might be a tad bit of a hack. This event only gets called by set
        // color scheme / preview color scheme, and in that case, we know the
        // control _is_ focused.
        s.SelectionBackground = _settings->FocusedAppearance()->SelectionBackground();

        return s;
    }

    // Method Description:
    // - Apply the given color scheme to this control. We'll take the colors out
    //   of it and apply them to our focused appearance, and update the terminal
    //   buffer with the new color table.
    // - This is here to support the Set Color Scheme action, and the ability to
    //   preview schemes in the control.
    // Arguments:
    // - scheme: the collection of colors to apply.
    // Return Value:
    // - <none>
    void ControlCore::ColorScheme(const Core::Scheme& scheme)
    {
        auto l{ _terminal->LockForWriting() };

        _settings->FocusedAppearance()->DefaultForeground(scheme.Foreground);
        _settings->FocusedAppearance()->DefaultBackground(scheme.Background);
        _settings->FocusedAppearance()->CursorColor(scheme.CursorColor);
        _settings->FocusedAppearance()->SelectionBackground(scheme.SelectionBackground);

        _settings->FocusedAppearance()->SetColorTableEntry(0, scheme.Black);
        _settings->FocusedAppearance()->SetColorTableEntry(1, scheme.Red);
        _settings->FocusedAppearance()->SetColorTableEntry(2, scheme.Green);
        _settings->FocusedAppearance()->SetColorTableEntry(3, scheme.Yellow);
        _settings->FocusedAppearance()->SetColorTableEntry(4, scheme.Blue);
        _settings->FocusedAppearance()->SetColorTableEntry(5, scheme.Purple);
        _settings->FocusedAppearance()->SetColorTableEntry(6, scheme.Cyan);
        _settings->FocusedAppearance()->SetColorTableEntry(7, scheme.White);
        _settings->FocusedAppearance()->SetColorTableEntry(8, scheme.BrightBlack);
        _settings->FocusedAppearance()->SetColorTableEntry(9, scheme.BrightRed);
        _settings->FocusedAppearance()->SetColorTableEntry(10, scheme.BrightGreen);
        _settings->FocusedAppearance()->SetColorTableEntry(11, scheme.BrightYellow);
        _settings->FocusedAppearance()->SetColorTableEntry(12, scheme.BrightBlue);
        _settings->FocusedAppearance()->SetColorTableEntry(13, scheme.BrightPurple);
        _settings->FocusedAppearance()->SetColorTableEntry(14, scheme.BrightCyan);
        _settings->FocusedAppearance()->SetColorTableEntry(15, scheme.BrightWhite);

        _terminal->ApplyScheme(scheme);

        _renderEngine->SetSelectionBackground(til::color{ _settings->SelectionBackground() });

        _renderer->TriggerRedrawAll(true);
    }

    bool ControlCore::HasUnfocusedAppearance() const
    {
        return _settings->HasUnfocusedAppearance();
    }

    void ControlCore::AdjustOpacity(const double opacityAdjust, const bool relative)
    {
        if (relative)
        {
            AdjustOpacity(opacityAdjust);
        }
        else
        {
            _setOpacity(opacityAdjust);
        }
    }

    // Method Description:
    // - Notifies the attached PTY that the window has changed visibility state
    // - NOTE: Most VT commands are generated in `TerminalDispatch` and sent to this
    //         class as the target for transmission. But since this message isn't
    //         coming in via VT parsing (and rather from a window state transition)
    //         we generate and send it here.
    // Arguments:
    // - visible: True for visible; false for not visible.
    // Return Value:
    // - <none>
    void ControlCore::WindowVisibilityChanged(const bool showOrHide)
    {
        if (_initializedTerminal)
        {
            // show is true, hide is false
            if (auto conpty{ _connection.try_as<TerminalConnection::ConptyConnection>() })
            {
                conpty.ShowHide(showOrHide);
            }
        }
    }

    // Method Description:
    // - When the control gains focus, it needs to tell ConPTY about this.
    //   Usually, these sequences are reserved for applications that
    //   specifically request SET_FOCUS_EVENT_MOUSE, ?1004h. ConPTY uses this
    //   sequence REGARDLESS to communicate if the control was focused or not.
    // - Even if a client application disables this mode, the Terminal & conpty
    //   should always request this from the hosting terminal (and just ignore
    //   internally to ConPTY).
    // - Full support for this sequence is tracked in GH#11682.
    // - This is related to work done for GH#2988.
    void ControlCore::GotFocus()
    {
        _terminal->FocusChanged(true);
    }

    // See GotFocus.
    void ControlCore::LostFocus()
    {
        _terminal->FocusChanged(false);
    }

    bool ControlCore::_isBackgroundTransparent()
    {
        // If we're:
        // * Not fully opaque
        // * On an acrylic background (of any opacity)
        // * rendering on top of an image
        //
        // then the renderer should not render "default background" text with a
        // fully opaque background. Doing that would cover up our nice
        // transparency, or our acrylic, or our image.
        return Opacity() < 1.0f || UseAcrylic() || !_settings->BackgroundImage().empty() || _settings->UseBackgroundImageForWindow();
    }
}
