// Overlay pixels drawn over the high-resolution render target (alpha = changed mask).
#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

layout(set = 2, binding = 0) uniform sampler2D tex;

void main() {
    vec4 t = texture(tex, v_uv);
    if (t.a < 0.5) discard;
    o_color = vec4(t.rgb, 1.0);
}
