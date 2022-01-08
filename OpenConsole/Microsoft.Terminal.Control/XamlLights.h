// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "cppwinrt_utils.h"
#include "VisualBellLight.g.h"

namespace winrt::Microsoft::Terminal::Control::implementation
{
    struct VisualBellLight : VisualBellLightT<VisualBellLight>
    {
        VisualBellLight();

        winrt::hstring GetId();

        static Microsoft::UI::Xaml::DependencyProperty IsTargetProperty() { return _IsTargetProperty; }

        static bool GetIsTarget(Microsoft::UI::Xaml::DependencyObject const& target)
        {
            return winrt::unbox_value<bool>(target.GetValue(_IsTargetProperty));
        }

        static void SetIsTarget(Microsoft::UI::Xaml::DependencyObject const& target, bool value)
        {
            target.SetValue(_IsTargetProperty, winrt::box_value(value));
        }

        void OnConnected(Microsoft::UI::Xaml::UIElement const& newElement);
        void OnDisconnected(Microsoft::UI::Xaml::UIElement const& oldElement);

        static void OnIsTargetChanged(Microsoft::UI::Xaml::DependencyObject const& d, Microsoft::UI::Xaml::DependencyPropertyChangedEventArgs const& e);

        inline static winrt::hstring GetIdStatic()
        {
            // This specifies the unique name of the light. In most cases you should use the type's full name.
            return winrt::xaml_typename<winrt::Microsoft::Terminal::Control::VisualBellLight>().Name;
        }

    private:
        static void _InitializeProperties();
        static Microsoft::UI::Xaml::DependencyProperty _IsTargetProperty;
    };
}

namespace winrt::Microsoft::Terminal::Control::factory_implementation
{
    BASIC_FACTORY(VisualBellLight);
}
