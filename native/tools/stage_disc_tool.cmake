# stage_disc_tool.cmake — run at build time (cmake -P) from a POST_BUILD step.
#
# Stages the PATCHED Dolphin disc tool beside the app, under `<exeDir>/disc-tool/`, together with the
# licence material that has to travel with it. EmulatorManager::seedDiscTool copies that folder into the
# managed Dolphin install at first use, and MainWindow's discToolPath() looks for exactly the staged name.
#
# WHY THE RENAME. The stock Dolphin download already contains a `DolphinTool.exe`, and it is the wrong
# binary for this job: measured on tag 2606, stock `DolphinTool convert -i <directory>` answers "The input
# file could not be opened." The two builds are indistinguishable by name, and telling them apart by
# running one would cost a process launch per install and would guess wrong when the guess is expensive.
# So the patched build is staged under a name the upstream archive does not contain, and the app trusts
# only that name — provenance, not behaviour. If it is absent the app refuses the install by name rather
# than starting a multi-minute compose that dies on the stock tool's own error.
#
# GRACEFUL WHEN THE TOOL IS ABSENT. The patched build comes from a local Dolphin source tree
# (EB_DOLPHIN_VEHICLE_DIR) that is git-ignored, absent on CI and on a fresh clone, and unbuildable by EB —
# the same situation stage_dolphin_vehicle.cmake is written for. When it is missing this stages NOTHING and
# succeeds: the app builds, and the Wii file-replacement install path reports itself unavailable. The
# licence files are deliberately NOT staged on their own — an offer of source for a program that is not
# there would be a lie about what the build contains.
#
# The existence check runs HERE, at build time, not at configure time: the tool may appear or disappear
# between `cmake` and `cmake --build` (it is a separate local build), so a configure-time check could stage
# a stale answer. Mirrors stage_dolphin_vehicle.cmake / stage_retropark_shim.cmake.
#
# Inputs (all via -D):
#   VEHICLE_DIR   EB_DOLPHIN_VEHICLE_DIR; the built tool at <VEHICLE_DIR>/Binary/x64/DolphinTool.exe.
#   LICENSE_DIR   native/licenses — the GPL notice + the source patches that discharge the offer.
#   DEST          <exeDir>/disc-tool — where the app looks for the shipped copy.
#   TOOL_NAME     the staged filename (EverythingBoxDiscTool.exe); must match EmulatorManager::discToolName().

set(_src "${VEHICLE_DIR}/Binary/x64/DolphinTool.exe")

if(NOT EXISTS "${_src}")
    message(STATUS "stage_disc_tool: patched DolphinTool absent at '${_src}' — Wii file-replacement mod "
                   "installs will be unavailable in this build (the app refuses them by name).")
    return()
endif()

file(MAKE_DIRECTORY "${DEST}")

# ONLY_IF_DIFFERENT: this is ~11 MB and POST_BUILD runs on every relink of the app.
file(COPY_FILE "${_src}" "${DEST}/${TOOL_NAME}" ONLY_IF_DIFFERENT)

# The licence notice and the full source difference from the pinned upstream commit. Dolphin is GPLv2+, so
# a modified binary may not travel without them; they are tracked in the repo, so unlike the binary they
# are always present here.
if(EXISTS "${LICENSE_DIR}/EverythingBoxDiscTool.LICENSE.txt")
    file(GLOB _lic "${LICENSE_DIR}/EverythingBoxDiscTool.LICENSE.txt" "${LICENSE_DIR}/*.patch")
    file(COPY ${_lic} DESTINATION "${DEST}")
else()
    message(FATAL_ERROR "stage_disc_tool: the GPL notice for the shipped disc tool is missing from "
                        "'${LICENSE_DIR}' — the modified binary must not be staged without it.")
endif()

message(STATUS "stage_disc_tool: staged the patched disc tool + its GPL source offer -> '${DEST}'")
