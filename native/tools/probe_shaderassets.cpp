// Headless proof for the SHIPPED shader assets + the RetroView present pass (issue #99, SLICE 4).
//
// Two layers, mirroring probe_shaderchain's GL-conditional shape:
//
//   CONTEXT-FREE (runs everywhere, incl. GPU-less CI) — the asset plumbing is pure:
//     * ShaderRenderer::ensureExtracted() mirrors the app-bundled :/eb/shaders tree onto disk, and every shipped
//       .slangp lands under ShaderRenderer::shadersRoot().
//     * slangpPathForPreset() resolves each shipped BUILTIN id ("scanlines"/"crt"/"lcd-grid"/"sharp") to an
//       existing on-disk .slangp, resolves "off"/"" to "" (draw the plain frame), resolves the UNSHIPPED
//       ecosystem preset "mega-bezel" to "" (graceful fallback), and resolves a non-existent custom path to "".
//
//   GL-CONDITIONAL (only when an offscreen GL context is obtainable — skipped, still PASSING, on CI):
//     * every shipped .slangp actually COMPILES in librashader and builds a live chain — the real value of this
//       probe: a broken shipped shader fails HERE, not on a user's screen.
//     * ShaderRenderer::render() — the exact code RetroView calls — takes a source QImage through each shipped
//       preset and returns a non-null, target-sized filtered image.
//
// Prints SHADERASSETS-OK on success; any failure prints SHADERASSETS-FAIL <cond> (line) and exits non-zero.
#include "emu/ShaderRenderer.h"
#include "core/ShaderPreset.h"

#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QString>
#include <QStringList>

#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "SHADERASSETS-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

int main(int argc, char** argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);

    // The shipped builtin ids and their expected on-disk basenames (must match ShaderPreset::registry()).
    struct Shipped { const char* id; const char* file; };
    const Shipped shipped[] = {
        { "scanlines", "scanlines.slangp" },
        { "crt",       "crt.slangp" },
        { "lcd-grid",  "lcd-grid.slangp" },
        { "sharp",     "sharp-bilinear.slangp" },
    };

    // ==== CONTEXT-FREE: extraction + resolution ==================================================
    ShaderRenderer::ensureExtracted();
    const QString root = ShaderRenderer::shadersRoot();

    // The passthrough is shipped for the pipeline proof (it is not a registry id).
    CHECK(QFileInfo::exists(root + QStringLiteral("/passthrough.slangp")));

    for (const Shipped& s : shipped) {
        const QString expect = root + QStringLiteral("/") + QString::fromLatin1(s.file);
        CHECK(QFileInfo::exists(expect));                                   // extracted to disk
        const QString resolved = ShaderRenderer::slangpPathForPreset(QString::fromLatin1(s.id));
        CHECK(resolved == expect);                                          // id -> the shipped file
        CHECK(!resolved.isEmpty() && QFileInfo::exists(resolved));
    }

    // Off / unset -> nothing renders.
    CHECK(ShaderRenderer::slangpPathForPreset(ShaderPreset::offId()).isEmpty());
    CHECK(ShaderRenderer::slangpPathForPreset(QString()).isEmpty());
    // The heavy ecosystem preset is NOT shipped -> resolves to "" (RetroView draws the plain frame).
    CHECK(ShaderPreset::isHeavyId(QStringLiteral("mega-bezel")));           // it IS a registry entry…
    CHECK(ShaderRenderer::slangpPathForPreset(QStringLiteral("mega-bezel")).isEmpty()); // …but not on disk.
    // A custom path that does not exist -> "".
    CHECK(ShaderRenderer::slangpPathForPreset(
              ShaderPreset::customPresetId(QStringLiteral("C:/no/such/preset.slangp"))).isEmpty());

    // ==== GL-CONDITIONAL: each shipped preset compiles + renders a frame =========================
    QOffscreenSurface surface;
    surface.create();
    QOpenGLContext ctx;
    const bool haveGl = surface.isValid() && ctx.create() && ctx.makeCurrent(&surface);
    ctx.doneCurrent();

    if (!haveGl) {
        std::printf("no GL context — skipping live shipped-shader render test (offscreen QPA has no GL here)\n");
    } else {
        // A small non-uniform source so a shader that samples SourceSize has something to chew on.
        QImage src(64, 48, QImage::Format_RGBA8888);
        for (int y = 0; y < src.height(); ++y)
            for (int x = 0; x < src.width(); ++x)
                src.setPixelColor(x, y, QColor((x * 4) % 256, (y * 5) % 256, 128, 255));
        const QSize target(128, 96);   // a 2x upscale, the shape RetroView hands in

        // One renderer reused across presets — also exercises the rebuild-on-preset-change path in render().
        ShaderRenderer r;

        // The passthrough first (direct path), then every shipped registry preset.
        QStringList paths;
        paths << root + QStringLiteral("/passthrough.slangp");
        QStringList ids;
        ids << QStringLiteral("passthrough");
        for (const Shipped& s : shipped) {
            paths << ShaderRenderer::slangpPathForPreset(QString::fromLatin1(s.id));
            ids   << QString::fromLatin1(s.id);
        }

        for (int i = 0; i < paths.size(); ++i) {
            double ms = -1.0;
            const QImage out = r.render(src, target, paths[i], ids[i], /*frame*/0, &ms);
            CHECK(!out.isNull());              // the shipped shader compiled AND filtered a frame
            CHECK(out.size() == target);       // at the requested display size
            CHECK(ms >= 0.0);                  // the frame-time was recorded
            if (out.isNull())
                std::fprintf(stderr, "  preset '%s' produced no image\n", ids[i].toUtf8().constData());
        }

        // A bogus preset path must fail softly (null image, no crash) — the RetroView fallback contract.
        double bad = -1.0;
        const QImage none = r.render(src, target, root + QStringLiteral("/does-not-exist.slangp"),
                                     QStringLiteral("bogus"), 0, &bad);
        CHECK(none.isNull());
    }

    if (failures == 0) std::printf("SHADERASSETS-OK\n");
    else               std::fprintf(stderr, "SHADERASSETS: %d check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
