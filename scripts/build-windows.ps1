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
    if (-not (Test-Path $cachePath)) { return }

    $cacheContent = Get-Content $cachePath -ErrorAction Stop
    $cachedGeneratorLine = $cacheContent | Where-Object { $_ -like "CMAKE_GENERATOR:INTERNAL=*" } | Select-Object -First 1
    $cachedPlatformLine = $cacheContent | Where-Object { $_ -like "CMAKE_GENERATOR_PLATFORM:INTERNAL=*" } | Select-Object -First 1
    $cachedToolsetLine = $cacheContent | Where-Object { $_ -like "CMAKE_GENERATOR_TOOLSET:INTERNAL=*" } | Select-Object -First 1

    $cachedGenerator = if ($cachedGeneratorLine) { $cachedGeneratorLine.Split("=", 2)[1] } else { "" }
    $cachedPlatform = if ($cachedPlatformLine) { $cachedPlatformLine.Split("=", 2)[1] } else { "" }
    $cachedToolset = if ($cachedToolsetLine) { $cachedToolsetLine.Split("=", 2)[1] } else { "" }

    if ($cachedGenerator -eq $GeneratorName -and $cachedPlatform -eq $PlatformName -and $cachedToolset -eq $ToolsetName) {
        return
    }

    Write-Host "Resetting build directory '$BuildDirectory' (Generator mismatch)."
    Remove-Item -LiteralPath $BuildDirectory -Recurse -Force
}

# --- Path Resolution ---
$repoRoot = Split-Path -Parent $PSScriptRoot
$resolvedBuildDir = Join-Path $repoRoot $BuildDir
$resolvedInstallDir = Join-Path $repoRoot $InstallDir

# LLVMDir is expected to be the directory containing LLVMConfig.cmake
$initialLLVMDir = (Resolve-Path $LLVMDir).Path
$initialClangDir = Join-Path (Split-Path $initialLLVMDir -Parent) "clang"

# These variables may be updated if we patch the package files
$finalLLVMDir = $initialLLVMDir
$finalClangDir = $initialClangDir

# Compute LLVM root from ...\lib\cmake\llvm
$llvmRoot = Split-Path (Split-Path (Split-Path $initialLLVMDir -Parent) -Parent) -Parent
$llvmBinDir = Join-Path $llvmRoot "bin"

# --- Add LLVM to PATH ---
if (Test-Path $llvmBinDir) {
    $env:PATH = "$llvmBinDir;$env:PATH"
}

$clangClPath = Join-Path $llvmBinDir "clang-cl.exe"
if (-not (Test-Path $clangClPath)) {
    throw "clang-cl.exe not found in '$llvmBinDir'."
}

# --- Find Visual Studio ---
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) {
    throw "Visual Studio C++ build tools not found."
}

# --- Reset Build if needed ---
$platformForCache = if ($Generator -like "Visual Studio*") { $Arch } else { "" }
$toolsetForCache = if ($Generator -like "Visual Studio*") { $Toolset } else { "" }

Reset-BuildDirectoryIfGeneratorChanged `
    -BuildDirectory $resolvedBuildDir `
    -GeneratorName $Generator `
    -PlatformName $platformForCache `
    -ToolsetName $toolsetForCache

# --- Patching Logic (LLVM 20.1.0 stale DIA path fix) ---
$diaguidsCandidate = Join-Path $vsPath "DIA SDK\lib\amd64\diaguids.lib"
$staleDiaPath = "C:/Program Files (x86)/Microsoft Visual Studio/2019/Professional/DIA SDK/lib/amd64/diaguids.lib"
$llvmExportsPath = Join-Path $initialLLVMDir "LLVMExports.cmake"

if ((Test-Path $llvmExportsPath) -and (Test-Path $diaguidsCandidate)) {
    $llvmExportsContent = Get-Content -Path $llvmExportsPath -Raw

    if ($llvmExportsContent.Contains($staleDiaPath)) {
        Write-Host "Detected stale DIA SDK paths. Creating patched CMake files..."

        $patchedRoot = Join-Path $resolvedBuildDir "__llvm_cmake_patched"
        $patchedLLVMDir = Join-Path $patchedRoot "llvm"
        $patchedClangDir = Join-Path $patchedRoot "clang"
        $replacementDiaPath = $diaguidsCandidate.Replace("\", "/")
        $importPrefix = $llvmRoot.Replace("\", "/")

        New-PatchedCMakePackageDir `
            -SourceDir $initialLLVMDir `
            -DestinationDir $patchedLLVMDir `
            -OldValue $staleDiaPath `
            -NewValue $replacementDiaPath `
            -ImportPrefix $importPrefix

        if (Test-Path $initialClangDir) {
            New-PatchedCMakePackageDir `
                -SourceDir $initialClangDir `
                -DestinationDir $patchedClangDir `
                -OldValue $staleDiaPath `
                -NewValue $replacementDiaPath `
                -ImportPrefix $importPrefix

            $finalClangDir = $patchedClangDir
        } else {
            $finalClangDir = $patchedLLVMDir
        }

        $finalLLVMDir = $patchedLLVMDir
    }
}

# --- Validate package files early ---
$llvmConfigPath = Join-Path $finalLLVMDir "LLVMConfig.cmake"
if (-not (Test-Path $llvmConfigPath)) {
    throw "LLVMConfig.cmake not found at: $llvmConfigPath"
}

$clangConfigPath = Join-Path $finalClangDir "ClangConfig.cmake"

# --- CMake Arguments ---
$cmakeArgs = @(
    "-S", $repoRoot,
    "-B", $resolvedBuildDir,
    "-G", $Generator,
    "-DLLVM_DIR=$finalLLVMDir",
    "-DClang_DIR=$finalClangDir",
    "-DCMAKE_PREFIX_PATH=$llvmRoot",
    "-DBUILD_ANALYZER_UNIT_TESTS=$(if ($BuildAnalyzerUnitTests) { "ON" } else { "OFF" })"
)

if ($CompilerSourceDir -ne "") {
    $cmakeArgs += "-DFETCHCONTENT_SOURCE_DIR_CC=$((Resolve-Path $CompilerSourceDir).Path)"
}

if ($LoggerSourceDir -ne "") {
    $cmakeArgs += "-DFETCHCONTENT_SOURCE_DIR_CORETRACE_LOGGER=$((Resolve-Path $LoggerSourceDir).Path)"
}

if ($Generator -like "Visual Studio*") {
    $cmakeArgs += @("-A", $Arch)
    if ($Toolset -ne "") {
        $cmakeArgs += @("-T", $Toolset)
    }
} else {
    $cmakeArgs += @(
        "-DCMAKE_C_COMPILER=$clangClPath",
        "-DCMAKE_CXX_COMPILER=$clangClPath"
    )
}

# --- Diagnostics ---
Write-Host "LLVMDir param: $LLVMDir"
Write-Host "Resolved initial LLVM dir: $initialLLVMDir"
Write-Host "Resolved final LLVM dir: $finalLLVMDir"
Write-Host "Resolved final Clang dir: $finalClangDir"
Write-Host "clang-cl path: $clangClPath"
Write-Host "clang-cl exists: $(Test-Path $clangClPath)"
Write-Host "llvmRoot: $llvmRoot"
Write-Host "llvmBinDir: $llvmBinDir"
Write-Host "LLVMConfig path: $llvmConfigPath"
Write-Host "LLVMConfig exists: $(Test-Path $llvmConfigPath)"
Write-Host "ClangConfig path: $clangConfigPath"
Write-Host "ClangConfig exists: $(Test-Path $clangConfigPath)"
Write-Host "cmakeArgs:"
$cmakeArgs | ForEach-Object { Write-Host "  $_" }

if (Test-Path $finalLLVMDir) {
    Write-Host "`nContents of final LLVM dir:"
    Get-ChildItem $finalLLVMDir | Select-Object Name | Format-Table -AutoSize
} else {
    Write-Host "final LLVM dir does not exist"
}

if (Test-Path $finalClangDir) {
    Write-Host "`nContents of final Clang dir:"
    Get-ChildItem $finalClangDir | Select-Object Name | Format-Table -AutoSize
} else {
    Write-Host "final Clang dir does not exist"
}

# --- Execution ---
if ($Generator -like "Visual Studio*") {
    Invoke-NativeCommand cmake @cmakeArgs
} else {
    $devShell = Join-Path $vsPath "Common7\Tools\Launch-VsDevShell.ps1"

    if (Test-Path $devShell) {
        try {
            . $devShell -Arch amd64 -HostArch amd64 | Out-Null
        } catch {
            Write-Warning "Could not start Developer PowerShell using script path '$devShell'. Attempting to continue."
        }
    } else {
        Write-Warning "Launch-VsDevShell.ps1 not found at '$devShell'. Attempting to continue."
    }

    Invoke-NativeCommand cmake @cmakeArgs
}

Invoke-NativeCommand cmake --build $resolvedBuildDir --config $Configuration
Invoke-NativeCommand cmake --install $resolvedBuildDir --config $Configuration --prefix $resolvedInstallDir

# --- Packaging ---
if ($PackageZip) {
    $zipPath = Join-Path $repoRoot "coretrace-stack-analyzer-windows-$Configuration.zip"
    if (Test-Path $zipPath) {
        Remove-Item $zipPath -Force
    }

    Compress-Archive -Path (Join-Path $resolvedInstallDir "*") -DestinationPath $zipPath
}

Write-Host "Build completed successfully."