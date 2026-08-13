// RetroPark DOLPHIN presenting-core load proof (Slice 3b) — the strongest headless check short of a full GC boot.
//
// probe_retropark_present (3a) proved the PRESENTING mechanism generically: a Vulkan presenting core
// (refcore_present_vk) loads + renders + reads back inside EB's own process. This probe proves the SPECIFIC core
// RetroParkView's 3b path loads — the real Dolphin vehicle (dolphin_present.dll) — actually loads on that same
// headless Vulkan runtime, in-process, exactly as RetroParkView::openGame does it:
//     rp_runtime_create(RP_GFX_VULKAN, nullptr)  →  rp_runtime_resize(GC geometry)  →  rp_runtime_load_core(<dir>)
// It STOPS before rp_runtime_load_content: there is no committable GC ISO (the user's ISO is the live Task-6 proof),
// and Dolphin is requires_content, so a full boot is deliberately left to Task 6. load_core returning RP_OK is the
// claim: a PRESENTING core whose manifest graphics_api is "vulkan" is accepted by the runtime's api-match gate, its
// get_info metadata parses, and the (multi-hundred-MB) Dolphin DLL initialises without crashing our process.
//
// LOCAL-ONLY, graceful everywhere else. The Dolphin vehicle (dolphin_present.dll + Data/Sys) is git-ignored, not in
// the submodule, unbuildable by EB, and NEVER present on CI or a fresh clone. It is staged beside the app exe by
// EverythingBox's own POST_BUILD (which shares this build's output dir), so when EB has been built here with the
// vehicle, dolphin_present.dll sits at <exeDir>/cores/dolphin_present and Dolphin's Sys at <exeDir>/Sys (Dolphin
// resolves Sys via GetModuleFileNameW(nullptr) = THIS probe's exe, which is in that same dir). When the vehicle is
// absent (CI / fresh clone / EB not built), the DLL is not there and the probe DEFERS — a graceful, legible skip,
// never a failure — exactly like probe_retropark_present defers when no Vulkan device exists.
//
// Sentinel: prints "RETROPARK-DOLPHIN-OK" once its contract is satisfied — the real load_core proof passed, OR
// there is no Vulkan device, OR the vehicle is not staged. Each skip prints a "PROBE ... SKIPPED (reason)" line so
// a green result on CI reads as an honest skip, not a silent pass. On a machine with a Vulkan device AND the staged
// vehicle, load_core RP_OK is REQUIRED; any real failure prints the result code and returns non-zero.

#include <retropark/retropark.h>

#include <cstdint>
#include <cstdio>
#include <string>

#ifndef EB_RP_DOLPHIN_CORE_DIR
// Fallback (overridden by native/CMakeLists.txt to the staged dir beside this exe): the Dolphin core dir.
#define EB_RP_DOLPHIN_CORE_DIR "cores/dolphin_present"
#endif

int main() {
    const std::string coreDir = EB_RP_DOLPHIN_CORE_DIR;
    const std::string dllPath = coreDir + "/dolphin_present.dll";

    // Vehicle-present gate FIRST — cheap, and it lets a GPU-equipped box without the local Dolphin build defer
    // without paying for a Vulkan device create. dolphin_present.dll is the local-only artifact; its absence means
    // CI / fresh clone / EB-not-built-here, all of which must skip.
    if (std::FILE* f = std::fopen(dllPath.c_str(), "rb")) {
        std::fclose(f);
    } else {
        std::printf("PROBE probe_retropark_dolphin SKIPPED (Dolphin vehicle not staged at %s)\n", dllPath.c_str());
        std::printf("RETROPARK-DOLPHIN-OK\n");
        return 0;
    }

    // Headless PRESENTING runtime: RP_GFX_VULKAN + null window — the only config that reads a presenting frame back,
    // and the api the Dolphin core's "vulkan" manifest must match. A null runtime means no capable Vulkan device on
    // this box: a graceful skip, exactly like probe_retropark_present's no-device arm.
    rp_runtime* rt = rp_runtime_create(RP_GFX_VULKAN, nullptr);
    if (rt == nullptr) {
        std::printf("PROBE probe_retropark_dolphin SKIPPED (no vulkan device)\n");
        std::printf("RETROPARK-DOLPHIN-OK\n");
        return 0;
    }

    // Size to the GameCube output geometry RetroParkView uses (640x480), before load_core — the proven order for a
    // presenting core (probe_retropark_present resizes before load on the Vulkan runtime; resize also initialises
    // the graphics backend that load_core needs).
    if (rp_runtime_resize(rt, 640, 480) != RP_OK) {
        std::printf("PROBE probe_retropark_dolphin FAILED: rp_runtime_resize != RP_OK\n");
        rp_runtime_destroy(rt);
        return 1;
    }

    // THE CLAIM: the Dolphin presenting core loads on the Vulkan runtime in-process. RP_OK REQUIRED here — the
    // vehicle is staged (checked above) and a Vulkan device exists (create succeeded), so a non-OK means the
    // presenting/Vulkan load path (api-match gate, manifest parse, or the Dolphin DLL's own init) is broken. We do
    // NOT rp_runtime_load_content: no committable ISO, and Dolphin's full GC boot is the live Task-6 proof.
    const rp_result lr = rp_runtime_load_core(rt, coreDir.c_str());
    int rc = 0;
    if (lr != RP_OK) {
        std::printf("PROBE probe_retropark_dolphin FAILED: rp_runtime_load_core(\"%s\") returned %d, wanted RP_OK "
                    "(the Dolphin presenting core failed to load on the Vulkan runtime)\n", coreDir.c_str(), (int)lr);
        rc = 1;
    }

    rp_runtime_unload_core(rt);
    rp_runtime_destroy(rt);

    if (rc == 0) {
        std::printf("PROBE probe_retropark_dolphin PASSED (Dolphin presenting core loaded on the Vulkan runtime "
                    "in-process)\n");
        std::printf("RETROPARK-DOLPHIN-OK\n");
    }
    return rc;
}
