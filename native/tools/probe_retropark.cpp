// RetroPark foundation spike (build + link + driven-core headless proof).
//
// This is the FIRST slice of integrating the separate RetroPark runtime ("a better libretro", GPLv3, in its own
// repo) into EverythingBox as a future emulation backend. It does NOT touch any UI or the game-launch path. Its
// whole job is to prove, headlessly, that RetroPark's static library builds + links into EverythingBox's build,
// and that a DRIVEN reference core produces a real video frame INSIDE EverythingBox's own process — read back
// through rp_runtime_present exactly as EverythingBox reads a QImage. Nothing more.
//
// It mirrors RetroPark's own driven end-to-end test (tests/test_driven_e2e.cpp): create a headless runtime on
// D3D11 with a null window, load the DRIVEN reference core (which loads on any graphics API), then loop
// rp_runtime_present and assert the core rendered a green field with a blended overlay.
//
// STATIC-CORE path (the DLL-free / iOS-shape path): instead of load_core(dir) which dlopens a core DLL from the
// filesystem, RefCoreDriven.cpp is compiled straight into this probe with the exported getter renamed
// (-Drp_get_core_abi=refcore_driven_static_get_core_abi, wired in native/CMakeLists.txt), registered in the
// process-wide StaticCoreRegistry, and loaded by id via rp_runtime_load_static_core. No DLL, no ROM, no window.
//
// Sentinel: prints "RETROPARK-OK" once its contract is satisfied — either the real green-pixel proof passed, OR
// there is no D3D11 device to run on (a GPU-less environment with no WARP fallback), which is a graceful,
// HONEST skip modelled on how RetroPark's own Vulkan test guards on probe_vulkan_shared(). The skip additionally
// prints "PROBE probe_retropark SKIPPED (no D3D11 device)" so a green result on a headless CI box is legible as
// a skip, not a silent pass. On a machine with a device (this one) the full green-pixel run must pass. Any real
// failure prints which assertion failed and the actual pixel values, then returns non-zero.
//
// This probe is OPT-IN (CMake option EB_WITH_RETROPARK, OFF by default): RetroPark is a heavy optional
// dependency, not yet vendored/submoduled, and needs the Vulkan SDK + a D3D11 device — so the default build and
// default CI are completely unaffected and this binary simply does not exist there.

#include <retropark/retropark.h>
#include "loader/StaticCoreRegistry.h"

#include <cstdio>
#include <cstdint>
#include <vector>

// The driven reference core's getter, renamed at compile time by native/CMakeLists.txt so its RefCoreDriven.cpp
// can be linked straight into this probe without colliding with the ABI's canonical rp_get_core_abi symbol name.
extern "C" const rp_core_abi* refcore_driven_static_get_core_abi(void);

int main() {
    // Register the statically-compiled-in driven core under an id the runtime resolves with no DLL/filesystem.
    rp::StaticCoreRegistry::register_core("refcore_driven", &refcore_driven_static_get_core_abi);

    const uint32_t W = 64, H = 64;

    // Headless: RP_GFX_D3D11 + a null native window, exactly as the driven e2e test does.
    rp_runtime* rt = rp_runtime_create(RP_GFX_D3D11, nullptr);
    if (rt == nullptr) {
        // No D3D11 device (no GPU, no WARP fallback). Graceful skip — the probe's contract is satisfied by
        // reporting the absence honestly, not by faking a frame. RETROPARK-OK still prints so the gate stays
        // green where no device exists; the SKIPPED line makes that legible.
        std::printf("PROBE probe_retropark SKIPPED (no D3D11 device)\n");
        std::printf("RETROPARK-OK\n");
        return 0;
    }

    if (rp_runtime_resize(rt, W, H) != RP_OK) {
        std::printf("PROBE probe_retropark FAILED: rp_runtime_resize != RP_OK\n");
        rp_runtime_destroy(rt);
        return 1;
    }

    if (rp_runtime_load_static_core(rt, "refcore_driven") != RP_OK) {
        std::printf("PROBE probe_retropark FAILED: rp_runtime_load_static_core(\"refcore_driven\") != RP_OK\n");
        rp_runtime_destroy(rt);
        return 1;
    }

    std::vector<uint8_t> img((size_t)W * H * 4, 0);
    // Pixel accessor into the read-back RGBA8 frame, matching the driven e2e test's helper.
    auto at = [&](uint32_t x, uint32_t y, int c) -> int {
        return img[((size_t)y * W + x) * 4 + c];
    };

    // The whole point: create + core-loaded + a frame rendered + read back inside EverythingBox's own process.
    bool sawGreen = false;
    for (int i = 0; i < 10 && !sawGreen; i++) {
        if (rp_runtime_present(rt, img.data()) != RP_OK) continue;
        if (at(W - 4, H - 4, 1) > 150) sawGreen = true;  // bottom-right green channel of the driven core's field
    }

    int rc = 0;
    if (!sawGreen) {
        std::printf("PROBE probe_retropark FAILED: no green frame after 10 present() calls "
                    "(bottom-right pixel rgba = %d,%d,%d,%d; wanted G>150)\n",
                    at(W - 4, H - 4, 0), at(W - 4, H - 4, 1), at(W - 4, H - 4, 2), at(W - 4, H - 4, 3));
        rc = 1;
    } else {
        // The overlay blends over the green field: the top-left overlay pixel carries blue, and its green is
        // below the bottom-right field green (so the overlay really composited on top, not a flat fill).
        const int tlBlue  = at(4, 4, 2);
        const int tlGreen = at(4, 4, 1);
        const int brGreen = at(W - 4, H - 4, 1);
        if (!(tlBlue > 80)) {
            std::printf("PROBE probe_retropark FAILED: overlay blue too low "
                        "(top-left blue = %d; wanted >80)\n", tlBlue);
            rc = 1;
        } else if (!(tlGreen < brGreen)) {
            std::printf("PROBE probe_retropark FAILED: overlay did not blend over the field "
                        "(top-left green = %d not < bottom-right green = %d)\n", tlGreen, brGreen);
            rc = 1;
        }
    }

    rp_runtime_unload_core(rt);
    rp_runtime_destroy(rt);

    if (rc == 0) {
        std::printf("PROBE probe_retropark PASSED\n");
        std::printf("RETROPARK-OK\n");
    }
    return rc;
}
