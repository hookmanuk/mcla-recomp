# Builds the Windows VR release: a folder and a zip with no game files in them. On first run the
# launcher asks for the user's own disc image; setup\setup.ps1 extracts it and builds the game there.
#
#   .\packaging\windows\package.ps1 -Version 0.1
#
# Needs this repository built once (scripts\make_pc.ps1: the SDK installed, the DLSS bridge and the
# OpenXR loader beside out\build\win-amd64-release\mcla.exe) and the Build Tools for the launcher.
param([string]$Version = "dev")

$ErrorActionPreference = "Stop"
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$name = "mcla-vr-$Version-win64"
$dist = "$root\out\release\$name"
$downloads = "$root\out\packaging_dl"
$llvm = Get-ChildItem "$root\tools" -Directory -Filter "clang+llvm-*" | Select-Object -First 1
$sdk = "$root\third_party\rexglue-sdk"
$sdkInstall = "$sdk\out\install\win-amd64"
$built = "$root\out\build\win-amd64-release"
if (-not $llvm) { throw "no tools\clang+llvm-*: run scripts\make_pc.ps1 first" }
foreach ($file in "$sdkInstall\bin\rexglue.exe", "$built\bbport_dlss.dll", "$built\nvngx_dlss.dll", "$built\openxr_loader.dll") {
    if (-not (Test-Path $file)) { throw "missing ${file}: build the repository first (scripts\make_pc.ps1)" }
}

function Fetch([string]$file, [string]$url) {
    New-Item -ItemType Directory -Force $downloads | Out-Null
    if (-not (Test-Path "$downloads\$file")) { curl.exe -sSL --fail -o "$downloads\$file" $url; if ($LASTEXITCODE) { throw "download failed: $url" } }
    "$downloads\$file"
}
function Unzip([string]$zip, [string]$dest) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::ExtractToDirectory($zip, $dest)
}

if (Test-Path $dist) { Remove-Item -Recurse -Force $dist }
New-Item -ItemType Directory -Force "$dist\setup\project", "$dist\setup\tools\llvm\bin", "$dist\setup\runtime\licenses",
    "$dist\setup\licenses" | Out-Null

# --- the launcher (MSVC's libraries through the Build Tools, the project's Clang)
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
$launcherCmd = "$root\out\launcher.cmd"
@(
    "@call `"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul || exit /b 1",
    "`"$($llvm.FullName)\bin\clang++.exe`" -O2 -std=c++20 -fuse-ld=lld -municode `"$PSScriptRoot\launcher.cpp`" -o `"$dist\Midnight Club LA VR.exe`" -lole32 -lshell32 -luser32 -Wl,/subsystem:windows || exit /b 1"
) | Set-Content -Encoding Ascii $launcherCmd
cmd /c $launcherCmd
if ($LASTEXITCODE) { throw "the launcher did not build" }

# --- setup: the script, the extractor and the project's own source (no generated code, no game data)
Copy-Item "$PSScriptRoot\setup.ps1", "$root\scripts\extract_game.py" "$dist\setup"
foreach ($item in "CMakeLists.txt", "CMakePresets.json", "mcla_manifest.toml", "LICENSE", "README.md", "src", "config") {
    Copy-Item -Recurse "$root\$item" "$dist\setup\project"
}
New-Item -ItemType Directory -Force "$dist\setup\project\generated" | Out-Null
Copy-Item "$root\generated\rexglue.cmake" "$dist\setup\project\generated"

# --- the ReXGlue SDK (BSD-3, this project's VR fork), installed: headers, libraries, the recompiler
Copy-Item -Recurse $sdkInstall "$dist\setup\sdk"
Copy-Item "$sdk\LICENSE" "$dist\setup\licenses\rexglue-sdk-LICENSE.txt"
Get-ChildItem "$sdk\thirdparty" -Directory | ForEach-Object {
    $license = Get-ChildItem $_.FullName -File | Where-Object { $_.Name -match '^(LICEN[CS]E|COPYING)' } | Select-Object -First 1
    if ($license) { Copy-Item $license.FullName "$dist\setup\licenses\$($_.Name)-$($license.Name).txt" }
}

# --- tools: the programs of LLVM the build calls (clang++ and llvm-ranlib are copies setup makes),
# CMake, Ninja and Python (embeddable), each with its licence
foreach ($exe in "clang.exe", "lld-link.exe", "llvm-ar.exe", "llvm-rc.exe") { Copy-Item "$($llvm.FullName)\bin\$exe" "$dist\setup\tools\llvm\bin" }
New-Item -ItemType Directory -Force "$dist\setup\tools\llvm\lib" | Out-Null
Copy-Item -Recurse "$($llvm.FullName)\lib\clang" "$dist\setup\tools\llvm\lib"
Copy-Item (Fetch "LLVM-LICENSE.txt" "https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-20.1.8/llvm/LICENSE.TXT") "$dist\setup\licenses\LLVM-LICENSE.txt"
Unzip (Fetch "cmake.zip" "https://github.com/Kitware/CMake/releases/download/v3.31.6/cmake-3.31.6-windows-x86_64.zip") "$dist\setup\tools"
Rename-Item "$dist\setup\tools\cmake-3.31.6-windows-x86_64" "cmake"
Remove-Item -Recurse -Force "$dist\setup\tools\cmake\doc", "$dist\setup\tools\cmake\man" -ErrorAction SilentlyContinue
Unzip (Fetch "ninja.zip" "https://github.com/ninja-build/ninja/releases/download/v1.12.1/ninja-win.zip") "$dist\setup\tools\ninja"
Copy-Item (Fetch "ninja-COPYING.txt" "https://raw.githubusercontent.com/ninja-build/ninja/v1.12.1/COPYING") "$dist\setup\licenses\ninja-COPYING.txt"
Unzip (Fetch "python.zip" "https://www.python.org/ftp/python/3.12.8/python-3.12.8-embed-amd64.zip") "$dist\setup\tools\python"

# --- runtime beside mcla.exe: the OpenXR loader (Apache-2.0), the DLSS bridge (MIT) and NVIDIA's DLSS
Copy-Item "$built\openxr_loader.dll", "$built\bbport_dlss.dll", "$built\nvngx_dlss.dll" "$dist\setup\runtime"
Copy-Item "$built\licenses\*" "$dist\setup\runtime\licenses"
Copy-Item (Fetch "OpenXR-LICENSE.txt" "https://raw.githubusercontent.com/KhronosGroup/OpenXR-SDK-Source/release-1.1.63/LICENSE") "$dist\setup\runtime\licenses\OpenXR-LICENSE.txt"

Copy-Item "$PSScriptRoot\README.txt" $dist

# --- check: nothing of the game in the release
$bad = Get-ChildItem -Recurse $dist -File | Where-Object { $_.Extension -in ".xex", ".iso", ".rpf", ".bik" -or $_.FullName -like "*\generated\default\*" }
if ($bad) { throw "game files in the release: $($bad.FullName -join ', ')" }

$zip = "$root\out\release\$name.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path $dist -DestinationPath $zip -CompressionLevel Optimal
"release: $dist"
"zip: $zip ($([math]::Round((Get-Item $zip).Length / 1MB)) MB)"
