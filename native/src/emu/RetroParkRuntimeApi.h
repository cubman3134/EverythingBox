// RetroPark runtime graphics-API selection (Slice 3a). ONE responsibility: given the KIND of core a game will
// run on, name the rp_graphics_api the runtime must be CREATED with — a decision that has to be made at
// rp_runtime_create, BEFORE any core is loaded, so it cannot be read back off the core after the fact.
//
// Why this is not a constant. RetroPark cores come in two shapes:
//   * DRIVEN     — the core produces CPU pixels the runtime uploads; it is graphics-api-EXEMPT (its declared
//                  graphics_api is "none"), so it runs on EverythingBox's proven headless D3D11 runtime. This is
//                  the refcore/FCEUmm-shim path Slice 2a/2b ship, and it must stay exactly that.
//   * PRESENTING — the core renders on the GPU ITSELF (the heavy-app path, e.g. Dolphin) and the runtime reads
//                  its composited frame back through rp_runtime_present. That read-back only works when the
//                  runtime was created HEADLESS on VULKAN, and RetroPark's Runtime.cpp additionally REQUIRES the
//                  presenting core's graphics_api to equal the runtime's api. dolphin_present and
//                  refcore_present_vk are both "vulkan", so a presenting core mandates a Vulkan runtime.
//
// Header-only + pure: the single map is trivial and depends only on the C-ABI enum header (retropark_abi.h,
// which is just enums/typedefs — no link, no GPU), so a headless probe unit-tests it (probe_retropark_apiselect)
// and RetroParkView consumes it, both from this one home. No spelling of the D3D11-vs-Vulkan choice lives twice.
#pragma once
#include <retropark/retropark_abi.h>   // rp_graphics_api, RP_GFX_D3D11, RP_GFX_VULKAN

namespace rpapi {

// The two core shapes RetroPark distinguishes for the purpose of picking a runtime graphics API.
enum class CoreKind { Driven, Presenting };

// The graphics API to hand rp_runtime_create for a core of this kind. A PRESENTING core MUST get a Vulkan
// runtime (present() only reads a presenting frame back when the runtime is headless Vulkan, and the runtime
// rejects a presenting core whose graphics_api differs from the runtime's). A DRIVEN core keeps the proven
// headless D3D11 path. Pure and constexpr so it is a compile-time fact every caller and the probe share.
inline constexpr rp_graphics_api runtimeApiForCore(CoreKind kind)
{
    return kind == CoreKind::Presenting ? RP_GFX_VULKAN : RP_GFX_D3D11;
}

} // namespace rpapi
