// RetroPark PRESENTING-core foundation spike (Slice 3a) — the de-risk proof for the heavy-app path.
//
// Slice 2a/2b proved EverythingBox can consume a DRIVEN RetroPark core headlessly on D3D11 (probe_retropark /
// _loop / _content). This probe proves the OTHER shape — a PRESENTING core — the same CPU-readback way. A
// presenting core renders on the GPU itself (this is how Dolphin / RPCS3 will plug in later) and the runtime
// composites + reads its frame back through rp_runtime_present. That read-back only works when the runtime is
// created HEADLESS on VULKAN: a real window makes it RP_ERR_UNSUPPORTED, and RetroPark's Runtime.cpp requires a
// presenting core's graphics_api to equal the runtime's api — refcore_present_vk (and dolphin_present) are
// "vulkan", so presenting REQUIRES rp_runtime_create(RP_GFX_VULKAN, nullptr). That single fact is what this spike
// exercises inside EverythingBox's OWN process, exactly as the app will.
//
// It mirrors RetroPark's own presenting-vulkan end-to-end test (tests/test_vulkan_e2e.cpp): create a headless
// Vulkan runtime, load the DYNAMIC refcore_present_vk core (a light test-pattern presenting core that needs NO
// content), poll rp_runtime_present up to ~60 frames, and assert the presenting core's frame landed — its green
// shows up in the bottom-right quadrant (the exact pixel that test keys on: img[((H-4)*W+(W-4))*4 + 1] > 150).
//
// Unlike the driven probes this is the DYNAMIC-DLL path: refcore_present_vk.dll + core.json are staged beside
// this exe by the build (EB_RP_VK_CORE_DIR points at that staged dir), and rp_runtime_load_core loads it — which
// is itself the proof that a PRESENTING core with graphics_api "vulkan" loads on a Vulkan runtime (the api-match
// gate in Runtime.cpp). REQUIRE-strength: a driven probe could fall back to the static path, but a presenting
// core is a DLL by nature, so the staged dir must be present when a Vulkan device exists.
//
// Sentinel: prints "RETROPARK-PRESENT-OK" once its contract is satisfied — either the real Vulkan-presenting
// green-frame proof passed, OR there is no Vulkan device / the Vulkan runtime could not be created (a GPU-less
// CI box), a graceful HONEST skip modelled on the D3D11 probes. The skip additionally prints "PROBE
// probe_retropark_present SKIPPED (no vulkan device)" so a green result on a headless CI box reads as a skip,
// not a silent pass. On a machine with a Vulkan device the full green-frame run must pass; any real failure
// prints which assertion failed and the actual pixel values, then returns non-zero.

#include <retropark/retropark.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#ifndef EB_RP_VK_CORE_DIR
// Fallback (overridden by native/CMakeLists.txt to the staged dir beside this exe): the presenting core dir.
#define EB_RP_VK_CORE_DIR "cores/refcore_present_vk"
#endif

int main() {
    const uint32_t W = 64, H = 64;
    const size_t   N = (size_t)W * H * 4;

    // Headless PRESENTING path: RP_GFX_VULKAN + a null native window. A Vulkan runtime is MANDATORY here — a
    // presenting core cannot read back on D3D11 or on a real window. A null runtime means no capable Vulkan
    // device on this box (no GPU / no software Vulkan): a graceful skip, exactly like the D3D11 probes' no-device
    // arm. RETROPARK-PRESENT-OK still prints so a device-less gate stays green; the SKIPPED line makes it legible.
    rp_runtime* rt = rp_runtime_create(RP_GFX_VULKAN, nullptr);
    if (rt == nullptr) {
        std::printf("PROBE probe_retropark_present SKIPPED (no vulkan device)\n");
        std::printf("RETROPARK-PRESENT-OK\n");
        return 0;
    }

    if (rp_runtime_resize(rt, W, H) != RP_OK) {
        std::printf("PROBE probe_retropark_present FAILED: rp_runtime_resize != RP_OK\n");
        rp_runtime_destroy(rt);
        return 1;
    }

    // Load the DYNAMIC presenting core (DLL + core.json staged beside this exe). RP_OK REQUIRED: this is the spike's
    // core claim — a PRESENTING core whose manifest graphics_api is "vulkan" loads on a Vulkan runtime (the
    // api-match gate). A non-OK here means either the core wasn't staged or the presenting/Vulkan load path broke.
    const rp_result lr = rp_runtime_load_core(rt, EB_RP_VK_CORE_DIR);
    if (lr != RP_OK) {
        std::printf("PROBE probe_retropark_present FAILED: rp_runtime_load_core(\"%s\") returned %d, wanted RP_OK "
                    "(is refcore_present_vk.dll + core.json staged there?)\n", EB_RP_VK_CORE_DIR, (int)lr);
        rp_runtime_unload_core(rt);
        rp_runtime_destroy(rt);
        return 1;
    }

    // Poll present() for up to ~60 frames, giving the core thread time to submit a Vulkan-rendered frame the
    // runtime composites and reads back. Mirrors test_vulkan_e2e.cpp: the presenting core paints its green into
    // the bottom-right quadrant, so once a frame lands, the green channel of the (W-4,H-4) pixel rises past 150.
    std::vector<uint8_t> img(N, 0);
    auto at = [&](uint32_t x, uint32_t y, int c) -> int { return img[((size_t)y * W + x) * 4 + c]; };

    rp_result pr = RP_ERR_INTERNAL;
    bool sawCore = false;
    for (int i = 0; i < 60 && !sawCore; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        pr = rp_runtime_present(rt, img.data());
        if (pr != RP_OK) continue;
        if (at(W - 4, H - 4, 1) > 150) sawCore = true;   // bottom-right green channel of the presenting core's frame
    }

    int rc = 0;
    if (pr != RP_OK) {
        std::printf("PROBE probe_retropark_present FAILED: rp_runtime_present never returned RP_OK "
                    "(last result = %d)\n", (int)pr);
        rc = 1;
    } else if (!sawCore) {
        std::printf("PROBE probe_retropark_present FAILED: the presenting core's frame never read back "
                    "(bottom-right pixel rgba = %d,%d,%d,%d after 60 present() calls; wanted G>150)\n",
                    at(W - 4, H - 4, 0), at(W - 4, H - 4, 1), at(W - 4, H - 4, 2), at(W - 4, H - 4, 3));
        rc = 1;
    }

    rp_runtime_unload_core(rt);
    rp_runtime_destroy(rt);

    if (rc == 0) {
        std::printf("PROBE probe_retropark_present PASSED (Vulkan presenting core rendered + read back in-process)\n");
        std::printf("RETROPARK-PRESENT-OK\n");
    }
    return rc;
}
