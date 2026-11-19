# Play Punch-Out!! (the Windows download's "Play Punch-Out.bat" runs this).
#
# The download holds everything but the game's own code: that is made here,
# from your own disc, the first time (and again after an update): qilin
# lifts main.dol into C++, the bundled compiler (LLVM-MinGW) compiles it, and
# it's linked with the prebuilt rest into Punch-Out.exe. Then, and every time
# after, it starts the game.
#
#   play.ps1 [-Rebuild] [-BuildOnly] [-ChooseDisc]
#
# -Rebuild builds again even if Punch-Out.exe is up to date; -BuildOnly
# doesn't start the game afterwards; -ChooseDisc asks for the disc folder again.
param(
    [switch]$Rebuild,
    [switch]$BuildOnly,
    [switch]$ChooseDisc
)
$ErrorActionPreference = "Stop"
trap {
    Write-Host ""
    Write-Host "Stopped: $_" -ForegroundColor Red
    exit 1
}

$kit = $PSScriptRoot
$top = Split-Path -Parent $kit
$work = Join-Path $top "work"
$game = Join-Path $top "Punch-Out.exe"
$stamp = Join-Path $work "built-with.txt"
$discFile = Join-Path $work "disc-folder.txt"
$kitVersion = (Get-Content (Join-Path $kit "VERSION") -Raw).Trim()
$onWindows = ($PSVersionTable.PSEdition -eq "Desktop") -or $IsWindows   # Windows PowerShell 5.1 has no $IsWindows
# Off Windows (testing the download under Wine), QILIN_KIT_RUNNER runs the kit's programs.
$runner = $env:QILIN_KIT_RUNNER

# Runs one of the kit's programs, its output to the window; returns its exit code.
function Invoke-Kit([string]$exe, [string[]]$arguments) {
    if ($runner) { & $runner $exe @arguments | Out-Host } else { & $exe @arguments | Out-Host }
    return $LASTEXITCODE
}
# The same, its output (error output too) returned as text, with the exit code.
function Read-Kit([string]$exe, [string[]]$arguments) {
    $saved = $ErrorActionPreference
    $ErrorActionPreference = "Continue"   # Windows PowerShell 5.1 turns redirected error output into errors
    $text = if ($runner) { & $runner $exe @arguments 2>&1 | Out-String } else { & $exe @arguments 2>&1 | Out-String }
    $code = $LASTEXITCODE
    $ErrorActionPreference = $saved
    return @{ text = $text; code = $code }
}

$qilin = Join-Path $kit "qilin.exe"

# Whether a folder is the extracted disc this port supports. Returns "" if it
# is, else why not. (A folder picked one level too deep, sys\, is allowed for.)
function Test-Disc([string]$folder) {
    $dol = Join-Path (Join-Path $folder "sys") "main.dol"
    if (-not (Test-Path $dol)) {
        if ((Split-Path -Leaf $folder) -eq "sys" -and (Test-Path (Join-Path $folder "main.dol"))) { return "pick the folder above sys" }
        return "there's no sys\main.dol in it (extract the whole disc with Dolphin: right-click the game, Extract Entire Disc)"
    }
    if (-not (Test-Path (Join-Path $folder "files"))) { return "there's no files folder in it (extract the whole disc, not just main.dol)" }
    $report = Read-Kit $qilin @("info", $dol)
    if ($report.code -ne 0) { return "qilin can't read its main.dol: $($report.text.Trim())" }
    $info = $report.text | ConvertFrom-Json
    if (-not $info.build) { return "its main.dol isn't the supported build (SHA-256 $($info.sha256)): this port needs Punch-Out!! for Europe/Australia, disc R7PP01, revision 1" }
    if ($info.build.disc_id -ne "R7PP01" -or $info.build.revision -ne 1) { return "it's $($info.build.title): this port needs disc R7PP01, revision 1" }
    return ""
}

# The disc folder: the one used last, else ask (until one holds the supported build).
$disc = if (-not $ChooseDisc -and (Test-Path $discFile)) { (Get-Content $discFile -Raw).Trim() } else { "" }
while ($true) {
    if ($disc) {
        if ((Split-Path -Leaf $disc) -eq "sys" -and (Test-Path (Join-Path $disc "main.dol"))) { $disc = Split-Path -Parent $disc }
        $why = Test-Disc $disc
        if (-not $why) { break }
        Write-Host "$disc can't be used: $why" -ForegroundColor Yellow
    }
    if (-not $onWindows) { throw "no disc folder: write it into $discFile" }
    Add-Type -AssemblyName System.Windows.Forms
    $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
    $dialog.Description = "Choose your extracted Punch-Out!! disc: the folder that holds sys and files (Dolphin: Extract Entire Disc)"
    $dialog.ShowNewFolderButton = $false
    if ($dialog.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) {
        Write-Host "No disc folder chosen."
        exit 0
    }
    $disc = $dialog.SelectedPath
}
$disc = $disc.TrimEnd('\')
New-Item -ItemType Directory -Force -Path $work | Out-Null
Set-Content -Path $discFile -Value $disc -Encoding UTF8

# Up to date: just play.
$built = if (Test-Path $stamp) { (Get-Content $stamp -Raw).Trim() } else { "" }
if (-not $Rebuild -and (Test-Path $game) -and $built -eq $kitVersion) {
    if (-not $BuildOnly) { Start-Process -FilePath $game -ArgumentList "`"$disc`"" -WorkingDirectory $top }
    exit 0
}

Write-Host "Punch-Out!! QilinComp $kitVersion" -ForegroundColor Cyan
if (Test-Path $game) { Write-Host "Updating the game for this version: a few minutes." }
else { Write-Host "Building the game from your disc. This happens once (and after an update): a few minutes (up to 15 on a slower PC)." }
Write-Host "Disc: $disc"
Write-Host ""

# Lift main.dol (files whose text is unchanged are left alone, so an update
# recompiles only what changed).
Write-Host "== Recompiling main.dol into C++" -ForegroundColor Cyan
$gen = Join-Path $work "gen"
$lift = @("lift", (Join-Path (Join-Path $disc "sys") "main.dol"), "-o", $gen, "--plan", (Join-Path $kit "plan.tsv"), "--files", "32")
if ((Invoke-Kit $qilin $lift) -ne 0) { throw "qilin lift failed" }

# Compile and link: a ninja file over what the lift wrote, with the kit's
# flags and libraries (kit.ninja). Paths are relative to the work folder, so
# spaces in where the download was unpacked don't matter.
$sources = @(Get-ChildItem $gen -Filter *.cpp | Sort-Object Name)
$lines = @(
    "# Written by play.ps1: the game's lifted code, compiled against the kit.",
    "kit = ../kit",
    "include ../kit/kit.ninja",
    "cxx = ..\kit\toolchain\bin\x86_64-w64-mingw32-clang++.exe",
    "rule cxx",
    "  command = `$cxx `$cxxflags -Igen -MD -MF `$out.d -c `$in -o `$out",
    "  depfile = `$out.d",
    "  deps = gcc",
    "  description = Compiling `$in",
    "rule link",
    "  command = `$cxx `$ldflags -o `$out `$in `$libs",
    "  description = Linking `$out"
)
if (-not $onWindows) { $lines[3] = "cxx = " + $env:QILIN_KIT_CXX }   # testing off Windows: a cross compiler
$objects = @()
foreach ($s in $sources) {
    $object = "obj/$($s.BaseName).o"
    $objects += $object
    $lines += "build ${object}: cxx gen/$($s.Name)"
}
$lines += "build Punch-Out.exe: link $($objects -join ' ') | ../kit/lib/libpunchout_kit.a"
$lines += "default Punch-Out.exe"
Set-Content -Path (Join-Path $work "build.ninja") -Value $lines -Encoding ASCII

Write-Host "== Compiling ($($sources.Count) files; the biggest take a few minutes each)" -ForegroundColor Cyan
$ninja = if ($onWindows) { Join-Path $kit "ninja.exe" } else { "ninja" }
$jobs = [Math]::Max(1, [Environment]::ProcessorCount)
if ($onWindows) { $code = Invoke-Kit $ninja @("-C", $work, "-j", "$jobs") }
else { & $ninja -C $work -j $jobs | Out-Host; $code = $LASTEXITCODE }
if ($code -ne 0) { throw "compiling failed" }
Copy-Item (Join-Path $work "Punch-Out.exe") $game -Force
Set-Content -Path $stamp -Value $kitVersion -Encoding ASCII
Write-Host ""
Write-Host "Built: $game" -ForegroundColor Green
if (-not $BuildOnly) {
    Write-Host "Starting Punch-Out!!..."
    Start-Process -FilePath $game -ArgumentList "`"$disc`"" -WorkingDirectory $top
}
exit 0
