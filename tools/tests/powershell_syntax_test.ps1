# Parse our launchers on every machine with PowerShell, without game files.
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
foreach ($relative in @("tools/build-punchout.ps1", "tools/player-kit/play.ps1")) {
    $tokens = $null
    $parseErrors = $null
    $null = [System.Management.Automation.Language.Parser]::ParseFile(
        (Join-Path $root $relative), [ref]$tokens, [ref]$parseErrors)
    if ($parseErrors.Count) {
        throw "${relative}: $($parseErrors | Out-String)"
    }
    Write-Host "Parsed $relative"
}
