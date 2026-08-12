#include "ShaderRenderer.h"

#include "../core/AppPaths.h"
#include "../core/ShaderPreset.h"

#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QSurfaceFormat>

namespace {

// librashader resolves GL entry points through this loader. It is only ever called synchronously from inside
// ShaderChain::create(), on the thread whose context we make current just before the call — so a file-static
// pointer set around that one call is safe (no shader work is concurrent; the present pass is GUI-thread only).
QOpenGLContext* g_loaderCtx = nullptr;
const void* shaderGlLoader(const char* name)
{
    if (!g_loaderCtx || !name) return nullptr;
    return reinterpret_cast<const void*>(g_loaderCtx->getProcAddress(name));
}

} // namespace

QString ShaderRenderer::shadersRoot()
{
    return AppPaths::dataDir() + QStringLiteral("/shaders");
}

void ShaderRenderer::ensureExtracted()
{
    const QString root = shadersRoot();
    // Walk the bundled resource tree and mirror it onto disk, writing only files that are missing or whose bytes
    // differ (so an upgraded shader is refreshed, but an unchanged tree is a pure no-op). If the bundle is absent
    // (never expected in the app; a probe that did not compile the .qrc in) this loop simply does nothing.
    QDirIterator it(QStringLiteral(":/eb/shaders"), QDir::Files, QDirIterator::Subdirectories);
    bool any = false;
    while (it.hasNext())
    {
        const QString resPath = it.next();                 // e.g. ":/eb/shaders/crt.slangp"
        const QString rel = resPath.mid(QStringLiteral(":/eb/shaders/").size());
        const QString dst = root + QStringLiteral("/") + rel;

        QFile in(resPath);
        if (!in.open(QIODevice::ReadOnly)) continue;
        const QByteArray bytes = in.readAll();
        in.close();

        bool needWrite = true;
        QFile existing(dst);
        if (existing.exists() && existing.open(QIODevice::ReadOnly))
        {
            needWrite = (existing.readAll() != bytes);
            existing.close();
        }
        if (!needWrite) { any = true; continue; }

        QDir().mkpath(QFileInfo(dst).absolutePath());
        QFile out(dst);
        if (out.open(QIODevice::WriteOnly)) { out.write(bytes); out.close(); any = true; }
    }
    (void)any;
}

QString ShaderRenderer::slangpPathForPreset(const QString& presetId)
{
    switch (ShaderPreset::kindForId(presetId))
    {
    case ShaderPreset::Kind::Off:
        return QString();
    case ShaderPreset::Kind::Custom:
    {
        const QString abs = ShaderPreset::customPath(presetId);
        return QFileInfo::exists(abs) ? abs : QString();
    }
    case ShaderPreset::Kind::Builtin:
    {
        const ShaderPreset::Entry e = ShaderPreset::entryForId(presetId);
        if (e.slangp.isEmpty()) return QString();
        ensureExtracted();
        const QString abs = shadersRoot() + QStringLiteral("/") + e.slangp;
        return QFileInfo::exists(abs) ? abs : QString();  // ecosystem-only presets (unshipped) -> plain draw
    }
    }
    return QString();
}

ShaderRenderer::~ShaderRenderer() { teardown(); }

bool ShaderRenderer::ensureContext()
{
    if (ctx_)       return true;
    if (ctxFailed_) return false;

    // Use the DEFAULT surface format — the same setup slice 3's GPU-verified probe_shaderchain uses. A machine's
    // default desktop GL is 4.x compatibility here, which satisfies librashader's >= 3.3 (glsl 330) requirement;
    // forcing an explicit 3.3 CORE profile instead fails to create a context on this driver. RetroView's own HW
    // path (setupHwRender) likewise runs on a default-format context.
    surface_ = new QOffscreenSurface();
    surface_->create();

    ctx_ = new QOpenGLContext();
    if (!surface_->isValid() || !ctx_->create() || !ctx_->makeCurrent(surface_))
    {
        // No usable GL here (a GPU-less machine, a headless CI runner). Remember it so we never retry per frame.
        delete ctx_;     ctx_ = nullptr;
        delete surface_; surface_ = nullptr;
        ctxFailed_ = true;
        return false;
    }
    return true;
}

bool ShaderRenderer::ensureSourceTex(QSize s)
{
    QOpenGLFunctions* f = ctx_->functions();
    if (!srcTex_)
    {
        f->glGenTextures(1, &srcTex_);
        srcSize_ = QSize();
    }
    f->glBindTexture(GL_TEXTURE_2D, srcTex_);
    if (s != srcSize_)
    {
        f->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, s.width(), s.height(), 0,
                        GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        srcSize_ = s;
    }
    return srcTex_ != 0;
}

bool ShaderRenderer::ensureTarget(QSize t)
{
    if (outFbo_ && targetSize_ == t) return true;
    delete outFbo_;
    outFbo_ = new QOpenGLFramebufferObject(t, QOpenGLFramebufferObject::NoAttachment); // color-only RGBA8 texture
    targetSize_ = t;
    return outFbo_->isValid();
}

QImage ShaderRenderer::render(const QImage& src, QSize target, const QString& slangpPath,
                              const QString& presetId, std::size_t frameCount, double* lastFrameMs)
{
    if (lastFrameMs) *lastFrameMs = 0.0;
    if (src.isNull() || target.isEmpty() || slangpPath.isEmpty()) return QImage();
    if (!ensureContext()) return QImage();

    ctx_->makeCurrent(surface_);
    QOpenGLFunctions* f = ctx_->functions();
    f->initializeOpenGLFunctions();

    QElapsedTimer clock;
    clock.start();

    // (Re)build the chain only when the preset changes. A build FAILURE still records chainPreset_ so we do not
    // recompile a known-bad preset every frame; chain_.isValid() stays false and we bail to the plain draw.
    if (presetId != chainPreset_)
    {
        chainPreset_ = presetId;
        g_loaderCtx = ctx_;
        QString err;
        const bool ok = chain_.create(slangpPath, &shaderGlLoader, &err);
        g_loaderCtx = nullptr;
        if (!ok)
            qWarning("shader: preset '%s' failed to load (%s) — falling back to the plain frame",
                     presetId.toUtf8().constData(), err.toUtf8().constData());
    }
    if (!chain_.isValid()) { ctx_->doneCurrent(); return QImage(); }

    // Upload the frame (as tightly-packed RGBA8) into the source texture.
    const QImage rgba = src.convertToFormat(QImage::Format_RGBA8888);
    f->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (!ensureSourceTex(rgba.size())) { ctx_->doneCurrent(); return QImage(); }
    f->glBindTexture(GL_TEXTURE_2D, srcTex_);
    f->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, rgba.width(), rgba.height(),
                       GL_RGBA, GL_UNSIGNED_BYTE, rgba.constBits());
    f->glBindTexture(GL_TEXTURE_2D, 0);

    if (!ensureTarget(target)) { ctx_->doneCurrent(); return QImage(); }

    QString ferr;
    const bool fok = chain_.filter(srcTex_, rgba.width(), rgba.height(),
                                   outFbo_->texture(), target.width(), target.height(),
                                   frameCount, &ferr);
    if (!fok)
    {
        qWarning("shader: filter failed for '%s' (%s)", presetId.toUtf8().constData(), ferr.toUtf8().constData());
        ctx_->doneCurrent();
        return QImage();
    }

    // Read the filtered result back. librashader renders GL bottom-left origin, so flip to a top-down QImage.
    QImage outImg(target, QImage::Format_RGBA8888);
    outFbo_->bind();
    f->glPixelStorei(GL_PACK_ALIGNMENT, 1);
    f->glReadPixels(0, 0, target.width(), target.height(), GL_RGBA, GL_UNSIGNED_BYTE, outImg.bits());
    outFbo_->release();

    if (lastFrameMs) *lastFrameMs = clock.nsecsElapsed() / 1.0e6;
    ctx_->doneCurrent();
    return outImg.mirrored(false, true);
}

void ShaderRenderer::teardown()
{
    if (ctx_)
    {
        ctx_->makeCurrent(surface_);
        chain_ = ShaderChain();                 // free the librashader chain with its creating context current
        delete outFbo_; outFbo_ = nullptr;
        if (srcTex_) { ctx_->functions()->glDeleteTextures(1, &srcTex_); srcTex_ = 0; }
        ctx_->doneCurrent();
        delete ctx_;     ctx_ = nullptr;
    }
    delete surface_; surface_ = nullptr;
}
