# SDRNative
# Windows build
#
# Copyright (c) 2026 Ruben Roy
# SPDX-License-Identifier: MPL-2.0
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $Root

function Assert-NativeSuccess([string]$Step) {
    if ($LASTEXITCODE -ne 0) {
        throw "$Step failed (exit $LASTEXITCODE)"
    }
}

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    throw "cmake not found; install CMake and Visual Studio C++ Build Tools"
}

$Vcpkg = $env:VCPKG_ROOT
if (-not $Vcpkg) {
    $Candidates = @("C:\vcpkg", "$env:USERPROFILE\vcpkg")
    foreach ($Candidate in $Candidates) {
        if (Test-Path (Join-Path $Candidate "scripts\buildsystems\vcpkg.cmake")) {
            $Vcpkg = $Candidate
            break
        }
    }
}
if (-not $Vcpkg) {
    throw "VCPKG_ROOT not set; point it to a vcpkg installation"
}
$VcpkgExe = Join-Path $Vcpkg "vcpkg.exe"
if (-not (Test-Path $VcpkgExe)) { throw "vcpkg.exe not found at $VcpkgExe" }

& $VcpkgExe install libusb:x64-windows
Assert-NativeSuccess "vcpkg libusb install"
$Toolchain = Join-Path $Vcpkg "scripts\buildsystems\vcpkg.cmake"
cmake -S . -B build-win -A x64 `
    -DCMAKE_TOOLCHAIN_FILE="$Toolchain" `
    -DVCPKG_TARGET_TRIPLET=x64-windows `
    -DBUILD_SHARED_LIBS=ON `
    -DSDRNATIVE_REQUIRE_LIBUSB=ON `
    -DSDRNATIVE_BUILD_TESTS=ON `
    -DSDRNATIVE_BUILD_EXAMPLES=ON
Assert-NativeSuccess "CMake configure"
cmake --build build-win --config Release --parallel
Assert-NativeSuccess "CMake/MSBuild Release build"

$ReleaseDir = Join-Path $Root "build-win\Release"
$LibusbDll = Join-Path $Vcpkg "installed\x64-windows\bin\libusb-1.0.dll"
if (Test-Path $LibusbDll) {
    Copy-Item $LibusbDll $ReleaseDir -Force
}

# Build Tools can exist without runtime DLLs
# copy matching x64 CRT beside Release binaries when available
$VsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $VsWhere) {
    $VsPath = & $VsWhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath
    if ($VsPath) {
        $Crt = Get-ChildItem (Join-Path $VsPath "VC\Redist\MSVC") -Recurse -File -Filter "VCRUNTIME140.dll" -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match '\\x64\\Microsoft\.VC14[0-9]\.CRT\\VCRUNTIME140\.dll$' } |
            Sort-Object FullName -Descending |
            Select-Object -First 1
        if ($Crt) {
            Copy-Item (Join-Path $Crt.Directory.FullName "*.dll") $ReleaseDir -Force
        }
    }
}

if (-not (Test-Path (Join-Path $ReleaseDir "VCRUNTIME140.dll")) -and
    -not (Test-Path "$env:SystemRoot\System32\VCRUNTIME140.dll")) {
    throw "MSVC x64 runtime not found; install Microsoft.VCRedist.2015+.x64"
}

ctest --test-dir build-win -C Release --output-on-failure
Assert-NativeSuccess "CTest Release suite"

Write-Host ""
Write-Host "build complete"
Write-Host "probe:   $Root\build-win\Release\sdrn_probe.exe"
Write-Host "DVB-T:   $Root\build-win\Release\sdrn_dvbt_capture.exe <freq_mhz> <6|7|8> <seconds> <output.ts>"
Write-Host "IQ:      $Root\build-win\Release\sdrn_iq_capture.exe <freq_mhz> <sample_rate_hz> <seconds> <output.iq>"
