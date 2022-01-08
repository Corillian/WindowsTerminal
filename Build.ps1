param(
    [switch] $restore,
    [switch] $clean,
    [switch] $skipbuild,
    [switch] $skipdeps,
    [Parameter(Mandatory=$false)]
    [string] $configuration = 'Debug',
    [Parameter(Mandatory=$false)]
    [string] $platform = 'x64'
)

$rootDir = [System.IO.Path]::GetDirectoryName($PSCommandPath)
$openConsolePath = [System.IO.Path]::Combine($rootDir, 'external', 'terminal')
$openConsoleSolution = [System.IO.Path]::Combine($openConsolePath, 'OpenConsole.sln')
$windowsTerminalSolution = [System.IO.Path]::Combine($rootDir, 'WindowsTerminal.sln')

if([System.IO.File]::Exists($openConsoleSolution) -eq $false)
{
    throw "Failed to locate OpenConsole.sln at path '$openConsolePath'. Make sure you've pulled submodules."
}

try
{
    $latestVsInstallationInfo = Get-VSSetupInstance -All | Sort-Object -Property InstallationVersion -Descending | Select-Object -First 1
}
catch
{
    Install-Module VSSetup -Scope CurrentUser -Force

    $latestVsInstallationInfo = Get-VSSetupInstance -All | Sort-Object -Property InstallationVersion -Descending | Select-Object -First 1
}

Write-Host "Found $($latestVsInstallationInfo.DisplayName)"

if($null -eq $latestVsInstallationInfo)
{
    throw 'Error: Failed to locate an instance of MSBuild.'
}

$msbuild = [System.IO.Path]::Combine($latestVsInstallationInfo.InstallationPath, 'MSBuild', 'Current', 'Bin', 'amd64', 'MSBuild.exe')

if([System.IO.File]::Exists($msbuild) -eq $false)
{
    throw 'Error: Failed to locate an instance of MSBuild.'
}

Write-Host "Found MSBuild at $msbuild"

if($clean -eq $true)
{
    if($skipdeps -eq $false)
    {
        & $msbuild $openConsoleSolution -p:Configuration=$configuration -p:Platform=$platform -p:RestorePackagesConfig=true -t:Clean
    }
    
    & $msbuild $windowsTerminalSolution -p:Configuration=$configuration -p:Platform=$platform -p:RestorePackagesConfig=true -t:Clean
}

if($restore -eq $true)
{
    if($skipdeps -eq $false)
    {
        & $msbuild $openConsoleSolution -p:Configuration=$configuration -p:Platform=$platform -p:RestorePackagesConfig=true -t:Restore
    }
    
    & $msbuild $windowsTerminalSolution -p:Configuration=$configuration -p:Platform=$platform -p:RestorePackagesConfig=true -t:Restore
}

if($skipbuild -eq $false)
{
    if($skipdeps -eq $false)
    {
        & $msbuild $openConsoleSolution -p:Configuration=$configuration -p:Platform=$platform -p:RestorePackagesConfig=true -t:Conhost\OpenConsoleProxy '-t:Shared\Virtual Terminal\TerminalInput' -t:Shared\Rendering\RendererUia -t:Terminal\TerminalConnection -t:Terminal\Control\TerminalCore -t:Terminal\Settings\Microsoft_Terminal_Settings_Model

        if($LastExitCode -ne 0)
        {
            throw 'Error: Failed to build dependencies.'
        }
    }

    Write-Host "Building WindowsTerminal.sln $configuration|$platform"

    & $msbuild $windowsTerminalSolution -p:Configuration=$configuration -p:Platform=$platform -p:RestorePackagesConfig=true -p:PublishReadyToRun=false
}
