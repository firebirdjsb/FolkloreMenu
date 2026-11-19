#requires -version 5.1
<#
.SYNOPSIS
  Run Folklore Hunter against an isolated Unreal SaveGames profile.
.DESCRIPTION
  Redirects ONLY the chosen local SaveGames directory using a temporary
  directory junction. Original saves are renamed, never overwritten.
  Use -Recover after interrupted runs. Disable cloud synchronization first.
.EXAMPLE
  .\Start-CryptidHunt-Isolated.ps1 -GameExe 'C:\Games\FolkloreHunter-Win64-Shipping.exe' -SaveGameDirectory "$env:LOCALAPPDATA\FolkloreHunter\Saved\SaveGames" -DryRun
#>
[CmdletBinding()]
param(
    [string]$GameExe,
    [Parameter(Mandatory=$true)][string]$SaveGameDirectory,
    [ValidatePattern('^[A-Za-z0-9_-]{1,40}$')][string]$ProfileName='CryptidHunt',
    [string]$GameArguments='',
    [switch]$DryRun,
    [switch]$Recover
)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
function Full([string]$p) { [IO.Path]::GetFullPath($p).TrimEnd('\') }
function IsLink($item) {
    return ($null -ne $item) -and
        (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)
}
function Unlink-VerifiedJunction([string]$path,[string]$expected) {
    $item=Get-Item -LiteralPath $path -Force
    if(-not (IsLink $item)) { throw "Not a directory junction: $path" }
    $targets=@($item.Target)
    if($targets.Count -ne 1 -or -not $targets[0] -or
       (Full ([string]$targets[0])) -ine (Full $expected)) {
        throw 'The junction now points somewhere else. Refusing removal.'
    }
    # cmd.exe rmdir removes the junction, not its destination directory.
    $cmd='rmdir "' + $path + '"'
    & $env:ComSpec /d /c $cmd | Out-Null
    if($LASTEXITCODE -ne 0 -or (Test-Path -LiteralPath $path)) {
        throw "Unable to unlink junction: $path"
    }
}
function Restore-Profile([string]$marker,[string]$requested) {
    if(-not (Test-Path -LiteralPath $marker -PathType Leaf)) { return }
    $state=Get-Content -LiteralPath $marker -Raw | ConvertFrom-Json
    if((Full ([string]$state.SavePath)) -ine (Full $requested)) {
        throw 'The marker belongs to a different SaveGames directory.'
    }
    $saved=[string]$state.SavePath
    $backup=[string]$state.BackupPath
    $profile=[string]$state.ProfilePath
    $item=Get-Item -LiteralPath $saved -Force -ErrorAction SilentlyContinue
    if(IsLink $item) {
        Unlink-VerifiedJunction $saved $profile
    } elseif($null -ne $item) {
        if([bool]$state.HadOriginal -and
           -not (Test-Path -LiteralPath $backup)) {
            # The original SaveGames never moved; undo only the marker.
            Remove-Item -LiteralPath $marker -Force
            Write-Host 'Nothing changed: original SaveGames remains in place.'
            return
        }
        throw 'A real SaveGames folder exists while backup is pending; manual review required.'
    }
    if([bool]$state.HadOriginal) {
        if(-not (Test-Path -LiteralPath $backup -PathType Container)) {
            throw "Backup missing. Recovery marker preserved: $backup"
        }
        if(Test-Path -LiteralPath $saved) {
            throw 'Restore destination exists: refusing to overwrite.'
        }
        Move-Item -LiteralPath $backup -Destination $saved
    } elseif(-not (Test-Path -LiteralPath $saved)) {
        New-Item -ItemType Directory -Path $saved | Out-Null
    }
    Remove-Item -LiteralPath $marker -Force
    Write-Host "Original save directory restored: $saved"
}
$saved=Full $SaveGameDirectory
if([IO.Path]::GetFileName($saved) -ine 'SaveGames') {
    throw '-SaveGameDirectory must end with SaveGames.'
}
$parent=Split-Path -Parent $saved
if(-not (Test-Path -LiteralPath $parent -PathType Container)) {
    throw "Parent Saved directory not found: $parent"
}
$profiles=Join-Path $parent 'CryptidHuntProfiles'
$isolated=Join-Path $profiles $ProfileName
$backup=Join-Path $parent 'SaveGames.CryptidHuntOriginal'
$marker=Join-Path $parent 'CryptidHunt-ActiveIsolation.json'
if($Recover) {
    if(Test-Path -LiteralPath $marker) {
        Restore-Profile $marker $saved
    } else {
        Write-Host 'No interrupted save isolation to recover.'
    }
    return
}
if(-not $GameExe -or -not (Test-Path -LiteralPath $GameExe -PathType Leaf)) {
    throw 'Specify -GameExe with the full path to the shipping executable.'
}
$exe=Full $GameExe
if([IO.Path]::GetFileName($exe) -ine 'FolkloreHunter-Win64-Shipping.exe') {
    throw 'For safety, select FolkloreHunter-Win64-Shipping.exe itself, not a launcher.'
}
$procName=[IO.Path]::GetFileNameWithoutExtension($exe)
if(Get-Process -Name $procName -ErrorAction SilentlyContinue) {
    throw 'The game is already running. Exit before isolation.'
}
if(Test-Path -LiteralPath $marker) { throw "Run -Recover before starting another isolated session: $marker" }
if(Test-Path -LiteralPath $backup) { throw "Original backup exists; refusing overwrite: $backup" }
if(Test-Path -LiteralPath $saved) {
    $item=Get-Item -LiteralPath $saved -Force
    if(IsLink $item) { throw 'SaveGames is already a reparse point. Refusing modification.' }
    if(-not $item.PSIsContainer) { throw 'SaveGames is a file, not a directory.' }
}
Write-Host "Game: $exe"
Write-Host "Original: $saved"
Write-Host "Backup: $backup"
Write-Host "Separate mode profile: $isolated"
Write-Warning 'Steam/cloud saves, achievements, unlocks and progression stored elsewhere are NOT isolated. Disable game cloud synchronization manually.'
if($DryRun) {
    Write-Host 'Dry run passed. No files were touched.'
    return
}
New-Item -ItemType Directory -Path $isolated -Force | Out-Null
$hadOriginal=Test-Path -LiteralPath $saved -PathType Container
$state=[pscustomobject]@{
    SavePath=$saved
    BackupPath=$backup
    ProfilePath=$isolated
    HadOriginal=[bool]$hadOriginal
    StartedUtc=[DateTime]::UtcNow.ToString('o')
}
# Write recovery marker FIRST. An OS crash can be fixed with -Recover.
$state | ConvertTo-Json | Set-Content -LiteralPath $marker -Encoding UTF8
try {
    if($hadOriginal) { Move-Item -LiteralPath $saved -Destination $backup }
    New-Item -ItemType Junction -Path $saved -Target $isolated | Out-Null
    Write-Host "Cryptid Hunt save profile active: $ProfileName"
    $params=@{
        FilePath=$exe
        WorkingDirectory=(Split-Path -Parent $exe)
        PassThru=$true
    }
    if($GameArguments) { $params.ArgumentList=$GameArguments }
    $process=Start-Process @params
    try { Wait-Process -Id $process.Id }
    finally { $process.Dispose() }
} finally {
    if(Get-Process -Name $procName -ErrorAction SilentlyContinue) {
        Write-Warning 'Game still running! Original saves remain backed up. Exit, then run -Recover with the same SaveGameDirectory.'
    } else {
        Restore-Profile $marker $saved
    }
}
