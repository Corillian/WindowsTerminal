// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "KeyChordSerialization.h"
#include "KeyChordSerialization.g.cpp"

#include <winrt/Windows.System.h>
#include <til/static_map.h>

using namespace winrt::Microsoft::Terminal::Control;
using namespace winrt::Microsoft::Terminal::Settings::Model::implementation;
using namespace Microsoft::Terminal::Settings::Model::JsonUtils;
using VirtualKeyModifiers = winrt::Windows::System::VirtualKeyModifiers;

constexpr std::wstring_view CTRL_KEY{ L"ctrl" };
constexpr std::wstring_view SHIFT_KEY{ L"shift" };
constexpr std::wstring_view ALT_KEY{ L"alt" };
constexpr std::wstring_view WIN_KEY{ L"win" };

// If you modify this list you should modify the
// KeyChordSegment description in profiles.schema.json.
#define VKEY_NAME_PAIRS(XX)                              \
    XX(VK_RETURN, L"enter")                              \
    XX(VK_TAB, L"tab")                                   \
    XX(VK_SPACE, L"space")                               \
    XX(VK_BACK, L"backspace")                            \
    XX(VK_APPS, L"menu", L"app")                         \
    XX(VK_INSERT, L"insert")                             \
    XX(VK_DELETE, L"delete")                             \
    XX(VK_HOME, L"home")                                 \
    XX(VK_END, L"end")                                   \
    XX(VK_NEXT, L"pgdn", L"pagedown")                    \
    XX(VK_PRIOR, L"pgup", L"pageup")                     \
    XX(VK_ESCAPE, L"esc", L"escape")                     \
    XX(VK_LEFT, L"left")                                 \
    XX(VK_RIGHT, L"right")                               \
    XX(VK_UP, L"up")                                     \
    XX(VK_DOWN, L"down")                                 \
    XX(VK_F1, L"f1")                                     \
    XX(VK_F2, L"f2")                                     \
    XX(VK_F3, L"f3")                                     \
    XX(VK_F4, L"f4")                                     \
    XX(VK_F5, L"f5")                                     \
    XX(VK_F6, L"f6")                                     \
    XX(VK_F7, L"f7")                                     \
    XX(VK_F8, L"f8")                                     \
    XX(VK_F9, L"f9")                                     \
    XX(VK_F10, L"f10")                                   \
    XX(VK_F11, L"f11")                                   \
    XX(VK_F12, L"f12")                                   \
    XX(VK_F13, L"f13")                                   \
    XX(VK_F14, L"f14")                                   \
    XX(VK_F15, L"f15")                                   \
    XX(VK_F16, L"f16")                                   \
    XX(VK_F17, L"f17")                                   \
    XX(VK_F18, L"f18")                                   \
    XX(VK_F19, L"f19")                                   \
    XX(VK_F20, L"f20")                                   \
    XX(VK_F21, L"f21")                                   \
    XX(VK_F22, L"f22")                                   \
    XX(VK_F23, L"f23")                                   \
    XX(VK_F24, L"f24")                                   \
    XX(VK_ADD, L"numpad_plus", L"numpad_add")            \
    XX(VK_SUBTRACT, L"numpad_minus", L"numpad_subtract") \
    XX(VK_MULTIPLY, L"numpad_multiply")                  \
    XX(VK_DIVIDE, L"numpad_divide")                      \
    XX(VK_DECIMAL, L"numpad_period", L"numpad_decimal")  \
    XX(VK_NUMPAD0, L"numpad0", L"numpad_0")              \
    XX(VK_NUMPAD1, L"numpad1", L"numpad_1")              \
    XX(VK_NUMPAD2, L"numpad2", L"numpad_2")              \
    XX(VK_NUMPAD3, L"numpad3", L"numpad_3")              \
    XX(VK_NUMPAD4, L"numpad4", L"numpad_4")              \
    XX(VK_NUMPAD5, L"numpad5", L"numpad_5")              \
    XX(VK_NUMPAD6, L"numpad6", L"numpad_6")              \
    XX(VK_NUMPAD7, L"numpad7", L"numpad_7")              \
    XX(VK_NUMPAD8, L"numpad8", L"numpad_8")              \
    XX(VK_NUMPAD9, L"numpad9", L"numpad_9")              \
    XX(VK_OEM_PLUS, L"plus") /* '+' any country */       \
    XX(VK_OEM_COMMA, L"comma") /* ',' any country */     \
    XX(VK_OEM_MINUS, L"minus") /* '-' any country */     \
    XX(VK_OEM_PERIOD, L"period") /* '.' any country */   \
    XX(VK_BROWSER_BACK, L"browser_back")                 \
    XX(VK_BROWSER_FORWARD, L"browser_forward")           \
    XX(VK_BROWSER_REFRESH, L"browser_refresh")           \
    XX(VK_BROWSER_STOP, L"browser_stop")                 \
    XX(VK_BROWSER_SEARCH, L"browser_search")             \
    XX(VK_BROWSER_FAVORITES, L"browser_favorites")       \
    XX(VK_BROWSER_HOME, L"browser_home")

constexpr std::wstring_view vkeyPrefix{ L"vk(" };
constexpr std::wstring_view scanCodePrefix{ L"sc(" };
constexpr std::wstring_view codeSuffix{ L")" };

// Parses a vk(nnn) or sc(nnn) key chord part.
// If the part doesn't contain either of these two this function returns 0.
// For invalid arguments we throw an exception.
static int32_t parseNumericCode(const std::wstring_view& str, const std::wstring_view& prefix, const std::wstring_view& suffix)
{
    if (!til::ends_with(str, suffix) || !til::starts_with(str, prefix))
    {
        return 0;
    }

    const auto value = til::from_wchars({ str.data() + prefix.size(), str.size() - prefix.size() - suffix.size() });
    if (value > 0 && value < 256)
    {
        return gsl::narrow_cast<int32_t>(value);
    }

    throw winrt::hresult_invalid_argument(L"Invalid numeric argument to vk() or sc()");
}

// Function Description:
// - Deserializes the given string into a new KeyChord instance. If this
//   fails to translate the string into a keychord, it will throw a
//   hresult_invalid_argument exception.
// - The string should fit the format "[ctrl+][alt+][shift+]<keyName>",
//   where each modifier is optional, and keyName is either one of the
//   names listed in the vkeyNamePairs vector above, or is one of 0-9a-zA-Z.
// Arguments:
// - hstr: the string to parse into a keychord.
// Return Value:
// - a newly constructed KeyChord
static KeyChord _fromString(std::wstring_view wstr)
{
    using nameToVkeyPair = std::pair<std::wstring_view, int32_t>;
    static const til::static_map nameToVkey{
    // The above VKEY_NAME_PAIRS macro contains a list of key-binding names for each virtual key.
    // This god-awful macro inverts VKEY_NAME_PAIRS and creates a static map of key-binding names to virtual keys.
    // clang-format off
#define GENERATOR_1(vkey, name1) nameToVkeyPair{ name1, vkey },
#define GENERATOR_2(vkey, name1, name2) nameToVkeyPair{ name1, vkey }, nameToVkeyPair{ name2, vkey },
#define GENERATOR_3(vkey, name1, name2, name3) nameToVkeyPair{ name1, vkey }, nameToVkeyPair{ name2, vkey }, nameToVkeyPair{ name3, vkey },
#define GENERATOR_N(vkey, name1, name2, name3, MACRO, ...) MACRO
#define GENERATOR(...) GENERATOR_N(__VA_ARGS__, GENERATOR_3, GENERATOR_2, GENERATOR_1)(__VA_ARGS__)
        //VKEY_NAME_PAIRS(GENERATOR)

        // NOTE: There appears to be a bug in the latest Microsoft C++ compiler with correctly generating the prior macros
        // so the following was copy pasted from a Clang preprocessed file. I guess the terminal team is using a different
        // version of the compiler?
        nameToVkeyPair{ L"enter", 0x0D }, nameToVkeyPair{ L"tab", 0x09 }, nameToVkeyPair{ L"space", 0x20 }, nameToVkeyPair{ L"backspace", 0x08 }, nameToVkeyPair{ L"menu", 0x5D }, nameToVkeyPair{ L"app", 0x5D }, nameToVkeyPair{ L"insert", 0x2D }, nameToVkeyPair{ L"delete", 0x2E }, nameToVkeyPair{ L"home", 0x24 }, nameToVkeyPair{ L"end", 0x23 }, nameToVkeyPair{ L"pgdn", 0x22 }, nameToVkeyPair{ L"pagedown", 0x22 }, nameToVkeyPair{ L"pgup", 0x21 }, nameToVkeyPair{ L"pageup", 0x21 }, nameToVkeyPair{ L"esc", 0x1B }, nameToVkeyPair{ L"escape", 0x1B }, nameToVkeyPair{ L"left", 0x25 }, nameToVkeyPair{ L"right", 0x27 }, nameToVkeyPair{ L"up", 0x26 }, nameToVkeyPair{ L"down", 0x28 }, nameToVkeyPair{ L"f1", 0x70 }, nameToVkeyPair{ L"f2", 0x71 }, nameToVkeyPair{ L"f3", 0x72 }, nameToVkeyPair{ L"f4", 0x73 }, nameToVkeyPair{ L"f5", 0x74 }, nameToVkeyPair{ L"f6", 0x75 }, nameToVkeyPair{ L"f7", 0x76 }, nameToVkeyPair{ L"f8", 0x77 }, nameToVkeyPair{ L"f9", 0x78 }, nameToVkeyPair{ L"f10", 0x79 }, nameToVkeyPair{ L"f11", 0x7A }, nameToVkeyPair{ L"f12", 0x7B }, nameToVkeyPair{ L"f13", 0x7C }, nameToVkeyPair{ L"f14", 0x7D }, nameToVkeyPair{ L"f15", 0x7E }, nameToVkeyPair{ L"f16", 0x7F }, nameToVkeyPair{ L"f17", 0x80 }, nameToVkeyPair{ L"f18", 0x81 }, nameToVkeyPair{ L"f19", 0x82 }, nameToVkeyPair{ L"f20", 0x83 }, nameToVkeyPair{ L"f21", 0x84 }, nameToVkeyPair{ L"f22", 0x85 }, nameToVkeyPair{ L"f23", 0x86 }, nameToVkeyPair{ L"f24", 0x87 }, nameToVkeyPair{ L"numpad_plus", 0x6B }, nameToVkeyPair{ L"numpad_add", 0x6B }, nameToVkeyPair{ L"numpad_minus", 0x6D }, nameToVkeyPair{ L"numpad_subtract", 0x6D }, nameToVkeyPair{ L"numpad_multiply", 0x6A }, nameToVkeyPair{ L"numpad_divide", 0x6F }, nameToVkeyPair{ L"numpad_period", 0x6E }, nameToVkeyPair{ L"numpad_decimal", 0x6E }, nameToVkeyPair{ L"numpad0", 0x60 }, nameToVkeyPair{ L"numpad_0", 0x60 }, nameToVkeyPair{ L"numpad1", 0x61 }, nameToVkeyPair{ L"numpad_1", 0x61 }, nameToVkeyPair{ L"numpad2", 0x62 }, nameToVkeyPair{ L"numpad_2", 0x62 }, nameToVkeyPair{ L"numpad3", 0x63 }, nameToVkeyPair{ L"numpad_3", 0x63 }, nameToVkeyPair{ L"numpad4", 0x64 }, nameToVkeyPair{ L"numpad_4", 0x64 }, nameToVkeyPair{ L"numpad5", 0x65 }, nameToVkeyPair{ L"numpad_5", 0x65 }, nameToVkeyPair{ L"numpad6", 0x66 }, nameToVkeyPair{ L"numpad_6", 0x66 }, nameToVkeyPair{ L"numpad7", 0x67 }, nameToVkeyPair{ L"numpad_7", 0x67 }, nameToVkeyPair{ L"numpad8", 0x68 }, nameToVkeyPair{ L"numpad_8", 0x68 }, nameToVkeyPair{ L"numpad9", 0x69 }, nameToVkeyPair{ L"numpad_9", 0x69 }, nameToVkeyPair{ L"plus", 0xBB }, nameToVkeyPair{ L"comma", 0xBC }, nameToVkeyPair{ L"minus", 0xBD }, nameToVkeyPair{ L"period", 0xBE }, nameToVkeyPair{ L"browser_back", 0xA6 }, nameToVkeyPair{ L"browser_forward", 0xA7 }, nameToVkeyPair{ L"browser_refresh", 0xA8 }, nameToVkeyPair{ L"browser_stop", 0xA9 }, nameToVkeyPair{ L"browser_search", 0xAA }, nameToVkeyPair{ L"browser_favorites", 0xAB }, nameToVkeyPair{ L"browser_home", 0xAC },
#undef GENERATOR_1
#undef GENERATOR_2
#undef GENERATOR_3
#undef GENERATOR_N
#undef GENERATOR
        // clang-format on
    };

    auto modifiers = VirtualKeyModifiers::None;
    auto vkey = 0;
    auto scanCode = 0;

    while (!wstr.empty())
    {
        const auto part = til::prefix_split(wstr, L"+");

        if (til::equals_insensitive_ascii(part, CTRL_KEY))
        {
            modifiers |= VirtualKeyModifiers::Control;
        }
        else if (til::equals_insensitive_ascii(part, ALT_KEY))
        {
            modifiers |= VirtualKeyModifiers::Menu;
        }
        else if (til::equals_insensitive_ascii(part, SHIFT_KEY))
        {
            modifiers |= VirtualKeyModifiers::Shift;
        }
        else if (til::equals_insensitive_ascii(part, WIN_KEY))
        {
            modifiers |= VirtualKeyModifiers::Windows;
        }
        else
        {
            if (vkey || scanCode)
            {
                throw winrt::hresult_invalid_argument(L"Key bindings like Ctrl+A+B are not valid");
            }

            // Characters 0-9, a-z, A-Z directly map to virtual keys.
            if (part.size() == 1)
            {
                const auto wch = til::toupper_ascii(part[0]);
                if ((wch >= L'0' && wch <= L'9') || (wch >= L'A' && wch <= L'Z'))
                {
                    vkey = static_cast<int32_t>(wch);
                    continue;
                }
            }

            // vk() allows a user to specify a virtual key code
            // and sc() allows them to specify a scan code manually.
            //
            // ctrl+vk(0x09) for instance is the same as ctrl+tab, while win+sc(41) specifies
            // a key binding which is (seemingly) always bound to the key below Esc.
            vkey = parseNumericCode(part, vkeyPrefix, codeSuffix);
            if (vkey)
            {
                continue;
            }
            scanCode = parseNumericCode(part, scanCodePrefix, codeSuffix);
            if (scanCode)
            {
                continue;
            }

            // nameToVkey contains a few more mappings like "F11".
            if (const auto it = nameToVkey.find(part); it != nameToVkey.end())
            {
                vkey = it->second;
                continue;
            }

            // If we haven't found a key, attempt a keyboard mapping
            if (part.size() == 1)
            {
                const auto oemVk = VkKeyScanW(part[0]);
                if (oemVk != -1)
                {
                    vkey = oemVk & 0xff;
                    const auto oemModifiers = oemVk >> 8;
                    // NOTE: WI_UpdateFlag _replaces_ a bit. This code _adds_ a bit.
                    WI_SetFlagIf(modifiers, VirtualKeyModifiers::Shift, WI_IsFlagSet(oemModifiers, 1U));
                    WI_SetFlagIf(modifiers, VirtualKeyModifiers::Control, WI_IsFlagSet(oemModifiers, 2U));
                    WI_SetFlagIf(modifiers, VirtualKeyModifiers::Menu, WI_IsFlagSet(oemModifiers, 4U));
                    continue;
                }
            }

            throw winrt::hresult_invalid_argument(L"Invalid key binding");
        }
    }

    return KeyChord{ static_cast<VkModifiers>(modifiers), vkey, scanCode };
}

// Function Description:
// - Serialize this keychord into a string representation.
// - The string will fit the format "[ctrl+][alt+][shift+]<keyName>",
//   where each modifier is optional, and keyName is either one of the
//   names listed in the vkeyNamePairs vector above, or is one of 0-9a-z.
// Return Value:
// - a string which is an equivalent serialization of this object.
static std::wstring _toString(const KeyChord& chord)
{
    using vkeyToNamePair = std::pair<int32_t, std::wstring_view>;
    static const til::static_map vkeyToName{
    // The above VKEY_NAME_PAIRS macro contains a list of key-binding strings for each virtual key.
    // This macro picks the first (most preferred) name and creates a static map of virtual keys to key-binding names.
#define GENERATOR(vkey, name1, ...) vkeyToNamePair{ vkey, name1 },
        VKEY_NAME_PAIRS(GENERATOR)
#undef GENERATOR
    };

    if (!chord)
    {
        return {};
    }

    const auto modifiers = chord.Modifiers();
    const auto vkey = chord.Vkey();
    const auto scanCode = chord.ScanCode();
    std::wstring buffer;

    // Add modifiers
    if (WI_IsFlagSet(modifiers, static_cast<VkModifiers>(VirtualKeyModifiers::Windows)))
    {
        buffer.append(WIN_KEY);
        buffer.push_back(L'+');
    }
    if (WI_IsFlagSet(modifiers, static_cast<VkModifiers>(VirtualKeyModifiers::Control)))
    {
        buffer.append(CTRL_KEY);
        buffer.push_back(L'+');
    }
    if (WI_IsFlagSet(modifiers, static_cast<VkModifiers>(VirtualKeyModifiers::Menu)))
    {
        buffer.append(ALT_KEY);
        buffer.push_back(L'+');
    }
    if (WI_IsFlagSet(modifiers, static_cast<VkModifiers>(VirtualKeyModifiers::Shift)))
    {
        buffer.append(SHIFT_KEY);
        buffer.push_back(L'+');
    }

    if (scanCode)
    {
        buffer.append(scanCodePrefix);
        buffer.append(std::to_wstring(scanCode));
        buffer.append(codeSuffix);
        return buffer;
    }

    // Quick lookup: ranges of vkeys that correlate directly to a key.
    if ((vkey >= L'0' && vkey <= L'9') || (vkey >= L'A' && vkey <= L'Z'))
    {
        buffer.push_back(til::tolower_ascii(gsl::narrow_cast<wchar_t>(vkey)));
        return buffer;
    }

    if (const auto it = vkeyToName.find(vkey); it != vkeyToName.end())
    {
        buffer.append(it->second);
        return buffer;
    }

    const auto mappedChar = MapVirtualKeyW(vkey, MAPVK_VK_TO_CHAR);
    if (mappedChar != 0)
    {
        buffer.push_back(gsl::narrow_cast<wchar_t>(mappedChar));
        return buffer;
    }

    if (vkey)
    {
        buffer.append(vkeyPrefix);
        buffer.append(std::to_wstring(vkey));
        buffer.append(codeSuffix);
        return buffer;
    }

    return {};
}

KeyChord KeyChordSerialization::FromString(const winrt::hstring& hstr)
{
    return _fromString(hstr);
}

winrt::hstring KeyChordSerialization::ToString(const KeyChord& chord)
{
    return hstring{ _toString(chord) };
}

KeyChord ConversionTrait<KeyChord>::FromJson(const Json::Value& json)
{
    try
    {
        std::string keyChordText;
        if (json.isString())
        {
            // "keys": "ctrl+c"
            keyChordText = json.asString();
        }
        else if (json.isArray() && json.size() == 1 && json[0].isString())
        {
            // "keys": [ "ctrl+c" ]
            keyChordText = json[0].asString();
        }
        else
        {
            throw winrt::hresult_invalid_argument{};
        }
        return _fromString(til::u8u16(keyChordText));
    }
    catch (...)
    {
        return nullptr;
    }
}

bool ConversionTrait<KeyChord>::CanConvert(const Json::Value& json)
{
    return json.isString() || (json.isArray() && json.size() == 1 && json[0].isString());
}

Json::Value ConversionTrait<KeyChord>::ToJson(const KeyChord& val)
{
    return til::u16u8(_toString(val));
}

std::string ConversionTrait<KeyChord>::TypeDescription() const
{
    return "key chord";
}
