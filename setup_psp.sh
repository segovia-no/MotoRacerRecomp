#!/bin/bash
# One-step PSP build: unpacks your GOG Moto Racer installer, recompiles the game for the PSP and makes
# a folder to copy to the memory stick (PSP/GAME/MOTORACER: the EBOOT and your game files, the CD music
# as MP3). macOS and Linux; on Windows run setup_psp.cmd (it runs this script in WSL). The package
# runs on every PSP model (PSP-1000 included) with custom firmware. Guide: docs/30-psp-build.md.
#
#   ./setup_psp.sh [path/to/setup_moto_racer_2.1.0.11.exe]
#
# Without an argument the installer is looked for in ~/Downloads and this folder; when the game is
# already unpacked (by this script or the desktop setup) it is reused.
#
# Downloads the PSP toolchain (pspdev, pinned release, SHA-256 checked) into build/pspdev unless
# PSPDEV points at a working one. Missing system packages (cmake, innoextract, ffmpeg, ...) are
# installed with Homebrew on macOS and, after asking, with your package manager on Linux.
#
# Options (environment):
#   GAME_DIR=<dir>   game files (default: the desktop setup's folder)
#   OUT=<dir>        where PSP/ goes (default: build/psp-hw)
#   PSPDEV=<dir>     an installed pspdev toolchain to use instead of downloading one
#   NO_INSTALL=1     report missing system packages instead of installing them
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"
say() { printf '\n==> %s\n' "$*"; }
die() { printf '\nerror: %s\n' "$*" >&2; exit 1; }
ask() {   # ask "question" -> 0 = yes (default yes; no terminal: yes)
    [ -t 0 ] || return 0
    printf '%s [Y/n] ' "$1"
    read -r answer || answer=""
    case $answer in [nN]*) return 1 ;; esac
    return 0
}

# --- platform -------------------------------------------------------------------------------------
OS=$(uname -s) ARCH=$(uname -m)
WSL=false
grep -qi microsoft /proc/version 2>/dev/null && WSL=true
case $OS in
    Darwin) GAME_DEFAULT="$HOME/Library/Application Support/MotoRacerRecomp/game"
            JOBS=$(sysctl -n hw.ncpu) ;;
    Linux)  GAME_DEFAULT="${XDG_DATA_HOME:-$HOME/.local/share}/MotoRacerRecomp/game"
            JOBS=$(nproc) ;;
    *)      die "this script is for macOS and Linux; on Windows run setup_psp.cmd" ;;
esac
GAME_DIR=${GAME_DIR:-$GAME_DEFAULT}
OUT=${OUT:-$HERE/build/psp-hw}
# The build's own files: in build/. Under WSL with the checkout on a Windows drive (/mnt/c/...) they go
# to the Linux side (much faster than through the drive mapping, and Python environments need it).
BUILD_ROOT="$HERE/build"
if $WSL && case $HERE in /mnt/*) true ;; *) false ;; esac; then BUILD_ROOT="$HOME/.cache/motoracer-psp"; fi
mkdir -p "$BUILD_ROOT"

# --- find the installer ---------------------------------------------------------------------------
SETUP=${1:-}
if [ -z "$SETUP" ] && [ ! -f "$GAME_DIR/game.EXE" ]; then
    for f in "$HOME"/Downloads/setup_moto_racer*.exe ./setup_moto_racer*.exe; do
        [ -f "$f" ] && SETUP=$f && break
    done
    [ -n "$SETUP" ] || die "GOG installer not found: pass it as an argument, e.g. ./setup_psp.sh ~/Downloads/setup_moto_racer_2.1.0.11.exe"
fi
[ -z "$SETUP" ] || [ -f "$SETUP" ] || die "installer not found: $SETUP"

# --- system packages ------------------------------------------------------------------------------
# commands this script needs, and (for the toolchain's compiler) libraries checked further down
NEED_CMDS="cmake make innoextract ffmpeg python3 curl tar"
missing_cmds() {
    local m=""
    for c in $NEED_CMDS; do command -v "$c" >/dev/null 2>&1 || m="$m $c"; done
    python3 -c 'import venv, ensurepip' 2>/dev/null || m="$m python3-venv"
    echo "$m"
}
# package names per package manager for the commands (and the compiler's libraries: libs)
pkgs_for() {   # <manager> <missing...>
    local pm=$1; shift
    local out=""
    for c in "$@"; do
        case $pm:$c in
            brew:make|brew:tar|brew:curl|brew:python3-venv) ;;   # part of macOS / Xcode tools
            brew:python3) out="$out python" ;;
            brew:libs) out="$out libmpc gmp mpfr zstd" ;;
            apt:python3-venv) out="$out python3-venv" ;;
            apt:libs) out="$out libmpc3 libmpfr6 libgmp10 libzstd1 zlib1g" ;;
            dnf:ffmpeg) out="$out ffmpeg-free" ;;
            dnf:python3-venv) out="$out python3" ;;
            dnf:libs) out="$out libmpc mpfr gmp libzstd zlib" ;;
            pacman:python3|pacman:python3-venv) out="$out python" ;;
            pacman:libs) out="$out libmpc mpfr gmp zstd zlib" ;;
            zypper:python3-venv) out="$out python3" ;;
            zypper:libs) out="$out libmpc3 libmpfr6 libgmp10 libzstd1 libz1" ;;
            *:python3-venv) out="$out python3" ;;
            *) out="$out $c" ;;
        esac
    done
    echo "$out" | tr ' ' '\n' | awk 'NF && !seen[$0]++' | tr '\n' ' '
}
PM=""
if [ "$OS" = Darwin ]; then
    command -v brew >/dev/null 2>&1 || die "Homebrew is required: see https://brew.sh, then re-run this script"
    xcode-select -p >/dev/null 2>&1 || die "Xcode Command Line Tools are missing: run 'xcode-select --install' and re-run"
    PM=brew
else
    for m in apt-get dnf pacman zypper; do command -v $m >/dev/null 2>&1 && { PM=${m%-get}; break; }; done
fi
install_pkgs() {   # <package names...>
    [ $# -gt 0 ] || return 0
    local cmd sudo=sudo
    [ "$(id -u)" -ne 0 ] || sudo=""
    case $PM in
        brew)   cmd="brew install $*" ;;
        apt)    cmd="$sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y $*" ;;
        dnf)    cmd="$sudo dnf install -y $*" ;;
        pacman) cmd="$sudo pacman -S --needed --noconfirm $*" ;;
        zypper) cmd="$sudo zypper install -y $*" ;;
        *)      die "missing: $* (install them with your package manager and re-run)" ;;
    esac
    [ -z "${NO_INSTALL:-}" ] || die "missing packages; install them with: $cmd"
    if [ "$PM" != brew ]; then
        printf '\nThese packages are needed: %s\n' "$*"
        ask "Install them now ($cmd)?" || die "install them with: $cmd"
        if [ "$PM" = apt ]; then $sudo apt-get update -qq; fi
    fi
    say "Installing: $*"
    $cmd
}

say "Checking system packages"
# shellcheck disable=SC2046
MISSING=$(missing_cmds)
if [ -n "$MISSING" ]; then
    # shellcheck disable=SC2086
    install_pkgs $(pkgs_for "$PM" $MISSING)
    MISSING=$(missing_cmds)
    [ -z "$MISSING" ] || die "still missing:$MISSING"
fi
# the toolchain's compiler links these on macOS (Homebrew)
if [ "$OS" = Darwin ]; then
    LIBS_MISSING=""
    for p in libmpc gmp mpfr zstd; do brew list --versions "$p" >/dev/null 2>&1 || LIBS_MISSING="$LIBS_MISSING $p"; done
    # shellcheck disable=SC2086
    [ -z "$LIBS_MISSING" ] || install_pkgs $LIBS_MISSING
fi

# --- PSP toolchain ----------------------------------------------------------------------------------
PSPDEV_TAG=v20261001
sha256() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
    else shasum -a 256 "$1" | cut -d' ' -f1; fi
}
toolchain_works() {   # <pspdev dir>
    [ -x "$1/bin/psp-gcc" ] || return 1
    local t
    t=$(mktemp -d)
    echo 'int main(void) { return 0; }' > "$t/t.c"
    "$1/bin/psp-gcc" -c "$t/t.c" -o "$t/t.o" 2>"$t/err"
    local r=$?
    [ $r -eq 0 ] || cp "$t/err" "$BUILD_ROOT/pspdev-check.log"
    rm -rf "$t"
    return $r
}
if [ -n "${PSPDEV:-}" ]; then
    toolchain_works "$PSPDEV" || die "PSPDEV=$PSPDEV does not hold a working pspdev toolchain"
else
    PSPDEV="$BUILD_ROOT/pspdev"
    if ! toolchain_works "$PSPDEV"; then
        case $OS:$ARCH in
            Darwin:arm64)  asset=pspdev-macos-latest-arm64.tar.gz
                           sha=8efc2e9c41ad3c7e0b0eab2ca0ce2ae69dff329cee577ffd9e4a53edbe2e3acb ;;
            Darwin:x86_64) asset=pspdev-macos-15-intel-x86_64.tar.gz
                           sha=bcf1133c438cd632671312b1399cf57b1e2f066665cdc9b0141255ae1cf8d19d ;;
            Linux:aarch64) asset=pspdev-ubuntu-24.04-arm-arm64.tar.gz
                           sha=0638f5f32fe4354c70040f8ee987b2e7111a981e93499d3986d30b15d1fb8e61 ;;
            Linux:x86_64)
                distro=$(. /etc/os-release 2>/dev/null; echo "${ID:-} ${ID_LIKE:-}")
                case " $distro " in
                    *" fedora "*|*" rhel "*) asset=pspdev-fedora-latest.tar.gz
                        sha=7874d4d696f1abe58d3d17fd8ae365e9b129c4bea16b383bf708655037c0e21c ;;
                    *" debian "*) case " $distro " in
                        *" ubuntu "*) asset=pspdev-ubuntu-latest-x86_64.tar.gz
                            sha=a86efe624e770005290919617859e9df24bd8fcd3e364f4587f23ec7364b0acd ;;
                        *) asset=pspdev-debian-latest.tar.gz
                            sha=39b20efc7c8492cdbd9929178f45afc801dabb0967b7c882573520f22536920c ;;
                        esac ;;
                    *) asset=pspdev-ubuntu-latest-x86_64.tar.gz
                        sha=a86efe624e770005290919617859e9df24bd8fcd3e364f4587f23ec7364b0acd ;;
                esac ;;
            *) die "no pspdev toolchain build for $OS $ARCH: install pspdev (https://pspdev.github.io) and set PSPDEV" ;;
        esac
        say "Downloading the PSP toolchain (pspdev $PSPDEV_TAG, ~170 MB)"
        tarball="$BUILD_ROOT/$asset"
        if [ ! -f "$tarball" ] || [ "$(sha256 "$tarball")" != "$sha" ]; then
            curl -fL --progress-bar -o "$tarball.part" "https://github.com/pspdev/pspdev/releases/download/$PSPDEV_TAG/$asset"
            mv "$tarball.part" "$tarball"
        fi
        [ "$(sha256 "$tarball")" = "$sha" ] ||
            { rm -f "$tarball"; die "the toolchain download does not match its checksum: try again"; }
        rm -rf "$PSPDEV"
        tar xzf "$tarball" -C "$BUILD_ROOT"
        [ -d "$PSPDEV" ] || die "unexpected toolchain archive layout (no pspdev/ folder)"
        rm -f "$tarball"
        if ! toolchain_works "$PSPDEV" && [ "$OS" = Linux ]; then
            # the compiler needs a few shared libraries (GMP, MPFR, MPC, zstd)
            # shellcheck disable=SC2046
            install_pkgs $(pkgs_for "$PM" libs)
        fi
        toolchain_works "$PSPDEV" || die "the PSP compiler does not run (details: $BUILD_ROOT/pspdev-check.log)"
    fi
fi
export PSPDEV PATH="$PSPDEV/bin:$PATH"

# --- the recompiler's Python environment -----------------------------------------------------------
say "Preparing the recompiler (Python virtual environment)"
VENV="$HERE/.venv"
[ "$BUILD_ROOT" = "$HERE/build" ] || VENV="$BUILD_ROOT/venv"
[ -x "$VENV/bin/python" ] || python3 -m venv "$VENV"
"$VENV/bin/python" -m pip install --quiet --disable-pip-version-check -r tools/recomp/requirements.txt pillow

# --- game files ------------------------------------------------------------------------------------
if [ -n "$SETUP" ]; then
    say "Unpacking $(basename "$SETUP") to $GAME_DIR"
    tools/install_game.sh "$SETUP" "$GAME_DIR"
else
    say "Game files already installed in $GAME_DIR"
fi
[ -f "$GAME_DIR/game.EXE" ] || die "no game.EXE in $GAME_DIR"

# --- build -----------------------------------------------------------------------------------------
PSP_BUILD="$BUILD_ROOT/psp"
say "Building for the PSP (the first build recompiles game.EXE: several minutes)"
cmake -S "$HERE" -B "$PSP_BUILD" -DCMAKE_TOOLCHAIN_FILE="$PSPDEV/psp/share/pspdev.cmake" \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo -DGAME_EXE="$GAME_DIR/game.EXE" -DRECOMP_PYTHON="$VENV/bin/python" \
      -DMOTORACER_PSP_1000=ON >/dev/null
cmake --build "$PSP_BUILD" -j "$JOBS"

say "Making the home-menu images"
PY="$VENV/bin/python" tools/psp/xmb_assets.sh "$GAME_DIR" >/dev/null || true

say "Packaging (game files, CD music as MP3)"
PSP_BUILD="$PSP_BUILD" PSP_OUT="$OUT" tools/psp/package_hw.sh "$GAME_DIR"

cat <<EOF

Done. Copy this folder to the root of your PSP's memory stick (merge it with the PSP folder there):
    $OUT/PSP
The game appears as "Moto Racer Recompiled" under Game > Memory Stick (custom firmware needed).
EOF
