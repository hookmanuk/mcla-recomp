# First run of the MCLA VR release: from your own disc image to bin\mcla.exe, built on this PC.
#
# The launcher (Midnight Club LA VR.exe) runs this with the disc image you pick. Nothing of the game
# ships in the release: the files are extracted from your image, the game's code is recompiled from
# your own default.xex here, and the result stays on this PC.
#
#   setup\setup.ps1 -Iso C:\path\to\your.iso
#
# Each step is skipped when its result is already there, so after a failure the same run continues.
param([Parameter(Mandatory = $true)][string]$Iso)

$ErrorActionPreference = "Stop"
$setup = $PSScriptRoot
$root = Split-Path $setup -Parent
$project = "$setup\project"
$sdk = "$setup\sdk"
$tools = "$setup\tools"
$runtime = "$setup\runtime"
$bin = "$root\bin"
$game = "$root\game"
$logs = "$setup\logs"
New-Item -ItemType Directory -Force $logs | Out-Null
$Host.UI.RawUI.WindowTitle = "Midnight Club LA VR - first-run setup"

function Step([string]$text) { Write-Host ""; Write-Host "==> $text" -ForegroundColor Cyan }
function Fail([string]$text) {
    Write-Host ""
    Write-Host "FAILED: $text" -ForegroundColor Red
    Write-Host "Logs: $logs"
    Read-Host "Press Enter to close"
    exit 1
}
# Run a command with its output in a log; on failure show the end of the log.
function Logged([string]$name, [scriptblock]$command) {
    $log = "$logs\$name.log"
    # Windows PowerShell 5.1 turns a program's redirected stderr into errors; the exit code is what counts.
    $ErrorActionPreference = "Continue"
    & $command 2>&1 | ForEach-Object { "$_" } > $log
    if ($LASTEXITCODE -ne 0) {
        Get-Content $log -Tail 25 | ForEach-Object { Write-Host $_ }
        Fail "$name (full log: $log)"
    }
}

Write-Host "Midnight Club: Los Angeles VR - building the game on this PC from your own disc image."
Write-Host "This takes 10 to 30 minutes the first time and needs about 15 GB of free disk space."

# ---------------------------------------------------------------------------------------------------
Step "1/5 Microsoft C++ Build Tools (the compiler's Windows headers and libraries)"
# Clang, CMake, Ninja and Python come with the release; Microsoft's Build Tools may not be passed on,
# so they are installed from Microsoft (free, several GB, asks for administrator rights once).
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
function FindVcvars {
    if (-not (Test-Path $vswhere)) { return $null }
    $path = & $vswhere -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath |
        Select-Object -First 1
    if ($path -and (Test-Path "$path\VC\Auxiliary\Build\vcvars64.bat")) { return "$path\VC\Auxiliary\Build\vcvars64.bat" }
    return $null
}
$vcvars = FindVcvars
if (-not $vcvars) {
    if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
        Fail "the Visual Studio 2022 Build Tools (C++ workload) are missing and winget is not available. Install them from https://visualstudio.microsoft.com/visual-cpp-build-tools/ (workload 'Desktop development with C++'), then start the game again"
    }
    Write-Host "Installing the Visual Studio 2022 Build Tools with the C++ workload from Microsoft (several GB; Windows asks for administrator rights)."
    Logged "install-build-tools" {
        winget install --id Microsoft.VisualStudio.2022.BuildTools -e --accept-source-agreements --accept-package-agreements `
            --override "--passive --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
    }
    $vcvars = FindVcvars
    if (-not $vcvars) { Fail "the Build Tools were installed but vcvars64.bat was not found; restart the PC and start the game again" }
}
"Build Tools: $vcvars"
# The release ships one copy of each LLVM program; the build also calls them by these names.
if (-not (Test-Path "$tools\llvm\bin\clang++.exe")) { Copy-Item "$tools\llvm\bin\clang.exe" "$tools\llvm\bin\clang++.exe" }
if (-not (Test-Path "$tools\llvm\bin\llvm-ranlib.exe")) { Copy-Item "$tools\llvm\bin\llvm-ar.exe" "$tools\llvm\bin\llvm-ranlib.exe" }

# ---------------------------------------------------------------------------------------------------
Step "2/5 your game: extract from the disc image"
if (-not (Test-Path "$game\default.xex")) {
    if (-not (Test-Path -LiteralPath $Iso)) { Fail "no such file: $Iso" }
    "from $Iso"
    Logged "extract" { & "$tools\python\python.exe" "$setup\extract_game.py" $Iso $game }
} else {
    "already extracted: $game"
}
if (-not (Test-Path "$game\default.xex")) { Fail "game\default.xex is missing after extraction: is this the Complete Edition disc image (title 545407F8)?" }
# The project reads game\default.xex from its own folder: a junction to the extracted game.
if (-not (Test-Path "$project\game")) {
    New-Item -ItemType Junction -Path "$project\game" -Target $game | Out-Null
}

# ---------------------------------------------------------------------------------------------------
Step "3/5 recompile the game's code from your default.xex"
if (-not (Test-Path "$project\generated\default\mcla_init.cpp")) {
    Push-Location $project
    try {
        Logged "codegen" { & "$sdk\bin\rexglue.exe" codegen mcla_manifest.toml }
    } finally { Pop-Location }
} else {
    "already recompiled"
}

# ---------------------------------------------------------------------------------------------------
Step "4/5 build (the long step)"
$cc = "$tools\llvm\bin\clang.exe"
$cxx = "$tools\llvm\bin\clang++.exe"
$buildCmd = "$logs\build.cmd"
@(
    "@call `"$vcvars`" >nul || exit /b 1",
    "set PATH=$tools\llvm\bin;$tools\cmake\bin;$tools\ninja;%PATH%",
    "cd /d `"$project`"",
    "if not exist out\build\win-amd64-release\build.ninja cmake --preset win-amd64-release `"-DCMAKE_PREFIX_PATH=$sdk`" `"-DCMAKE_C_COMPILER=$cc`" `"-DCMAKE_CXX_COMPILER=$cxx`" `"-DCMAKE_MAKE_PROGRAM=$tools\ninja\ninja.exe`" || exit /b 1",
    "cmake --build --preset win-amd64-release || exit /b 1"
) | Set-Content -Encoding Ascii $buildCmd
Write-Host "Compiling (progress in $logs\build.log)..."
Logged "build" { cmd /c $buildCmd }
$built = "$project\out\build\win-amd64-release\mcla.exe"
if (-not (Test-Path $built)) { Fail "mcla.exe was not built (log: $logs\build.log)" }

# ---------------------------------------------------------------------------------------------------
Step "5/5 install into bin\"
New-Item -ItemType Directory -Force $bin, "$bin\licenses" | Out-Null
Copy-Item $built $bin -Force
Copy-Item "$sdk\bin\rexgpu-xenos.dll", "$sdk\bin\rexruntime.dll" $bin -Force
Copy-Item "$runtime\*.dll" $bin -Force
Copy-Item "$runtime\licenses\*" "$bin\licenses" -Force
# The build's intermediate files are several GB; the recompiled code stays for a later rebuild.
Remove-Item -Recurse -Force "$project\out\build\win-amd64-release\CMakeFiles" -ErrorAction SilentlyContinue
"installed: $bin\mcla.exe"
Write-Host ""
Write-Host "Done. The game starts now; next time the launcher starts it straight away." -ForegroundColor Green
exit 0
