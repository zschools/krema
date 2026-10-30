#version 440

layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;

    // Panel geometry (pixels, matching the effect item)
    float resX;
    float resY;
    float cornerRadius;

    // Width of the bezel band the rim light falls off over
    float bezelWidth;
    // Overall strength multiplier
    float intensity;
};

// SDF for a rounded rectangle centered at the origin (same as acrylic_overlay.frag)
float roundedBoxSDF(vec2 center, vec2 halfSize, float radius) {
    vec2 d = abs(center) - halfSize + vec2(radius);
    return min(max(d.x, d.y), 0.0) + length(max(d, 0.0)) - radius;
}

// Liquid-glass specular layer for the Tinted glass effect: an approximation
// of the web/SVG liquid glass look (kube.io/blog/liquid-glass-css-svg).
// True refraction is impossible here — on Wayland only the compositor sees
// the backdrop — so this reproduces the parts a client can draw: a rim
// light whose strength follows the surface normal against a light from
// above, a hard 1px glass edge line (CSS: inset 0 0 2px 1px white 35%),
// a broad top sheen, and a soft inner shade at the bottom edge (CSS: the
// dark inset shadows). The backdrop blur + saturation boost live on the
// compositor side (backgroundstyle.cpp, KWin background contrast).
void main() {
    vec2 size = vec2(resX, resY);
    vec2 pos = qt_TexCoord0 * size;
    vec2 center = pos - size * 0.5;
    vec2 halfSize = size * 0.5;

    float dist = roundedBoxSDF(center, halfSize, cornerRadius);
    float mask = 1.0 - smoothstep(-0.5, 0.5, dist);
    if (mask <= 0.0) {
        fragColor = vec4(0.0);
        return;
    }

    // Depth inside the edge in pixels.
    float d = max(-dist, 0.0);

    // Surface normal from the SDF gradient; near the panel middle the
    // gradient degenerates, but every term below is weighted to ~0 there.
    float e = 1.0;
    vec2 grad = vec2(
        roundedBoxSDF(center + vec2(e, 0.0), halfSize, cornerRadius)
            - roundedBoxSDF(center - vec2(e, 0.0), halfSize, cornerRadius),
        roundedBoxSDF(center + vec2(0.0, e), halfSize, cornerRadius)
            - roundedBoxSDF(center - vec2(0.0, e), halfSize, cornerRadius));
    vec2 n = length(grad) > 0.0001 ? normalize(grad) : vec2(0.0, -1.0);

    // Light fixed above the panel: edges facing up catch the most light.
    float ndotl = max(dot(n, vec2(0.0, -1.0)), 0.0);

    // Rim light: strongest at the edge, exponential falloff over the bezel.
    float rim = exp(-d / max(bezelWidth * 0.45, 1.0))
        * (0.25 + 0.75 * pow(ndotl, 1.5)) * 0.38;
    // Hard glass edge line, ~1px.
    float edgeLine = (1.0 - smoothstep(0.0, 1.6, d)) * 0.30;
    // Broad sheen across the face, brightest at the top.
    float sheen = smoothstep(1.05, 0.1, qt_TexCoord0.y) * 0.055;
    // Soft inner shade pooled at the bottom edge for lens depth; edges
    // facing the light keep less of it.
    float shade = exp(-(size.y - pos.y) / max(bezelWidth * 0.9, 1.0))
        * 0.10 * (1.0 - ndotl * 0.5);

    float net = (rim + edgeLine + sheen - shade) * intensity;
    vec3 col = net >= 0.0 ? vec3(1.0) : vec3(0.0);
    float a = clamp(abs(net), 0.0, 1.0) * mask * qt_Opacity;
    // Premultiplied alpha output (Qt Quick uses GL_ONE, GL_ONE_MINUS_SRC_ALPHA)
    fragColor = vec4(col * a, a);
}
