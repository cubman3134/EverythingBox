#version 450
// Scanlines — a minimal, single-pass horizontal-line darkening keyed to the game's native vertical
// resolution, rendered at display resolution so the lines are visible (issue #99).
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
    vec3 col = texture(Source, vTexCoord).rgb;
    // One bright band per source scanline: brightest at the line centre, dark at the gap.
    float t = fract(vTexCoord.y * params.SourceSize.y);
    float mult = 0.55 + 0.45 * sin(t * PI);
    FragColor = vec4(col * mult, 1.0);
}
