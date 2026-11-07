<#
PSP build for Windows: unpacks your GOG Moto Racer installer, recompiles the game for the PSP and makes
build\psp-hw\PSP to copy to the memory stick. Started by setup.cmd --psp [path\to\setup_moto_racer_2.1.0.11.exe]
(from a Command Prompt or PowerShell in the source folder).

The PSP toolchain (pspdev) has no Windows build, so this runs scripts/setup/psp.sh in WSL (Windows Subsystem
for Linux; Ubuntu is used when installed). Without WSL: open PowerShell as administrator, run
"wsl --install", restart, open Ubuntu once to create your user, then run this again. The script asks
for your Linux password once to install the packages it needs there (apt). Guide: docs\guides\psp-build.md.

Without an argument the installer is looked for in Downloads and in the source folder.
#>
param([string]$Installer = "")
$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)   # the source folder
function Say($msg) { Write-Host "`n==> $msg" -ForegroundColor Cyan }
function Die($msg) { Write-Host "`nerror: $msg" -ForegroundColor Red; exit 1 }

if (-not (Get-Command wsl.exe -ErrorAction SilentlyContinue)) {
    Die "WSL is not installed. Open PowerShell as administrator, run 'wsl --install', restart Windows, open Ubuntu once to create your user, then run this again."
}
# wsl.exe prints UTF-16 unless WSL_UTF8 is set (older versions ignore it: strip the NULs either way)
$env:WSL_UTF8 = '1'
$distros = @(wsl.exe --list --quiet 2>$null | ForEach-Object { ($_ -replace "`0", '').Trim() } | Where-Object { $_ })
if ($distros.Count -eq 0) {
    Die "WSL has no Linux distribution yet. Run 'wsl --install -d Ubuntu', create your user when it opens, then run this again."
}
$Distro = $distros | Where-Object { $_ -like 'Ubuntu*' } | Select-Object -First 1
if (-not $Distro) { $Distro = $distros[0] }
Say "Building in WSL ($Distro)"

if (-not $Installer) {
    foreach ($dir in @((Join-Path $env:USERPROFILE 'Downloads'), $Root)) {
        $f = Get-ChildItem -Path $dir -Filter 'setup_moto_racer*.exe' -File -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($f) { $Installer = $f.FullName; break }
    }
}
if ($Installer -and -not (Test-Path -LiteralPath $Installer -PathType Leaf)) { Die "installer not found: $Installer" }

function To-Wsl([string]$path) {
    $p = (wsl.exe -d $Distro -- wslpath -a ($path -replace '\\', '/')) -replace "`0", ''
    if ($LASTEXITCODE -ne 0 -or -not $p) { Die "could not map $path into WSL" }
    return $p.Trim()
}
$RepoWsl = To-Wsl $Root
$OutDir = Join-Path $Root 'build\psp-hw'
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$OutWsl = To-Wsl $OutDir
$ScriptArgs = @()
if ($Installer) {
    Say "Installer: $Installer"
    $ScriptArgs += (To-Wsl $Installer)
}

wsl.exe -d $Distro --cd $RepoWsl -- env "OUT=$OutWsl" bash ./setup.sh --psp @ScriptArgs
if ($LASTEXITCODE -ne 0) { Die "the PSP build failed (see the messages above)" }

Say "Copy $OutDir\PSP to the root of your PSP's memory stick."
Start-Process explorer.exe $OutDir
