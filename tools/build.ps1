[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string] $Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
$buildDirectory = Join-Path $projectRoot 'build'
$editorOutputDirectory = Join-Path $buildDirectory 'In Libertas'
$modsOutputDirectory = Join-Path $buildDirectory 'Mods'
$nativeBridgeBuildDirectory = Join-Path $buildDirectory 'native_jacket_resolver_bridge'
$hookProject = Join-Path $projectRoot 'src\hook\InFalsusCustomSongHook.csproj'
$nativeBridgeSource = Join-Path $projectRoot 'src\hook\native_jacket_resolver_bridge\native_jacket_resolver_bridge.cpp'
$nativeBridgeObject = Join-Path $nativeBridgeBuildDirectory 'native_jacket_resolver_bridge.obj'
$nativeBridgePdb = Join-Path $nativeBridgeBuildDirectory 'native_jacket_resolver_bridge.pdb'
$nativeBridgeImportLibrary = Join-Path $nativeBridgeBuildDirectory 'InFalsusNativeJacketResolverBridge.lib'
$nativeBridgeOutput = Join-Path $modsOutputDirectory 'InFalsusNativeJacketResolverBridge.dll'
$vcpkgRoot = 'C:\vcpkg\vcpkg'
$toolchainFile = Join-Path $vcpkgRoot 'scripts\buildsystems\vcpkg.cmake'
$vswherePath = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$cmakePath = (Get-Command cmake -ErrorAction Stop).Source

if (-not (Test-Path -LiteralPath $toolchainFile -PathType Leaf)) {
    throw "vcpkg toolchain file not found: $toolchainFile"
}

if (-not (Test-Path -LiteralPath $vswherePath -PathType Leaf)) {
    throw "Visual Studio locator not found: $vswherePath"
}

$visualStudioRoot = & $vswherePath -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$developerCommand = Join-Path $visualStudioRoot 'Common7\Tools\VsDevCmd.bat'
if (-not $visualStudioRoot -or -not (Test-Path -LiteralPath $developerCommand -PathType Leaf)) {
    throw 'Visual Studio with the C++ x64 toolset was not found.'
}

function Invoke-VisualStudioCommand {
    param([string] $Command)

    $batchCommand = 'call "' + $developerCommand + '" -arch=x64 -host_arch=x64 >nul && ' + $Command
    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $sourceEnvironment = [Environment]::GetEnvironmentVariables()
    $seenEnvironmentNames = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)

    $startInfo.FileName = $env:ComSpec
    $startInfo.Arguments = '/d /c ' + $batchCommand
    $startInfo.UseShellExecute = $false
    $processEnvironment = $startInfo.EnvironmentVariables
    if ($null -eq $processEnvironment) {
        $processEnvironment = $startInfo.EnvironmentVariables
    }
    $processEnvironment.Clear()
    foreach ($environmentName in $sourceEnvironment.Keys) {
        if (-not $seenEnvironmentNames.Add([string]$environmentName)) {
            continue
        }
        if ([string]$environmentName -ieq 'Path') {
            $processEnvironment['PATH'] = [string]$sourceEnvironment['Path']
        } else {
            $processEnvironment[[string]$environmentName] = [string]$sourceEnvironment[$environmentName]
        }
    }

    $process = [System.Diagnostics.Process]::Start($startInfo)
    $process.WaitForExit()
    if ($process.ExitCode -ne 0) {
        throw "Visual Studio command failed: $Command"
    }
}

$configureCommand = "`"$cmakePath`" -S `"$projectRoot`" -B `"$buildDirectory`" -G `"Visual Studio 17 2022`" -A x64 -DCMAKE_TOOLCHAIN_FILE=`"$toolchainFile`" -DCMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG=`"$editorOutputDirectory`" -DCMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE=`"$editorOutputDirectory`""
$buildCommand = "`"$cmakePath`" --build `"$buildDirectory`" --config $Configuration --parallel"
$nativeBridgeOptimization = if ($Configuration -eq 'Debug') { '/Od /Zi' } else { '/O2' }
$nativeBridgeCommand = "cl.exe /nologo /std:c++20 /EHsc /LD /MD $nativeBridgeOptimization /Fo`"$nativeBridgeObject`" /Fd`"$nativeBridgePdb`" /Fe`"$nativeBridgeOutput`" `"$nativeBridgeSource`" /link /IMPLIB:`"$nativeBridgeImportLibrary`""

Invoke-VisualStudioCommand $configureCommand
Invoke-VisualStudioCommand $buildCommand
New-Item -ItemType Directory -Force -Path $nativeBridgeBuildDirectory, $modsOutputDirectory | Out-Null
Invoke-VisualStudioCommand $nativeBridgeCommand

& dotnet build $hookProject --configuration $Configuration --output $modsOutputDirectory --nologo
if ($LASTEXITCODE -ne 0) {
    throw "Hook build failed with exit code $LASTEXITCODE."
}
