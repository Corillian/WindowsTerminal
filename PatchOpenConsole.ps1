function Write-OpenConsolePatch
{
    param(
        [Parameter(Mandatory=$true)]
        [string] $source,
        [Parameter(Mandatory=$true)]
        [string] $dest
    )

    $srcContent = Get-Content $source
    
    if([System.IO.File]::Exists($dest))
    {
        $destContent = Get-Content $dest

        if($srcContent -eq $destContent)
        {
            return
        }
    }

    Write-Host "Writing patch '$source' to '$dest'"
    Set-Content -Path $dest -Value $srcContent
}

$rootDir = [System.IO.Path]::GetDirectoryName($PSCommandPath)
$openConsolePath = [System.IO.Path]::Combine($rootDir, 'external', 'terminal')
$proxyHostPath = [System.IO.Path]::Combine($openConsolePath, 'src', 'host', 'proxy')
$proxyHostProjPath = [System.IO.Path]::Combine($proxyHostPath, 'Host.Proxy.vcxproj')
$proxyShimPath = [System.IO.Path]::Combine($proxyHostPath, 'nodefaultlib_shim.h')

$fixedHostProj = $false

if([System.IO.File]::Exists($proxyHostProjPath) -eq $true)
{
    $content = Get-Content $proxyHostProjPath

    if($content.Contains('<BasicRuntimeChecks>') -eq $false)
    {
        $fixedHostProj = $true
        $content.Replace('</PrecompiledHeader>', "</PrecompiledHeader>`r`n	    <BasicRuntimeChecks>Default</BasicRuntimeChecks>") | Set-Content $proxyHostProjPath
    }
}

if($fixedHostProj -eq $true)
{
    Write-Host 'Fixed the proxy host project file.'
}
else
{
    Write-Host 'The proxy host project file is already fixed.'
}

$shimText =
@"
// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <guiddef.h>

#define memcmp(a, b, c) (!InlineIsEqualGUID(a, b))

"@

$fixedShim = $false

if([System.IO.File]::Exists($proxyShimPath) -eq $true)
{
    $content = Get-Content $proxyShimPath

    if($content.Contains('#define memcmp(a, b, c) (!InlineIsEqualGUID(a, b))') -eq $false)
    {
        $fixedShim = $true
        Set-Content -Path $proxyShimPath -Value $shimText
    }
}

if($fixedShim -eq $true)
{
    Write-Host 'Fixed the proxy host memcmp shim.'
}
else
{
    Write-Host 'The proxy host memcmp shim is already fixed.'
}

$rootDir = [System.IO.Path]::GetDirectoryName($PSCommandPath)
$patchesDir = [System.IO.Path]::Combine($rootDir, 'OpenConsole', 'patches')
$openConsoleSrcPath = [System.IO.Path]::Combine($openConsolePath, 'src')

$controlcorepatch = [System.IO.Path]::Combine($patchesDir, 'ControlCore.cpp')
$controlcoredest = [System.IO.Path]::Combine($openConsoleSrcPath, 'cascadia', 'TerminalControl', 'ControlCore.cpp')
$keychord_h_patch = [System.IO.Path]::Combine($patchesDir, 'KeyChord.h')
$keychord_h_dest = [System.IO.Path]::Combine($openConsoleSrcPath, 'cascadia', 'TerminalControl', 'KeyChord.h')
$keychord_cpp_patch = [System.IO.Path]::Combine($patchesDir, 'KeyChord.cpp')
$keychord_cpp_dest = [System.IO.Path]::Combine($openConsoleSrcPath, 'cascadia', 'TerminalControl', 'KeyChord.cpp')
$keychord_idl_patch = [System.IO.Path]::Combine($patchesDir, 'KeyChord.idl')
$keychord_idl_dest = [System.IO.Path]::Combine($openConsoleSrcPath, 'cascadia', 'TerminalControl', 'KeyChord.idl')
$actionmap_h_patch = [System.IO.Path]::Combine($patchesDir, 'ActionMap.h')
$actionmap_h_dest = [System.IO.Path]::Combine($openConsoleSrcPath, 'cascadia', 'TerminalSettingsModel', 'ActionMap.h')
$terminalsettings_cpp_patch = [System.IO.Path]::Combine($patchesDir, 'TerminalSettings.cpp')
$terminalsettings_cpp_dest = [System.IO.Path]::Combine($openConsoleSrcPath, 'cascadia', 'TerminalSettingsModel', 'TerminalSettings.cpp')

Write-OpenConsolePatch -source $controlcorepatch -dest $controlcoredest
Write-OpenConsolePatch -source $keychord_h_patch -dest $keychord_h_dest
Write-OpenConsolePatch -source $keychord_cpp_patch -dest $keychord_cpp_dest
Write-OpenConsolePatch -source $keychord_idl_patch -dest $keychord_idl_dest
Write-OpenConsolePatch -source $actionmap_h_patch -dest $actionmap_h_dest
Write-OpenConsolePatch -source $terminalsettings_cpp_patch -dest $terminalsettings_cpp_dest