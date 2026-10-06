[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string] $Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
$vcpkgRoot = 'C:\vcpkg\vcpkg'
$binaryDirectory = Join-Path $projectRoot 'build\In Libertas'
$applicationPath = Join-Path $binaryDirectory 'In Libertas.exe'
$runtimeDirectory = if ($Configuration -eq 'Debug') {
    Join-Path $vcpkgRoot 'installed\x64-windows\debug\bin'
} else {
    Join-Path $vcpkgRoot 'installed\x64-windows\bin'
}
$pluginDirectory = Join-Path $vcpkgRoot 'installed\x64-windows\Qt6\plugins'

& (Join-Path $PSScriptRoot 'build.ps1') -Configuration $Configuration

if (-not (Test-Path -LiteralPath $applicationPath -PathType Leaf)) {
    throw "Built application not found: $applicationPath"
}

$env:PATH = "$runtimeDirectory;$env:PATH"
$env:QT_PLUGIN_PATH = $pluginDirectory
$env:QT_MEDIA_BACKEND = 'ffmpeg'
& $applicationPath
