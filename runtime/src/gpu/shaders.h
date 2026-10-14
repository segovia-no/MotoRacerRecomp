// Shader programs for SDL_GPU, independent of the host GPU API.
//
// The programs are written once in GLSL (runtime/shaders/) and compiled offline by
// tools/build_shaders.sh into every bytecode format we ship. Each format is a ShaderBackend
// adapter in shaders.cpp; create() picks the one the device accepts (Metal: MSL, Vulkan: SPIR-V).
#pragma once
#include <SDL3/SDL.h>

namespace gpu_shaders {

enum class Program { D3dVertex, D3dFragment, OverlayVertex, OverlayFragment };

// Every format compiled in, for SDL_CreateGPUDevice.
SDL_GPUShaderFormat formats();

// nullptr (SDL_GetError() says why) if the device takes none of our formats.
SDL_GPUShader *create(SDL_GPUDevice *dev, Program program);

}  // namespace gpu_shaders
