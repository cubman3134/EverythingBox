# stage_dolphin_vehicle.cmake — run at build time (cmake -P) from a POST_BUILD step.
#
# Stages the RetroPark DOLPHIN presenting vehicle beside the app so the presenting path can do
# rp_runtime_load_core("<exeDir>/cores/dolphin_present") and Dolphin can find its Sys data.
#
# The vehicle (dolphin_present.dll + Data/Sys) is a full local Dolphin build. It is GIT-IGNORED, is NOT in
# the RetroPark submodule, is unbuildable by EB, and is NEVER present on CI or a fresh clone. Only
# cores/dolphin_present/core.json is tracked (EB gets it from the submodule). So this stager must:
#   * ALWAYS stage core.json (tracked, present everywhere) — the descriptor the GC backend keys off.
#   * IF the DLL exists locally, also stage dolphin_present.dll + Data/Sys.
#   * IF the DLL is absent, stage only core.json, emit a STATUS message, and SUCCEED (no error) — the
#     Dolphin/GC backend is simply unavailable in that build; every other path is unaffected. EB never
#     SHIPS Dolphin — it stages the user's own local build on the user's own machine.
#
# The existence check runs HERE, at build time, not at configure time: the vehicle DLL may appear or
# disappear between `cmake` and `cmake --build` (it is a separate local build), so a configure-time
# check could stage the wrong state. This mirrors stage_retropark_shim.cmake / stage_retropark_vk_core.cmake.
#
# WHERE Sys goes — VERIFIED, not guessed. dolphin_present.dll resolves its Sys directory as
# `GetSysDirectory() = GetExeDirectory() + "/Sys/"`, and GetExeDirectory() derives from
# `Common::GetModuleName(nullptr)` == `GetModuleFileNameW(nullptr, ...)`, i.e. the MAIN PROCESS
# executable — NOT the DLL's own directory. (See the vehicle source:
#   external/dolphin/Source/Core/Common/FileUtil.cpp  GetExePath()/GetExeDirectory()/CreateSysDirectoryPath()
#   external/dolphin/Source/Core/Common/CommonFuncs.cpp GetModuleName() -> GetModuleFileNameW(nullptr)
#   external/dolphin/Source/Core/DolphinNoGUI/rp_dolphin.cpp HostThread() -> UICommon::SetUserDirectory("")
#     and BootDolphinThread() which passes an EMPTY user_dir; nothing calls SetSysDirectory.)
# When dolphin_present.dll is LoadLibrary'd into EverythingBox.exe, GetModuleFileNameW(nullptr) returns
# EverythingBox.exe's path, so Dolphin looks for Sys at `<exeDir>/Sys/` (beside the app exe), NOT under
# cores/dolphin_present/. Hence Data/Sys is staged to EXE_DIR/Sys, while the DLL + core.json go to the
# core dir the Runtime load_core's from (EXE_DIR/cores/dolphin_present).
#
# Inputs (all via -D):
#   VEHICLE_DIR    EB_DOLPHIN_VEHICLE_DIR; DLL at <VEHICLE_DIR>/Binary/x64/dolphin_present.dll, Sys at
#                  <VEHICLE_DIR>/Data/Sys. May be absent (CI / fresh clone) — degrade gracefully.
#   CORE_JSON_SRC  the TRACKED core.json in the submodule (${RETROPARK_DIR}/cores/dolphin_present/core.json).
#   EXE_DIR        $<TARGET_FILE_DIR:everythingbox> — the app exe dir. Data/Sys is copied here -> EXE_DIR/Sys.
#   DEST_CORE      EXE_DIR/cores/dolphin_present — the DLL + core.json land here (load_core dir).

# --- ALWAYS: stage the tracked core.json (present on every machine, incl. CI) --------------------------------
if(NOT EXISTS "${CORE_JSON_SRC}")
    message(FATAL_ERROR "stage_dolphin_vehicle: tracked core.json missing at '${CORE_JSON_SRC}' — the "
                        "dolphin_present submodule descriptor must exist (it is committed in RetroPark).")
endif()
file(MAKE_DIRECTORY "${DEST_CORE}")
file(COPY "${CORE_JSON_SRC}" DESTINATION "${DEST_CORE}")

# --- CONDITIONAL: stage the local vehicle (DLL + Sys) if present, else degrade gracefully --------------------
set(_dll "${VEHICLE_DIR}/Binary/x64/dolphin_present.dll")
set(_sys "${VEHICLE_DIR}/Data/Sys")
if(EXISTS "${_dll}")
    file(COPY "${_dll}" DESTINATION "${DEST_CORE}")
    if(EXISTS "${_sys}")
        # Copy the Data/Sys directory INTO EXE_DIR, producing EXE_DIR/Sys/... (GC/, Wii/, Resources/, ...),
        # exactly where dolphin_present's GetSysDirectory() (main-exe-relative) looks. No trailing slash on
        # the source => the "Sys" directory itself is created under EXE_DIR.
        file(COPY "${_sys}" DESTINATION "${EXE_DIR}")
        message(STATUS "stage_dolphin_vehicle: staged dolphin_present.dll -> '${DEST_CORE}' and Data/Sys -> "
                       "'${EXE_DIR}/Sys' (Dolphin resolves Sys beside the main exe).")
    else()
        message(STATUS "stage_dolphin_vehicle: dolphin_present.dll staged but Data/Sys ABSENT at '${_sys}' — "
                       "Dolphin cannot find its Sys data; the GC backend will fail to boot until Sys is present.")
    endif()
else()
    message(STATUS "Dolphin vehicle absent at ${VEHICLE_DIR} — RetroPark GC backend will be unavailable in this build")
endif()
