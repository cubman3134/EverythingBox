// Headless proof that librashader (issue #99, SLICE 2) is IN THE BUILD: that librashader-capi — the Rust
// C-ABI crate, compiled FROM SOURCE via corrosion — links into a C++ program, and that its C ABI is callable
// WITHOUT any GPU context. This is the link+call proof the slice exists to give. Creating a filter CHAIN needs
// a live GL/D3D/Vulkan device and is a LATER slice; it is deliberately NOT exercised here (no runtime backend
// is even compiled in this slice — see the NO_DEFAULT_FEATURES note in native/CMakeLists.txt).
//
// Prints LIBRASHADER-OK on success; any failure prints LIBRASHADER-FAIL <cond> (line) and exits non-zero.
//
// WHAT IT PINS (all CPU-only — no GL/D3D/Vulkan device is created):
//   * the ABI/API version entry points resolve and return the versions the VENDORED header was generated at
//     (libra_instance_abi_version() == LIBRASHADER_CURRENT_ABI,
//      libra_instance_api_version() == LIBRASHADER_CURRENT_VERSION). This is the cheapest possible "the real
//     library is linked and its exported symbols resolve" check — a broken link fails to build, a wrong pin
//     fails this compare.
//   * libra_preset_create parses a hand-written passthrough .slangp into a NON-null preset handle and returns
//     NO error (a null libra_error_t); libra_preset_free then releases it. The preset loader is reachable
//     through the C ABI end to end.
//   * a NONEXISTENT preset path returns a NON-null error object (the error surface works, not only the happy
//     path), and libra_error_free clears it back to null.
//
// The .slangp + .slang fixtures are hand-authored below and written to a unique OS temp directory that is
// removed on exit, so this probe writes NOTHING into the build folder (the exe-folder contamination gate stays
// quiet) and needs no Qt — it is plain C++17 + <filesystem>.
#include "librashader.h"

#include <chrono>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "LIBRASHADER-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

// A valid slang "stock" passthrough shader. Whether libra_preset_create merely parses the .slangp and resolves
// this path, or additionally preprocesses the referenced source, a real and existing shader satisfies both.
static const char* kPassthroughSlang =
    "#version 450\n"
    "\n"
    "layout(push_constant) uniform Push\n"
    "{\n"
    "    vec4 SourceSize;\n"
    "    vec4 OriginalSize;\n"
    "    vec4 OutputSize;\n"
    "    uint FrameCount;\n"
    "} params;\n"
    "\n"
    "#pragma stage vertex\n"
    "layout(location = 0) in vec4 Position;\n"
    "layout(location = 1) in vec2 TexCoord;\n"
    "layout(location = 0) out vec2 vTexCoord;\n"
    "void main()\n"
    "{\n"
    "    gl_Position = Position;\n"
    "    vTexCoord = TexCoord;\n"
    "}\n"
    "\n"
    "#pragma stage fragment\n"
    "layout(location = 0) in vec2 vTexCoord;\n"
    "layout(location = 0) out vec4 FragColor;\n"
    "layout(set = 0, binding = 2) uniform sampler2D Source;\n"
    "void main()\n"
    "{\n"
    "    FragColor = texture(Source, vTexCoord);\n"
    "}\n";

// A single-pass passthrough preset that references the .slang beside it.
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

int main()
{
    // ==== 1. VERSION ABI — the library links and its exported version functions resolve + agree with the pin =
    // libra_instance_abi_version()/api_version() are compiled UNCONDITIONALLY by librashader-capi (they do not
    // depend on any runtime-backend feature), so they are the right symbols to prove the link with no GPU.
    CHECK(libra_instance_abi_version() == LIBRASHADER_CURRENT_ABI);
    CHECK(libra_instance_api_version() == LIBRASHADER_CURRENT_VERSION);

    // A scratch dir in the OS temp area (NOT the build folder), unique per process, removed on the way out.
    std::error_code ec;
    const auto uniq = static_cast<unsigned long long>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    fs::path dir = fs::temp_directory_path(ec) /
        (std::string("eb-librashader-") + std::to_string(uniq));
    fs::create_directories(dir, ec);
    CHECK(!ec);

    const fs::path slang  = dir / "passthrough.slang";
    const fs::path slangp = dir / "passthrough.slangp";
    CHECK(writeFile(slang,  kPassthroughSlang));
    CHECK(writeFile(slangp, kPassthroughSlangp));

    // ==== 2. PRESET LOAD — the happy path: a real preset parses to a handle with no error, then frees ========
    {
        libra_shader_preset_t preset = nullptr;
        libra_error_t err = libra_preset_create(slangp.string().c_str(), &preset);
        CHECK(err == nullptr);       // null libra_error_t == success
        CHECK(preset != nullptr);    // a real handle came back
        if (err) libra_error_free(&err);
        if (preset) {
            libra_error_t ferr = libra_preset_free(&preset);
            CHECK(ferr == nullptr);
            if (ferr) libra_error_free(&ferr);
        }
    }

    // ==== 3. ERROR SURFACE — a nonexistent preset yields a NON-null error (not a silent success) =============
    {
        libra_shader_preset_t bad = nullptr;
        const fs::path missing = dir / "no-such-preset.slangp";
        libra_error_t err = libra_preset_create(missing.string().c_str(), &bad);
        CHECK(err != nullptr);       // a missing/unparseable preset must be reported, not swallowed
        CHECK(bad == nullptr);       // ...and no handle is produced
        if (err) libra_error_free(&err);
        if (bad) libra_preset_free(&bad);
    }

    fs::remove_all(dir, ec);

    if (failures == 0) std::printf("LIBRASHADER-OK\n");
    else               std::fprintf(stderr, "LIBRASHADER: %d check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
