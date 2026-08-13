# stage_retropark_shim.cmake — run at build time (cmake -P) from the everythingbox POST_BUILD step.
#
# Places RetroPark's libretro shim beside the built app so RetroParkView can do
# rp_runtime_load_core("<exeDir>/cores/libretro_shim"): the shim (LibretroShim.dll) + its core.json
# come from the retropark_ext ExternalProject build tree; fceumm_libretro.dll is EB's OWN core — the
# shim self-locates it from its own directory at runtime, so it must sit next to LibretroShim.dll.
#
# Inputs (all via -D):
#   RP_BUILD_DIR  the retropark_ext BINARY_DIR (search root for the shim outputs)
#   RP_CONFIG     the build config ($<CONFIG>), used to disambiguate a multi-config layout
#   DEST          <exeDir>/cores/libretro_shim  (where the three files must land)
#   FCEUMM_SRC    EB's own downloaded core: <exeDir>/cores/fceumm_libretro.dll (default source)
#   FCEUMM_ALT    optional -DEB_FCEUMM_DLL override (an fceumm to stage now); may be empty
#
# The shim + core.json are REQUIRED (a missing one is a real build break). fceumm is OPTIONAL here:
# on a first build EB has not downloaded it yet, so we warn rather than fail; a later relink (once
# fceumm sits in <exeDir>/cores/) mirrors it in, or -DEB_FCEUMM_DLL supplies it directly.

# --- locate the STAGED shim output under the ExternalProject build tree (layout-agnostic) -----------
# The shim's own POST_BUILD copies LibretroShim.dll + core.json together into
# $<TARGET_FILE_DIR:LibretroShim>/cores/libretro_shim/. There is ALSO a bare linker-output
# LibretroShim.dll one level up with no core.json beside it, so the selection must key on the DLL
# that has core.json in its OWN directory (that is the staged pair we want) — and, when both Debug
# and Release trees exist, prefer the one under the matching config subdir.
file(GLOB_RECURSE _shim_dlls "${RP_BUILD_DIR}/*LibretroShim.dll")
set(_shim_dll "")
foreach(_cand ${_shim_dlls})                 # pass 1: has core.json beside it AND matches config
    get_filename_component(_d "${_cand}" DIRECTORY)
    if(EXISTS "${_d}/core.json" AND _cand MATCHES "/${RP_CONFIG}/")
        set(_shim_dll "${_cand}")
        break()
    endif()
endforeach()
if(NOT _shim_dll)
    foreach(_cand ${_shim_dlls})             # pass 2: has core.json beside it (any config)
        get_filename_component(_d "${_cand}" DIRECTORY)
        if(EXISTS "${_d}/core.json")
            set(_shim_dll "${_cand}")
            break()
        endif()
    endforeach()
endif()
if(NOT _shim_dll)
    message(FATAL_ERROR "stage_retropark_shim: no LibretroShim.dll with a core.json beside it found under "
                        "'${RP_BUILD_DIR}'. Did retropark_ext build the LibretroShim target (its POST_BUILD "
                        "stages the shim + core.json together)?")
endif()
get_filename_component(_shim_dir "${_shim_dll}" DIRECTORY)

file(MAKE_DIRECTORY "${DEST}")
file(COPY "${_shim_dll}" DESTINATION "${DEST}")
file(COPY "${_shim_dir}/core.json" DESTINATION "${DEST}")

# --- fceumm: EB's own core, mirrored next to the shim (optional at build time) ----------------------
set(_fceumm "")
if(FCEUMM_ALT AND EXISTS "${FCEUMM_ALT}")
    set(_fceumm "${FCEUMM_ALT}")
elseif(EXISTS "${FCEUMM_SRC}")
    set(_fceumm "${FCEUMM_SRC}")
endif()
if(_fceumm)
    file(COPY "${_fceumm}" DESTINATION "${DEST}")
    message(STATUS "stage_retropark_shim: staged shim + core.json + fceumm into '${DEST}' (fceumm from '${_fceumm}').")
else()
    message(WARNING "stage_retropark_shim: staged shim + core.json into '${DEST}', but fceumm_libretro.dll "
                    "is not present yet (looked at '${FCEUMM_SRC}'). The shim will not load a NES ROM until "
                    "fceumm sits beside it. EB downloads fceumm into <exeDir>/cores/ on first NES launch; a "
                    "later relink mirrors it here, or pass -DEB_FCEUMM_DLL=<path-to-fceumm_libretro.dll>.")
endif()
