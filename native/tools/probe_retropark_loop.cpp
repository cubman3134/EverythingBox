// RetroPark live-loop headless proof (Slice 2a) — the behavioural guard behind RetroParkView's play surface.
//
// probe_retropark (Slice 1) proved a single driven frame reads back inside EverythingBox's process. This probe
// proves the LIVE LOOP RetroParkView drives: that repeatedly calling rp_runtime_present on a driven core makes
// the picture ADVANCE (animation), that rp_runtime_pause FREEZES it (present repeats the retained frame byte
// for byte), and that rp_runtime_resume makes it advance again. Those three are the entire contract the view's
// QTimer + pause menu rely on, so they are asserted here — where there is no window, no GUI thread and no GPU
// requirement beyond a D3D11 device — rather than in a live UI run (that is Task 7).
//
// STATIC-CORE path (the DLL-free / iOS-shape path), identical to probe_retropark: RefCoreDriven.cpp is compiled
// straight into this probe with the exported getter renamed (-Drp_get_core_abi=refcore_driven_static_get_core_abi,
// wired in native/CMakeLists.txt), registered in the process-wide StaticCoreRegistry, and loaded by id via
// rp_runtime_load_static_core. No DLL, no ROM, no window.
//
// The driven reference core derives every pixel from a frame counter (RefCoreDriven.cpp: rising blue over a
// green field), so one forward frame changes the picture by a couple of levels of blue in every pixel — a
// memcmp catches it, and the diagnostics print the exact differing-byte count so a regression is legible.
//
// Sentinel: prints "RETROPARK-LOOP-OK" once its contract is satisfied — either the real advance/pause/resume
// proof passed, OR there is no D3D11 device to run on (a GPU-less environment with no WARP fallback), a graceful
// HONEST skip modelled on probe_retropark. The skip additionally prints "PROBE probe_retropark_loop SKIPPED
// (no D3D11 device)". On a machine with a device the full run must pass. Any real failure prints which assertion
// failed and the actual byte-diff counts, then returns non-zero.

#include <retropark/retropark.h>
#include "loader/StaticCoreRegistry.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

// The driven reference core's getter, renamed at compile time by native/CMakeLists.txt so its RefCoreDriven.cpp
// can be linked straight into this probe without colliding with the ABI's canonical rp_get_core_abi symbol name.
extern "C" const rp_core_abi* refcore_driven_static_get_core_abi(void);

namespace {
// Number of bytes that differ between two equal-length RGBA8 frames. An independent oracle for "did the picture
// change": 0 == byte-identical, > 0 == advanced. Not std::memcmp alone, because the diagnostics want the count.
size_t byte_diffs(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    size_t n = 0;
    for (size_t i = 0; i < a.size(); ++i) if (a[i] != b[i]) ++n;
    return n;
}
}

int main() {
    // Register the statically-compiled-in driven core under an id the runtime resolves with no DLL/filesystem.
    rp::StaticCoreRegistry::register_core("refcore_driven", &refcore_driven_static_get_core_abi);

    const uint32_t W = 64, H = 64;
    const size_t   N = (size_t)W * H * 4;

    // Headless: RP_GFX_D3D11 + a null native window, exactly as probe_retropark / the driven e2e test do.
    rp_runtime* rt = rp_runtime_create(RP_GFX_D3D11, nullptr);
    if (rt == nullptr) {
        // No D3D11 device (no GPU, no WARP fallback). Graceful skip — the contract is satisfied by reporting the
        // absence honestly, not by faking frames. The token still prints so the gate stays green where no device
        // exists; the SKIPPED line makes that legible.
        std::printf("PROBE probe_retropark_loop SKIPPED (no D3D11 device)\n");
        std::printf("RETROPARK-LOOP-OK\n");
        return 0;
    }

    if (rp_runtime_resize(rt, W, H) != RP_OK) {
        std::printf("PROBE probe_retropark_loop FAILED: rp_runtime_resize != RP_OK\n");
        rp_runtime_destroy(rt);
        return 1;
    }

    if (rp_runtime_load_static_core(rt, "refcore_driven") != RP_OK) {
        std::printf("PROBE probe_retropark_loop FAILED: rp_runtime_load_static_core(\"refcore_driven\") != RP_OK\n");
        rp_runtime_destroy(rt);
        return 1;
    }

    std::vector<uint8_t> a(N, 0), b(N, 0), c(N, 0), d(N, 0);
    int rc = 0;

    // 1. ANIMATION ADVANCES. Two consecutive forward presents must differ: the driven core ran a frame between
    //    them, so the picture is not the same bytes.
    if (rp_runtime_present(rt, a.data()) != RP_OK || rp_runtime_present(rt, b.data()) != RP_OK) {
        std::printf("PROBE probe_retropark_loop FAILED: a forward rp_runtime_present returned != RP_OK\n");
        rc = 1;
    } else if (byte_diffs(a, b) == 0) {
        std::printf("PROBE probe_retropark_loop FAILED: two forward presents were byte-identical "
                    "(%zu differing bytes; the driven core did not advance)\n", byte_diffs(a, b));
        rc = 1;
    }

    // 2. PAUSE FREEZES. After rp_runtime_pause, present does not advance — it re-composites the retained frame —
    //    so two paused presents are byte-for-byte identical (the overlay is a fixed-colour quad, deterministic).
    if (rc == 0) {
        if (rp_runtime_pause(rt) != RP_OK) {
            std::printf("PROBE probe_retropark_loop FAILED: rp_runtime_pause != RP_OK\n");
            rc = 1;
        } else if (rp_runtime_present(rt, c.data()) != RP_OK || rp_runtime_present(rt, d.data()) != RP_OK) {
            std::printf("PROBE probe_retropark_loop FAILED: a paused rp_runtime_present returned != RP_OK\n");
            rc = 1;
        } else if (byte_diffs(c, d) != 0) {
            std::printf("PROBE probe_retropark_loop FAILED: two PAUSED presents differed "
                        "(%zu differing bytes; pause must repeat the retained frame)\n", byte_diffs(c, d));
            rc = 1;
        }
    }

    // 3. RESUME ADVANCES AGAIN. After rp_runtime_resume the next present runs a frame, so it differs from the
    //    frozen paused frame.
    if (rc == 0) {
        std::vector<uint8_t> e(N, 0);
        if (rp_runtime_resume(rt) != RP_OK) {
            std::printf("PROBE probe_retropark_loop FAILED: rp_runtime_resume != RP_OK\n");
            rc = 1;
        } else if (rp_runtime_present(rt, e.data()) != RP_OK) {
            std::printf("PROBE probe_retropark_loop FAILED: the post-resume rp_runtime_present returned != RP_OK\n");
            rc = 1;
        } else if (byte_diffs(d, e) == 0) {
            std::printf("PROBE probe_retropark_loop FAILED: the frame did not advance after resume "
                        "(%zu differing bytes vs the paused frame)\n", byte_diffs(d, e));
            rc = 1;
        }
    }

    rp_runtime_unload_core(rt);
    rp_runtime_destroy(rt);

    if (rc == 0) {
        std::printf("PROBE probe_retropark_loop PASSED\n");
        std::printf("RETROPARK-LOOP-OK\n");
    }
    return rc;
}
