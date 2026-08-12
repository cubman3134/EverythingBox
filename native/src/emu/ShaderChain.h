#pragma once

// librashader OpenGL filter-chain wrapper (issue #99, SLICE 3).
//
// The vendored librashader.h gates its OpenGL declarations — libra_gl_loader_t,
// libra_gl_filter_chain_t, libra_gl_filter_chain_create/frame/free, libra_image_gl_t,
// filter_chain_gl_opt_t — behind #if defined(LIBRA_RUNTIME_OPENGL). Define it before the
// include so those types/functions are visible even in a translation unit that did not get
// the CMake target_compile_definition (belt and braces; the #ifndef makes it a no-op when
// the build already defined it, so there is no redefinition warning).
#ifndef LIBRA_RUNTIME_OPENGL
#define LIBRA_RUNTIME_OPENGL 1
#endif
#include "librashader.h"

#include <QString>
#include <cstddef>

// Thin RAII wrapper over librashader's OpenGL filter-chain C API.
//
// It does NOT own or manage a GL context. The CALLER is responsible for making its GL context
// current before every create()/filter() call AND before this object is destroyed —
// libra_gl_filter_chain_free requires the context the chain was created with to be current.
// RetroView wires that up in slice 4; this unit is standalone and context-agnostic.
//
// Movable, non-copyable: the wrapped libra_gl_filter_chain_t is a unique owning handle.
class ShaderChain {
public:
    ShaderChain() = default;
    ~ShaderChain();

    ShaderChain(ShaderChain&& other) noexcept;
    ShaderChain& operator=(ShaderChain&& other) noexcept;
    ShaderChain(const ShaderChain&) = delete;
    ShaderChain& operator=(const ShaderChain&) = delete;

    // Load `slangpPath` as a slang-shader preset and build a GL filter chain from it, resolving
    // GL entry points through `glLoader` (the caller's GL context must be current). On ANY
    // librashader error — a missing/garbage preset, a shader that fails to compile — the chain
    // is left null, *error (when non-null) is filled with the librashader message, and false is
    // returned. Never throws; never crashes on a bad preset. Re-calling create() on a live chain
    // frees the old one first. Note: a bad path fails at preset load, BEFORE any GL call — so
    // create() with a missing path reports false even without a live context.
    bool create(const QString& slangpPath, libra_gl_loader_t glLoader, QString* error);

    // Run one frame through the chain: sample `sourceTex` (srcW x srcH) and render the filtered
    // result into `targetTex` (dstW x dstH). `frameCount` is the running frame number the shader
    // sees (drives FrameCount-based effects). Returns false and fills *error on failure —
    // including being called with no chain loaded, which is reported (not a crash) and needs no
    // GL context. Textures are treated as GL_RGBA8 internally by default; override the trailing
    // format params for other internal formats.
    bool filter(unsigned sourceTex, int srcW, int srcH,
                unsigned targetTex, int dstW, int dstH,
                std::size_t frameCount, QString* error,
                unsigned sourceFormat = kGlRgba8,
                unsigned targetFormat = kGlRgba8);

    bool isValid() const { return chain_ != nullptr; }

    // GL_RGBA8 (0x8058). Spelled numerically so this header pulls in no GL headers of its own.
    static constexpr unsigned kGlRgba8 = 0x8058u;

private:
    void reset();  // frees chain_ if set — the creating GL context must be current
    libra_gl_filter_chain_t chain_ = nullptr;
};
