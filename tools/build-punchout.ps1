# Builds punchout in one go from the source, as the APB recomp's
# build-apb.ps1: configures (the first time), lifts your disc's main.dol and
# builds the game. Run it again after a `git pull`: only what changed is
# rebuilt.
#
#   powershell -ExecutionPolicy Bypass -File tools\build-punchout.ps1 [-Disc <folder>] [-Run] [-Pull] [-ChooseDisc]
#
# On Windows, "Play Punch-Out.bat" and "Update Punch-Out.bat" in the
# project's folder run it for you: double-click them. It needs Visual Studio
# 2022 17.10 or newer (with "Desktop development with C++") and finds its
# CMake and Git itself, from any window. With no -Disc it uses the folder it
# used last (build-game\disc-folder.txt), else asks with a folder picker.
# -Run starts the game after; -Pull fetches the latest code first; -Play
# starts it straight away if it's already built.
param(
    [string]$Disc = "",
    [switch]$Run,
    [switch]$Pull,
    [switch]$ChooseDisc,
    [switch]$Play      # start the game if it's built (Play Punch-Out.bat); else build it, then start it
)
$ErrorActionPreference = "Stop"
trap {   # any failure: say what in one red line, and end with an error code
    Write-Host ""
    Write-Host "Stopped: $_" -ForegroundColor Red
    exit 1
}
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "build-game"
$discFile = Join-Path $build "disc-folder.txt"
$onWindows = ($PSVersionTable.PSEdition -eq "Desktop") -or $IsWindows   # Windows PowerShell 5.1 has no $IsWindows

if ($Play -and -not $Disc -and -not $ChooseDisc -and -not $Pull) {
    $built = Join-Path $build "Release\punchout.exe"
    if ((Test-Path $built -PathType Leaf) -and (Test-Path $discFile -PathType Leaf)) {
        $last = (Get-Content $discFile -Raw).Trim()
        if (-not (Test-Path (Join-Path $last "sys/main.dol") -PathType Leaf) -or
            -not (Test-Path (Join-Path $last "sys/fst.bin") -PathType Leaf) -or
            -not (Test-Path (Join-Path $last "files") -PathType Container)) {
            throw "The remembered disc is incomplete or moved: use -ChooseDisc to select an extracted disc."
        }
        Start-Process -FilePath $built -ArgumentList "`"$last`"" -WorkingDirectory (Split-Path -Parent $built)
        exit 0
    }
    Write-Host "Punch-Out!! isn't built yet: building it now. The first build takes 10 to 40 minutes."
    $Run = $true
}

if ($Play) { $Run = $true }

function Invoke-Step([string]$what, [string]$exe, [string[]]$arguments) {
    Write-Host "== $what" -ForegroundColor Cyan
    & $exe @arguments
    if ($LASTEXITCODE -ne 0) { throw "$what failed (exit code $LASTEXITCODE)" }
}

# CMake and Git: on the PATH, else the ones Visual Studio installs, else the usual places.
function Add-Tool([string]$name, [string[]]$folders) {
    if (Get-Command $name -ErrorAction SilentlyContinue) { return $true }
    foreach ($f in $folders) {
        if ($f -and (Test-Path (Join-Path $f "$name.exe"))) {
            $env:PATH = "$f;$env:PATH"
            return $true
        }
    }
    return $false
}
if ($onWindows) {
    $vs = ""
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) { $vs = & $vswhere -latest -products * -property installationPath | Select-Object -First 1 }
    $ide = if ($vs) { Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft" } else { "" }
    $haveCmake = Add-Tool "cmake" @($(if ($ide) { Join-Path $ide "CMake\CMake\bin" }), (Join-Path $env:ProgramFiles "CMake\bin"))
    $haveGit = Add-Tool "git" @((Join-Path $env:ProgramFiles "Git\cmd"), $(if ($ide) { Join-Path $ide "TeamFoundation\Team Explorer\Git\cmd" }))
    if (-not $vs) { throw "Visual Studio 2022 wasn't found. Install it (the free Community edition) with 'Desktop development with C++'." }
    if (-not $haveCmake) { throw "CMake wasn't found. In the Visual Studio Installer, add 'C++ CMake tools for Windows'." }
    if (-not $haveGit) { throw "Git wasn't found. Install Git for Windows (git-scm.com), or add 'Git for Windows' in the Visual Studio Installer." }
} elseif (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    throw "CMake isn't installed."
}

if ($Pull) {
    Write-Host "== Getting the latest code" -ForegroundColor Cyan
    & git -C $root pull --ff-only
    if ($LASTEXITCODE -ne 0) { throw "git pull failed (exit code $LASTEXITCODE). If you changed files in the project folder, that's why." }
}

# The extracted disc: the one used last, else ask.
if (-not $Disc -and -not $ChooseDisc -and (Test-Path $discFile)) { $Disc = (Get-Content $discFile -Raw).Trim() }
while (-not $Disc -or -not (Test-Path (Join-Path $Disc "sys/main.dol") -PathType Leaf) -or
       -not (Test-Path (Join-Path $Disc "sys/fst.bin") -PathType Leaf) -or
       -not (Test-Path (Join-Path $Disc "files") -PathType Container)) {
    if ($Disc) {
        if ((Split-Path -Leaf $Disc) -eq "sys" -and (Test-Path (Join-Path $Disc "main.dol"))) { $Disc = Split-Path -Parent $Disc; continue }
        Write-Host "$Disc is incomplete: choose the folder Dolphin's Extract Entire Disc made (it holds sys/main.dol, sys/fst.bin and files)." -ForegroundColor Yellow
    }
    if (-not $onWindows) { throw "give the extracted disc with -Disc <folder>" }
    Add-Type -AssemblyName System.Windows.Forms
    $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
    $dialog.Description = "Choose your extracted Punch-Out!! disc: the folder that holds sys and files (Dolphin: Extract Entire Disc)"
    $dialog.ShowNewFolderButton = $false
    if ($dialog.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) { throw "no disc folder chosen" }
    $Disc = $dialog.SelectedPath
}
$Disc = (Resolve-Path -LiteralPath $Disc).Path
$dol = Join-Path $Disc "sys\main.dol"

# Built for this PC's own processor (QILIN_NATIVE: AVX2 and FMA where it has them).
$configure = @("-S", $root, "-B", $build, "-DQILIN_GAME_DOL=$dol", "-DQILIN_NATIVE=ON")
if ($onWindows -and -not (Test-Path (Join-Path $build "CMakeCache.txt"))) { $configure += @("-G", "Visual Studio 17 2022", "-A", "x64") }
if (-not $onWindows) { $configure += "-DCMAKE_BUILD_TYPE=Release" }
Invoke-Step "Configuring" "cmake" $configure
Invoke-Step "Building Punch-Out!! (the first time lifts and compiles the whole game: 10 to 40 minutes)" "cmake" @("--build", $build, "--config", "Release", "--target", "punchout", "--parallel")
$exe = if ($onWindows) { ".exe" } else { "" }
$game = @((Join-Path $build "Release/punchout$exe"), (Join-Path $build "punchout$exe")) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $game) { throw "built, but punchout$exe isn't where it should be" }

# Commit the selection only after both compilation and finding the output succeed.
Set-Content -LiteralPath "$discFile.tmp" -Value $Disc -Encoding UTF8
Move-Item -LiteralPath "$discFile.tmp" -Destination $discFile -Force
Write-Host "Built: $game" -ForegroundColor Green
if ($Run) {
    Write-Host "Starting Punch-Out!!..."
    Start-Process -FilePath $game -ArgumentList "`"$Disc`"" -WorkingDirectory (Split-Path -Parent $game)
} else {
    Write-Host "Run it with:  & `"$game`" `"$Disc`""
}
