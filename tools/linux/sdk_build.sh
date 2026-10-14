#!/bin/bash
# Runs inside the Steam Runtime 3 "sniper" SDK container (started by tools/linux/setup_linux.sh):
# builds motoracer with gcc-14 and SDL3 linked statically, so the binary needs only glibc 2.29+
# and runs on SteamOS and other Linux distributions.
#   /src  this repository (read-only)    /build  build directory    /game/game.EXE  your game.EXE
set -euo pipefail
export HOME=/build/home
mkdir -p "$HOME"
if [ ! -x /build/venv/bin/pip ]; then   # the SDK's Python has no pip/ensurepip
    rm -rf /build/venv
    python3 -m venv --without-pip /build/venv
    curl -sSf https://bootstrap.pypa.io/pip/3.9/get-pip.py | /build/venv/bin/python - -q
fi
/build/venv/bin/pip install -q --disable-pip-version-check -r /src/tools/recomp/requirements.txt
cmake -S /src -B /build/cmake -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_C_COMPILER=gcc-14 -DCMAKE_CXX_COMPILER=g++-14 -DCMAKE_ASM_COMPILER=gcc-14 \
      -DGAME_EXE=/game/game.EXE -DRECOMP_PYTHON=/build/venv/bin/python -DMOTORACER_BUNDLED_SDL=ON >/dev/null
cmake --build /build/cmake
