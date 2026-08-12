#version 450
// Passthrough — samples the source 1:1. The end-to-end proof that the librashader GL pass is wired
// correctly (issue #99, slice 4). Single pass, no effect. (#version MUST be the first line for slang.)
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
void main() { FragColor = vec4(texture(Source, vTexCoord).rgb, 1.0); }
