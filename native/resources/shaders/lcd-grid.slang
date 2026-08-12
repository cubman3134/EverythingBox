#version 450
// LCD grid — a minimal single-pass per-source-pixel grid, darkening the cell edges to fake the dot
// matrix of a handheld panel (issue #99). Rendered at display resolution.
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

void main() {
    vec3 col = texture(Source, vTexCoord).rgb;
    // Position within the current source pixel (0..1), distance from its centre.
    vec2 cell = abs(fract(vTexCoord * params.SourceSize.xy) - 0.5);
    // Bright in the cell centre, darkening toward the gridlines at the edges.
    float grid = smoothstep(0.5, 0.40, max(cell.x, cell.y));
    float mult = 0.78 + 0.22 * grid;
    FragColor = vec4(col * mult, 1.0);
}
