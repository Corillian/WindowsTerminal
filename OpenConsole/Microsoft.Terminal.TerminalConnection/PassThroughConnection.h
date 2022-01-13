// Written by Corillian, NOT Microsoft
// Unfortunately, the WinRT toolchain loudly complains
// if I create a WinRT class under a namespace other
// than the default namespace. I think this will likely
// be something desired by a lot of users and I don't
// think it makes sense to create a whole new assembly
// for a single class so... this is why we can't have
// nice things.

#pragma once

#include "PassThroughConnection.g.h"
#include "../cascadia/inc/cppwinrt_utils.h"

namespace winrt::Microsoft::Terminal::TerminalConnection::implementation
{
    struct PassThroughConnection : PassThroughConnectionT<PassThroughConnection>
    {
        PassThroughConnection() noexcept;
        PassThroughConnection(const bool passThroughInput) noexcept;

        void Start() noexcept;
        void WriteInput(hstring const& data);
        void Resize(uint32_t rows, uint32_t columns) noexcept;
        void Close() noexcept;

        void Initialize(const Windows::Foundation::Collections::ValueSet& /*settings*/) const noexcept {};

        void ClearBufferedData();
        void WriteOutput(hstring const& data);
        void WriteLine(hstring const& data);

        ConnectionState State() const noexcept { return _connectionState.load(std::memory_order_acquire); }

        bool PassThroughInput() const noexcept;
        void PassThroughInput(const bool value) noexcept;

        WINRT_CALLBACK(TerminalOutput, TerminalOutputHandler);
        TYPED_EVENT(StateChanged, ITerminalConnection, IInspectable);

    private:
        std::unique_ptr<std::vector<hstring>> _bufferedData;
        std::mutex _bufferedDataMutex;
        std::atomic<ConnectionState> _connectionState{ ConnectionState::NotConnected };
        std::atomic<bool> _passThroughInput;
    };
}

namespace winrt::Microsoft::Terminal::TerminalConnection::factory_implementation
{
    struct PassThroughConnection : PassThroughConnectionT<PassThroughConnection, implementation::PassThroughConnection>
    {
    };
}
