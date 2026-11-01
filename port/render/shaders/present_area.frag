#version 450
// "Smooth pixel edges": the picture scaled to the size the window shows it at (render_native.cpp, show), each
// window pixel the average of the picture's pixels under it, weighed by how much of it each covers (an area
// filter). Made smaller (Match the window rounds the resolution scale up: a 1080-line window shows a 1200-line
// picture), every picture pixel counts by its share, so a block of N64 pixels stays a block with at most one
// mixed pixel at its edges; a linear blit read 2 x 2 pixels per window pixel and dropped or doubled lines.
// Made larger (Original 1x in a big window), it is sharp bilinear: whole pixels with one mixed pixel at each
// edge. The average is taken on linear light (the picture's sRGB values decoded and encoded again).
layout(set = 0, binding = 0) uniform sampler2D u_src;
layout(push_constant) uniform Push {
    ivec2 src_size; // the picture's pixels
    vec2 ratio;     // picture pixels a window pixel spans, across and down
} push;
layout(location = 0) out vec4 o_color;

vec3 to_linear(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

vec3 to_srgb(vec3 c) {
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}

void main() {
    vec2 lo = floor(gl_FragCoord.xy) * push.ratio, hi = lo + push.ratio; // the window pixel on the picture
    ivec2 a = clamp(ivec2(floor(lo)), ivec2(0), push.src_size - 1);
    ivec2 b = clamp(ivec2(ceil(hi)) - 1, ivec2(0), push.src_size - 1);
    vec3 sum = vec3(0.0);
    float total = 0.0;
    for (int y = a.y; y <= b.y; y++) {
        float wy = min(hi.y, float(y + 1)) - max(lo.y, float(y));
        if (wy <= 0.0) continue;
        for (int x = a.x; x <= b.x; x++) {
            float w = (min(hi.x, float(x + 1)) - max(lo.x, float(x))) * wy;
            if (w <= 0.0) continue;
            sum += to_linear(texelFetch(u_src, ivec2(x, y), 0).rgb) * w;
            total += w;
        }
    }
    vec3 c = total > 0.0 ? sum / total : vec3(0.0);
    o_color = vec4(clamp(to_srgb(max(c, vec3(0.0))), 0.0, 1.0), 1.0);
}
