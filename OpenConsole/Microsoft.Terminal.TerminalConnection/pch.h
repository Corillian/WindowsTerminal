#pragma once

// Needs to be defined or we get redeclaration errors
#define WIN32_LEAN_AND_MEAN
#define NOMCX
#define NOHELP
#define NOCOMM

// Manually include til after we include Windows.Foundation to give it winrt superpowers
#define BLOCK_TIL
#include <LibraryIncludes.h>

#include <unknwn.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Security.Credentials.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <wil/cppwinrt_helpers.h>

#include <Windows.h>

#include <til.h>

#include <cppwinrt_utils.h>