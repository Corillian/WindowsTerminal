﻿// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "ConptyConnection.h"

#include <winternl.h>

#include "ConptyConnection.g.cpp"
#include "CTerminalHandoff.h"

#include "../../types/inc/utils.hpp"
#include "../../types/inc/Environment.hpp"
#include "LibraryResources.h"

using namespace ::Microsoft::Console;
using namespace std::string_view_literals;

// Format is: "DecimalResult (HexadecimalForm)"
static constexpr auto _errorFormat = L"{0} ({0:#010x})"sv;

// Notes:
// There is a number of ways that the Conpty connection can be terminated (voluntarily or not):
// 1. The connection is Close()d
// 2. The pseudoconsole or process cannot be spawned during Start()
// 3. The client process exits with a code.
//    (Successful (0) or any other code)
// 4. The read handle is terminated.
//    (This usually happens when the pseudoconsole host crashes.)
// In each of these termination scenarios, we need to be mindful of tripping the others.
// Closing the pseudoconsole in response to the client exiting (3) can trigger (4).
// Close() (1) will cause the automatic triggering of (3) and (4).
// In a lot of cases, we use the connection state to stop "flapping."
//
// To figure out where we handle these, search for comments containing "EXIT POINT"

namespace winrt::Microsoft::Terminal::TerminalConnection::implementation
{
    // Function Description:
    // - creates some basic anonymous pipes and passes them to CreatePseudoConsole
    // Arguments:
    // - size: The size of the conpty to create, in characters.
    // - phInput: Receives the handle to the newly-created anonymous pipe for writing input to the conpty.
    // - phOutput: Receives the handle to the newly-created anonymous pipe for reading the output of the conpty.
    // - phPc: Receives a token value to identify this conpty
#pragma warning(suppress : 26430) // This statement sufficiently checks the out parameters. Analyzer cannot find this.
    static HRESULT _CreatePseudoConsoleAndPipes(const COORD size, const DWORD dwFlags, HANDLE* phInput, HANDLE* phOutput, HPCON* phPC) noexcept
    {
        RETURN_HR_IF(E_INVALIDARG, phPC == nullptr || phInput == nullptr || phOutput == nullptr);

        wil::unique_hfile outPipeOurSide, outPipePseudoConsoleSide;
        wil::unique_hfile inPipeOurSide, inPipePseudoConsoleSide;

        RETURN_IF_WIN32_BOOL_FALSE(CreatePipe(&inPipePseudoConsoleSide, &inPipeOurSide, nullptr, 0));
        RETURN_IF_WIN32_BOOL_FALSE(CreatePipe(&outPipeOurSide, &outPipePseudoConsoleSide, nullptr, 0));
        RETURN_IF_FAILED(ConptyCreatePseudoConsole(size, inPipePseudoConsoleSide.get(), outPipePseudoConsoleSide.get(), dwFlags, phPC));
        *phInput = inPipeOurSide.release();
        *phOutput = outPipeOurSide.release();
        return S_OK;
    }

    // Function Description:
    // - launches the client application attached to the new pseudoconsole
    HRESULT ConptyConnection::_LaunchAttachedClient() noexcept
    try
    {
        STARTUPINFOEX siEx{ 0 };
        siEx.StartupInfo.cb = sizeof(STARTUPINFOEX);
        siEx.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        SIZE_T size{};
        // This call will return an error (by design); we are ignoring it.
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
#pragma warning(suppress : 26414) // We don't move/touch this smart pointer, but we have to allocate strangely for the adjustable size list.
        auto attrList{ std::make_unique<std::byte[]>(size) };
#pragma warning(suppress : 26490) // We have to use reinterpret_cast because we allocated a byte array as a proxy for the adjustable size list.
        siEx.lpAttributeList = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attrList.get());
        RETURN_IF_WIN32_BOOL_FALSE(InitializeProcThreadAttributeList(siEx.lpAttributeList, 1, 0, &size));

        RETURN_IF_WIN32_BOOL_FALSE(UpdateProcThreadAttribute(siEx.lpAttributeList,
                                                             0,
                                                             PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                                             _hPC.get(),
                                                             sizeof(HPCON),
                                                             nullptr,
                                                             nullptr));

        auto cmdline{ wil::ExpandEnvironmentStringsW<std::wstring>(_commandline.c_str()) }; // mutable copy -- required for CreateProcessW

        Utils::EnvironmentVariableMapW environment;
        auto zeroEnvMap = wil::scope_exit([&]() noexcept {
            // Can't zero the keys, but at least we can zero the values.
            for (auto& [name, value] : environment)
            {
                ::SecureZeroMemory(value.data(), value.size() * sizeof(decltype(value.begin())::value_type));
            }

            environment.clear();
        });

        // Populate the environment map with the current environment.
        RETURN_IF_FAILED(Utils::UpdateEnvironmentMapW(environment));

        {
            // Convert connection Guid to string and ignore the enclosing '{}'.
            auto wsGuid{ Utils::GuidToString(_guid) };
            wsGuid.pop_back();

            const auto guidSubStr = std::wstring_view{ wsGuid }.substr(1);

            // Ensure every connection has the unique identifier in the environment.
            environment.insert_or_assign(L"WT_SESSION", guidSubStr.data());

            if (_environment)
            {
                // add additional WT env vars like WT_SETTINGS, WT_DEFAULTS and WT_PROFILE_ID
                for (auto item : _environment)
                {
                    try
                    {
                        auto key = item.Key();
                        // This will throw if the value isn't a string. If that
                        // happens, then just skip this entry.
                        auto value = winrt::unbox_value<hstring>(item.Value());

                        // avoid clobbering WSLENV
                        if (std::wstring_view{ key } == L"WSLENV")
                        {
                            auto current = environment[L"WSLENV"];
                            value = current + L":" + value;
                        }

                        environment.insert_or_assign(key.c_str(), value.c_str());
                    }
                    CATCH_LOG();
                }
            }

            // WSLENV is a colon-delimited list of environment variables (+flags) that should appear inside WSL
            // https://devblogs.microsoft.com/commandline/share-environment-vars-between-wsl-and-windows/

            auto wslEnv = environment[L"WSLENV"];
            wslEnv = L"WT_SESSION:" + wslEnv; // prepend WT_SESSION to make sure it's visible inside WSL.
            environment.insert_or_assign(L"WSLENV", wslEnv);
        }

        std::vector<wchar_t> newEnvVars;
        auto zeroNewEnv = wil::scope_exit([&]() noexcept {
            ::SecureZeroMemory(newEnvVars.data(),
                               newEnvVars.size() * sizeof(decltype(newEnvVars.begin())::value_type));
        });

        RETURN_IF_FAILED(Utils::EnvironmentMapToEnvironmentStringsW(environment, newEnvVars));

        auto lpEnvironment = newEnvVars.empty() ? nullptr : newEnvVars.data();

        // If we have a startingTitle, create a mutable character buffer to add
        // it to the STARTUPINFO.
        std::wstring mutableTitle{};
        if (!_startingTitle.empty())
        {
            mutableTitle = _startingTitle;
            siEx.StartupInfo.lpTitle = mutableTitle.data();
        }

        auto [newCommandLine, newStartingDirectory] = Utils::MangleStartingDirectoryForWSL(cmdline, _startingDirectory);
        const auto startingDirectory = newStartingDirectory.size() > 0 ? newStartingDirectory.c_str() : nullptr;

        RETURN_IF_WIN32_BOOL_FALSE(CreateProcessW(
            nullptr,
            newCommandLine.data(),
            nullptr, // lpProcessAttributes
            nullptr, // lpThreadAttributes
            false, // bInheritHandles
            EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT, // dwCreationFlags
            lpEnvironment, // lpEnvironment
            startingDirectory,
            &siEx.StartupInfo, // lpStartupInfo
            &_piClient // lpProcessInformation
            ));

        DeleteProcThreadAttributeList(siEx.lpAttributeList);

        const std::filesystem::path processName = wil::GetModuleFileNameExW<std::wstring>(_piClient.hProcess, nullptr);
        _clientName = processName.filename().wstring();

//#pragma warning(suppress : 26477 26485 26494 26482 26446) // We don't control TraceLoggingWrite
//        TraceLoggingWrite(
//            g_hTerminalConnectionProvider,
//            "ConPtyConnected",
//            TraceLoggingDescription("Event emitted when ConPTY connection is started"),
//            TraceLoggingGuid(_guid, "SessionGuid", "The WT_SESSION's GUID"),
//            TraceLoggingWideString(_clientName.c_str(), "Client", "The attached client process"),
//            TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
//            TelemetryPrivacyDataTag(PDT_ProductAndServicePerformance));

        return S_OK;
    }
    CATCH_RETURN();

    ConptyConnection::ConptyConnection(const HANDLE hSig,
                                       const HANDLE hIn,
                                       const HANDLE hOut,
                                       const HANDLE hRef,
                                       const HANDLE hServerProcess,
                                       const HANDLE hClientProcess) :
        _initialRows{ 25 },
        _initialCols{ 80 },
        _guid{ Utils::CreateGuid() },
        _inPipe{ hIn },
        _outPipe{ hOut }
    {
        THROW_IF_FAILED(ConptyPackPseudoConsole(hServerProcess, hRef, hSig, &_hPC));
        _piClient.hProcess = hClientProcess;

        try
        {
            _commandline = _commandlineFromProcess(hClientProcess);
        }
        CATCH_LOG()
    }

    // Function Description:
    // - Helper function for constructing a ValueSet that we can use to get our settings from.
    Windows::Foundation::Collections::ValueSet ConptyConnection::CreateSettings(const winrt::hstring& cmdline,
                                                                                const winrt::hstring& startingDirectory,
                                                                                const winrt::hstring& startingTitle,
                                                                                const Windows::Foundation::Collections::IMapView<hstring, hstring>& environment,
                                                                                uint32_t rows,
                                                                                uint32_t columns,
                                                                                const winrt::guid& guid)
    {
        Windows::Foundation::Collections::ValueSet vs{};

        vs.Insert(L"commandline", Windows::Foundation::PropertyValue::CreateString(cmdline));
        vs.Insert(L"startingDirectory", Windows::Foundation::PropertyValue::CreateString(startingDirectory));
        vs.Insert(L"startingTitle", Windows::Foundation::PropertyValue::CreateString(startingTitle));
        vs.Insert(L"initialRows", Windows::Foundation::PropertyValue::CreateUInt32(rows));
        vs.Insert(L"initialCols", Windows::Foundation::PropertyValue::CreateUInt32(columns));
        vs.Insert(L"guid", Windows::Foundation::PropertyValue::CreateGuid(guid));

        if (environment)
        {
            Windows::Foundation::Collections::ValueSet env{};
            for (const auto& [k, v] : environment)
            {
                env.Insert(k, Windows::Foundation::PropertyValue::CreateString(v));
            }
            vs.Insert(L"environment", env);
        }
        return vs;
    }

    void ConptyConnection::Initialize(const Windows::Foundation::Collections::ValueSet& settings)
    {
        if (settings)
        {
            // For the record, the following won't crash:
            // auto bad = unbox_value_or<hstring>(settings.TryLookup(L"foo").try_as<IPropertyValue>(), nullptr);
            // It'll just return null

            _commandline = winrt::unbox_value_or<winrt::hstring>(settings.TryLookup(L"commandline").try_as<Windows::Foundation::IPropertyValue>(), _commandline);
            _startingDirectory = winrt::unbox_value_or<winrt::hstring>(settings.TryLookup(L"startingDirectory").try_as<Windows::Foundation::IPropertyValue>(), _startingDirectory);
            _startingTitle = winrt::unbox_value_or<winrt::hstring>(settings.TryLookup(L"startingTitle").try_as<Windows::Foundation::IPropertyValue>(), _startingTitle);
            _initialRows = winrt::unbox_value_or<uint32_t>(settings.TryLookup(L"initialRows").try_as<Windows::Foundation::IPropertyValue>(), _initialRows);
            _initialCols = winrt::unbox_value_or<uint32_t>(settings.TryLookup(L"initialCols").try_as<Windows::Foundation::IPropertyValue>(), _initialCols);
            _guid = winrt::unbox_value_or<winrt::guid>(settings.TryLookup(L"guid").try_as<Windows::Foundation::IPropertyValue>(), _guid);
            _environment = settings.TryLookup(L"environment").try_as<Windows::Foundation::Collections::ValueSet>();
            /*if constexpr (Feature_VtPassthroughMode::IsEnabled())
            {
                _passthroughMode = winrt::unbox_value_or<bool>(settings.TryLookup(L"passthroughMode").try_as<Windows::Foundation::IPropertyValue>(), _passthroughMode);
            }*/
        }

        if (_guid == guid{})
        {
            _guid = Utils::CreateGuid();
        }
    }

    winrt::guid ConptyConnection::Guid() const noexcept
    {
        return _guid;
    }

    winrt::hstring ConptyConnection::Commandline() const
    {
        return _commandline;
    }

    void ConptyConnection::Start()
    try
    {
        _transitionToState(ConnectionState::Connecting);

        const COORD dimensions{ gsl::narrow_cast<SHORT>(_initialCols), gsl::narrow_cast<SHORT>(_initialRows) };

        // If we do not have pipes already, then this is a fresh connection... not an inbound one that is a received
        // handoff from an already-started PTY process.
        if (!_inPipe)
        {
            DWORD flags = PSEUDOCONSOLE_RESIZE_QUIRK | PSEUDOCONSOLE_WIN32_INPUT_MODE;

            /*if constexpr (Feature_VtPassthroughMode::IsEnabled())
            {
                if (_passthroughMode)
                {
                    WI_SetFlag(flags, PSEUDOCONSOLE_PASSTHROUGH_MODE);
                }
            }*/

            THROW_IF_FAILED(_CreatePseudoConsoleAndPipes(dimensions, flags, &_inPipe, &_outPipe, &_hPC));

            // NOTE: For some reason this works with regular WindowsTerminal but torches the conpty connection here.
            // Given that it's an internal API there's no documentation so... no idea why.

            /*if (_initialParentHwnd != 0)
            {
                THROW_IF_FAILED(ConptyReparentPseudoConsole(_hPC.get(), reinterpret_cast<HWND>(_initialParentHwnd)));
            }*/

            // NOTE: For some reason this works with regular WindowsTerminal but torches the conpty connection here.
            // Given that it's an internal API there's no documentation so... no idea why.
            // 
            // GH#12515: The conpty assumes it's hidden at the start. If we're visible, let it know now.
            /*if (_initialVisibility)
            {
                THROW_IF_FAILED(ConptyShowHidePseudoConsole(_hPC.get(), _initialVisibility));
            }*/

            THROW_IF_FAILED(_LaunchAttachedClient());
        }
        // But if it was an inbound handoff... attempt to synchronize the size of it with what our connection
        // window is expecting it to be on the first layout.
        else
        {
//#pragma warning(suppress : 26477 26485 26494 26482 26446) // We don't control TraceLoggingWrite
//            TraceLoggingWrite(
//                g_hTerminalConnectionProvider,
//                "ConPtyConnectedToDefterm",
//                TraceLoggingDescription("Event emitted when ConPTY connection is started, for a defterm session"),
//                TraceLoggingGuid(_guid, "SessionGuid", "The WT_SESSION's GUID"),
//                TraceLoggingWideString(_clientName.c_str(), "Client", "The attached client process"),
//                TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
//                TelemetryPrivacyDataTag(PDT_ProductAndServicePerformance));

            THROW_IF_FAILED(ConptyResizePseudoConsole(_hPC.get(), dimensions));

            // NOTE: For some reason this works with regular WindowsTerminal but torches the conpty connection here.
            // Given that it's an internal API there's no documentation so... no idea why.

            /*THROW_IF_FAILED(ConptyReparentPseudoConsole(_hPC.get(), reinterpret_cast<HWND>(_initialParentHwnd)));

            if (_initialVisibility)
            {
                THROW_IF_FAILED(ConptyShowHidePseudoConsole(_hPC.get(), _initialVisibility));
            }*/
        }

        _startTime = std::chrono::high_resolution_clock::now();

        // Create our own output handling thread
        // This must be done after the pipes are populated.
        // Each connection needs to make sure to drain the output from its backing host.
        _hOutputThread.reset(CreateThread(
            nullptr,
            0,
            [](LPVOID lpParameter) noexcept {
                const auto pInstance = static_cast<ConptyConnection*>(lpParameter);
                if (pInstance)
                {
                    return pInstance->_OutputThread();
                }
                return gsl::narrow_cast<DWORD>(E_INVALIDARG);
            },
            this,
            0,
            nullptr));

        THROW_LAST_ERROR_IF_NULL(_hOutputThread);

        LOG_IF_FAILED(SetThreadDescription(_hOutputThread.get(), L"ConptyConnection Output Thread"));

        _clientExitWait.reset(CreateThreadpoolWait(
            [](PTP_CALLBACK_INSTANCE /*callbackInstance*/, PVOID context, PTP_WAIT /*wait*/, TP_WAIT_RESULT /*waitResult*/) noexcept {
                const auto pInstance = static_cast<ConptyConnection*>(context);
                if (pInstance)
                {
                    pInstance->_ClientTerminated();
                }
            },
            this,
            nullptr));

        SetThreadpoolWait(_clientExitWait.get(), _piClient.hProcess, nullptr);

        _transitionToState(ConnectionState::Connected);
    }
    catch (...)
    {
        // EXIT POINT
        const auto hr = wil::ResultFromCaughtException();

        // GH#11556 - make sure to format the error code to this string as an UNSIGNED int
        winrt::hstring failureText{ fmt::format(std::wstring_view{ RS_(L"ProcessFailedToLaunch") },
                                                fmt::format(_errorFormat, static_cast<unsigned int>(hr)),
                                                _commandline) };
        _TerminalOutputHandlers(failureText);

        // If the path was invalid, let's present an informative message to the user
        if (hr == HRESULT_FROM_WIN32(ERROR_DIRECTORY))
        {
            winrt::hstring badPathText{ fmt::format(std::wstring_view{ RS_(L"BadPathText") },
                                                    _startingDirectory) };
            _TerminalOutputHandlers(L"\r\n");
            _TerminalOutputHandlers(badPathText);
        }

        _transitionToState(ConnectionState::Failed);

        // Tear down any state we may have accumulated.
        _hPC.reset();
    }

    // Method Description:
    // - prints out the "process exited" message formatted with the exit code
    // Arguments:
    // - status: the exit code.
    void ConptyConnection::_indicateExitWithStatus(unsigned int status) noexcept
    {
        try
        {
            // GH#11556 - make sure to format the error code to this string as an UNSIGNED int
            winrt::hstring exitText{ fmt::format(std::wstring_view{ RS_(L"ProcessExited") }, fmt::format(_errorFormat, status)) };
            _TerminalOutputHandlers(L"\r\n");
            _TerminalOutputHandlers(exitText);
        }
        CATCH_LOG();
    }

    // Method Description:
    // - called when the client application (not necessarily its pty) exits for any reason
    void ConptyConnection::_ClientTerminated() noexcept
    try
    {
        if (_isStateAtOrBeyond(ConnectionState::Closing))
        {
            // This termination was expected.
            return;
        }

        // EXIT POINT
        DWORD exitCode{ 0 };
        GetExitCodeProcess(_piClient.hProcess, &exitCode);

        // Signal the closing or failure of the process.
        // Load bearing. Terminating the pseudoconsole will make the output thread exit unexpectedly,
        // so we need to signal entry into the correct closing state before we do that.
        _transitionToState(exitCode == 0 ? ConnectionState::Closed : ConnectionState::Failed);

        // Close the pseudoconsole and wait for all output to drain.
        _hPC.reset();
        if (auto localOutputThreadHandle = std::move(_hOutputThread))
        {
            LOG_LAST_ERROR_IF(WAIT_FAILED == WaitForSingleObject(localOutputThreadHandle.get(), INFINITE));
        }

        _indicateExitWithStatus(exitCode);

        _piClient.reset();
    }
    CATCH_LOG()

    void ConptyConnection::WriteInput(const hstring& data)
    {
        if (!_isConnected())
        {
            return;
        }

        // convert from UTF-16LE to UTF-8 as ConPty expects UTF-8
        // TODO GH#3378 reconcile and unify UTF-8 converters
        auto str = winrt::to_string(data);
        LOG_IF_WIN32_BOOL_FALSE(WriteFile(_inPipe.get(), str.c_str(), (DWORD)str.length(), nullptr, nullptr));
    }

    void ConptyConnection::Resize(uint32_t rows, uint32_t columns)
    {
        // If we haven't started connecting at all, it's still fair to update
        // the initial rows and columns before we set things up.
        if (!_isStateAtOrBeyond(ConnectionState::Connecting))
        {
            _initialRows = rows;
            _initialCols = columns;
        }
        // Otherwise, we can really only dispatch a resize if we're already connected.
        else if (_isConnected())
        {
            THROW_IF_FAILED(ConptyResizePseudoConsole(_hPC.get(), { Utils::ClampToShortMax(columns, 1), Utils::ClampToShortMax(rows, 1) }));
        }
    }

    void ConptyConnection::ClearBuffer()
    {
        // If we haven't connected yet, then we really don't need to do
        // anything. The connection should already start clear!
        if (_isConnected())
        {
            THROW_IF_FAILED(ConptyClearPseudoConsole(_hPC.get()));
        }
    }

    void ConptyConnection::ShowHide(const bool show)
    {
        // If we haven't connected yet, then stash for when we do connect.
        if (_isConnected())
        {
            THROW_IF_FAILED(ConptyShowHidePseudoConsole(_hPC.get(), show));
        }
        else
        {
            _initialVisibility = show;
        }
    }

    void ConptyConnection::ReparentWindow(const uint64_t newParent)
    {
        // If we haven't started connecting at all, stash this HWND to use once we have started.
        if (!_isStateAtOrBeyond(ConnectionState::Connecting))
        {
            _initialParentHwnd = newParent;
        }
        // Otherwise, just inform the conpty of the new owner window handle.
        // This shouldn't be hittable until GH#5000 / GH#1256, when it's
        // possible to reparent terminals to different windows.
        else if (_isConnected())
        {
            THROW_IF_FAILED(ConptyReparentPseudoConsole(_hPC.get(), reinterpret_cast<HWND>(newParent)));
        }
    }

    void ConptyConnection::Close() noexcept
    try
    {
        if (_transitionToState(ConnectionState::Closing))
        {
            // EXIT POINT
            _clientExitWait.reset(); // immediately stop waiting for the client to exit.

            _hPC.reset(); // tear down the pseudoconsole (this is like clicking X on a console window)

            _inPipe.reset(); // break the pipes
            _outPipe.reset();

            if (_hOutputThread)
            {
                // Tear down our output thread -- now that the output pipe was closed on the
                // far side, we can run down our local reader.
                LOG_LAST_ERROR_IF(WAIT_FAILED == WaitForSingleObject(_hOutputThread.get(), INFINITE));
                _hOutputThread.reset();
            }

            if (_piClient.hProcess)
            {
                // Wait for the client to terminate (which it should do successfully)
                LOG_LAST_ERROR_IF(WAIT_FAILED == WaitForSingleObject(_piClient.hProcess, INFINITE));
                _piClient.reset();
            }

            _transitionToState(ConnectionState::Closed);
        }
    }
    CATCH_LOG()

    // Returns the command line of the given process.
    // Requires PROCESS_BASIC_INFORMATION | PROCESS_VM_READ privileges.
    winrt::hstring ConptyConnection::_commandlineFromProcess(HANDLE process)
    {
        struct PROCESS_BASIC_INFORMATION
        {
            NTSTATUS ExitStatus;
            PPEB PebBaseAddress;
            ULONG_PTR AffinityMask;
            KPRIORITY BasePriority;
            ULONG_PTR UniqueProcessId;
            ULONG_PTR InheritedFromUniqueProcessId;
        } info;
        THROW_IF_NTSTATUS_FAILED(NtQueryInformationProcess(process, ProcessBasicInformation, &info, sizeof(info), nullptr));

        // PEB: Process Environment Block
        // This is a funny structure allocated by the kernel which contains all sorts of useful
        // information, only a tiny fraction of which are documented publicly unfortunately.
        // Fortunately however it contains a copy of the command line the process launched with.
        PEB peb;
        THROW_IF_WIN32_BOOL_FALSE(ReadProcessMemory(process, info.PebBaseAddress, &peb, sizeof(peb), nullptr));

        RTL_USER_PROCESS_PARAMETERS params;
        THROW_IF_WIN32_BOOL_FALSE(ReadProcessMemory(process, peb.ProcessParameters, &params, sizeof(params), nullptr));

        // Yeah I know... Don't use "impl" stuff... But why do you make something _that_ useful private? :(
        // The hstring_builder allows us to create a hstring without intermediate copies. Neat!
        winrt::impl::hstring_builder commandline{ params.CommandLine.Length / 2u };
        THROW_IF_WIN32_BOOL_FALSE(ReadProcessMemory(process, params.CommandLine.Buffer, commandline.data(), params.CommandLine.Length, nullptr));
        return commandline.to_hstring();
    }

    DWORD ConptyConnection::_OutputThread()
    {
        // Keep us alive until the output thread terminates; the destructor
        // won't wait for us, and the known exit points _do_.
        auto strongThis{ get_strong() };

        // process the data of the output pipe in a loop
        while (true)
        {
            DWORD read{};

            const auto readFail{ !ReadFile(_outPipe.get(), _buffer.data(), gsl::narrow_cast<DWORD>(_buffer.size()), &read, nullptr) };
            if (readFail) // reading failed (we must check this first, because read will also be 0.)
            {
                const auto lastError = GetLastError();
                if (lastError != ERROR_BROKEN_PIPE && !_isStateAtOrBeyond(ConnectionState::Closing))
                {
                    // EXIT POINT
                    _indicateExitWithStatus(HRESULT_FROM_WIN32(lastError)); // print a message
                    _transitionToState(ConnectionState::Failed);
                    return gsl::narrow_cast<DWORD>(HRESULT_FROM_WIN32(lastError));
                }
                // else we call convertUTF8ChunkToUTF16 with an empty string_view to convert possible remaining partials to U+FFFD
            }

            const auto result{ til::u8u16(std::string_view{ _buffer.data(), read }, _u16Str, _u8State) };
            if (FAILED(result))
            {
                if (_isStateAtOrBeyond(ConnectionState::Closing))
                {
                    // This termination was expected.
                    return 0;
                }

                // EXIT POINT
                _indicateExitWithStatus(result); // print a message
                _transitionToState(ConnectionState::Failed);
                return gsl::narrow_cast<DWORD>(result);
            }

            if (_u16Str.empty())
            {
                return 0;
            }

            if (!_receivedFirstByte)
            {
                const auto now = std::chrono::high_resolution_clock::now();
                const std::chrono::duration<double> delta = now - _startTime;

//#pragma warning(suppress : 26477 26485 26494 26482 26446) // We don't control TraceLoggingWrite
//                TraceLoggingWrite(g_hTerminalConnectionProvider,
//                                  "ReceivedFirstByte",
//                                  TraceLoggingDescription("An event emitted when the connection receives the first byte"),
//                                  TraceLoggingGuid(_guid, "SessionGuid", "The WT_SESSION's GUID"),
//                                  TraceLoggingFloat64(delta.count(), "Duration"),
//                                  TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
//                                  TelemetryPrivacyDataTag(PDT_ProductAndServicePerformance));
                _receivedFirstByte = true;
            }

            // Pass the output to our registered event handlers
            _TerminalOutputHandlers(_u16Str);
        }

        return 0;
    }

    static winrt::event<NewConnectionHandler> _newConnectionHandlers;

    winrt::event_token ConptyConnection::NewConnection(const NewConnectionHandler& handler) { return _newConnectionHandlers.add(handler); };
    void ConptyConnection::NewConnection(const winrt::event_token& token) { _newConnectionHandlers.remove(token); };

    HRESULT ConptyConnection::NewHandoff(HANDLE in, HANDLE out, HANDLE signal, HANDLE ref, HANDLE server, HANDLE client) noexcept
    try
    {
        _newConnectionHandlers(winrt::make<ConptyConnection>(signal, in, out, ref, server, client));

        return S_OK;
    }
    CATCH_RETURN()

    void ConptyConnection::StartInboundListener()
    {
        THROW_IF_FAILED(CTerminalHandoff::s_StartListening(&ConptyConnection::NewHandoff));
    }

    void ConptyConnection::StopInboundListener()
    {
        THROW_IF_FAILED(CTerminalHandoff::s_StopListening());
    }

    // Function Description:
    // - This function will be called (by C++/WinRT) after the final outstanding reference to
    //   any given connection instance is released.
    //   When a client application exits, its termination will wait for the output thread to
    //   run down. However, because our teardown is somewhat complex, our last reference may
    //   be owned by the very output thread that the client wait threadpool is blocked on.
    //   During destruction, we'll try to release any outstanding handles--including the one
    //   we have to the threadpool wait. As you might imagine, this takes us right to deadlock
    //   city.
    //   Deferring the final destruction of the connection to a background thread that can't
    //   be awaiting our destruction breaks the deadlock.
    // Arguments:
    // - connection: the final living reference to an outgoing connection
    winrt::fire_and_forget ConptyConnection::final_release(std::unique_ptr<ConptyConnection> connection)
    {
        co_await winrt::resume_background(); // move to background
        connection.reset(); // explicitly destruct
    }

}
