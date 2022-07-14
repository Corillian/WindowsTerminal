// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "Terminal.hpp"
#include <DefaultSettings.h>

using namespace Microsoft::Terminal::Core;
using namespace Microsoft::Console::Types;
using namespace Microsoft::Console::Render;

Viewport Terminal::GetViewport() noexcept
{
    return _GetVisibleViewport();
}

COORD Terminal::GetTextBufferEndPosition() const noexcept
{
    // We use the end line of mutableViewport as the end
    // of the text buffer, it always moves with the written
    // text
    COORD endPosition{ _GetMutableViewport().Width() - 1, gsl::narrow<short>(ViewEndIndex()) };
    return endPosition;
}

const TextBuffer& Terminal::GetTextBuffer() const noexcept
{
    return _activeBuffer();
}

const FontInfo& Terminal::GetFontInfo() const noexcept
{
    return _fontInfo;
}

void Terminal::SetFontInfo(const FontInfo& fontInfo)
{
    _fontInfo = fontInfo;
}

COORD Terminal::GetCursorPosition() const noexcept
{
    const auto& cursor = _activeBuffer().GetCursor();
    return cursor.GetPosition();
}

bool Terminal::IsCursorVisible() const noexcept
{
    const auto& cursor = _activeBuffer().GetCursor();
    return cursor.IsVisible() && !cursor.IsPopupShown();
}

bool Terminal::IsCursorOn() const noexcept
{
    const auto& cursor = _activeBuffer().GetCursor();
    return cursor.IsOn();
}

ULONG Terminal::GetCursorPixelWidth() const noexcept
{
    return 1;
}

ULONG Terminal::GetCursorHeight() const noexcept
{
    return _activeBuffer().GetCursor().GetSize();
}

CursorType Terminal::GetCursorStyle() const noexcept
{
    return _activeBuffer().GetCursor().GetType();
}

bool Terminal::IsCursorDoubleWidth() const
{
    const auto position = _activeBuffer().GetCursor().GetPosition();
    TextBufferTextIterator it(TextBufferCellIterator(_activeBuffer(), position));
    return IsGlyphFullWidth(*it);
}

const std::vector<RenderOverlay> Terminal::GetOverlays() const noexcept
{
    return {};
}

const bool Terminal::IsGridLineDrawingAllowed() noexcept
{
    return true;
}

const std::wstring Microsoft::Terminal::Core::Terminal::GetHyperlinkUri(uint16_t id) const noexcept
{
    return _activeBuffer().GetHyperlinkUriFromId(id);
}

const std::wstring Microsoft::Terminal::Core::Terminal::GetHyperlinkCustomId(uint16_t id) const noexcept
{
    return _activeBuffer().GetCustomIdFromId(id);
}

// Method Description:
// - Gets the regex pattern ids of a location
// Arguments:
// - The location
// Return value:
// - The pattern IDs of the location
const std::vector<size_t> Terminal::GetPatternId(const COORD location) const noexcept
{
    // Look through our interval tree for this location
    const auto intervals = _patternIntervalTree.findOverlapping(til::point{ location.X + 1, location.Y }, til::point{ location });
    if (intervals.size() == 0)
    {
        return {};
    }
    else
    {
        std::vector<size_t> result{};
        for (const auto& interval : intervals)
        {
            result.emplace_back(interval.value);
        }
        return result;
    }
    return {};
}

std::pair<COLORREF, COLORREF> Terminal::GetAttributeColors(const TextAttribute& attr) const noexcept
{
    return _renderSettings.GetAttributeColors(attr);
}

std::vector<Microsoft::Console::Types::Viewport> Terminal::GetSelectionRects() noexcept
try
{
    std::vector<Viewport> result;

    for (const auto& lineRect : _GetSelectionRects())
    {
        result.emplace_back(Viewport::FromInclusive(lineRect));
    }

    return result;
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();
    return {};
}

void Terminal::SelectNewRegion(const COORD coordStart, const COORD coordEnd)
{
#pragma warning(push)
#pragma warning(disable : 26496) // cpp core checks wants these const, but they're decremented below.
    auto realCoordStart = coordStart;
    auto realCoordEnd = coordEnd;
#pragma warning(pop)

    auto notifyScrollChange = false;
    if (coordStart.Y < _VisibleStartIndex())
    {
        // recalculate the scrollOffset
        _scrollOffset = ViewStartIndex() - coordStart.Y;
        notifyScrollChange = true;
    }
    else if (coordEnd.Y > _VisibleEndIndex())
    {
        // recalculate the scrollOffset, note that if the found text is
        // beneath the current visible viewport, it may be within the
        // current mutableViewport and the scrollOffset will be smaller
        // than 0
        _scrollOffset = std::max(0, ViewStartIndex() - coordStart.Y);
        notifyScrollChange = true;
    }

    if (notifyScrollChange)
    {
        _activeBuffer().TriggerScroll();
        _NotifyScrollEvent();
    }

    realCoordStart.Y -= gsl::narrow<short>(_VisibleStartIndex());
    realCoordEnd.Y -= gsl::narrow<short>(_VisibleStartIndex());

    SetSelectionAnchor(realCoordStart);
    SetSelectionEnd(realCoordEnd, SelectionExpansion::Char);
}

const std::wstring_view Terminal::GetConsoleTitle() const noexcept
try
{
    if (_title.has_value())
    {
        return _title.value();
    }
    return _startingTitle;
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();
    return {};
}

// Method Description:
// - Lock the terminal for reading the contents of the buffer. Ensures that the
//      contents of the terminal won't be changed in the middle of a paint
//      operation.
//   Callers should make sure to also call Terminal::UnlockConsole once
//      they're done with any querying they need to do.
void Terminal::LockConsole() noexcept
{
    _readWriteLock.lock();
#ifndef NDEBUG
    _lastLocker = GetCurrentThreadId();
#endif
}

// Method Description:
// - Unlocks the terminal after a call to Terminal::LockConsole.
void Terminal::UnlockConsole() noexcept
{
    _readWriteLock.unlock();
}

const bool Terminal::IsUiaDataInitialized() const noexcept
{
    // GH#11135: Windows Terminal needs to create and return an automation peer
    // when a screen reader requests it. However, the terminal might not be fully
    // initialized yet. So we use this to check if any crucial components of
    // UiaData are not yet initialized.
    return !!_mainBuffer;
}
