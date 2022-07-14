// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "CascadiaSettings.h"

#include <LibraryResources.h>
#include <fmt/chrono.h>
#include <shlobj.h>
#include <til/latch.h>

#include "AzureCloudShellGenerator.h"
#include "PowershellCoreProfileGenerator.h"
#include "VisualStudioGenerator.h"
#include "WslDistroGenerator.h"

// The following files are generated at build time into the "Generated Files" directory.
// defaults(-universal).h is a file containing the default json settings in a std::string_view.
#include "defaults.h"
#include "defaults-universal.h"
// userDefault.h is like the above, but with a default template for the user's settings.json.
#include <LegacyProfileGeneratorNamespaces.h>

#include "userDefaults.h"

#include "ApplicationState.h"
#include "DefaultTerminal.h"
#include "FileUtils.h"

using namespace winrt::Windows::Foundation::Collections;
//using namespace winrt::Windows::ApplicationModel::AppExtensions;
using namespace winrt::Microsoft::Terminal::Settings;
using namespace winrt::Microsoft::Terminal::Settings::Model::implementation;

static constexpr std::wstring_view SettingsFilename{ L"settings.json" };
static constexpr std::wstring_view DefaultsFilename{ L"defaults.json" };

static constexpr std::string_view ProfilesKey{ "profiles" };
static constexpr std::string_view DefaultSettingsKey{ "defaults" };
static constexpr std::string_view ProfilesListKey{ "list" };
static constexpr std::string_view SchemesKey{ "schemes" };

static constexpr std::wstring_view jsonExtension{ L".json" };
static constexpr std::wstring_view FragmentsSubDirectory{ L"\\Fragments" };
static constexpr std::wstring_view FragmentsPath{ L"\\Microsoft\\Windows Terminal\\Fragments" };

static constexpr std::wstring_view AppExtensionHostName{ L"com.microsoft.windows.terminal.settings" };

// make sure this matches defaults.json.
static constexpr winrt::guid DEFAULT_WINDOWS_POWERSHELL_GUID{ 0x61c54bbd, 0xc2c6, 0x5271, { 0x96, 0xe7, 0x00, 0x9a, 0x87, 0xff, 0x44, 0xbf } };
static constexpr winrt::guid DEFAULT_COMMAND_PROMPT_GUID{ 0x0caa0dad, 0x35be, 0x5f56, { 0xa8, 0xff, 0xaf, 0xce, 0xee, 0xaa, 0x61, 0x01 } };

// Function Description:
// - Extracting the value from an async task (like talking to the app catalog) when we are on the
//   UI thread causes C++/WinRT to complain quite loudly (and halt execution!)
//   This templated function extracts the result from a task with chicanery.
template<typename TTask>
static auto extractValueFromTaskWithoutMainThreadAwait(TTask&& task) -> decltype(task.get())
{
    std::optional<decltype(task.get())> finalVal;
    til::latch latch{ 1 };

    const auto _ = [&]() -> winrt::fire_and_forget {
        co_await winrt::resume_background();
        finalVal.emplace(co_await task);
        latch.count_down();
    }();

    latch.wait();
    return finalVal.value();
}

// Concatenates the two given strings (!) and returns them as a path.
// You better make sure there's a path separator at the end of lhs or at the start of rhs.
static std::filesystem::path buildPath(const std::wstring_view& lhs, const std::wstring_view& rhs)
{
    std::wstring buffer;
    buffer.reserve(lhs.size() + rhs.size());
    buffer.append(lhs);
    buffer.append(rhs);
    return { std::move(buffer) };
}

void ParsedSettings::clear()
{
    globals = {};
    baseLayerProfile = {};
    profiles.clear();
    profilesByGuid.clear();
}

// This is a convenience method used by the CascadiaSettings constructor.
// It runs some basic settings layering without relying on external programs or files.
// This makes it suitable for most unit tests.
SettingsLoader SettingsLoader::Default(const std::string_view& userJSON, const std::string_view& inboxJSON)
{
    SettingsLoader loader{ userJSON, inboxJSON };
    loader.MergeInboxIntoUserSettings();
    loader.FinalizeLayering();
    return loader;
}

// The SettingsLoader class is an internal implementation detail of CascadiaSettings.
// Member methods aren't safe against misuse and you need to ensure to call them in a specific order.
// See CascadiaSettings::LoadAll() for a specific usage example.
//
// This constructor only handles parsing the two given JSON strings.
// At a minimum you should do at least everything that SettingsLoader::Default does.
SettingsLoader::SettingsLoader(const std::string_view& userJSON, const std::string_view& inboxJSON)
{
    _parse(OriginTag::InBox, {}, inboxJSON, inboxSettings);

    try
    {
        _parse(OriginTag::User, {}, userJSON, userSettings);
    }
    catch (const JsonUtils::DeserializationError& e)
    {
        _rethrowSerializationExceptionWithLocationInfo(e, userJSON);
    }

    if (const auto sources = userSettings.globals->DisabledProfileSources())
    {
        _ignoredNamespaces.reserve(sources.Size());
        for (const auto& id : sources)
        {
            _ignoredNamespaces.emplace(id);
        }
    }

    // See member description of _userProfileCount.
    _userProfileCount = userSettings.profiles.size();
}

// Generate dynamic profiles and add them to the list of "inbox" profiles
// (meaning profiles specified by the application rather by the user).
void SettingsLoader::GenerateProfiles()
{
    _executeGenerator(PowershellCoreProfileGenerator{});
    _executeGenerator(WslDistroGenerator{});
    _executeGenerator(AzureCloudShellGenerator{});
    _executeGenerator(VisualStudioGenerator{});
}

// A new settings.json gets a special treatment:
// 1. The default profile is a PowerShell 7+ one, if one was generated,
//    and falls back to the standard PowerShell 5 profile otherwise.
// 2. cmd.exe gets a localized name.
void SettingsLoader::ApplyRuntimeInitialSettings()
{
    // 1.
    {
        const auto preferredPowershellProfile = PowershellCoreProfileGenerator::GetPreferredPowershellProfileName();
        auto guid = DEFAULT_WINDOWS_POWERSHELL_GUID;

        for (const auto& profile : inboxSettings.profiles)
        {
            if (profile->Name() == preferredPowershellProfile)
            {
                guid = profile->Guid();
                break;
            }
        }

        userSettings.globals->DefaultProfile(guid);
    }

    // 2.
    {
        for (const auto& profile : userSettings.profiles)
        {
            if (profile->Guid() == DEFAULT_COMMAND_PROMPT_GUID)
            {
                profile->Name(RS_(L"CommandPromptDisplayName"));
                break;
            }
        }
    }
}

// Adds profiles from .inboxSettings as parents of matching profiles in .userSettings.
// That way the user profiles will get appropriate defaults from the generators (like icons and such).
// If a matching profile doesn't exist yet in .userSettings, one will be created.
void SettingsLoader::MergeInboxIntoUserSettings()
{
    for (const auto& profile : inboxSettings.profiles)
    {
        _addUserProfileParent(profile);
    }
}

// Searches AppData/ProgramData and app extension directories for settings JSON files.
// If such JSON files are found, they're read and their contents added to .userSettings.
//
// Of course it would be more elegant to add fragments to .inboxSettings first and then have MergeInboxIntoUserSettings
// merge them. Unfortunately however the "updates" key in fragment profiles make this impossible:
// The targeted profile might be one that got created as part of SettingsLoader::MergeInboxIntoUserSettings.
// Additionally the GUID in "updates" will conflict with existing GUIDs in .inboxSettings.
void SettingsLoader::FindFragmentsAndMergeIntoUserSettings()
{
    ParsedSettings fragmentSettings;

    const auto parseAndLayerFragmentFiles = [&](const std::filesystem::path& path, const winrt::hstring& source) {
        for (const auto& fragmentExt : std::filesystem::directory_iterator{ path })
        {
            if (fragmentExt.path().extension() == jsonExtension)
            {
                try
                {
                    const auto content = ReadUTF8File(fragmentExt.path());
                    _parseFragment(source, content, fragmentSettings);
                }
                CATCH_LOG();
            }
        }
    };

    for (const auto& rfid : std::array{ FOLDERID_LocalAppData, FOLDERID_ProgramData })
    {
        wil::unique_cotaskmem_string folder;
        THROW_IF_FAILED(SHGetKnownFolderPath(rfid, 0, nullptr, &folder));

        const auto fragmentPath = buildPath(folder.get(), FragmentsPath);

        if (std::filesystem::is_directory(fragmentPath))
        {
            for (const auto& fragmentExtFolder : std::filesystem::directory_iterator{ fragmentPath })
            {
                const auto filename = fragmentExtFolder.path().filename();
                const auto& source = filename.native();

                if (!_ignoredNamespaces.count(std::wstring_view{ source }) && fragmentExtFolder.is_directory())
                {
                    parseAndLayerFragmentFiles(fragmentExtFolder.path(), winrt::hstring{ source });
                }
            }
        }
    }

    // Search through app extensions.
    // Gets the catalog of extensions with the name "com.microsoft.windows.terminal.settings".
    //
    // GH#12305: Open() can throw an 0x80070490 "Element not found.".
    // It's unclear to me under which circumstances this happens as no one on the team
    // was able to reproduce the user's issue, even if the application was run unpackaged.
    // The error originates from `CallerIdentity::GetCallingProcessAppId` which returns E_NOT_SET.
    // A comment can be found, reading:
    // > Gets the "strong" AppId from the process token. This works for UWAs and Centennial apps,
    // > strongly named processes where the AppId is stored securely in the process token. [...]
    // > E_NOT_SET is returned for processes without strong AppIds.
    /*IVectorView<AppExtension> extensions;
    try
    {
        const auto catalog = AppExtensionCatalog::Open(AppExtensionHostName);
        extensions = extractValueFromTaskWithoutMainThreadAwait(catalog.FindAllAsync());
    }
    CATCH_LOG();

    if (!extensions)
    {
        return;
    }

    for (const auto& ext : extensions)
    {
        const auto packageName = ext.Package().Id().FamilyName();
        if (_ignoredNamespaces.count(std::wstring_view{ packageName }))
        {
            continue;
        }

        // Likewise, getting the public folder from an extension is an async operation.
        auto foundFolder = extractValueFromTaskWithoutMainThreadAwait(ext.GetPublicFolderAsync());
        if (!foundFolder)
        {
            continue;
        }

        // the StorageFolder class has its own methods for obtaining the files within the folder
        // however, all those methods are Async methods
        // you may have noticed that we need to resort to clunky implementations for async operations
        // (they are in extractValueFromTaskWithoutMainThreadAwait)
        // so for now we will just take the folder path and access the files that way
        const auto path = buildPath(foundFolder.Path(), FragmentsSubDirectory);

        if (std::filesystem::is_directory(path))
        {
            parseAndLayerFragmentFiles(path, packageName);
        }
    }*/
}

// See FindFragmentsAndMergeIntoUserSettings.
// This function does the same, but for a single given JSON blob and source
// and at the time of writing is used for unit tests only.
void SettingsLoader::MergeFragmentIntoUserSettings(const winrt::hstring& source, const std::string_view& content)
{
    ParsedSettings fragmentSettings;
    _parseFragment(source, content, fragmentSettings);
}

// Call this method before passing SettingsLoader to the CascadiaSettings constructor.
// It layers all remaining objects onto each other (those that aren't covered
// by MergeInboxIntoUserSettings/FindFragmentsAndMergeIntoUserSettings).
void SettingsLoader::FinalizeLayering()
{
    // Layer default globals -> user globals
    userSettings.globals->AddLeastImportantParent(inboxSettings.globals);
    userSettings.globals->_FinalizeInheritance();
    // Layer default profile defaults -> user profile defaults
    userSettings.baseLayerProfile->AddLeastImportantParent(inboxSettings.baseLayerProfile);
    userSettings.baseLayerProfile->_FinalizeInheritance();
    // Layer user profile defaults -> user profiles
    for (const auto& profile : userSettings.profiles)
    {
        profile->AddMostImportantParent(userSettings.baseLayerProfile);

        // This completes the parenting process that was started in _addUserProfileParent().
        profile->_FinalizeInheritance();
        if (profile->Origin() == OriginTag::None)
        {
            // If you add more fields here, make sure to do the same in
            // implementation::CreateChild().
            profile->Origin(OriginTag::User);
            profile->Name(profile->Name());
            profile->Hidden(profile->Hidden());
        }
    }
}

// Let's say a user doesn't know that they need to write `"hidden": true` in
// order to prevent a profile from showing up (and a settings UI doesn't exist).
// Naturally they would open settings.json and try to remove the profile object.
// This section of code recognizes if a profile was seen before and marks it as
// `"hidden": true` by default and thus ensures the behavior the user expects:
// Profiles won't show up again after they've been removed from settings.json.
//
// Returns true if something got changed and
// the settings need to be saved to disk.
bool SettingsLoader::DisableDeletedProfiles()
{
    const auto& state = winrt::get_self<ApplicationState>(ApplicationState::SharedInstance());
    auto generatedProfileIds = state->GeneratedProfiles();
    auto newGeneratedProfiles = false;

    for (const auto& profile : _getNonUserOriginProfiles())
    {
        if (generatedProfileIds.emplace(profile->Guid()).second)
        {
            newGeneratedProfiles = true;
        }
        else
        {
            profile->Deleted(true);
            profile->Hidden(true);
        }
    }

    if (newGeneratedProfiles)
    {
        state->GeneratedProfiles(generatedProfileIds);
    }

    return newGeneratedProfiles;
}

// Runs migrations and fixups on user settings.
// Returns true if something got changed and
// the settings need to be saved to disk.
bool SettingsLoader::FixupUserSettings()
{
    struct CommandlinePatch
    {
        winrt::guid guid;
        std::wstring_view before;
        std::wstring_view after;
    };

    static constexpr std::array commandlinePatches{
        CommandlinePatch{ DEFAULT_COMMAND_PROMPT_GUID, L"cmd.exe", L"%SystemRoot%\\System32\\cmd.exe" },
        CommandlinePatch{ DEFAULT_WINDOWS_POWERSHELL_GUID, L"powershell.exe", L"%SystemRoot%\\System32\\WindowsPowerShell\\v1.0\\powershell.exe" },
    };

    auto fixedUp = false;

    for (const auto& profile : userSettings.profiles)
    {
        if (!profile->HasCommandline())
        {
            continue;
        }

        for (const auto& patch : commandlinePatches)
        {
            if (profile->Guid() == patch.guid && til::equals_insensitive_ascii(profile->Commandline(), patch.before))
            {
                profile->ClearCommandline();

                // GH#12842:
                // With the commandline field on the user profile gone, it's actually unknown what
                // commandline it'll inherit, since a user profile can have multiple parents. We have to
                // make sure we restore the correct commandline in case we don't inherit the expected one.
                if (profile->Commandline() != patch.after)
                {
                    profile->Commandline(winrt::hstring{ patch.after });
                }

                fixedUp = true;
                break;
            }
        }
    }

    return fixedUp;
}

// Give a string of length N and a position of [0,N) this function returns
// the line/column within the string, similar to how text editors do it.
// Newlines are considered part of the current line (as per POSIX).
std::pair<size_t, size_t> SettingsLoader::_lineAndColumnFromPosition(const std::string_view& string, const size_t position)
{
    size_t line = 1;
    size_t column = 0;

    for (;;)
    {
        const auto p = string.find('\n', column);
        if (p >= position)
        {
            break;
        }

        column = p + 1;
        line++;
    }

    return { line, position - column + 1 };
}

// Formats a JSON exception for humans to read and throws that.
void SettingsLoader::_rethrowSerializationExceptionWithLocationInfo(const JsonUtils::DeserializationError& e, const std::string_view& settingsString)
{
    std::string jsonValueAsString;
    try
    {
        jsonValueAsString = e.jsonValue.asString();
        if (e.jsonValue.isString())
        {
            jsonValueAsString = fmt::format("\"{}\"", jsonValueAsString);
        }
    }
    catch (...)
    {
        jsonValueAsString = "array or object";
    }

    const auto [line, column] = _lineAndColumnFromPosition(settingsString, static_cast<size_t>(e.jsonValue.getOffsetStart()));

    fmt::memory_buffer msg;
    fmt::format_to(msg, "* Line {}, Column {}", line, column);
    if (e.key)
    {
        fmt::format_to(msg, " ({})", *e.key);
    }
    fmt::format_to(msg, "\n  Have: {}\n  Expected: {}\0", jsonValueAsString, e.expectedType);

    throw SettingsTypedDeserializationException{ msg.data() };
}

// Simply parses the given content to a Json::Value.
Json::Value SettingsLoader::_parseJSON(const std::string_view& content)
{
    Json::Value json;
    std::string errs;
    const std::unique_ptr<Json::CharReader> reader{ Json::CharReaderBuilder::CharReaderBuilder().newCharReader() };

    if (!reader->parse(content.data(), content.data() + content.size(), &json, &errs))
    {
        throw winrt::hresult_error(WEB_E_INVALID_JSON_STRING, winrt::to_hstring(errs));
    }

    return json;
}

// A helper method similar to Json::Value::operator[], but compatible with std::string_view.
const Json::Value& SettingsLoader::_getJSONValue(const Json::Value& json, const std::string_view& key) noexcept
{
    if (json.isObject())
    {
        if (const auto val = json.find(key.data(), key.data() + key.size()))
        {
            return *val;
        }
    }

    return Json::Value::nullSingleton();
}

// We treat userSettings.profiles as an append-only array and will
// append profiles into the userSettings as necessary in this function.
// _userProfileCount stores the number of profiles that were in userJSON during construction.
//
// Thus no matter how many profiles are added later on, the following condition holds true:
// The userSettings.profiles in the range [0, _userProfileCount) contain all profiles specified by the user.
// In turn all profiles in the range [_userProfileCount, ∞) contain newly generated/added profiles.
// gsl::make_span(userSettings.profiles).subspan(_userProfileCount) gets us the latter range.
gsl::span<const winrt::com_ptr<Profile>> SettingsLoader::_getNonUserOriginProfiles() const
{
    return gsl::make_span(userSettings.profiles).subspan(_userProfileCount);
}

// Parses the given JSON string ("content") and fills a ParsedSettings instance with it.
// This function is to be used for user settings files.
void SettingsLoader::_parse(const OriginTag origin, const winrt::hstring& source, const std::string_view& content, ParsedSettings& settings)
{
    const auto json = _parseJson(content);

    settings.clear();

    {
        settings.globals = GlobalAppSettings::FromJson(json.root);

        for (const auto& schemeJson : json.colorSchemes)
        {
            if (const auto scheme = ColorScheme::FromJson(schemeJson))
            {
                settings.globals->AddColorScheme(*scheme);
            }
        }
    }

    {
        settings.baseLayerProfile = Profile::FromJson(json.profileDefaults);
        // Remove the `guid` member from the default settings.
        // That will hyper-explode, so just don't let them do that.
        settings.baseLayerProfile->ClearGuid();
        settings.baseLayerProfile->Origin(OriginTag::ProfilesDefaults);
    }

    {
        const auto size = json.profilesList.size();
        settings.profiles.reserve(size);
        settings.profilesByGuid.reserve(size);

        for (const auto& profileJson : json.profilesList)
        {
            auto profile = _parseProfile(origin, source, profileJson);
            // GH#9962: Discard Guid-less, Name-less profiles.
            if (profile->HasGuid())
            {
                _appendProfile(std::move(profile), profile->Guid(), settings);
            }
        }
    }
}

// Just like _parse, but is to be used for fragment files, which don't support anything but color
// schemes and profiles. Additionally this function supports profiles which specify an "updates" key.
void SettingsLoader::_parseFragment(const winrt::hstring& source, const std::string_view& content, ParsedSettings& settings)
{
    const auto json = _parseJson(content);

    settings.clear();

    {
        settings.globals = winrt::make_self<GlobalAppSettings>();

        for (const auto& schemeJson : json.colorSchemes)
        {
            try
            {
                if (const auto scheme = ColorScheme::FromJson(schemeJson))
                {
                    settings.globals->AddColorScheme(*scheme);
                }
            }
            CATCH_LOG()
        }
    }

    {
        const auto size = json.profilesList.size();
        settings.profiles.reserve(size);
        settings.profilesByGuid.reserve(size);

        for (const auto& profileJson : json.profilesList)
        {
            try
            {
                auto profile = _parseProfile(OriginTag::Fragment, source, profileJson);
                // GH#9962: Discard Guid-less, Name-less profiles, but...
                // allow ones with an Updates field, as those are special for fragments.
                // We need to make sure to only call Guid() if HasGuid() is true,
                // as Guid() will dynamically generate a return value otherwise.
                const auto guid = profile->HasGuid() ? profile->Guid() : profile->Updates();
                if (guid != winrt::guid{})
                {
                    _appendProfile(std::move(profile), guid, settings);
                }
            }
            CATCH_LOG()
        }
    }

    for (const auto& fragmentProfile : settings.profiles)
    {
        if (const auto updates = fragmentProfile->Updates(); updates != winrt::guid{})
        {
            if (const auto it = userSettings.profilesByGuid.find(updates); it != userSettings.profilesByGuid.end())
            {
                it->second->AddMostImportantParent(fragmentProfile);
            }
        }
        else
        {
            _addUserProfileParent(fragmentProfile);
        }
    }

    for (const auto& kv : settings.globals->ColorSchemes())
    {
        userSettings.globals->AddColorScheme(kv.Value());
    }
}

SettingsLoader::JsonSettings SettingsLoader::_parseJson(const std::string_view& content)
{
    auto root = content.empty() ? Json::Value{ Json::ValueType::objectValue } : _parseJSON(content);
    const auto& colorSchemes = _getJSONValue(root, SchemesKey);
    const auto& profilesObject = _getJSONValue(root, ProfilesKey);
    const auto& profileDefaults = _getJSONValue(profilesObject, DefaultSettingsKey);
    const auto& profilesList = profilesObject.isArray() ? profilesObject : _getJSONValue(profilesObject, ProfilesListKey);
    return JsonSettings{ std::move(root), colorSchemes, profileDefaults, profilesList };
}

// Just a common helper function between _parse and _parseFragment.
// Parses a profile and ensures it has a Guid if possible.
winrt::com_ptr<Profile> SettingsLoader::_parseProfile(const OriginTag origin, const winrt::hstring& source, const Json::Value& profileJson)
{
    auto profile = Profile::FromJson(profileJson);
    profile->Origin(origin);

    // The Guid() generation below depends on the value of Source().
    // --> Provide one if we got one.
    if (!source.empty())
    {
        profile->Source(source);
    }

    // If none exists. the Guid() getter generates one from Name() and optionally Source().
    // We want to ensure that every profile has a GUID no matter what, not just to
    // cache the value, but also to make them consistently identifiable later on.
    if (!profile->HasGuid() && profile->HasName())
    {
        profile->Guid(profile->Guid());
    }

    return profile;
}

// Adds a profile to the ParsedSettings instance. Takes ownership of the profile.
// It ensures no duplicate GUIDs are added to the ParsedSettings instance.
void SettingsLoader::_appendProfile(winrt::com_ptr<Profile>&& profile, const winrt::guid& guid, ParsedSettings& settings)
{
    // FYI: The static_cast ensures we don't move the profile into
    // `profilesByGuid`, even though we still need it later for `profiles`.
    if (settings.profilesByGuid.emplace(guid, static_cast<const winrt::com_ptr<Profile>&>(profile)).second)
    {
        settings.profiles.emplace_back(profile);
    }
    else
    {
        duplicateProfile = true;
    }
}

// If the given ParsedSettings instance contains a profile with the given profile's GUID,
// the profile is added as a parent. Otherwise a new child profile is created.
void SettingsLoader::_addUserProfileParent(const winrt::com_ptr<implementation::Profile>& profile)
{
    if (const auto [it, inserted] = userSettings.profilesByGuid.emplace(profile->Guid(), nullptr); !inserted)
    {
        // If inserted is false, we got a matching user profile with identical GUID.
        // --> The generated profile is a parent of the existing user profile.
        it->second->AddLeastImportantParent(profile);
    }
    else
    {
        // If inserted is true, then this is a generated profile that doesn't exist
        // in the user's settings (which makes this branch somewhat unlikely).
        //
        // When a user modifies a profile they shouldn't modify the (static/constant)
        // inbox profile of course. That's why we need to create a child.
        // And since we previously added the (now) parent profile into profilesByGuid
        // we'll have to replace it->second with the (new) child profile.
        //
        // These additional things are required to complete a (user) profile:
        // * A call to _FinalizeInheritance()
        // * Every profile should at least have Origin(), Name() and Hidden() set
        // They're handled by SettingsLoader::FinalizeLayering() and detected by
        // the missing Origin(). Setting these fields as late as possible ensures
        // that we pick up the correct, inherited values of all of the child's parents.
        //
        // If you add more fields here, make sure to do the same in
        // implementation::CreateChild().
        auto child = winrt::make_self<Profile>();
        child->AddLeastImportantParent(profile);
        child->Guid(profile->Guid());

        // If profile is a dynamic/generated profile, a fragment's
        // Source() should have no effect on this user profile.
        if (profile->HasSource())
        {
            child->Source(profile->Source());
        }

        it->second = child;
        userSettings.profiles.emplace_back(std::move(child));
    }
}

// As the name implies it executes a generator.
// Generated profiles are added to .inboxSettings. Used by GenerateProfiles().
void SettingsLoader::_executeGenerator(const IDynamicProfileGenerator& generator)
{
    const auto generatorNamespace = generator.GetNamespace();
    if (_ignoredNamespaces.count(generatorNamespace))
    {
        return;
    }

    const auto previousSize = inboxSettings.profiles.size();

    try
    {
        generator.GenerateProfiles(inboxSettings.profiles);
    }
    CATCH_LOG_MSG("Dynamic Profile Namespace: \"%.*s\"", gsl::narrow<int>(generatorNamespace.size()), generatorNamespace.data())

    // If the generator produced some profiles we're going to give them default attributes.
    // By setting the Origin/Source/etc. here, we deduplicate some code and ensure they aren't missing accidentally.
    if (inboxSettings.profiles.size() > previousSize)
    {
        const winrt::hstring source{ generatorNamespace };

        for (const auto& profile : gsl::span(inboxSettings.profiles).subspan(previousSize))
        {
            profile->Origin(OriginTag::Generated);
            profile->Source(source);
        }
    }
}

// Method Description:
// - Creates a CascadiaSettings from whatever's saved on disk, or instantiates
//      a new one with the default values. If we're running as a packaged app,
//      it will load the settings from our packaged localappdata. If we're
//      running as an unpackaged application, it will read it from the path
//      we've set under localappdata.
// - Loads both the settings from the defaults.json and the user's settings.json
// - Also runs and dynamic profile generators. If any of those generators create
//   new profiles, we'll write the user settings back to the file, with the new
//   profiles inserted into their list of profiles.
// Return Value:
// - a unique_ptr containing a new CascadiaSettings object.
Model::CascadiaSettings CascadiaSettings::LoadAll()
try
{
    const auto settingsString = ReadUTF8FileIfExists(_settingsPath()).value_or(std::string{});
    const auto firstTimeSetup = settingsString.empty();

    // GH#11119: If we find that the settings file doesn't exist, or is empty,
    // then let's quick delete the state file as well. If the user does have a
    // state file, and not a settings, then they probably tried to reset their
    // settings. It might have data in it that was only relevant for a previous
    // iteration of the settings file. If we don't, we'll load the old state and
    // ignore all dynamic profiles (for example)!
    if (firstTimeSetup)
    {
        ApplicationState::SharedInstance().Reset();
    }

    const auto settingsStringView = firstTimeSetup ? UserSettingsJson : settingsString;
    auto mustWriteToDisk = firstTimeSetup;

    SettingsLoader loader{ settingsStringView, DefaultJson };

    // Generate dynamic profiles and add them as parents of user profiles.
    // That way the user profiles will get appropriate defaults from the generators (like icons and such).
    loader.GenerateProfiles();

    // ApplyRuntimeInitialSettings depends on generated profiles.
    // --> ApplyRuntimeInitialSettings must be called after GenerateProfiles.
    if (firstTimeSetup)
    {
        loader.ApplyRuntimeInitialSettings();
    }

    loader.MergeInboxIntoUserSettings();
    // Fragments might reference user profiles created by a generator.
    // --> FindFragmentsAndMergeIntoUserSettings must be called after MergeInboxIntoUserSettings.
    loader.FindFragmentsAndMergeIntoUserSettings();
    loader.FinalizeLayering();

    // DisableDeletedProfiles returns true whenever we encountered any new generated/dynamic profiles.
    // Similarly FixupUserSettings returns true, when it encountered settings that were patched up.
    mustWriteToDisk |= loader.DisableDeletedProfiles();
    mustWriteToDisk |= loader.FixupUserSettings();

    // If this throws, the app will catch it and use the default settings.
    const auto settings = winrt::make_self<CascadiaSettings>(std::move(loader));

    // If we created the file, or found new dynamic profiles, write the user
    // settings string back to the file.
    if (mustWriteToDisk)
    {
        try
        {
            settings->WriteSettingsToDisk();
        }
        catch (...)
        {
            LOG_CAUGHT_EXCEPTION();
            settings->_warnings.Append(SettingsLoadWarnings::FailedToWriteToSettings);
        }
    }

    return *settings;
}
catch (const SettingsException& ex)
{
    const auto settings{ winrt::make_self<CascadiaSettings>() };
    settings->_loadError = ex.Error();
    return *settings;
}
catch (const SettingsTypedDeserializationException& e)
{
    const auto settings{ winrt::make_self<CascadiaSettings>() };
    settings->_deserializationErrorMessage = til::u8u16(e.what());
    return *settings;
}

// Function Description:
// - Loads a batch of settings curated for the Universal variant of the terminal app
// Arguments:
// - <none>
// Return Value:
// - a unique_ptr to a CascadiaSettings with the connection types and settings for Universal terminal
Model::CascadiaSettings CascadiaSettings::LoadUniversal()
{
    return *winrt::make_self<CascadiaSettings>(std::string_view{}, DefaultUniversalJson);
}

// Function Description:
// - Creates a new CascadiaSettings object initialized with settings from the
//   hardcoded defaults.json.
// Arguments:
// - <none>
// Return Value:
// - a unique_ptr to a CascadiaSettings with the settings from defaults.json
Model::CascadiaSettings CascadiaSettings::LoadDefaults()
{
    return *winrt::make_self<CascadiaSettings>(std::string_view{}, DefaultJson);
}

CascadiaSettings::CascadiaSettings(const winrt::hstring& userJSON, const winrt::hstring& inboxJSON) :
    CascadiaSettings{ SettingsLoader::Default(til::u16u8(userJSON), til::u16u8(inboxJSON)) }
{
}

CascadiaSettings::CascadiaSettings(const std::string_view& userJSON, const std::string_view& inboxJSON) :
    CascadiaSettings{ SettingsLoader::Default(userJSON, inboxJSON) }
{
}

CascadiaSettings::CascadiaSettings(SettingsLoader&& loader) :
    // The CascadiaSettings class declaration initializes these fields by default,
    // but we're going to set these fields in our constructor later on anyways.
    _globals{},
    _baseLayerProfile{},
    _allProfiles{},
    _activeProfiles{},
    _warnings{}
{
    std::vector<Model::Profile> allProfiles;
    std::vector<Model::Profile> activeProfiles;
    std::vector<Model::SettingsLoadWarnings> warnings;

    allProfiles.reserve(loader.userSettings.profiles.size());
    activeProfiles.reserve(loader.userSettings.profiles.size());

    for (const auto& profile : loader.userSettings.profiles)
    {
        // If a generator stops producing a certain profile (e.g. WSL or PowerShell were removed) or
        // a profile from a fragment doesn't exist anymore, we should also stop including the
        // matching user's profile in _allProfiles (since they aren't functional anyways).
        //
        // A user profile has a valid, dynamic parent if it has a parent with identical source.
        if (const auto source = profile->Source(); !source.empty())
        {
            const auto& parents = profile->Parents();
            if (std::none_of(parents.begin(), parents.end(), [&](const auto& parent) { return parent->Source() == source; }))
            {
                continue;
            }
        }

        allProfiles.emplace_back(*profile);
        if (!profile->Hidden())
        {
            activeProfiles.emplace_back(*profile);
        }
    }

    if (allProfiles.empty())
    {
        throw SettingsException(SettingsLoadErrors::NoProfiles);
    }
    if (activeProfiles.empty())
    {
        throw SettingsException(SettingsLoadErrors::AllProfilesHidden);
    }

    if (loader.duplicateProfile)
    {
        warnings.emplace_back(Model::SettingsLoadWarnings::DuplicateProfile);
    }

    // SettingsLoader and ParsedSettings are supposed to always
    // create these two members. We don't want null-pointer exceptions.
    assert(loader.userSettings.globals != nullptr);
    assert(loader.userSettings.baseLayerProfile != nullptr);

    _globals = loader.userSettings.globals;
    _baseLayerProfile = loader.userSettings.baseLayerProfile;
    _allProfiles = winrt::single_threaded_observable_vector(std::move(allProfiles));
    _activeProfiles = winrt::single_threaded_observable_vector(std::move(activeProfiles));
    _warnings = winrt::single_threaded_vector(std::move(warnings));

    _resolveDefaultProfile();
    _validateSettings();
}

// Method Description:
// - Returns the path of the settings.json file.
// Arguments:
// - <none>
// Return Value:
// - Returns a path in 80% of cases. I measured!
const std::filesystem::path& CascadiaSettings::_settingsPath()
{
    static const auto path = GetBaseSettingsPath() / SettingsFilename;
    return path;
}

// function Description:
// - Returns the full path to the settings file, either within the application
//   package, or in its unpackaged location. This path is under the "Local
//   AppData" folder, so it _doesn't_ roam to other machines.
// - If the application is unpackaged,
//   the file will end up under e.g. C:\Users\admin\AppData\Local\Microsoft\Windows Terminal\settings.json
// Arguments:
// - <none>
// Return Value:
// - the full path to the settings file
winrt::hstring CascadiaSettings::SettingsPath()
{
    return winrt::hstring{ _settingsPath().native() };
}

winrt::hstring CascadiaSettings::DefaultSettingsPath()
{
    // Both of these posts suggest getting the path to the exe, then removing
    // the exe's name to get the package root:
    // * https://blogs.msdn.microsoft.com/appconsult/2017/06/23/accessing-to-the-files-in-the-installation-folder-in-a-desktop-bridge-application/
    // * https://blogs.msdn.microsoft.com/appconsult/2017/03/06/handling-data-in-a-converted-desktop-app-with-the-desktop-bridge/
    //
    // This would break if we ever moved our exe out of the package root.
    // HOWEVER, if we try to look for a defaults.json that's simply in the same
    // directory as the exe, that will work for unpackaged scenarios as well. So
    // let's try that.

    const auto exePathString = wil::GetModuleFileNameW<std::wstring>(nullptr);

    std::filesystem::path path{ exePathString };
    path.replace_filename(DefaultsFilename);

    return winrt::hstring{ path.native() };
}

// Method Description:
// - Write the current state of CascadiaSettings to our settings file
// - Create a backup file with the current contents, if one does not exist
// - Persists the default terminal handler choice to the registry
// Arguments:
// - <none>
// Return Value:
// - <none>
void CascadiaSettings::WriteSettingsToDisk() const
{
    const auto settingsPath = _settingsPath();

    // write current settings to current settings file
    Json::StreamWriterBuilder wbuilder;
    wbuilder.settings_["indentation"] = "    ";
    wbuilder.settings_["enableYAMLCompatibility"] = true; // suppress spaces around colons

    const auto styledString{ Json::writeString(wbuilder, ToJson()) };
    WriteUTF8FileAtomic(settingsPath, styledString);

    // Persists the default terminal choice
    // GH#10003 - Only do this if _currentDefaultTerminal was actually initialized.
    if (_currentDefaultTerminal)
    {
        DefaultTerminal::Current(_currentDefaultTerminal);
    }
}

// Method Description:
// - Create a new serialized JsonObject from an instance of this class
// Arguments:
// - <none>
// Return Value:
// the JsonObject representing this instance
Json::Value CascadiaSettings::ToJson() const
{
    // top-level json object
    auto json{ _globals->ToJson() };
    json["$help"] = "https://aka.ms/terminal-documentation";
    json["$schema"] = "https://aka.ms/terminal-profiles-schema";

    // "profiles" will always be serialized as an object
    Json::Value profiles{ Json::ValueType::objectValue };
    profiles[JsonKey(DefaultSettingsKey)] = _baseLayerProfile ? _baseLayerProfile->ToJson() : Json::ValueType::objectValue;
    Json::Value profilesList{ Json::ValueType::arrayValue };
    for (const auto& entry : _allProfiles)
    {
        if (!entry.Deleted())
        {
            const auto prof{ winrt::get_self<Profile>(entry) };
            profilesList.append(prof->ToJson());
        }
    }
    profiles[JsonKey(ProfilesListKey)] = profilesList;
    json[JsonKey(ProfilesKey)] = profiles;

    // TODO GH#8100:
    // "schemes" will be an accumulation of _all_ the color schemes
    // including all of the ones from defaults.json
    Json::Value schemes{ Json::ValueType::arrayValue };
    for (const auto& entry : _globals->ColorSchemes())
    {
        const auto scheme{ winrt::get_self<ColorScheme>(entry.Value()) };
        schemes.append(scheme->ToJson());
    }
    json[JsonKey(SchemesKey)] = schemes;

    return json;
}

// Method Description:
// - Resolves the "defaultProfile", which can be a profile name, to a GUID
//   and stores it back to the globals.
void CascadiaSettings::_resolveDefaultProfile() const
{
    if (const auto unparsedDefaultProfile = _globals->UnparsedDefaultProfile(); !unparsedDefaultProfile.empty())
    {
        if (const auto profile = GetProfileByName(unparsedDefaultProfile))
        {
            _globals->DefaultProfile(profile.Guid());
            return;
        }

        _warnings.Append(SettingsLoadWarnings::MissingDefaultProfile);
    }

    // Use the first profile as the new default.
    GlobalSettings().DefaultProfile(_allProfiles.GetAt(0).Guid());
}
