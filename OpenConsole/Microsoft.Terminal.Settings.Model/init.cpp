// Copyright (c) Microsoft Corporation
// Licensed under the MIT license.

#include "pch.h"
#include <LibraryResources.h>

BOOL WINAPI DllMain(HINSTANCE hInstDll, DWORD reason, LPVOID /*reserved*/)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hInstDll);
        break;
    case DLL_PROCESS_DETACH:
        break;
    }

    return TRUE;
}

namespace Microsoft::Console::Internal::DefaultApp
{
    [[nodiscard]] HRESULT CheckShouldTerminalBeDefault(bool& isEnabled) noexcept
    {
        // False since setting Terminal as the default app is an OS feature and probably
        // should not be done in the open source conhost. We can always decide to turn it
        // on in the future though.
        isEnabled = false;
        return S_OK;
    }
}

UTILS_DEFINE_LIBRARY_RESOURCE_SCOPE(L"Microsoft.Terminal.Settings.Model/Resources")
