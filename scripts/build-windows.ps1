# SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$LLVMDir,

    [string]$BuildDir = "build-win",
    [string]$InstallDir = "dist\windows",
    [string]$Configuration = "Release",
    [string]$Generator = "Ninja Multi-Config",
    [string]$Arch = "x64",
    [string]$Toolset = "",
    [string]$CompilerSourceDir = "",
    [string]$LoggerSourceDir = "",
    [switch]$BuildAnalyzerUnitTests,
    [switch]$PackageZip
)

$ErrorActionPreference = "Stop"

function Invoke-NativeCommand {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,

        [Parameter(ValueFromRemainingArguments = $true)]
        [string[]]$Arguments
    )

    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0)
    {
        throw "Command failed with exit code ${LASTEXITCODE}: $FilePath $($Arguments -join ' ')"
    }
}

function New-PatchedCMakePackageDir {
    param(
        [Parameter(Mandatory = $true)]
        [string]$SourceDir,

        [Parameter(Mandatory = $true)]
        [string]$DestinationDir,

        [Parameter(Mandatory = $true)]
        [string]$OldValue,

        [Parameter(Mandatory = $true)]
        [string]$NewValue,

        [string]$ImportPrefix = ""
    )

    New-Item -ItemType Directory -Force -Path $DestinationDir | Out-Null
    Copy-Item -Path (Join-Path $SourceDir "*") -Destination $DestinationDir -Recurse -Force

    Get-ChildItem -Path $DestinationDir -Recurse -Filter *.cmake | ForEach-Object {
        $content = Get-Content -Path $_.FullName -Raw
        $updated = $content.Replace($OldValue, $NewValue)
        if ($ImportPrefix -ne "")
        {
            # Updated Regex to handle 'REALPATH' form in LLVM 20.1.0 using non-greedy match .*?
            $updated = [regex]::Replace(
                $updated,
                '(?ms)# Compute the installation prefix relative to this file\..*?if\(_IMPORT_PREFIX STREQUAL "/"\)\r?\n\s*set\(_IMPORT_PREFIX ""\)\r?\nendif\(\)',
                "# Compute the installation prefix relative to this file.`nset(_IMPORT_PREFIX `"$ImportPrefix`")"
            )
            $updated = [regex]::Replace(
                $updated,
                '(?ms)# Compute the installation prefix from this LLVMConfig\.cmake file location\..*?get_filename_component\(LLVM_INSTALL_PREFIX "\$\{LLVM_INSTALL_PREFIX\}" PATH\)',
                "# Compute the installation prefix from this LLVMConfig.cmake file location.`nset(LLVM_INSTALL_PREFIX `"$ImportPrefix`")"
            )
            $updated = [regex]::Replace(
                $updated,
                '(?ms)# Compute the installation prefix from this LLVMConfig\.cmake file location\..*?get_filename_component\(CLANG_INSTALL_PREFIX "\$\{CLANG_INSTALL_PREFIX\}" PATH\)',
                "# Compute the installation prefix from this LLVMConfig.cmake file location.`nset(CLANG_INSTALL_PREFIX `"$ImportPrefix`")"
            )
        }
        if ($updated -ne $content)
        {
            Set-Content -Path $_.FullName -Value $updated -NoNewline
        }
    }
}

function Reset-BuildDirectoryIfGeneratorChanged {
    param(
        [Parameter(Mandatory = $true)]
        [string]$BuildDirectory,

        [Parameter(Mandatory = $true)]
        [string]$GeneratorName,

        [string]$PlatformName = "",

        [string]$ToolsetName = ""
    )

    $cachePath = Join-Path $BuildDirectory "CMakeCache.txt"
    if (-not (Test-Path $cachePath))
    {
        return
    }

    $cacheContent = Get-Content $cachePath -ErrorAction Stop
    $cachedGeneratorLine = $cacheContent | Where-Object { $_ -like "CMAKE_GENERATOR:INTERNAL=*" } | Select-Object -First 1
    $cachedPlatformLine = $cacheContent | Where-Object { $_ -like "CMAKE_GENERATOR_PLATFORM:INTERNAL=*" } | Select-Object -First 1
    $cachedToolsetLine = $cacheContent | Where-Object { $_ -like "CMAKE_GENERATOR_TOOLSET:INTERNAL=*" } | Select-Object -First 1

    $cachedGenerator = if ($cachedGeneratorLine) { $cachedGeneratorLine.Split("=", 2)[1] } else { "" }
    $cachedPlatform = if ($cachedPlatformLine) { $cachedPlatformLine.Split("=", 2)[1] } else { "" }
    $cachedToolset = if ($cachedToolsetLine) { $cachedToolsetLine.Split("=", 2)[1] } else { "" }

    if ($cachedGenerator -eq $GeneratorName -and $cachedPlatform -eq $PlatformName -and $cachedToolset -eq $ToolsetName)
    {
        return
    }

    Write-Host "Resetting build directory '$BuildDirectory' because cached generator/platform/toolset '$cachedGenerator'/'$cachedPlatform'/'$cachedToolset' does not match '$GeneratorName'/'$PlatformName'/'$ToolsetName'."
    Remove-Item -LiteralPath $BuildDirectory -Recurse -Force
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$resolvedBuildDir = Join-Path $repoRoot $BuildDir
$resolvedInstallDir = Join-Path $repoRoot $InstallDir
$installedLLVMDir = (Resolve-Path $LLVMDir).Path
$installedClangDir = Join-Path (Split-Path $installedLLVMDir -Parent) "clang"
$resolvedLLVMDir = $installedLLVMDir
$resolvedClangDir = $installedClangDir
$llvmRoot = Split-Path (Split-Path (Split-Path $resolvedLLVMDir -Parent) -Parent) -Parent
$llvmBinDir = Join-Path $llvmRoot "bin"

if (Test-Path $llvmBinDir)
{
    $env:PATH = "$llvmBinDir;$env:PATH"
}

$clangClPath = Join-Path $llvmBinDir "clang-cl.exe"
if (-not (Test-Path $clangClPath))
{
    throw "clang-cl.exe was not found in '$llvmBinDir'."
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere))
{
    throw "Unable to find vswhere.exe."
}

$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath)
{
    throw "Unable to find a Visual Studio installation with C++ build tools."
}

$platformForCache = if ($Generator -like "Visual Studio*") { $Arch } else { "" }
$toolsetForCache = if ($Generator -like "Visual Studio*") { $Toolset } else { "" }
Reset-BuildDirectoryIfGeneratorChanged -BuildDirectory $resolvedBuildDir -GeneratorName $Generator -PlatformName $platformForCache -ToolsetName $toolsetForCache

$diaguidsCandidate = Join-Path $vsPath "DIA SDK\lib\amd64\diaguids.lib"
if (-not (Test-Path $diaguidsCandidate))
{
    $diaguidsCandidate = Get-ChildItem -Path $vsPath -Recurse -Filter diaguids.lib -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -like "*DIA SDK\\lib\\amd64\\diaguids.lib" } |
        Select-Object -First 1 -ExpandProperty FullName
}

$staleDiaPath = "C:/Program Files (x86)/Microsoft Visual Studio/2019/Professional/DIA SDK/lib/amd64/diaguids.lib"
$llvmExportsPath = Join-Path $installedLLVMDir "LLVMExports.cmake"
if ((Test-Path $llvmExportsPath) -and (Test-Path $diaguidsCandidate))
{
    $llvmExportsContent = Get-Content -Path $llvmExportsPath -Raw
    if ($llvmExportsContent.Contains($staleDiaPath))
    {
        $patchedRoot = Join-Path $resolvedBuildDir "__llvm_cmake_patched"
        $patchedCMakeRoot = Join-Path $patchedRoot "lib\\cmake"
        $patchedLLVMDir = Join-Path $patchedCMakeRoot "llvm"
        $patchedClangDir = Join-Path $patchedCMakeRoot "clang"
        $replacementDiaPath = $diaguidsCandidate.Replace("\", "/")
        $importPrefix = $llvmRoot.Replace("\", "/")

        New-PatchedCMakePackageDir -SourceDir $installedLLVMDir -DestinationDir $patchedLLVMDir -OldValue $staleDiaPath -NewValue $replacementDiaPath -ImportPrefix $importPrefix
        New-PatchedCMakePackageDir -SourceDir $installedClangDir -DestinationDir $patchedClangDir -OldValue $staleDiaPath -NewValue $replacementDiaPath -ImportPrefix $importPrefix

        $resolvedLLVMDir = $patchedLLVMDir
        $resolvedClangDir = $patchedClangDir
    }
}

$cmakeArgs = @(
    "-S", $repoRoot,
    "-B", $resolvedBuildDir,
    "-G", $Generator,
    "-DLLVM_DIR=$resolvedLLVMDir",
    "-DBUILD_ANALYZER_UNIT_TESTS=$(if ($BuildAnalyzerUnitTests) { "ON" } else { "OFF" })"
)

if ($resolvedClangDir -and (Test-Path $resolvedClangDir))
{
    $cmakeArgs += "-DClang_DIR=$resolvedClangDir"
}

if ($CompilerSourceDir -ne "")
{
    $resolvedCompilerSourceDir = (Resolve-Path $CompilerSourceDir).Path
    $cmakeArgs += "-DFETCHCONTENT_SOURCE_DIR_CC=$resolvedCompilerSourceDir"
}

if ($LoggerSourceDir -ne "")
{
    $resolvedLoggerSourceDir = (Resolve-Path $LoggerSourceDir).Path
    $cmakeArgs += "-DFETCHCONTENT_SOURCE_DIR_CORETRACE_LOGGER=$resolvedLoggerSourceDir"
}

if ($Generator -like "Visual Studio*")
{
    $cmakeArgs += @("-A", $Arch)
    if ($Toolset -ne "")
    {
        $cmakeArgs += @("-T", $Toolset)
    }
}
else
{
    $cmakeArgs += @(
        "-DCMAKE_C_COMPILER=$clangClPath",
        "-DCMAKE_CXX_COMPILER=$clangClPath"
    )
}

if ($Generator -like "Visual Studio*")
{
    Invoke-NativeCommand cmake @cmakeArgs
}
else
{
    $devShell = Join-Path $vsPath "Common7\Tools\Launch-VsDevShell.ps1"
    if (-not (Test-Path $devShell))
    {
        throw "Unable to find Launch-VsDevShell.ps1 at '$devShell'."
    }

    . $devShell -Arch amd64 -HostArch amd64 | Out-Null
    Invoke-NativeCommand cmake @cmakeArgs
}

Invoke-NativeCommand cmake --build $resolvedBuildDir --config $Configuration
Invoke-NativeCommand cmake --install $resolvedBuildDir --config $Configuration --prefix $resolvedInstallDir

$binaryPath = Join-Path $resolvedInstallDir "bin\stack_usage_analyzer.exe"
if (-not (Test-Path $binaryPath))
{
    throw "Expected output binary was not produced: $binaryPath"
}

if ($PackageZip)
{
    $zipPath = Join-Path $repoRoot "coretrace-stack-analyzer-windows-$Configuration.zip"
    if (Test-Path $zipPath)
    {
        Remove-Item -LiteralPath $zipPath -Force
    }
    Compress-Archive -Path (Join-Path $resolvedInstallDir "*") -DestinationPath $zipPath
    Write-Host "Created package: $zipPath"
}

Write-Host "Build completed successfully."
Write-Host "Executable: $binaryPath"