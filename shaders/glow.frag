#version 450
// Bright pass for the glow of explosions, muzzle flashes and projectiles: only what is already bright
// glows, scaled by its opacity (drawn on black)
layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

layout(set = 2, binding = 0) uniform sampler2D u_texture;

void main() {
    vec4 c = texture(u_texture, v_uv) * v_color;
    float luma = max(c.r, max(c.g, c.b));
    float k = smoothstep(0.35, 0.85, luma) * c.a;
    o_color = vec4(c.rgb * k, k);
}
