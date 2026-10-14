# Cross-compiling the Windows build from macOS/Linux with llvm-mingw (developer aid; players build
# on Windows with tools/windows/setup.ps1):
#   cmake -S . -B build/win -G Ninja -DCMAKE_TOOLCHAIN_FILE=tools/windows/mingw-cross.cmake \
#         -DLLVM_MINGW=/path/to/llvm-mingw -DMOTORACER_BUNDLED_SDL=ON -DGAME_EXE=...
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(LLVM_MINGW "$ENV{LLVM_MINGW}" CACHE PATH "llvm-mingw toolchain root")
set(_t "${LLVM_MINGW}/bin/x86_64-w64-mingw32")
set(CMAKE_C_COMPILER "${_t}-clang")
set(CMAKE_CXX_COMPILER "${_t}-clang++")
set(CMAKE_ASM_COMPILER "${_t}-clang")
set(CMAKE_RC_COMPILER "${_t}-windres")
set(CMAKE_FIND_ROOT_PATH "${LLVM_MINGW}/x86_64-w64-mingw32")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
