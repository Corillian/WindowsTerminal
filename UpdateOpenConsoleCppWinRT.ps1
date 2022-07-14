param(
    [Parameter(Mandatory=$false)]
    [string] $winrtver = '2.0.220608.4'
)

$rootDir = [System.IO.Path]::GetDirectoryName($PSCommandPath)
$openConsolePath = [System.IO.Path]::Combine($rootDir, 'external', 'terminal')
$oldVer = '2.0.210825.3'
$nugetProps = [System.IO.Path]::Combine($openConsolePath, 'src', 'common.nugetversions.props')
$nugetTargets = [System.IO.Path]::Combine($openConsolePath, 'src', 'common.nugetversions.targets')
$nugetConfig = [System.IO.Path]::Combine($openConsolePath, 'NuGet.config')
$configFiles = Get-ChildItem $openConsolePath packages.config -rec

foreach ($file in $configFiles)
{
    (Get-Content $file.PSPath) |
    Foreach-Object { $_ -replace $oldVer, $winrtver } |
    Set-Content $file.PSPath
}

(Get-Content $nugetProps).Replace($oldVer, $winrtver) | Set-Content $nugetProps
(Get-Content $nugetTargets).Replace($oldVer, $winrtver) | Set-Content $nugetTargets
(Get-Content $nugetConfig).Replace('https://pkgs.dev.azure.com/ms/terminal/_packaging/TerminalDependencies/nuget/v3/index.json', 'https://api.nuget.org/v3/index.json') | Set-Content $nugetConfig
