#requires -version 5.1
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$scriptPath=Join-Path $PSScriptRoot 'Start-CryptidHunt-Isolated.ps1'
$tokens=$null
$errors=$null
[System.Management.Automation.Language.Parser]::ParseFile(
    $scriptPath,[ref]$tokens,[ref]$errors) | Out-Null
if($errors.Count -ne 0) {
    throw "Isolation PowerShell syntax errors: $($errors | Out-String)"
}
$root=Join-Path ([IO.Path]::GetTempPath()) ('FolkloreIsolationTest-'+[Guid]::NewGuid().ToString('N'))
$parent=Join-Path $root 'Saved'
$saved=Join-Path $parent 'SaveGames'
$fakeExe=Join-Path $root 'FolkloreHunter-Win64-Shipping.exe'
try {
    New-Item -ItemType Directory -Path $saved -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $saved 'original.sav') -Value 'original-profile' -Encoding ASCII
    Copy-Item -LiteralPath $env:ComSpec -Destination $fakeExe
    & $scriptPath -GameExe $fakeExe -SaveGameDirectory $saved -DryRun
    if(-not (Test-Path -LiteralPath (Join-Path $saved 'original.sav'))) {
        throw 'Dry run modified the original save.'
    }
    & $scriptPath -GameExe $fakeExe -SaveGameDirectory $saved -GameArguments '/c exit 0'
    $content=Get-Content -LiteralPath (Join-Path $saved 'original.sav') -Raw
    if($content.Trim() -ne 'original-profile') {
        throw 'Original save did not survive the isolated launch.'
    }
    $item=Get-Item -LiteralPath $saved -Force
    if(($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw 'Original SaveGames still points to the isolated profile.'
    }
    if(Test-Path -LiteralPath (Join-Path $parent 'SaveGames.CryptidHuntOriginal')) {
        throw 'Backup was not restored after process exit.'
    }
    if(Test-Path -LiteralPath (Join-Path $parent 'CryptidHunt-ActiveIsolation.json')) {
        throw 'Stale recovery marker after successful restore.'
    }
    if(-not (Test-Path -LiteralPath (Join-Path $parent 'CryptidHuntProfiles\CryptidHunt'))) {
        throw 'Isolated profile directory missing.'
    }
    & $scriptPath -SaveGameDirectory $saved -Recover
    Write-Host 'PASS: script parses, dry-run leaves saves untouched, isolated process restores original and retains a separate profile.'
} finally {
    if(Test-Path -LiteralPath $root) {
        Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
    }
}
