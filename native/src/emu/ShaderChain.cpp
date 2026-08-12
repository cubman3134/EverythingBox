#include "ShaderChain.h"

#include <cstdint>

namespace {

// Pull a librashader error's message into *error (best-effort) and free the error object.
// libra_error_write allocates a string that must be released with libra_error_free_string;
// the error object itself is released with libra_error_free. If the message can't be read we
// still surface the numeric errno so the caller never gets an empty string for a real failure.
void takeError(libra_error_t err, QString* error)
{
    if (error) {
        char* msg = nullptr;
        if (err && libra_error_write(err, &msg) == 0 && msg) {
            *error = QString::fromUtf8(msg);
            libra_error_free_string(&msg);
        } else {
            *error = QStringLiteral("librashader error (errno %1)")
                         .arg(err ? static_cast<int>(libra_error_errno(err)) : 0);
        }
    }
    if (err) libra_error_free(&err);
}

} // namespace

ShaderChain::~ShaderChain() { reset(); }

ShaderChain::ShaderChain(ShaderChain&& other) noexcept : chain_(other.chain_)
{
    other.chain_ = nullptr;
}

ShaderChain& ShaderChain::operator=(ShaderChain&& other) noexcept
{
    if (this != &other) {
        reset();
        chain_ = other.chain_;
        other.chain_ = nullptr;
    }
    return *this;
}

void ShaderChain::reset()
{
    if (chain_) {
        // Nulls chain_ on success; assign null defensively in case of a null/invalid handle.
        libra_gl_filter_chain_free(&chain_);
        chain_ = nullptr;
    }
}

bool ShaderChain::create(const QString& slangpPath, libra_gl_loader_t glLoader, QString* error)
{
    reset();
    if (error) error->clear();

    // 1. Parse the preset. A missing/garbage path fails HERE, before any GL entry point is
    //    touched — so this branch reports false without needing a live GL context.
    const QByteArray path = slangpPath.toUtf8();
    libra_shader_preset_t preset = nullptr;
    if (libra_error_t e = libra_preset_create(path.constData(), &preset)) {
        takeError(e, error);
        return false;
    }

    // 2. Build the GL filter chain. libra_gl_filter_chain_create consumes and invalidates the
    //    preset whether it succeeds or fails.
    filter_chain_gl_opt_t opt{};
    opt.version         = LIBRASHADER_CURRENT_VERSION;
    opt.glsl_version    = 330;   // librashader requires >= 330
    opt.use_dsa         = false;
    opt.force_no_mipmaps = false;
    opt.disable_cache   = false;

    libra_gl_filter_chain_t chain = nullptr;
    if (libra_error_t e = libra_gl_filter_chain_create(&preset, glLoader, &opt, &chain)) {
        takeError(e, error);
        if (preset) libra_preset_free(&preset);  // defensive — normally already consumed
        return false;
    }

    chain_ = chain;
    return true;
}

bool ShaderChain::filter(unsigned sourceTex, int srcW, int srcH,
                         unsigned targetTex, int dstW, int dstH,
                         std::size_t frameCount, QString* error,
                         unsigned sourceFormat, unsigned targetFormat)
{
    if (error) error->clear();
    if (!chain_) {
        if (error)
            *error = QStringLiteral("ShaderChain::filter called with no chain loaded");
        return false;
    }

    libra_image_gl_t source{};
    source.handle = sourceTex;
    source.format = sourceFormat;
    source.width  = static_cast<uint32_t>(srcW < 0 ? 0 : srcW);
    source.height = static_cast<uint32_t>(srcH < 0 ? 0 : srcH);

    libra_image_gl_t out{};
    out.handle = targetTex;
    out.format = targetFormat;
    out.width  = static_cast<uint32_t>(dstW < 0 ? 0 : dstW);
    out.height = static_cast<uint32_t>(dstH < 0 ? 0 : dstH);

    // viewport null -> full render target; mvp null -> identity; opt null -> chain defaults.
    if (libra_error_t e = libra_gl_filter_chain_frame(&chain_, frameCount, source, out,
                                                      nullptr, nullptr, nullptr)) {
        takeError(e, error);
        return false;
    }
    return true;
}
