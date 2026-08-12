// Headless proof for the ShaderChain wrapper (issue #99, SLICE 3): the C++ RAII wrapper over librashader's
// OpenGL filter-chain C API compiles + links against that API, its ERROR paths behave without crashing, and —
// WHEN a GL context is available — a real passthrough preset creates a live chain and filters a frame.
//
// Prints SHADERCHAIN-OK on success; any failure prints SHADERCHAIN-FAIL <cond> (line) and exits non-zero.
//
// GL-CONDITIONAL BY DESIGN. GL chain creation + libra_gl_filter_chain_frame need a CURRENT OpenGL context,
// which a headless CI runner has no GPU for. So the probe forces the offscreen QPA platform (headless-safe)
// and TRIES to bring up an offscreen QOpenGLContext; if that fails (no GL — the common CI case) it prints a
// clear "no GL context — skipping live chain test" and PASSES. It never fails CI for lack of a GPU.
//
// What runs REGARDLESS of a GL context (pure wrapper logic, mutation-killable on CI):
//   * ShaderChain::filter() on a chain with nothing loaded returns false and fills the error string — the
//     no-chain guard fires before any GL call, so it needs no context.
//   * ShaderChain::create() with a MISSING preset path returns false, fills the error, leaves the chain
//     invalid, and does not crash — a bad path fails at preset LOAD, before any GL entry point is touched.
// What runs ONLY with a live GL context:
//   * create() of a hand-authored passthrough .slangp succeeds and reports valid;
//   * filter() of one frame (small source texture -> target texture) succeeds.
#include "emu/ShaderChain.h"

#include <QByteArray>
#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QString>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "SHADERCHAIN-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

// A valid slang "stock" passthrough shader (same shape probe_librashader uses).
static const char* kPassthroughSlang =
    "#version 450\n"
    "layout(push_constant) uniform Push {\n"
    "    vec4 SourceSize; vec4 OriginalSize; vec4 OutputSize; uint FrameCount;\n"
    "} params;\n"
    "#pragma stage vertex\n"
    "layout(location = 0) in vec4 Position;\n"
    "layout(location = 1) in vec2 TexCoord;\n"
    "layout(location = 0) out vec2 vTexCoord;\n"
    "void main() { gl_Position = Position; vTexCoord = TexCoord; }\n"
    "#pragma stage fragment\n"
    "layout(location = 0) in vec2 vTexCoord;\n"
    "layout(location = 0) out vec4 FragColor;\n"
    "layout(set = 0, binding = 2) uniform sampler2D Source;\n"
    "void main() { FragColor = texture(Source, vTexCoord); }\n";

static const char* kPassthroughSlangp =
    "shaders = 1\n"
    "shader0 = passthrough.slang\n"
    "filter_linear0 = true\n"
    "scale_type0 = source\n"
    "scale0 = 1.0\n";

static bool writeFile(const fs::path& p, const char* text)
{
    std::ofstream out(p, std::ios::binary);
    if (!out) return false;
    out << text;
    return static_cast<bool>(out);
}

// GL loader handed to librashader — resolves entry points through the current QOpenGLContext.
static QOpenGLContext* g_ctx = nullptr;
static const void* glLoader(const char* name)
{
    if (!g_ctx || !name) return nullptr;
    return reinterpret_cast<const void*>(g_ctx->getProcAddress(name));
}

// A never-called stub loader for the context-free error-path assertions (preset load fails first).
static const void* nullLoader(const char*) { return nullptr; }

int main(int argc, char** argv)
{
    // Headless-safe: force offscreen QPA unless the caller already pinned a platform. A GL context may or may
    // not be obtainable under offscreen — that is exactly what we test for below.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);

    // A scratch dir in the OS temp area (never the build folder), unique per process, removed on exit.
    std::error_code ec;
    const auto uniq = static_cast<unsigned long long>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    fs::path dir = fs::temp_directory_path(ec) / (std::string("eb-shaderchain-") + std::to_string(uniq));
    fs::create_directories(dir, ec);
    CHECK(!ec);
    const fs::path slang  = dir / "passthrough.slang";
    const fs::path slangp = dir / "passthrough.slangp";
    CHECK(writeFile(slang,  kPassthroughSlang));
    CHECK(writeFile(slangp, kPassthroughSlangp));

    // ==== CONTEXT-FREE ASSERTIONS (run everywhere, incl. GPU-less CI) ============================
    // 1. filter() with no chain loaded -> false + error, and NO crash (the guard returns before any GL call).
    {
        ShaderChain sc;
        CHECK(!sc.isValid());
        QString err;
        bool ok = sc.filter(/*src*/1, 4, 4, /*dst*/2, 4, 4, /*frame*/0, &err);
        CHECK(!ok);            // reported failure, not a crash
        CHECK(!err.isEmpty()); // and the error string is populated
        // The message is the guard's OWN, not a librashader error — pins that the no-chain guard fired
        // (removing the guard would let librashader produce a different message).
        CHECK(err.contains(QStringLiteral("no chain")));
    }

    // 2. create() with a MISSING preset path -> false + error, chain stays invalid, no crash. This fails at
    //    preset LOAD (before any GL entry point), so it exercises the real error path without a GL context.
    {
        ShaderChain sc;
        const fs::path missing = dir / "no-such-preset.slangp";
        QString err;
        bool ok = sc.create(QString::fromStdString(missing.string()), &nullLoader, &err);
        CHECK(!ok);
        CHECK(!sc.isValid());
        CHECK(!err.isEmpty());
    }

    // ==== GL-CONDITIONAL ASSERTIONS ==============================================================
    QOffscreenSurface surface;
    surface.create();
    QOpenGLContext ctx;
    bool haveGl = surface.isValid() && ctx.create() && ctx.makeCurrent(&surface);

    if (!haveGl) {
        std::printf("no GL context — skipping live chain test (offscreen QPA has no GL here)\n");
    } else {
        g_ctx = &ctx;
        QOpenGLFunctions* f = ctx.functions();
        f->initializeOpenGLFunctions();

        // 3. Build a live chain from the real passthrough preset.
        ShaderChain sc;
        QString err;
        bool ok = sc.create(QString::fromStdString(slangp.string()), &glLoader, &err);
        CHECK(ok);
        if (!ok) std::fprintf(stderr, "  create error: %s\n", err.toUtf8().constData());
        CHECK(sc.isValid());

        if (sc.isValid()) {
            // Small RGBA8 source + target textures.
            const int W = 8, H = 8;
            GLuint tex[2] = {0, 0};
            f->glGenTextures(2, tex);
            for (int i = 0; i < 2; ++i) {
                f->glBindTexture(GL_TEXTURE_2D, tex[i]);
                f->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            }
            f->glBindTexture(GL_TEXTURE_2D, 0);

            // 4. Filter one frame source -> target.
            QString ferr;
            bool fok = sc.filter(tex[0], W, H, tex[1], W, H, /*frame*/0, &ferr);
            CHECK(fok);
            if (!fok) std::fprintf(stderr, "  filter error: %s\n", ferr.toUtf8().constData());

            f->glDeleteTextures(2, tex);
        }
        g_ctx = nullptr;
        ctx.doneCurrent();
    }

    fs::remove_all(dir, ec);

    if (failures == 0) std::printf("SHADERCHAIN-OK\n");
    else               std::fprintf(stderr, "SHADERCHAIN: %d check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
