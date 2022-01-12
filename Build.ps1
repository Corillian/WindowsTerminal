[CmdletBinding(DefaultParameterSetName = 'BuildOne')]
param(
    [Parameter(Mandatory = $false, ParameterSetName = 'BuildOne')]
    [Parameter(Mandatory = $false, ParameterSetName = 'Pack')]
    [switch] $restore,

    [Parameter(Mandatory = $false, ParameterSetName = 'BuildOne')]
    [Parameter(Mandatory = $false, ParameterSetName = 'Pack')]
    [switch] $clean,

    [Parameter(Mandatory = $false, ParameterSetName = 'BuildOne')]
    [Parameter(Mandatory = $false, ParameterSetName = 'Pack')]
    [switch] $skipbuild,

    [Parameter(Mandatory = $false, ParameterSetName = 'BuildOne')]
    [Parameter(Mandatory = $false, ParameterSetName = 'Pack')]
    [switch] $skipdeps,

    [Parameter(Mandatory = $false, ParameterSetName = 'BuildOne')]
    [string] $configuration = 'Debug',

    [Parameter(Mandatory = $false, ParameterSetName = 'BuildOne')]
    [string] $platform = 'x64',

    [Parameter(Mandatory = $true, ParameterSetName = 'Pack')]
    [switch] $pack
)

$rootDir = [System.IO.Path]::GetDirectoryName($PSCommandPath)
$openConsolePath = [System.IO.Path]::Combine($rootDir, 'external', 'terminal')
$openConsoleSolution = [System.IO.Path]::Combine($openConsolePath, 'OpenConsole.sln')
$windowsTerminalSolution = [System.IO.Path]::Combine($rootDir, 'WindowsTerminal.sln')
$buildPath = [System.IO.Path]::Combine($rootDir, '_build')

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

if($pack -eq $true)
{
    if($clean -eq $true)
    {
        & $msbuild $openConsoleSolution -p:Configuration=Release -p:Platform=x64 -t:Clean
        & $msbuild $openConsoleSolution -p:Configuration=Release -p:Platform=x86 -t:Clean
        & $msbuild $openConsoleSolution -p:Configuration=Release -p:Platform=arm64 -t:Clean
    
        & $msbuild $windowsTerminalSolution -p:Configuration=Release -p:Platform=x64 -t:Clean
        & $msbuild $windowsTerminalSolution -p:Configuration=Release -p:Platform=x86 -t:Clean
        & $msbuild $windowsTerminalSolution -p:Configuration=Release -p:Platform=arm64 -t:Clean
    }

    if($restore -eq $true)
    {
        & $msbuild $openConsoleSolution -p:Configuration=Release -p:Platform=x64 -p:RestorePackagesConfig=true -t:Restore
        & $msbuild $openConsoleSolution -p:Configuration=Release -p:Platform=x86 -p:RestorePackagesConfig=true -t:Restore
        & $msbuild $openConsoleSolution -p:Configuration=Release -p:Platform=arm64 -p:RestorePackagesConfig=true -t:Restore

        & $windowsTerminalSolution $openConsoleSolution -p:Configuration=Release -p:Platform=x64 -p:RestorePackagesConfig=true -t:Restore
        & $windowsTerminalSolution $openConsoleSolution -p:Configuration=Release -p:Platform=x86 -p:RestorePackagesConfig=true -t:Restore
        & $windowsTerminalSolution $openConsoleSolution -p:Configuration=Release -p:Platform=arm64 -p:RestorePackagesConfig=true -t:Restore
    }

    if($skipbuild -eq $false)
    {
        if($skipdeps -eq $false)
        {
            Write-Host "Building OpenConsole.sln Release|x64"

            & $msbuild $openConsoleSolution -p:Configuration=Release -p:Platform=x64 -p:RestorePackagesConfig=true -t:Conhost\OpenConsoleProxy '-t:Shared\Virtual Terminal\TerminalInput' -t:Shared\Rendering\RendererUia -t:Terminal\TerminalConnection -t:Terminal\Control\TerminalCore -t:Terminal\Settings\Microsoft_Terminal_Settings_Model

            if($LastExitCode -ne 0)
            {
                throw 'Error: Failed to build x64 dependencies.'
            }

            Write-Host "Building OpenConsole.sln Release|x86"

            & $msbuild $openConsoleSolution -p:Configuration=Release -p:Platform=x86 -p:RestorePackagesConfig=true -t:Conhost\OpenConsoleProxy '-t:Shared\Virtual Terminal\TerminalInput' -t:Shared\Rendering\RendererUia -t:Terminal\TerminalConnection -t:Terminal\Control\TerminalCore -t:Terminal\Settings\Microsoft_Terminal_Settings_Model

            if($LastExitCode -ne 0)
            {
                throw 'Error: Failed to build x86 dependencies.'
            }

            Write-Host "Building OpenConsole.sln Release|arm64"

            & $msbuild $openConsoleSolution -p:Configuration=Release -p:Platform=arm64 -p:RestorePackagesConfig=true -t:Conhost\OpenConsoleProxy '-t:Shared\Virtual Terminal\TerminalInput' -t:Shared\Rendering\RendererUia -t:Terminal\TerminalConnection -t:Terminal\Control\TerminalCore -t:Terminal\Settings\Microsoft_Terminal_Settings_Model

            if($LastExitCode -ne 0)
            {
                throw 'Error: Failed to build arm64 dependencies.'
            }
        }

        Write-Host "Building WindowsTerminal.sln Release|x64"

        & $msbuild $windowsTerminalSolution -p:Configuration=Release -p:Platform=x64 -p:RestorePackagesConfig=true -p:PublishReadyToRun=false

        if($LastExitCode -ne 0)
        {
            throw 'Error: Failed to build x64 binaries.'
        }

        Write-Host "Building WindowsTerminal.sln Release|x86"

        & $msbuild $windowsTerminalSolution -p:Configuration=Release -p:Platform=x86 -p:RestorePackagesConfig=true -p:PublishReadyToRun=false

        if($LastExitCode -ne 0)
        {
            throw 'Error: Failed to build x86 binaries.'
        }

        Write-Host "Building WindowsTerminal.sln Release|arm64"

        & $msbuild $windowsTerminalSolution -p:Configuration=Release -p:Platform=arm64 -p:RestorePackagesConfig=true -p:PublishReadyToRun=false

        if($LastExitCode -ne 0)
        {
            throw 'Error: Failed to build arm64 binaries.'
        }

        & $msbuild $windowsTerminalSolution -p:Configuration=Release -p:Platform=AnyCPU -p:RestorePackagesConfig=true -p:PublishReadyToRun=false -t:Microsoft_Terminal_TerminalConnection_Projection

        if($LastExitCode -ne 0)
        {
            throw 'Error: Failed to build the AnyCPU Microsoft.Terminal.TerminalConnection.Projection.'
        }
    }

    $anyCpuGen = [System.IO.Path]::Combine($buildPath, 'AnyCPU', 'Release', 'AnyCpuGenerator', 'bin', 'AnyCpuGenerator.exe')
    $x64ControlProj = [System.IO.Path]::Combine($buildPath, 'x64', 'Release', 'Microsoft.Terminal.Control.Projection', 'bin', 'Microsoft.Terminal.Control.Projection.dll')
    $anyCpuControlProj = [System.IO.Path]::Combine($buildPath, 'AnyCPU', 'Release', 'Microsoft.Terminal.Control.Projection', 'bin', 'Microsoft.Terminal.Control.Projection.dll')
    $x64SettingsProj = [System.IO.Path]::Combine($buildPath, 'x64', 'Release', 'Microsoft.Terminal.Settings.Model.Projection', 'bin', 'Microsoft.Terminal.Settings.Model.Projection.dll')
    $anyCpuSettingsProj = [System.IO.Path]::Combine($buildPath, 'AnyCPU', 'Release', 'Microsoft.Terminal.Settings.Model.Projection', 'bin', 'Microsoft.Terminal.Settings.Model.Projection.dll')

    Write-Host "Generating AnyCPU assembly for $x64ControlProj at $anyCpuControlProj"

    & $anyCpuGen $x64ControlProj $anyCpuControlProj

    if($LastExitCode -ne 0)
    {
        throw 'Error: Failed to generate AnyCPU Microsoft.Terminal.Control.Projection.dll.'
    }

    Write-Host "Generating AnyCPU assembly for $x64SettingsProj at $anyCpuSettingsProj"

    & $anyCpuGen $x64SettingsProj $anyCpuSettingsProj

    if($LastExitCode -ne 0)
    {
        throw 'Error: Failed to generate AnyCPU Microsoft.Terminal.Settings.Model.Projection.dll.'
    }

    & $msbuild Microsoft.Terminal.Control.Projection\Microsoft.Terminal.Control.Projection.csproj -p:Configuration=Release -p:Platform=x64 -t:Pack

    if($LastExitCode -ne 0)
    {
        throw 'Error: Failed to generate WindowsTerminal.WinUI3.Control nupkg.'
    }

    & $msbuild Microsoft.Terminal.Settings.Model.Projection\Microsoft.Terminal.Settings.Model.Projection.csproj -p:Configuration=Release -p:Platform=x64 -t:Pack

    if($LastExitCode -ne 0)
    {
        throw 'Error: Failed to generate WindowsTerminal.WinUI3.Settings.Model nupkg.'
    }
}
else
{
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
}
