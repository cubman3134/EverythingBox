// Pure unit test for the RetroPark runtime graphics-API selector (Slice 3a).
//
// RetroParkView must create its runtime with the RIGHT graphics API BEFORE it loads a core — the API cannot be
// read off a core after rp_runtime_create, yet a PRESENTING core (the heavy-app / Dolphin path) only reads back
// through present() on a headless VULKAN runtime, while a DRIVEN core (the refcore / FCEUmm shim Slice 2a/2b
// ship) keeps the proven headless D3D11 path. That fork is the single pure map rpapi::runtimeApiForCore, and
// this probe pins both arms of it so a regression that (say) routed a presenting core onto D3D11 — which would
// make present() return RP_ERR_UNSUPPORTED and silently break the whole presenting path — goes red here.
//
// Header-only: it includes only retropark_abi.h (the C-ABI enum header) + the pure helper, so it links nothing,
// needs no GPU and no RetroPark static lib. It is a Windows-only target purely because the RetroPark submodule's
// headers live under the desktop guard in native/CMakeLists.txt; the logic itself is platform-agnostic.
//
// Sentinel: prints "RETROPARK-APISELECT-OK" and exits 0 iff BOTH arms map as required; on a mismatch it prints
// which arm was wrong (with the actual enum value) and returns non-zero.

#include "emu/RetroParkRuntimeApi.h"

#include <cstdio>

int main() {
    int rc = 0;

    // A PRESENTING core MUST create the runtime on Vulkan (the only api present() reads a presenting frame back on,
    // and the api the runtime demands a presenting core declare).
    const rp_graphics_api presentingApi = rpapi::runtimeApiForCore(rpapi::CoreKind::Presenting);
    if (presentingApi != RP_GFX_VULKAN) {
        std::printf("PROBE probe_retropark_apiselect FAILED: presenting core mapped to graphics api %d, "
                    "wanted RP_GFX_VULKAN (%d)\n", (int)presentingApi, (int)RP_GFX_VULKAN);
        rc = 1;
    }

    // A DRIVEN core MUST keep the proven headless D3D11 path (byte-behaviourally identical to Slice 2a/2b).
    const rp_graphics_api drivenApi = rpapi::runtimeApiForCore(rpapi::CoreKind::Driven);
    if (drivenApi != RP_GFX_D3D11) {
        std::printf("PROBE probe_retropark_apiselect FAILED: driven core mapped to graphics api %d, "
                    "wanted RP_GFX_D3D11 (%d)\n", (int)drivenApi, (int)RP_GFX_D3D11);
        rc = 1;
    }

    // The two kinds must not collapse onto the same api — a map that returned one constant for both would break
    // exactly one path while passing each single-arm check above only if the constant happened to match. Assert
    // they DIFFER so a "return RP_GFX_VULKAN for everything" (or the D3D11 twin) is caught even then.
    if (presentingApi == drivenApi) {
        std::printf("PROBE probe_retropark_apiselect FAILED: presenting and driven cores mapped to the SAME "
                    "graphics api %d — the two paths need different runtimes\n", (int)presentingApi);
        rc = 1;
    }

    // The DRIVEN host api is now user-SELECTABLE (OpenGL host runtime task). A driven core asked to run on OpenGL
    // must map to RP_GFX_OPENGL — the selector must forward the chosen driven api verbatim, not force D3D11.
    const rp_graphics_api drivenGlApi = rpapi::runtimeApiForCore(rpapi::CoreKind::Driven, RP_GFX_OPENGL);
    if (drivenGlApi != RP_GFX_OPENGL) {
        std::printf("PROBE probe_retropark_apiselect FAILED: driven core with drivenApi=OpenGL mapped to graphics "
                    "api %d, wanted RP_GFX_OPENGL (%d)\n", (int)drivenGlApi, (int)RP_GFX_OPENGL);
        rc = 1;
    }

    // A PRESENTING core IGNORES the chosen driven api: it must stay Vulkan even when OpenGL is requested, or the
    // presenting read-back path silently breaks. Pin that the drivenApi argument cannot leak onto presenting.
    const rp_graphics_api presentingGlApi = rpapi::runtimeApiForCore(rpapi::CoreKind::Presenting, RP_GFX_OPENGL);
    if (presentingGlApi != RP_GFX_VULKAN) {
        std::printf("PROBE probe_retropark_apiselect FAILED: presenting core with drivenApi=OpenGL mapped to "
                    "graphics api %d, wanted RP_GFX_VULKAN (%d) — presenting must ignore the driven api\n",
                    (int)presentingGlApi, (int)RP_GFX_VULKAN);
        rc = 1;
    }

    if (rc == 0) {
        std::printf("PROBE probe_retropark_apiselect PASSED\n");
        std::printf("RETROPARK-APISELECT-OK\n");
    }
    return rc;
}
