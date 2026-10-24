#!/bin/bash
# Builds a self-contained innoextract 1.9 for macOS (docs/28-easy-install.md, E3): Boost and liblzma
# linked statically, so the binary needs only system libraries (libc++, libiconv). There is no
# official macOS build. Needs Homebrew's boost and xz (static libraries) and cmake — on the build
# machine / CI only, never on a player's machine.
#
#   tools/release/build_innoextract_macos.sh <out-dir>     ->  <out-dir>/innoextract
#
# Source and patches as Homebrew's formula (CMake 4, Boost 1.85 and 1.89 fixes), SHA-256 pinned.
set -euo pipefail
out=$(mkdir -p "$1" && cd "$1" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
fetch() {   # url sha256 file
    curl -sSfL "$1" -o "$work/$3"
    echo "$2  $work/$3" | shasum -a 256 -c --quiet
}
fetch https://constexpr.org/innoextract/files/innoextract-1.9.tar.gz \
      6344a69fc1ed847d4ed3e272e0da5998948c6b828cb7af39c6321aba6cf88126 src.tar.gz
fetch "https://github.com/dscharrer/innoextract/commit/83d0bf4365b09ddd17dddb400ba5d262ddf16fb8.patch?full_index=1" \
      fe5299d1fdea5c66287aef2f70fee41d86aedc460c5b165da621d699353db07d cmake4.patch
fetch "https://github.com/dscharrer/innoextract/commit/264c2fe6b84f90f6290c670e5f676660ec7b2387.patch?full_index=1" \
      f968a9c0521083dd4076ce5eed56127099a9c9888113fc50f476b914396045cc boost185.patch
fetch "https://github.com/dscharrer/innoextract/commit/882796e0e9b134b02deeaae4bbfe92920adb6fe2.patch?full_index=1" \
      d5af3e86eb2b74bff559885440d330678e5dbb028ce165bb836ddb14224af201 boost189.patch
tar xzf "$work/src.tar.gz" -C "$work"
cd "$work/innoextract-1.9"
for p in cmake4 boost185 boost189; do patch -s -p1 < "$work/$p.patch"; done
brew_prefix=$(brew --prefix)
# Boost.Iostreams asks for -lzstd: offer only the static library (the folder has no .dylib)
mkdir -p "$work/static" && ln -s "$brew_prefix/opt/zstd/lib/libzstd.a" "$work/static/libzstd.a"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DUSE_STATIC_LIBS=ON -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 \
      -DCMAKE_PREFIX_PATH="$brew_prefix/opt/boost;$brew_prefix/opt/xz" \
      -DCMAKE_EXE_LINKER_FLAGS="-L$work/static" >/dev/null
cmake --build build -j "$(sysctl -n hw.ncpu)" >/dev/null
cp build/innoextract "$out/innoextract"
strip "$out/innoextract"
otool -L "$out/innoextract" | tail -n +2
