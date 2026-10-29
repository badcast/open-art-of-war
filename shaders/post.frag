#version 450
// The world picture onto the screen: upscaling of the reduced-resolution picture (low / medium quality),
// colour grading and lighting (exposure, contrast, saturation, tint, vignette)
layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

layout(set = 2, binding = 0) uniform sampler2D u_texture;
layout(set = 3, binding = 0) uniform Params {
    vec4 u_grade;    // exposure, contrast, saturation, vignette strength
    vec4 u_tint;     // rgb multiplier (light colour), w: grading on (> 0.5)
    vec4 u_filter;   // x: sharp upscaling (> 0.5)
};

void main() {
    vec2 uv = v_uv;
    if (u_filter.x > 0.5) {
        vec2 size = vec2(textureSize(u_texture, 0));
        vec2 p = uv * size;
        vec2 width = max(fwidth(p), vec2(1.0e-5));
        vec2 f = fract(p) - 0.5;
        uv = (floor(p) + 0.5 + clamp(f / width, -0.5, 0.5)) / size;
    }
    vec4 c = texture(u_texture, uv) * v_color;
    if (u_tint.w > 0.5) {
        vec3 col = c.rgb * u_grade.x;
        col = (col - 0.5) * u_grade.y + 0.5;
        float luma = dot(col, vec3(0.2126, 0.7152, 0.0722));
        col = mix(vec3(luma), col, u_grade.z);
        col *= u_tint.rgb;
        vec2 d = v_uv - 0.5;
        col *= 1.0 - u_grade.w * smoothstep(0.25, 0.75, dot(d, d) * 2.0);
        c.rgb = clamp(col, 0.0, 1.0);
    }
    o_color = c;
}
