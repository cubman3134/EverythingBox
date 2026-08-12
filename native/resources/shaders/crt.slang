#version 450
// CRT — a minimal single-pass tube look: scanlines + a faint aperture-grille column tint + a gentle
// corner vignette. Not CRT-Royale quality; a real Mega-Bezel-class chain is a load-from-disk follow-up
// (issue #99). Rendered at display resolution.
layout(push_constant) uniform Push {
    vec4 SourceSize;    // (w, h, 1/w, 1/h) of the game frame
    vec4 OriginalSize;
    vec4 OutputSize;    // (w, h, 1/w, 1/h) of the display target
    uint FrameCount;
} params;

#pragma stage vertex
layout(location = 0) in vec4 Position;
layout(location = 1) in vec2 TexCoord;
layout(location = 0) out vec2 vTexCoord;
void main() { gl_Position = Position; vTexCoord = TexCoord; }

#pragma stage fragment
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;
layout(set = 0, binding = 2) uniform sampler2D Source;

const float PI = 3.14159265;
void main() {
    vec2 uv = vTexCoord;
    vec3 col = texture(Source, uv).rgb;
    // Scanlines (per source line).
    float t = fract(uv.y * params.SourceSize.y);
    float scan = 0.60 + 0.40 * sin(t * PI);
    // Aperture-grille: a subtle per-source-column brightness ripple.
    float ap = 0.90 + 0.10 * sin(uv.x * params.SourceSize.x * PI);
    // Corner vignette.
    vec2 d = uv - 0.5;
    float vig = 1.0 - dot(d, d) * 0.35;
    FragColor = vec4(col * scan * ap * vig, 1.0);
}
