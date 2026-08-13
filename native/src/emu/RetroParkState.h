// Pure derivation of the on-disk path for a RetroPark savestate (Slice 2b, Task 4). Header-only and free of Qt,
// <windows.h> and any RetroPark include, so it unit-tests in probe_retropark_content with no runtime, GPU or DLL
// dependency — the non-collision property below is the thing most worth guarding, so it is the piece that gets
// asserted + mutation-killed.
//
// A game can be played on BOTH the libretro backend (RetroView) and the RetroPark backend, and each keeps its own
// save states. RetroView writes libretro states to  <dataDir>/states/<base>.state[N]  (RetroView::statePath).
// RetroPark states MUST NOT land on those files, or one backend's state would clobber the other's for the same
// ROM. We give RetroPark its own subdirectory AND its own suffix: <dataDir>/states/retropark/<base>.rpstate .
// BOTH differ from the libretro convention, so no libretro slot file ('<base>.state', '<base>.stateN',
// '<base>.state.auto') is ever the RetroPark path for that base, and vice-versa.
#pragma once
#include <string>

namespace rpstate {

// Subdir + suffix are deliberately BOTH distinct from libretro's 'states/<base>.state'. Changing either to the
// libretro shape reintroduces a cross-backend collision — probe_retropark_content asserts against exactly that.
inline std::string retroParkStatePath(const std::string& dataDir, const std::string& romBaseName) {
    return dataDir + "/states/retropark/" + romBaseName + ".rpstate";
}

} // namespace rpstate
