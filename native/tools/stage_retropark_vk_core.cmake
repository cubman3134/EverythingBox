# stage_retropark_vk_core.cmake — run at build time (cmake -P) from a POST_BUILD step.
#
# Places RetroPark's reference PRESENTING core (Vulkan) beside a built target so the app / a probe can do
# rp_runtime_load_core("<dir>/cores/refcore_present_vk"): refcore_present_vk.dll + its core.json come from the
# retropark_ext ExternalProject build tree (its own CMakeLists POST_BUILD stages the pair into
# $<TARGET_FILE_DIR:refcore_present_vk>/cores/refcore_present_vk/). This is the presenting twin of
# stage_retropark_shim.cmake and shares its layout-agnostic search.
#
# Inputs (all via -D):
#   RP_BUILD_DIR  the retropark_ext BINARY_DIR (search root for the vk core outputs)
#   RP_CONFIG     the build config ($<CONFIG>), used to disambiguate a multi-config layout
#   DEST          <dir>/cores/refcore_present_vk  (where the two files must land)
#
# refcore_present_vk needs NO extra content (it renders a Vulkan test pattern via shared surfaces), so unlike the
# libretro shim there is no fceumm to mirror — just the DLL + core.json, both REQUIRED (a missing one is a real
# build break: the presenting spike cannot load its core without them).

# --- locate the STAGED vk-core output under the ExternalProject build tree (layout-agnostic) ----------------
# The core's own POST_BUILD copies refcore_present_vk.dll + core.json together into
# $<TARGET_FILE_DIR:refcore_present_vk>/cores/refcore_present_vk/. There is ALSO a bare linker-output
# refcore_present_vk.dll one level up with no core.json beside it, so the selection keys on the DLL that has
# core.json in its OWN directory (that is the staged pair we want) — and, when both Debug and Release trees
# exist, prefer the one under the matching config subdir.
file(GLOB_RECURSE _vk_dlls "${RP_BUILD_DIR}/*refcore_present_vk.dll")
set(_vk_dll "")
foreach(_cand ${_vk_dlls})                   # pass 1: has core.json beside it AND matches config
    get_filename_component(_d "${_cand}" DIRECTORY)
    if(EXISTS "${_d}/core.json" AND _cand MATCHES "/${RP_CONFIG}/")
        set(_vk_dll "${_cand}")
        break()
    endif()
endforeach()
if(NOT _vk_dll)
    foreach(_cand ${_vk_dlls})               # pass 2: has core.json beside it (any config)
        get_filename_component(_d "${_cand}" DIRECTORY)
        if(EXISTS "${_d}/core.json")
            set(_vk_dll "${_cand}")
            break()
        endif()
    endforeach()
endif()
if(NOT _vk_dll)
    message(FATAL_ERROR "stage_retropark_vk_core: no refcore_present_vk.dll with a core.json beside it found "
                        "under '${RP_BUILD_DIR}'. Did retropark_ext build the refcore_present_vk target (its "
                        "POST_BUILD stages the dll + core.json together)?")
endif()
get_filename_component(_vk_dir "${_vk_dll}" DIRECTORY)

file(MAKE_DIRECTORY "${DEST}")
file(COPY "${_vk_dll}" DESTINATION "${DEST}")
file(COPY "${_vk_dir}/core.json" DESTINATION "${DEST}")
message(STATUS "stage_retropark_vk_core: staged refcore_present_vk.dll + core.json into '${DEST}'.")
