param(
    [Parameter(Mandatory=$false)]
    [string] $winrtver = '2.0.211028.7'
)

$rootDir = [System.IO.Path]::GetDirectoryName($PSCommandPath)
$openConsolePath = [System.IO.Path]::Combine($rootDir, 'external', 'terminal')
$oldVer = '2.0.210309.3'
$postProps = [System.IO.Path]::Combine($openConsolePath, 'src', 'cppwinrt.build.post.props')
$preProps = [System.IO.Path]::Combine($openConsolePath, 'src', 'cppwinrt.build.pre.props')
$nugetConfig = [System.IO.Path]::Combine($openConsolePath, 'NuGet.config')
$configFiles = Get-ChildItem $openConsolePath packages.config -rec

foreach ($file in $configFiles)
{
    (Get-Content $file.PSPath) |
    Foreach-Object { $_ -replace $oldVer, $winrtver } |
    Set-Content $file.PSPath
}

(Get-Content $postProps).Replace($oldVer, $winrtver) | Set-Content $postProps
(Get-Content $preProps).Replace($oldVer, $winrtver) | Set-Content $preProps
(Get-Content $nugetConfig).Replace('https://pkgs.dev.azure.com/ms/terminal/_packaging/TerminalDependencies/nuget/v3/index.json', 'https://api.nuget.org/v3/index.json') | Set-Content $nugetConfig
