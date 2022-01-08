// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <winrt/Microsoft.Windows.ApplicationModel.Resources.h>

class ScopedResourceLoader
{
public:
    ScopedResourceLoader(const std::wstring_view resourceLocatorBase);
    winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceMap GetResourceMap() const noexcept;
    winrt::hstring GetLocalizedString(const std::wstring_view resourceName) const;
    bool HasResourceWithName(const std::wstring_view resourceName) const;

private:
    winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceManager _resourceManager;
    winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceMap _resourceMap;
    winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceContext _resourceContext;
};
