#version 450
// One direction of a 9-tap Gaussian (linear sampling folds it into 5 fetches)
layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

layout(set = 2, binding = 0) uniform sampler2D u_texture;
layout(set = 3, binding = 0) uniform Params {
    vec4 u_step;   // xy: one texel along the blur direction
};

void main() {
    const float w0 = 0.2270270270;
    const float w1 = 0.3162162162;
    const float w2 = 0.0702702703;
    const float o1 = 1.3846153846;
    const float o2 = 3.2307692308;
    vec2 s = u_step.xy;
    vec4 c = texture(u_texture, v_uv) * w0;
    c += texture(u_texture, v_uv + s * o1) * w1;
    c += texture(u_texture, v_uv - s * o1) * w1;
    c += texture(u_texture, v_uv + s * o2) * w2;
    c += texture(u_texture, v_uv - s * o2) * w2;
    o_color = c * v_color;
}
