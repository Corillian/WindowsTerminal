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

UTILS_DEFINE_LIBRARY_RESOURCE_SCOPE(L"Microsoft.Terminal.Control/Resources");
