// Direct3D 5 fixed-function pixel stage: texture blend, specular, fog, alpha test, colour key.
#version 450

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec4 v_spec;
layout(location = 2) in vec2 v_uv;

layout(location = 0) out vec4 o_color;

layout(set = 2, binding = 0) uniform sampler2D tex;

layout(set = 3, binding = 0) uniform FragUniforms {
    vec4 fog_color;
    vec4 mode;    // x: textured, y: TEXTUREMAPBLEND, z: fog, w: specular
    vec4 alpha;   // x: alpha test on, y: ref (0..1), z: colour key on, w: texture format has alpha
    vec4 snap;    // x: sample at the original pixel centre, y: magnification, zw: grid origin (px)
} u;

void main() {
    vec4 c = v_color;
    if (u.mode.x > 0.5) {
        vec2 uv = v_uv;
        if (u.snap.x > 0.5) {
            // Flat 2D element (HUD/text): use the texture coordinate the original 640x480 frame
            // sampled for this pixel, so glyph atlases don't bleed at high resolution.
            // Unshifted geometry: D3D's sample point for original pixel i is edge coordinate i.
            vec2 gpos = (gl_FragCoord.xy - u.snap.zw) / u.snap.y;
            vec2 d = (floor(gpos) - gpos) * u.snap.y;
            uv += dFdx(v_uv) * d.x + dFdy(v_uv) * d.y;
        }
        vec4 t = texture(tex, uv);
        if (u.alpha.z > 0.5 && t.a < 0.5) discard;
        int blend = int(u.mode.y + 0.5);
        if (blend == 1 || blend == 7)  c = t;                                  // DECAL / COPY
        else if (blend == 3)           c = vec4(mix(c.rgb, t.rgb, t.a), c.a);  // DECALALPHA
        else if (blend == 4)           c = c * t;                              // MODULATEALPHA
        else if (blend == 8)           c = vec4(c.rgb + t.rgb, c.a);           // ADD
        else                           c = vec4(c.rgb * t.rgb, u.alpha.w > 0.5 ? t.a : c.a); // MODULATE
    }
    if (u.mode.w > 0.5) c.rgb = clamp(c.rgb + v_spec.rgb, 0.0, 1.0);
    if (u.mode.z > 0.5) c.rgb = mix(u.fog_color.rgb, c.rgb, v_spec.a);
    if (u.alpha.x > 0.5 && c.a <= u.alpha.y) discard;
    o_color = c;
}
