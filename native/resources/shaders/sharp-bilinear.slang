#version 450
// Sharp-bilinear — crisp integer-ish upscaling that keeps pixels sharp but antialiases the sub-pixel
// seams, the standard "sharp-bilinear" algorithm (issue #99). Needs a linear Source filter (.slangp).
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
    vec2 texSize = params.SourceSize.xy;
    // Whole-integer scale factor from source to output (>= 1), the width of the sharp region.
    vec2 scale = max(floor(params.OutputSize.xy * params.SourceSize.zw), vec2(1.0));
    vec2 p = vTexCoord * texSize;            // source-texel coordinate
    vec2 base = floor(p) + 0.5;              // centre of the source texel
    // Compress the interpolation to a narrow band around each texel boundary.
    vec2 f = p - base;
    vec2 region = clamp(f * scale, -0.5, 0.5);
    vec2 uv = (base + region) * params.SourceSize.zw;
    FragColor = vec4(texture(Source, uv).rgb, 1.0);
}
