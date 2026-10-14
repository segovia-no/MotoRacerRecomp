#include "shaders.h"

#include <cstring>

#include "shaders_gen.h"

namespace gpu_shaders {

namespace {

struct Code {
    const uint8_t *bytes;
    size_t size;
    const char *entrypoint;
};

// What each program needs bound, in SDL_GPU's per-stage resource layout.
struct Layout {
    SDL_GPUShaderStage stage;
    uint32_t samplers, uniform_buffers;
};

Layout layout(Program p) {
    switch (p) {
    case Program::D3dVertex: return {SDL_GPU_SHADERSTAGE_VERTEX, 0, 1};
    case Program::D3dFragment: return {SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1};
    case Program::OverlayVertex: return {SDL_GPU_SHADERSTAGE_VERTEX, 0, 0};
    case Program::OverlayFragment: return {SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0};
    }
    return {};
}

class ShaderBackend {
public:
    virtual ~ShaderBackend() = default;
    virtual SDL_GPUShaderFormat format() const = 0;
    virtual Code code(Program p) const = 0;
};

class SpirvBackend final : public ShaderBackend {
public:
    SDL_GPUShaderFormat format() const override { return SDL_GPU_SHADERFORMAT_SPIRV; }
    Code code(Program p) const override {
        switch (p) {
        case Program::D3dVertex: return {k_d3d_vert_spv, sizeof k_d3d_vert_spv, "main"};
        case Program::D3dFragment: return {k_d3d_frag_spv, sizeof k_d3d_frag_spv, "main"};
        case Program::OverlayVertex: return {k_overlay_vert_spv, sizeof k_overlay_vert_spv, "main"};
        case Program::OverlayFragment: return {k_overlay_frag_spv, sizeof k_overlay_frag_spv, "main"};
        }
        return {};
    }
};

class MslBackend final : public ShaderBackend {
public:
    SDL_GPUShaderFormat format() const override { return SDL_GPU_SHADERFORMAT_MSL; }
    Code code(Program p) const override {
        switch (p) {
        case Program::D3dVertex: return text(k_d3d_vert_msl);
        case Program::D3dFragment: return text(k_d3d_frag_msl);
        case Program::OverlayVertex: return text(k_overlay_vert_msl);
        case Program::OverlayFragment: return text(k_overlay_frag_msl);
        }
        return {};
    }

private:
    static Code text(const char *src) { return {reinterpret_cast<const uint8_t *>(src), strlen(src), "main0"}; }
};

const SpirvBackend spirv;
const MslBackend msl;
const ShaderBackend *const backends[] = {&spirv, &msl};

}  // namespace

SDL_GPUShaderFormat formats() {
    SDL_GPUShaderFormat f = 0;
    for (const ShaderBackend *b : backends) f |= b->format();
    return f;
}

SDL_GPUShader *create(SDL_GPUDevice *dev, Program program) {
    SDL_GPUShaderFormat accepted = SDL_GetGPUShaderFormats(dev);
    for (const ShaderBackend *b : backends) {
        if (!(accepted & b->format())) continue;
        Code c = b->code(program);
        Layout l = layout(program);
        SDL_GPUShaderCreateInfo sci{};
        sci.code = c.bytes;
        sci.code_size = c.size;
        sci.entrypoint = c.entrypoint;
        sci.format = b->format();
        sci.stage = l.stage;
        sci.num_samplers = l.samplers;
        sci.num_uniform_buffers = l.uniform_buffers;
        return SDL_CreateGPUShader(dev, &sci);
    }
    SDL_SetError("GPU device accepts none of the shader formats this build ships");
    return nullptr;
}

}  // namespace gpu_shaders
