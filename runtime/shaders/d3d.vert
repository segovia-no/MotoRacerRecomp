// Direct3D 5 transformed vertices (positions already in clip space, see d3d_gpu.cpp).
#version 450

layout(location = 0) in vec4 in_pos;
layout(location = 1) in vec4 in_color;   // D3DCOLOR bytes B,G,R,A
layout(location = 2) in vec4 in_spec;
layout(location = 3) in vec2 in_uv;

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec4 v_spec;
layout(location = 2) out vec2 v_uv;

layout(set = 1, binding = 0) uniform VertUniforms {
    vec4 offset;   // xy: clip-space shift per unit w
} vu;

void main() {
    gl_Position = in_pos;
    gl_Position.xy += vu.offset.xy * in_pos.w;
    v_color = in_color.zyxw;
    v_spec = in_spec.zyxw;
    v_uv = in_uv;
}
