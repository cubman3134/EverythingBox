#pragma once

// Off-screen slang-shader present pass (issue #99, SLICE 4).
//
// Runs a resolved shader preset over one emulator frame and hands back a filtered QImage the EXISTING QPainter
// present path draws — so the on-screen compositing (bezel, viewport, achievement toast) is untouched. It owns a
// DEDICATED offscreen GL context created lazily on first real use; it never reuses RetroView's HW-core glCtx_, so
// a shader pass can never disturb a hardware core's own rendering, and a software core needs no GL of its own.
// Every GL call happens on the thread that calls render() (RetroView calls it from paintEvent — the GUI thread).
//
// THE SAFETY POSTURE: this object is only constructed and touched when a NON-off shader preset is active. With
// the default (off) preset RetroView never calls in here, so the default display path allocates no GL and runs
// byte-for-byte as before this slice. Any failure in here (no GL, a preset that will not compile, a bad frame)
// returns a null QImage — the caller then draws the plain frame, never a blank screen and never a crash.

#include <QString>
#include <QImage>
#include <QSize>
#include <cstddef>

#include "ShaderChain.h"   // the RAII librashader GL filter-chain wrapper (slice 3)

class QOpenGLContext;
class QOffscreenSurface;
class QOpenGLFramebufferObject;

class ShaderRenderer {
public:
    ShaderRenderer() = default;
    ~ShaderRenderer();

    ShaderRenderer(const ShaderRenderer&) = delete;
    ShaderRenderer& operator=(const ShaderRenderer&) = delete;

    // ---- asset plumbing (pure, no GL) -----------------------------------------------------------------------
    // The on-disk shaders directory the app extracts its bundled :/eb/shaders tree into (under AppPaths::dataDir).
    static QString shadersRoot();
    // Copy the app-bundled :/eb/shaders/* into shadersRoot() once (idempotent: only writes files that are absent
    // or whose bytes differ). Needed because librashader loads a preset from a REAL filesystem path — a Qt
    // resource path (":/eb/…") is not one. A no-op after the first successful extraction.
    static void ensureExtracted();
    // The absolute .slangp path a resolved preset id should load, or "" when nothing should render:
    //   * "" / "off"                 -> "" (the caller draws the plain frame)
    //   * "custom:<abs>"             -> <abs> if it exists on disk, else ""
    //   * a builtin id (e.g. "crt")  -> shadersRoot()/<registry slangp> if that file exists, else "" (an
    //                                   ecosystem-only preset like mega-bezel, which this app does not ship,
    //                                   resolves to "" and falls back to the plain draw).
    // Extracts the bundle on demand for builtin ids.
    static QString slangpPathForPreset(const QString& presetId);

    // Filter `src` (drawn at its own native resolution) through the preset at `slangpPath`, producing a
    // `target`-sized image ready to draw 1:1. The compiled chain is cached by `presetId` and rebuilt only when
    // the id changes (a repeated bad id is not retried every frame — it stays null and returns null cheaply).
    // Returns a NULL QImage on any failure. `lastFrameMs`, when non-null, receives the GPU pass wall-time (ms).
    QImage render(const QImage& src, QSize target, const QString& slangpPath,
                  const QString& presetId, std::size_t frameCount, double* lastFrameMs = nullptr);

    bool glReady() const { return ctx_ != nullptr; } // a context has been brought up (for diagnostics/tests)

private:
    bool ensureContext();            // lazily bring up ctx_/surface_; false (and remembered) if no GL is available
    bool ensureSourceTex(QSize s);   // (re)allocate srcTex_ to hold a frame of size s
    bool ensureTarget(QSize t);      // (re)allocate outFbo_ at target size t
    void teardown();                 // free the chain + GL objects with the context current, then the context

    QOpenGLContext*           ctx_     = nullptr;
    QOffscreenSurface*        surface_ = nullptr;
    QOpenGLFramebufferObject* outFbo_  = nullptr;
    unsigned                  srcTex_  = 0;
    QSize                     srcSize_;        // current srcTex_ allocation
    QSize                     targetSize_;     // current outFbo_ allocation
    ShaderChain               chain_;          // compiled filter chain for chainPreset_
    QString                   chainPreset_;    // the preset id chain_ was built for (built, valid or not)
    bool                      ctxFailed_ = false; // a prior GL bring-up failed; do not retry every frame
};
