#version 450
// World sprites and tiles on the SDL GPU renderer. The sprite sheets themselves are never changed:
//  - sharp > 0.5: pixel-art anti-aliasing. Texels stay crisp squares when magnified, only the one
//    screen pixel on a texel boundary is blended (needs linear sampling on the texture)
//  - team 1: the blue team armour of the Confederation sheets takes the hue of the vertex colour (the
//    texture colour mod carries the army colour); every other pixel keeps its own colour
//  - team 2: the same for the red armour of the Resistance sheets
layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

layout(set = 2, binding = 0) uniform sampler2D u_texture;
layout(set = 3, binding = 0) uniform Params {
    vec4 u_params;   // x: sharp, y: team
};

vec3 rgb2hsv(vec3 c) {
    vec4 K = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}

vec3 hsv2rgb(vec3 c) {
    vec3 p = abs(fract(c.xxx + vec3(1.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0);
    return c.z * mix(vec3(1.0), clamp(p - 1.0, 0.0, 1.0), c.y);
}

void main() {
    vec2 uv = v_uv;
    if (u_params.x > 0.5) {
        vec2 size = vec2(textureSize(u_texture, 0));
        vec2 p = uv * size;
        vec2 width = max(fwidth(p), vec2(1.0e-5));
        vec2 f = fract(p) - 0.5;
        uv = (floor(p) + 0.5 + clamp(f / width, -0.5, 0.5)) / size;
    }
    vec4 c = texture(u_texture, uv);
    if (u_params.y > 0.5) {
        vec3 hsv = rgb2hsv(c.rgb);
        float deg = hsv.x * 360.0;
        float w;
        if (u_params.y < 1.5) {
            w = smoothstep(160.0, 185.0, deg) * (1.0 - smoothstep(235.0, 255.0, deg)) * smoothstep(0.18, 0.40, hsv.y);
        } else {
            // red armour: the red / rust hues of the Resistance, not the grey metal
            float hueDist = deg < 180.0 ? deg : 360.0 - deg;
            float redness = 1.0 - smoothstep(22.0, 36.0, hueDist);
            w = redness * smoothstep(0.10, 0.24, hsv.y) * smoothstep(0.005, 0.02, hsv.z);
        }
        vec3 team = rgb2hsv(v_color.rgb);
        // the rust of the Resistance is muted: its recoloured armour gets more saturation to read as the new colour
        float gain = u_params.y > 1.5 ? 1.7 : 1.0;
        vec3 recoloured = hsv2rgb(vec3(team.x, min(1.0, hsv.y * gain * (0.6 + 0.5 * team.y)), hsv.z));
        c.rgb = mix(c.rgb, recoloured, w);
        c.a *= v_color.a;
    } else {
        c *= v_color;
    }
    o_color = c;
}
