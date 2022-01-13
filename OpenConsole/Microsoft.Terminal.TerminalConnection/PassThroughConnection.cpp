// Written by Corillian, NOT Microsoft
// Unfortunately, the WinRT toolchain loudly complains
// if I create a WinRT class under a namespace other
// than the default namespace. I think this will likely
// be something desired by a lot of users and I don't
// think it makes sense to create a whole new assembly
// for a single class so... this is why we can't have
// nice things.

#include "pch.h"
#include "PassThroughConnection.h"
#include "PassThroughConnection.g.cpp"

namespace winrt::Microsoft::Terminal::TerminalConnection::implementation
{
    PassThroughConnection::PassThroughConnection() noexcept
        : _passThroughInput(true)
    {
    }

    PassThroughConnection::PassThroughConnection(const bool passThroughInput) noexcept
        : _passThroughInput(passThroughInput)
    {
    }

    void PassThroughConnection::Start() noexcept
    {
        ConnectionState expected = ConnectionState::NotConnected;

        if(_connectionState.compare_exchange_strong(expected, ConnectionState::Connected, std::memory_order_acq_rel, std::memory_order_acquire))
        {
            std::vector<hstring> tmp;

            {
                std::lock_guard<std::mutex> guard(_bufferedDataMutex);

                if(_bufferedData)
                {
                    tmp = std::move(*_bufferedData);
                    _bufferedData.reset();
                }
            }

            _StateChangedHandlers(*this, nullptr);

            for(auto iter = tmp.cbegin(); iter != tmp.cend(); ++iter)
            {
                _TerminalOutputHandlers(*iter);
            }
        }
    }

    void PassThroughConnection::WriteInput(hstring const& data)
    {
        if(_passThroughInput.load(std::memory_order_acquire))
        {
            WriteOutput(data);
        }
    }

    void PassThroughConnection::Resize(uint32_t /*rows*/, uint32_t /*columns*/) noexcept
    {
    }

    void PassThroughConnection::Close() noexcept
    {
        ConnectionState expected = ConnectionState::Connected;

        if(_connectionState.compare_exchange_strong(expected, ConnectionState::Closed, std::memory_order_acq_rel, std::memory_order_acquire))
        {
            {
                std::lock_guard<std::mutex> guard(_bufferedDataMutex);
                _bufferedData.reset();
            }
            
            _StateChangedHandlers(*this, nullptr);
        }
    }

    void PassThroughConnection::ClearBufferedData()
    {
        std::lock_guard<std::mutex> guard(_bufferedDataMutex);

        if(_bufferedData)
        {
            _bufferedData->clear();
        }
    }

    void PassThroughConnection::WriteOutput(hstring const& data)
    {
        auto state = _connectionState.load(std::memory_order_acquire);

        if(state == ConnectionState::NotConnected)
        {
            bool written = false;

            {
                std::lock_guard<std::mutex> guard(_bufferedDataMutex);

                state = _connectionState.load(std::memory_order_acquire);

                if(state == ConnectionState::NotConnected)
                {
                    if(!_bufferedData)
                    {
                        _bufferedData = std::make_unique<std::vector<hstring>>();
                    }

                    _bufferedData->push_back(data);
                }
                else if(state == ConnectionState::Connected && _bufferedData && _bufferedData->size() > 0)
                {
                    // We managed to acquire the lock before Start() could get it and Start() should be purging the buffer
                    // as soon as we release it
                    _bufferedData->push_back(data);
                    written = true;
                }
            }
            
            if(state == ConnectionState::Connected && !written)
            {
                _TerminalOutputHandlers(data);
            }
        }
        else if(state == ConnectionState::Connected)
        {
            _TerminalOutputHandlers(data);
        }
    }

    void PassThroughConnection::WriteLine(hstring const& data)
    {
        std::wstring tmp;

        tmp.reserve(static_cast<size_t>(data.size()) + 2);
        tmp.assign(data.c_str(), data.size());
        tmp += L"\r\n";

        WriteOutput(hstring{ tmp });
    }

    bool PassThroughConnection::PassThroughInput() const noexcept
    {
        return _passThroughInput.load(std::memory_order_acquire);
    }

    void PassThroughConnection::PassThroughInput(const bool value) noexcept
    {
        _passThroughInput.store(value, std::memory_order_release);
    }
}
