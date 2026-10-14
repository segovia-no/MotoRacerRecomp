<#
Windows setup: unpacks your GOG Moto Racer installer, builds the recompiled game and adds a
"Moto Racer" Start menu shortcut. Double-click setup.cmd, or in PowerShell:

    powershell -ExecutionPolicy Bypass -File setup.ps1 [path\to\setup_moto_racer_2.1.0.11.exe]

Without an argument the installer is looked for in Downloads and in this folder.
Needs Windows 10/11 (x64) and Python 3, which is installed for your user with winget if missing.
The C toolchain (llvm-mingw), innoextract, CMake and Ninja are downloaded into build\ (pinned
versions, SHA-256 checked); nothing else is installed. Re-running rebuilds and reinstalls; game
files, saves and settings stay.

Environment: GAME_DIR=<dir> puts the game files elsewhere (default %LOCALAPPDATA%\MotoRacerRecomp\game).
#>
param([string]$Installer = "")
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'   # Invoke-WebRequest is very slow with a progress bar

$Root = $PSScriptRoot
$Data = Join-Path $env:LOCALAPPDATA 'MotoRacerRecomp'
$GameDir = if ($env:GAME_DIR) { $env:GAME_DIR } else { Join-Path $Data 'game' }
$Tools = Join-Path $Root 'build\windows-tools'
$Build = Join-Path $Root 'build\windows'
$Venv = Join-Path $Root 'build\windows-venv'

function Say($msg) { Write-Host "`n==> $msg" -ForegroundColor Cyan }
function Die($msg) { Write-Host "`nerror: $msg" -ForegroundColor Red; exit 1 }

$LlvmMingw = @{
    Name   = 'llvm-mingw-20260922-ucrt-x86_64'
    Url    = 'https://github.com/mstorsjo/llvm-mingw/releases/download/20260922/llvm-mingw-20260922-ucrt-x86_64.zip'
    Sha256 = 'e3ad77d117a4bea19a7a3b333341824d79a5a371004a10e25b8504e7b3047666'
}
$InnoExtract = @{
    Name   = 'innoextract-1.9'
    Url    = 'https://github.com/dscharrer/innoextract/releases/download/1.9/innoextract-1.9-windows.zip'
    Sha256 = '6989342c9b026a00a72a38f23b62a8e6a22cc5de69805cf47d68ac2fec993065'
}
$PipTools = @('cmake==4.4.3', 'ninja==1.13.2')

if (-not [Environment]::Is64BitOperatingSystem) { Die "a 64-bit Windows is required" }

# Downloads a pinned zip into build\windows-tools\<Name> (once), checking its SHA-256.
function Get-Tool($tool) {
    $dir = Join-Path $Tools $tool.Name
    if (Test-Path $dir) { return $dir }
    New-Item -ItemType Directory -Force $Tools | Out-Null
    $zip = Join-Path $Tools "$($tool.Name).zip"
    Say "Downloading $($tool.Name)"
    Invoke-WebRequest -Uri $tool.Url -OutFile $zip -UseBasicParsing
    if ((Get-FileHash $zip -Algorithm SHA256).Hash -ne $tool.Sha256) {
        Remove-Item $zip
        Die "checksum mismatch for $($tool.Url)"
    }
    $tmp = "$dir.tmp"
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    New-Item -ItemType Directory $tmp | Out-Null
    if (Get-Command tar.exe -ErrorAction SilentlyContinue) {
        & tar.exe -xf $zip -C $tmp   # Windows 10 1803+ ships bsdtar: far faster than Expand-Archive
        if ($LASTEXITCODE) { Die "could not unpack $zip" }
    } else {
        Expand-Archive $zip -DestinationPath $tmp -Force
    }
    $inner = Join-Path $tmp $tool.Name   # llvm-mingw unpacks into its own folder
    if (Test-Path $inner) { Move-Item $inner $dir; Remove-Item -Recurse -Force $tmp } else { Move-Item $tmp $dir }
    Remove-Item $zip
    return $dir
}

# A real Python 3.9+ (not the Microsoft Store stub).
function Find-Python {
    # Windows PowerShell turns a native command's redirected stderr into errors; probing may fail.
    $ErrorActionPreference = 'Continue'
    $candidates = @(
        @{ Exe = 'py'; Args = @('-3') },
        @{ Exe = 'python'; Args = @() },
        @{ Exe = (Join-Path $env:LOCALAPPDATA 'Programs\Python\Python312\python.exe'); Args = @() }
    )
    foreach ($c in $candidates) {
        $cmd = Get-Command $c.Exe -ErrorAction SilentlyContinue
        if (-not $cmd -or $cmd.Source -like '*\WindowsApps\*') { continue }
        $ok = try { & $cmd.Source @($c.Args + @('-c', 'import sys; print(sys.version_info >= (3, 9))')) 2>$null } catch { $null }
        if ($ok -eq 'True') { return @{ Exe = $cmd.Source; Args = $c.Args } }
    }
    return $null
}

# --- find the installer ---------------------------------------------------------------------------
$haveGame = Test-Path (Join-Path $GameDir 'game.EXE')
if (-not $Installer -and -not $haveGame) {
    $found = @(Get-ChildItem -Path (Join-Path $env:USERPROFILE 'Downloads'), $Root -Filter 'setup_moto_racer*.exe' -ErrorAction SilentlyContinue)
    if ($found.Count -eq 0) { Die "GOG installer not found: pass it as an argument, e.g. setup.cmd %USERPROFILE%\Downloads\setup_moto_racer_2.1.0.11.exe" }
    $Installer = $found[0].FullName
}

# --- tools -----------------------------------------------------------------------------------------
Say "Checking Python"
$py = Find-Python
if (-not $py) {
    if (-not (Get-Command winget -ErrorAction SilentlyContinue)) { Die "Python 3 is required: install it from https://www.python.org and re-run" }
    Say "Installing Python 3.12 for your user (winget)"
    & winget install -e --id Python.Python.3.12 --scope user --accept-source-agreements --accept-package-agreements
    $py = Find-Python
    if (-not $py) { Die "Python was installed but not found: open a new window and re-run setup" }
}

$venvPy = Join-Path $Venv 'Scripts\python.exe'
if (-not (Test-Path $venvPy)) {
    Say "Preparing the recompiler (Python virtual environment)"
    & $py.Exe @($py.Args + @('-m', 'venv', $Venv))
    if ($LASTEXITCODE) { Die "could not create a Python virtual environment" }
}
& $venvPy -m pip install --quiet --disable-pip-version-check -r (Join-Path $Root 'tools\recomp\requirements.txt') @PipTools
if ($LASTEXITCODE) { Die "pip install failed" }

$llvm = Get-Tool $LlvmMingw
$inno = Get-Tool $InnoExtract

# --- game files ------------------------------------------------------------------------------------
if ($Installer) {
    $Installer = (Resolve-Path $Installer).Path
    Say "Unpacking $(Split-Path $Installer -Leaf) to $GameDir"
    $tmp = Join-Path $env:TEMP ("motoracer-" + [guid]::NewGuid())
    & (Join-Path $inno 'innoextract.exe') --gog --silent -d $tmp $Installer
    if ($LASTEXITCODE -or -not (Test-Path (Join-Path $tmp 'app\game.EXE'))) {
        Die "game.EXE not found in the installer - is this the GOG Moto Racer setup?"
    }
    New-Item -ItemType Directory -Force $GameDir | Out-Null
    Copy-Item -Path (Join-Path $tmp 'app\*') -Destination $GameDir -Recurse -Force
    Remove-Item -Recurse -Force $tmp
} else {
    Say "Game files already installed in $GameDir"
}

$ini = Join-Path $GameDir 'motoracer.ini'
if ((Test-Path $ini) -and -not (Select-String -Path $ini -Pattern '^\[Recomp\]' -Quiet)) {
    Say "Writing default settings to motoracer.ini"
    Add-Content -Path $ini -Value (Get-Content (Join-Path $Root 'tools\default_settings.ini'))
}

# --- build -----------------------------------------------------------------------------------------
Say "Building (the first build recompiles game.EXE and takes a few minutes)"
$bin = (Join-Path $llvm 'bin').Replace('\', '/')
$scripts = Join-Path $Venv 'Scripts'
& (Join-Path $scripts 'cmake.exe') -S $Root -B $Build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo `
    "-DCMAKE_MAKE_PROGRAM=$((Join-Path $scripts 'ninja.exe').Replace('\', '/'))" `
    "-DCMAKE_C_COMPILER=$bin/x86_64-w64-mingw32-clang.exe" "-DCMAKE_CXX_COMPILER=$bin/x86_64-w64-mingw32-clang++.exe" `
    "-DCMAKE_ASM_COMPILER=$bin/x86_64-w64-mingw32-clang.exe" "-DCMAKE_RC_COMPILER=$bin/x86_64-w64-mingw32-windres.exe" `
    "-DGAME_EXE=$((Join-Path $GameDir 'game.EXE').Replace('\', '/'))" "-DRECOMP_PYTHON=$($venvPy.Replace('\', '/'))" `
    -DMOTORACER_BUNDLED_SDL=ON | Out-Null
if ($LASTEXITCODE) { Die "CMake configuration failed" }
& (Join-Path $scripts 'cmake.exe') --build $Build
if ($LASTEXITCODE) { Die "build failed" }

# --- install ---------------------------------------------------------------------------------------
$binDir = Join-Path $Data 'bin'
$exe = Join-Path $binDir 'MotoRacer.exe'
Say "Installing to $exe"
New-Item -ItemType Directory -Force $binDir | Out-Null
Copy-Item (Join-Path $Build 'motoracer.exe') $exe -Force
& "$bin/llvm-strip.exe" --strip-debug $exe

$lnk = Join-Path ([Environment]::GetFolderPath('Programs')) 'Moto Racer.lnk'
$shell = New-Object -ComObject WScript.Shell
$s = $shell.CreateShortcut($lnk)
$s.TargetPath = $exe
$s.IconLocation = "$exe,0"
$s.Description = 'Moto Racer, recompiled from your GOG copy'
$s.Save()

Write-Host @"

Done. Start "Moto Racer" from the Start menu, or run:
    $exe

First start: the game picks its 16/9 MODE at a native resolution for your display, and a connected
gamepad gets an Xbox-style layout (RT/LT throttle/brake, LB/RB gears, A stunts, Start pause).
Game files and settings: $GameDir
Log: $(Join-Path $Data 'logs\motoracer.log')
"@
