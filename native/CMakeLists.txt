cmake_minimum_required(VERSION 3.21)
project(EverythingBoxNative VERSION 0.5.341 LANGUAGES C CXX) # keep in sync with kAppVersion in src/main.cpp
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# MSVC: emit a .pdb for Release too, so a crash's WER fault offset can be mapped back to a function/line (the
# Release build otherwise ships no symbols — we had to reverse-engineer a Qt crash from a bare module offset).
# /Zi only writes symbols to a side .pdb (the exe stays fully optimized); /DEBUG makes the linker emit the .pdb,
# and /OPT:REF,/OPT:ICF restore the dead-code/COMDAT-folding size optimizations that a bare /DEBUG turns off.
# Set before any target so the static libs (our libretro frontend, etc.) contribute symbols to the final .pdb.
if(MSVC)
    # Treat sources AND narrow string literals as UTF-8. Our source files hold raw UTF-8 emoji literals
    # (e.g. the profile-icon glyph set in ProfileDialog.cpp); without /utf-8, MSVC decodes source bytes via the
    # system codepage and re-encodes literals to the ANSI codepage, which mangles multibyte glyphs into
    # mojibake. This makes emoji round-trips correct-by-construction regardless of the build host's locale.
    add_compile_options(/utf-8)
    # /MP: compile a target's translation units in parallel (multi-processor). Two wins in one flag:
    #   1. SPEED — the biggest cheap build lever here. A change to a shared core file (Settings.cpp,
    #      MetaCache.cpp, CloudMerge.cpp) fans out to every probe that links it; without /MP those TUs
    #      recompile one at a time. (It cannot help the single giant MainWindow.cpp TU — one file is
    #      single-threaded regardless — but it parallelizes everything around it.)
    #   2. CORRECTNESS — with the /Zi below, parallel CL.EXE processes otherwise race on the shared
    #      vc145.pdb and fail with C1041 ("multiple CL.EXE write to the same .PDB, use /FS"). /MP
    #      coordinates that PDB access, so it removes the intermittent C1041 the app target hit under
    #      MSBuild's cross-target parallelism.
    add_compile_options(/MP)
    add_compile_options($<$<CONFIG:Release>:/Zi>)
    add_link_options($<$<CONFIG:Release>:/DEBUG>)
    add_link_options($<$<CONFIG:Release>:/OPT:REF>)
    add_link_options($<$<CONFIG:Release>:/OPT:ICF>)
endif()

# ---------------------------------------------------------------------------
# libretro frontend - no external deps. Loads/runs any libretro core via the
# C ABI and exposes BGRA frames. VERIFIED: builds with MSVC, loads + inits mGBA.
# ---------------------------------------------------------------------------
add_library(everythingbox_libretro STATIC src/libretro/LibretroCore.cpp)
target_include_directories(everythingbox_libretro PUBLIC src/libretro)

# A console probe that exercises the frontend (no Qt) - used for verification. Desktop-only: Android builds
# the app as a shared library packaged into an APK, and iOS builds an app bundle, so standalone console
# executables don't fit either mobile platform.
if(NOT ANDROID AND NOT IOS)
    add_executable(probe_core tools/probe_core.cpp)
    target_link_libraries(probe_core PRIVATE everythingbox_libretro)

    # Crash-report formatter (issue #28). Declared HERE, outside the EVERYTHINGBOX_BUILD_APP gate,
    # rather than down with the Qt probes — CrashReport's formatter is deliberately Qt-free,
    # allocation-free and platform-free (its Windows half is #ifdef'd out entirely elsewhere), so this
    # probe links against nothing at all and builds in any configuration, including a bare
    # `cmake -S native -B build` with no Qt installed. That is not a convenience: the reporter is what
    # records the next occurrence of a bug whose five WER dumps were destroyed by LocalDumps rotation,
    # so its assertions should be the ones LEAST able to be skipped by a configuration.
    add_executable(probe_crashreport tools/probe_crashreport.cpp
        src/core/CrashReport.cpp src/core/CrashReport.h)
    target_include_directories(probe_crashreport PRIVATE src src/core)
endif()

# ---------------------------------------------------------------------------
# RetroPark integration — the separate RetroPark runtime ("a better libretro", GPLv3, its own repo) wired into
# EverythingBox as an emulation backend. As of Slice 2a this is a PERMANENT desktop/Windows dependency: RetroPark
# is vendored as a pinned git submodule at external/RetroPark (see .gitmodules) and built from source via the
# ExternalProject recipe below, then linked into BOTH probe_retropark AND the `everythingbox` app target. The
# Slice-1 EB_WITH_RETROPARK opt-in gate is retired; the only remaining guard is the desktop-Windows one
# (RetroPark's static lib links Vulkan::Vulkan + D3D11/XAudio2, which exist on Windows desktop only here).
#
# WHY ExternalProject AND NOT add_subdirectory. RetroPark's CMake assumes it is the TOP-LEVEL project: it uses
# ${CMAKE_SOURCE_DIR} pervasively — not only in its tests (tests/CMakeLists.txt static_core_a/b) but in
# cmake/Shaders.cmake, on which the `retropark` static lib's OWN Vulkan-shader-embed step depends
# (`-P ${CMAKE_SOURCE_DIR}/cmake/EmbedSpv.cmake`). Nested via add_subdirectory, CMAKE_SOURCE_DIR would resolve to
# THIS native/ dir, so the tests fail to configure AND the static lib fails to build its shaders. RetroPark has no
# option to turn its tests off, and it is read-only here (a separate repo), so rather than edit it or hand-import
# its exact source list + shader recipe, we build it AS its own top-level project (CMAKE_SOURCE_DIR correct) via
# ExternalProject, building ONLY the `retropark` target, and import the resulting static lib. This genuinely
# builds RetroPark's lib as part of EverythingBox's build and links it into the app + probe, honouring the brief's
# fall-back ("import just the retropark static lib") in the form that keeps RetroPark's build recipe as its own.
set(RETROPARK_DIR "${CMAKE_SOURCE_DIR}/../external/RetroPark" CACHE PATH "RetroPark source dir (the pinned submodule)")
if(WIN32 AND NOT ANDROID AND NOT IOS)
    include(ExternalProject)
    # RetroPark's root CMake references Vulkan::Vulkan but does not find it itself — the consumer provides it. We
    # need it here too, to hand the probe the same Vulkan import lib RetroPark's PUBLIC link pulls in.
    find_package(Vulkan REQUIRED)

    set(EB_RETROPARK_BUILD_DIR "${CMAKE_BINARY_DIR}/retropark-build")
    # Multi-config (Visual Studio): the static lib lands in a per-config subdir, and $<CONFIG> tracks whichever
    # config the main build is producing, so the probe never links a Debug lib into a Release exe (CRT mismatch).
    set(EB_RETROPARK_LIB "${EB_RETROPARK_BUILD_DIR}/$<CONFIG>/retropark.lib")
    ExternalProject_Add(retropark_ext
        SOURCE_DIR      "${RETROPARK_DIR}"
        BINARY_DIR      "${EB_RETROPARK_BUILD_DIR}"
        CMAKE_GENERATOR "${CMAKE_GENERATOR}"
        # doctest/nlohmann_json (RetroPark's FetchContent deps) declare a pre-3.5 cmake_minimum_required that
        # CMake >= 4.0 refuses; RetroPark's own root sets this same floor, mirrored here for its sub-configure.
        CMAKE_ARGS      "-DCMAKE_POLICY_VERSION_MINIMUM=3.5"
        # Build ONLY the static lib target (+ its shader-gen dependency) — never RetroPark's cores, harness or
        # doctest tests. Its configure step still processes those, which is fine: they configure cleanly as a
        # top-level project (CMAKE_SOURCE_DIR = RetroPark root); we simply don't compile them.
        BUILD_COMMAND   "${CMAKE_COMMAND}" --build "${EB_RETROPARK_BUILD_DIR}" --config $<CONFIG> --target retropark
        INSTALL_COMMAND ""
        BUILD_BYPRODUCTS "${EB_RETROPARK_LIB}")

    # probe_retropark compiles the DRIVEN reference core straight into itself with the exported getter renamed
    # (the DLL-free static-core path), mirroring RetroPark's tests/CMakeLists.txt static_core_a pattern.
    add_executable(probe_retropark tools/probe_retropark.cpp
        ${RETROPARK_DIR}/cores/refcore_driven/RefCoreDriven.cpp)
    set_source_files_properties(${RETROPARK_DIR}/cores/refcore_driven/RefCoreDriven.cpp PROPERTIES
        COMPILE_DEFINITIONS "rp_get_core_abi=refcore_driven_static_get_core_abi")
    add_dependencies(probe_retropark retropark_ext)
    # include/ for <retropark/retropark.h> + the renamed core's headers; src/ for loader/StaticCoreRegistry.h.
    target_include_directories(probe_retropark PRIVATE ${RETROPARK_DIR}/include ${RETROPARK_DIR}/src)
    # retropark is STATIC, so a consumer must supply its PUBLIC transitive link deps by hand: Vulkan (linked
    # unconditionally) and, on Windows, the D3D11 / DXGI / XAudio2 / OLE / Winsock imports. nlohmann_json is
    # header-only and fully compiled into retropark.lib, so nothing to link for it here.
    target_link_libraries(probe_retropark PRIVATE
        "${EB_RETROPARK_LIB}" Vulkan::Vulkan
        d3d11 dxgi dxguid d3dcompiler xaudio2 ole32 ws2_32)
    target_compile_definitions(probe_retropark PRIVATE EB_HAVE_RETROPARK)

    # probe_retropark_loop (Slice 2a): the live driven-loop proof behind RetroParkView — present ADVANCES the
    # driven core, pause FREEZES it (present repeats the retained frame), resume advances again. Built exactly
    # like probe_retropark: the driven reference core compiled straight in with the getter renamed (DLL-free
    # static-core path), the RetroPark static lib + its Windows transitive link deps supplied by hand.
    add_executable(probe_retropark_loop tools/probe_retropark_loop.cpp
        ${RETROPARK_DIR}/cores/refcore_driven/RefCoreDriven.cpp)
    set_source_files_properties(${RETROPARK_DIR}/cores/refcore_driven/RefCoreDriven.cpp PROPERTIES
        COMPILE_DEFINITIONS "rp_get_core_abi=refcore_driven_static_get_core_abi")
    add_dependencies(probe_retropark_loop retropark_ext)
    target_include_directories(probe_retropark_loop PRIVATE ${RETROPARK_DIR}/include ${RETROPARK_DIR}/src)
    target_link_libraries(probe_retropark_loop PRIVATE
        "${EB_RETROPARK_LIB}" Vulkan::Vulkan
        d3d11 dxgi dxguid d3dcompiler xaudio2 ole32 ws2_32)
    target_compile_definitions(probe_retropark_loop PRIVATE EB_HAVE_RETROPARK)
endif()

# ---------------------------------------------------------------------------
# The Qt application (UI + libmpv video). Needs Qt6 + libmpv installed, so it
# is OFF by default; the libretro lib + probe above build without them.
#   Qt6:    https://www.qt.io/download-qt-installer  (or `aqt`)
#   libmpv: vcpkg install libmpv   (or distro mpv-dev / a prebuilt SDK)
# Configure with: cmake -S native -B build -DEVERYTHINGBOX_BUILD_APP=ON -DCMAKE_PREFIX_PATH=<Qt>
# ---------------------------------------------------------------------------
option(EVERYTHINGBOX_BUILD_APP "Build the Qt application (requires Qt6 + libmpv)" OFF)
if(EVERYTHINGBOX_BUILD_APP)
    # QtPdf (PDFium) isn't shipped for Android in the open-source packages, so it's desktop-only; PdfView is
    # stubbed on Android (see src/pdf/PdfView.cpp).
    set(EB_QT_COMPONENTS Widgets OpenGLWidgets Multimedia Network Concurrent Test)
    if(NOT ANDROID)
        list(APPEND EB_QT_COMPONENTS Pdf PdfWidgets)
    endif()
    find_package(Qt6 REQUIRED COMPONENTS ${EB_QT_COMPONENTS})
    qt_standard_project_setup()  # enables AUTOMOC etc.

    # ---------------------------------------------------------------------------
    # qt.conf beside the build output — so every probe/app exe finds Qt's platform
    # plugins (qwindows / qoffscreen) WITHOUT anyone exporting QT_PLUGIN_PATH by hand.
    #
    # The bug this kills: a bare launch of probe_notifier / probe_nav / probe_navqml /
    # any windowed probe died with
    #   "This application failed to start because no Qt platform plugin could be
    #    initialized."
    # The plugins are NOT deployed beside the build-tree exes, so unless the caller had
    # already `export`ed QT_PLUGIN_PATH=<Qt>/plugins Qt couldn't locate qwindows/qoffscreen
    # and aborted. Qt reads a qt.conf sitting next to the exe and takes its [Paths] Plugins=
    # as the plugin search root — the canonical fix, no per-probe source edits, no env var.
    #
    # QT6_INSTALL_PREFIX / QT6_INSTALL_PLUGINS come from Qt's own CMake package
    # (QtInstallPaths.cmake) — i.e. exactly what `qmake -query QT_INSTALL_PLUGINS` reports —
    # so this tracks whatever Qt configured this build: this machine resolves to
    # C:/Qt/6.8.3/msvc2022_64/plugins, the CI Linux runner resolves ITS own Qt's plugins dir.
    # This only affects build-tree runs; the DEPLOYED app (release.yml's Package step) has its
    # plugins staged beside it, so the release/packaging path is unaffected by this qt.conf.
    #
    # NOTE — build/Release is NOT a portable distribution, do not zip it as a release. This
    # qt.conf pins [Paths] Plugins to the ABSOLUTE Qt install path above, so a build-tree app
    # always loads its plugins from C:/Qt/... — and the plugin directories windeployqt stages
    # beside the exe (EB_WINDEPLOYQT, far below) sit shadowed and unused. It runs correctly on
    # THIS machine only; the relocatable bundle is what release.yml's Package step produces.
    #
    # Multi-config (Visual Studio) puts exes in build/Release AND build/Debug, so emit one
    # qt.conf per config dir via $<CONFIG>; single-config (Ninja/Make, e.g. CI Linux) drops
    # exes straight in the build dir, so emit a single qt.conf there.
    set(EB_QT_PLUGINS_DIR "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_PLUGINS}")
    message(STATUS "EverythingBox: qt.conf Plugins path = ${EB_QT_PLUGINS_DIR}")
    set(EB_QT_CONF_CONTENT "[Paths]\nPlugins = ${EB_QT_PLUGINS_DIR}\n")
    get_property(_eb_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if(_eb_multi_config)
        file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/$<CONFIG>/qt.conf" CONTENT "${EB_QT_CONF_CONTENT}")
    else()
        file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/qt.conf" CONTENT "${EB_QT_CONF_CONTENT}")
    endif()

    # Embedded provider dev credentials (FIRST configure_file in this repo). Reads the git-ignored
    # native/secrets/<provider>.secrets files (screenscraper, thegamesdb, igdb, steamgriddb), obfuscates
    # each embedded value, and generates BuiltinSecrets.h into the BUILD TREE only. Absent/blank secrets ->
    # empty header slots + a loud STATUS line (each provider falls back to user settings). The generated dir
    # is added to the include path of every target that compiles AddonContext.cpp (the app + the addon probes).
    set(EB_SECRETS_DIR      "${CMAKE_CURRENT_SOURCE_DIR}/secrets")
    set(EB_SECRETS_TEMPLATE "${CMAKE_CURRENT_SOURCE_DIR}/src/addons/BuiltinSecrets.h.in")
    set(EB_SECRETS_GEN_DIR  "${CMAKE_CURRENT_BINARY_DIR}/generated/secrets")
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateSecrets.cmake")

    # QML theme engine (declarative, fully themeable front end). OPTIONAL so a Qt install without qtdeclarative
    # still builds the app (the theme engine just isn't compiled). Gate its targets on TARGET Qt6::Quick. Runs
    # for every platform (incl. Android) now that the kits provision qtdeclarative; a kit still missing it
    # degrades gracefully via the STATUS message below.
    find_package(Qt6 QUIET OPTIONAL_COMPONENTS Quick Qml QuickWidgets)
    if(TARGET Qt6::Quick AND TARGET Qt6::Qml AND TARGET Qt6::QuickWidgets)
        set(EB_HAVE_QML ON)
        message(STATUS "EverythingBox: QML theme engine enabled")
    else()
        message(STATUS "EverythingBox: QML theme engine OFF (qtdeclarative not found)")
    endif()

    add_subdirectory(third_party/rcheevos) # RetroAchievements runtime + client (static lib, no extra deps)
    add_subdirectory(third_party/lzma)     # read-only .7z decoder (LZMA SDK) for archived ROMs, all platforms

    # ---------------------------------------------------------------------------
    # librashader (issue #99, SLICE 2): the slang-shader runtime, BUILT FROM RUST SOURCE.
    #
    # librashader is Rust; corrosion is the CMake<->cargo bridge that compiles its C-ABI crate
    # (librashader-capi) as part of this build and hands back an ordinary CMake target to link. Both are pulled
    # with FetchContent at PINNED tags — never a branch — so the build is reproducible: a moving upstream would
    # silently change the ABI under us, and the vendored header (below) would drift from the crate it describes.
    #
    # This lives inside EVERYTHINGBOX_BUILD_APP, and inside the desktop-only guard, on purpose. The bare
    # libretro frontend + its probe build with nothing but CMake and a C++17 compiler (top of this file);
    # dragging a Rust toolchain and a cargo build into THAT configuration would break the "no external deps"
    # contract those targets keep. Mobile (Android/iOS) cross-compilation of the Rust crate is a separate
    # problem deferred with the rest of the mobile port, so it is gated out here too.
    #
    # SLICE SCOPE: this brings the library into the build and proves the C ABI links + is callable
    # (probe_librashader). Render integration and the present-path conversion are LATER slices and touch
    # RetroView, not this block.
    #
    # FEATURES = NO_DEFAULT_FEATURES + exactly `runtime-vulkan`. librashader-capi's default feature is
    # `runtime-all`, which compiles the OpenGL, D3D9/11/12, Vulkan and Metal filter-chain backends
    # (glow / ash / windows-rs / objc2) — far more than this slice needs. But it is NOT possible to build with
    # zero runtime backends: librashader-capi/src/ctypes.rs references `librashader::runtime` unconditionally,
    # and that module only exists when at least ONE runtime feature is enabled (librashader/src/lib.rs gates
    # `pub mod runtime` on `any(runtime-*)`). So the minimum that compiles is one backend.
    # `runtime-vulkan` is that one: it is cross-platform (builds on both this Windows host and the Linux CI
    # runner — ash is pure-Rust FFI that dlopens libvulkan at RUNTIME, so it needs no Vulkan headers to
    # COMPILE), and it is one of the two backends RetroView's present path will use in a later slice. The
    # preset loader, error API and version functions this probe calls are compiled regardless of which backend
    # is on. A later slice adds `runtime-d3d11` (Windows) beside this. The reflect core pulls naga either way,
    # so the FIRST cargo build is slow — expected. NOTE: enabling runtime-vulkan in the Rust crate does NOT
    # make the vendored C header pull <vulkan/vulkan.h> — that include is gated on the CONSUMER defining
    # LIBRA_RUNTIME_VULKAN, which neither the app nor this probe does.
    #
    # HEADER = VENDORED (third_party/librashader/include/librashader.h) rather than corrosion's cbindgen path:
    # it is the exact librashader.h committed at the pinned tag, so the header the C++ compiles against and the
    # ABI the crate exports move together and cannot drift within a pin. Its runtime chain declarations are
    # #ifdef'd behind LIBRA_RUNTIME_* macros we do not define, so with no runtime features only the
    # preset/error/version surface is visible — no missing-symbol risk from the unused declarations.
    if(NOT ANDROID AND NOT IOS)
        include(FetchContent)
        FetchContent_Declare(Corrosion
            GIT_REPOSITORY https://github.com/corrosion-rs/corrosion.git
            GIT_TAG        v0.6.1)
        FetchContent_MakeAvailable(Corrosion)
        # librashader is a pure Cargo workspace with no root CMakeLists; SOURCE_SUBDIR points at a directory
        # that has none so FetchContent_MakeAvailable only POPULATES the source and never add_subdirectory's it.
        FetchContent_Declare(librashader
            GIT_REPOSITORY https://github.com/SnowflakePowered/librashader.git
            GIT_TAG        librashader-v0.12.0
            SOURCE_SUBDIR  include)
        FetchContent_MakeAvailable(librashader)
        corrosion_import_crate(
            MANIFEST_PATH   "${librashader_SOURCE_DIR}/librashader-capi/Cargo.toml"
            CRATE_TYPES     staticlib
            NO_DEFAULT_FEATURES
            # runtime-vulkan (slice 2) + runtime-opengl (slice 3). runtime-opengl brings librashader's OpenGL
            # filter-chain backend that ShaderChain (src/emu/ShaderChain) wraps. glow (its GL binding) is pure
            # Rust FFI that resolves GL entry points at RUNTIME through the caller-supplied loader, so it needs
            # NO GL dev headers and NO display to COMPILE — it builds on the Linux CI runner exactly as
            # runtime-vulkan (ash) does. The header's GL declarations still only appear for a consumer that
            # defines LIBRA_RUNTIME_OPENGL (ShaderChain + probe_shaderchain do; nothing else).
            FEATURES        runtime-vulkan runtime-opengl
            PROFILE         release)
        set(EB_LIBRASHADER_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/librashader/include")
        # The corrosion target is named after the crate's cargo LIB target, which cargo derives by replacing
        # '-' with '_' — so the linkable target is `librashader_capi` (underscore), NOT `librashader-capi`.
        # Linking the hyphen spelling silently degrades to a bare `librashader-capi.lib` link-name (no such
        # file), which fails at LINK, not configure. Keep this variable as the single source of that name.
        set(EB_LIBRASHADER_TARGET librashader_capi)
        set(EB_HAVE_LIBRASHADER ON)
        message(STATUS "EverythingBox: librashader (librashader-v0.12.0) built from Rust source via corrosion")
    endif()

    # libmpv: install it (e.g. `vcpkg install libmpv`, distro `libmpv-dev`, or a prebuilt SDK) and make it
    # discoverable via CMAKE_PREFIX_PATH / a vcpkg toolchain, or set -DMPV_INCLUDE_DIR= and -DMPV_LIBRARY=.
    find_path(MPV_INCLUDE_DIR mpv/client.h)
    find_library(MPV_LIBRARY NAMES mpv libmpv mpv-2 libmpv.dll.a)
    if(NOT MPV_INCLUDE_DIR OR NOT MPV_LIBRARY)
        message(FATAL_ERROR "libmpv not found. Install it and pass CMAKE_PREFIX_PATH (or -DMPV_INCLUDE_DIR / -DMPV_LIBRARY).")
    endif()

    qt_add_executable(everythingbox
        src/main.cpp
        src/video/MpvWidget.cpp   src/video/MpvWidget.h
        src/video/MpvHeaderApply.h
        src/emu/RetroView.cpp     src/emu/RetroView.h
        src/emu/RetroParkView.cpp src/emu/RetroParkView.h  # Slice 2a: the RetroPark driven play surface (sibling of RetroView)
        src/emu/StateSlots.h      # pure slot-selection + auto-state-validity logic (#93), header-only
        src/emu/VirtualPad.cpp    src/emu/VirtualPad.h
        src/emu/NetplaySession.cpp src/emu/NetplaySession.h
        src/ebook/EbookSource.h
        src/ebook/EpubBook.cpp    src/ebook/EpubBook.h
        src/ebook/MobiBook.cpp    src/ebook/MobiBook.h
        src/ebook/PdfTextBook.cpp src/ebook/PdfTextBook.h
        src/ebook/EbookView.cpp   src/ebook/EbookView.h
        src/pdf/PdfView.cpp       src/pdf/PdfView.h
        src/comic/ComicView.cpp   src/comic/ComicView.h
        src/comic/Tar.h           src/comic/ComicPageOrder.h  # #144 CB7/CBT comic readers (header-only)
        src/addons/AddonModels.cpp  src/addons/AddonModels.h
        src/addons/AddonContext.cpp src/addons/AddonContext.h
        src/addons/JsAddon.cpp      src/addons/JsAddon.h
        src/addons/AddonManager.cpp src/addons/AddonManager.h
        src/addons/CatalogPrefetcher.cpp src/addons/CatalogPrefetcher.h
        src/addons/GameMetaAggregator.cpp src/addons/GameMetaAggregator.h
        src/browse/SyntheticCatalogs.cpp src/browse/SyntheticCatalogs.h
        src/browse/LiveTvGuide.cpp src/browse/LiveTvGuide.h        # EPG now/next + guide-grid builders (#75 inc 3)
        src/browse/SearchAggregator.cpp  src/browse/SearchAggregator.h
        src/ui/LibraryView.cpp    src/ui/LibraryView.h
        src/ui/HomeView.cpp       src/ui/HomeView.h
        src/ui/MediaPane.cpp      src/ui/MediaPane.h
        src/ui/SplitView.cpp      src/ui/SplitView.h
        src/ui/AddonSettingsDialog.cpp src/ui/AddonSettingsDialog.h
        src/ui/ProfileDialog.cpp  src/ui/ProfileDialog.h
        src/ui/RegistryBrowser.cpp src/ui/RegistryBrowser.h
        src/ui/CarouselView.cpp   src/ui/CarouselView.h
        src/ui/XmbView.cpp        src/ui/XmbView.h
        src/ui/nav/Nav.cpp        src/ui/nav/Nav.h
        src/ui/nav/NavGraph.cpp   src/ui/nav/NavGraph.h
        src/ui/nav/NavOverlay.cpp src/ui/nav/NavOverlay.h
        src/ui/nav/Osk.cpp        src/ui/nav/Osk.h
        src/ui/nav/PasscodePad.cpp src/ui/nav/PasscodePad.h  # 4-digit numeric entry overlay (#30)
        src/input/Gamepad.cpp     src/input/Gamepad.h
        src/input/Keymap.cpp      src/input/Keymap.h
        src/core/Settings.cpp     src/core/Settings.h
        src/core/OverrideLayer.cpp src/core/OverrideLayer.h  # per-game override layering (#95): pure baseline+delta fold
        src/core/SyncOffsets.cpp  src/core/SyncOffsets.h  # player audio/sub sync-offset store (Task 2 wires the card onto it)
        src/core/ExternalPlayer.cpp src/core/ExternalPlayer.h # external-player handoff (VLC/MPC/custom/Android intent); Task 2 routes onto it
        # FormFactor: QtCore-only mode authority. UNCONDITIONAL — Settings.cpp + RetroView.cpp call
        # FormFactor::instance() and both compile in every configuration, incl. the no-QML Android kit.
        src/theme2/FormFactor.cpp src/theme2/FormFactor.h
        # VideoPreviewBridge: QtCore-only singleton for the video hover previews (issue #55). UNCONDITIONAL —
        # MainWindow.cpp seeds it + connects its BGM-duck signal in the no-QML build too (the classic settings
        # builder writes it); the themed QML surface reads it as a context property when EB_HAVE_QML.
        src/theme2/VideoPreviewBridge.cpp src/theme2/VideoPreviewBridge.h
        # AssetBootstrap: QtCore-only first-run APK-asset extractor (D2 Task 2). UNCONDITIONAL — main.cpp
        # calls it on Android and under the EB_TEST_BOOTSTRAP_SRC desktop override; compiles in every config.
        src/core/AssetBootstrap.cpp src/core/AssetBootstrap.h
        src/core/SafeAreaInsets.cpp src/core/SafeAreaInsets.h
        src/core/BackgroundMusic.cpp src/core/BackgroundMusic.h
        src/core/Achievements.cpp src/core/Achievements.h
        src/core/PerfTrace.cpp    src/core/PerfTrace.h
        # First-chance access-violation reporter (issue #28) — installed early in main(). Qt-free and
        # allocation-free by design; on non-Windows the installer compiles to an empty function.
        src/core/CrashReport.cpp  src/core/CrashReport.h
        src/core/CoreManager.cpp  src/core/CoreManager.h
        src/core/EmulatorManager.cpp src/core/EmulatorManager.h
        src/core/AppUpdater.cpp   src/core/AppUpdater.h
        src/core/SubtitleFetcher.cpp src/core/SubtitleFetcher.h
        src/core/SubtitleHash.cpp src/core/SubtitleHash.h
        src/core/SubtitleCache.cpp src/core/SubtitleCache.h
        src/addons/StremioTranslate.cpp src/addons/StremioTranslate.h
        src/core/StreamHeaders.cpp      src/core/StreamHeaders.h
        src/core/NetHeaderApply.h       # header-only, and listed for the same reason src/video/MpvHeaderApply.h is
        src/core/BingeStore.cpp         src/core/BingeStore.h
        src/core/MediaSegments.cpp src/core/MediaSegments.h
        src/core/SegmentStore.cpp  src/core/SegmentStore.h
        src/core/CastManager.cpp  src/core/CastManager.h
        src/core/PortMapper.cpp   src/core/PortMapper.h
        src/core/TraktClient.cpp  src/core/TraktClient.h
        src/core/SingleFlight.h   # ensureValidToken's one-refresh-many-waiters queue (probe_trakt §12)
        src/core/TraktRead.cpp  src/core/TraktRead.h  # Trakt read layer: calendar parser + id mapper (#23)
        src/core/TraktSync.cpp  src/core/TraktSync.h  # Trakt read layer 2: the lists + the watched backfill (#23)
        src/core/TraktMissed.cpp   src/core/TraktMissed.h    # "You missed": the pure selection rule (#25)
        src/core/MissedDismiss.cpp src/core/MissedDismiss.h  # ...and the per-show dismissal watermarks it reads
        src/core/DownloadManager.cpp src/core/DownloadManager.h
        src/core/EmulatorRegistry.h
        src/core/BiosCatalog.h
        src/core/SevenZip.cpp     src/core/SevenZip.h
        src/core/ArchiveRom.cpp   src/core/ArchiveRom.h
        src/core/RecentStore.cpp  src/core/RecentStore.h
        src/core/ResumeStore.cpp  src/core/ResumeStore.h  # resume/<hash> key scheme + the tombstoned clear (#150)
        src/core/SpeedStore.cpp   src/core/SpeedStore.h   # per-item playback-speed memory, per-item-synced (issue #140)
        src/media/SleepTimer.h                            # pure sleep-timer decision: expiry/fade/nudge (issue #140, header-only)
        src/media/LrcLyrics.h                             # pure LRC lyric parser + current-line lookup (issue #142, header-only)
        src/core/MetaCache.cpp src/core/MetaOverrides.cpp    src/core/MetaCache.h
        src/core/Miximage.cpp src/core/Miximage.h   # issue #90: composite one game card per item from cached art
        src/core/AttractController.cpp src/core/AttractController.h  # issue #54: idle-screensaver fire/rotation/round-trip
        src/core/ScrapedSnapshot.h   # issue #24: the metadata editor's baseline, keyed to its item
        src/core/GamelistStore.cpp src/core/GamelistStore.h
        src/core/UiTestServer.cpp src/core/UiTestServer.h
        src/core/RemoteApi.cpp    src/core/RemoteApi.h     # pure remote-control request parse + command router (#76)
        src/core/RemoteServer.cpp src/core/RemoteServer.h  # off-by-default local HTTP remote (QTcpServer) driving the app (#76)
        src/core/ProfileStore.cpp src/core/ProfileStore.h
        src/core/ProfilePasscode.cpp src/core/ProfilePasscode.h  # per-profile passcode hash/policy/rate-limit (#30)
        src/core/ThemeChoice.cpp  src/core/ThemeChoice.h  # single owner of the per-profile themed-home theme key (#57)
        src/core/ThemeFormFactors.cpp src/core/ThemeFormFactors.h  # theme `formFactors` declaration -> verdict for this device (#32)
        # ThemeRegistry: what a community registry index means and which remote paths may become filenames.
        # QtCore-only, so it compiles in every configuration incl. the no-QML kit; RegistryBrowser links it.
        src/core/ThemeRegistry.cpp src/core/ThemeRegistry.h
        src/core/SettingsTxn.cpp  src/core/SettingsTxn.h  # settings save/discard snapshot-and-restore (#26)
        src/core/PendingPush.cpp  src/core/PendingPush.h  # durable pending cloud push + retry policy (#34)
        src/core/FavoritesStore.cpp src/core/FavoritesStore.h
        src/core/BookmarkStore.cpp src/core/BookmarkStore.h  # per-book reading bookmarks, per-item-synced (issue #136)
        src/core/AudioBookmarkStore.cpp src/core/AudioBookmarkStore.h  # per-item audio bookmarks, per-item-synced (issue #140)
        src/ebook/ReaderAnchor.h                             # the one repagination-stable reader anchor model (issue #136, header-only)
        src/core/GameFilter.cpp   src/core/GameFilter.h  # pure game-library filter model + evaluator (#63)
        src/core/FilterPresetStore.cpp src/core/FilterPresetStore.h  # per-profile saved filter presets (#63)
        src/core/ItemMarks.cpp    src/core/ItemMarks.h  # per-profile hidden/completion/tags store (marks/shelves T1)
        src/core/LaunchOptionsStore.cpp src/core/LaunchOptionsStore.h  # per-game core/emulator/args override (issue #51)
        src/core/LaunchHooks.h                                          # pure argv tokenizer + {rom} subst (issue #64, header-only)
        src/core/LaunchHooksStore.cpp src/core/LaunchHooksStore.h       # per-game pre-launch/post-exit hooks, device-local (issue #64)
        src/core/EmuSettings.h                                          # pure cross-emulator graphics quartet + per-emulator INI mapping (issue #103, header-only)
        src/core/EmuGfxStore.cpp src/core/EmuGfxStore.h                 # per-game standalone-emulator graphics override, device-local (issue #103)
        src/core/ShaderPreset.h                                         # pure slang-shader preset model + curated registry + scope resolution (issue #99, header-only)
        src/core/ShaderPresetStore.cpp src/core/ShaderPresetStore.h     # per-game/per-system slang-shader preset override, device-local (issue #99)
        src/core/DeviceProfile.h                                        # pure device identification + tuned per-emulator defaults table (issue #119, header-only)
        src/core/DeviceProfileDetect.cpp src/core/DeviceProfileDetect.h # platform detection glue + device-local profile cache/override (issue #119)
        src/core/Pad2Key.h                                              # pure pad-to-keyboard model + translate() + hysteresis (issue #105, header-only)
        src/core/Pad2KeyStore.cpp src/core/Pad2KeyStore.h               # per-game pad-to-keyboard enable + profile, synced (issue #105)
        src/input/Pad2KeyRuntime.cpp src/input/Pad2KeyRuntime.h         # SendInput keystroke injector for keyboard-only PC games (issue #105)
        src/core/ControllerSeats.h                                      # pure multi-seat controller model + per-emulator player-N INI mapping (issue #104, header-only)
        src/core/Tombstones.cpp   src/core/Tombstones.h  # per-store deletion tombstones for multi-device merge (mdsync T1)
        src/core/CloudMerge.cpp   src/core/CloudMerge.h  # generalized per-item merge document serialize/merge (mdsync T2)
        src/core/ConsumptionStats.cpp src/core/ConsumptionStats.h  # per-profile media/reading accrual store (stats T1)
        src/core/PlaylistStore.cpp src/core/PlaylistStore.h
        src/core/IptvSourceStore.cpp src/core/IptvSourceStore.h  # per-profile Live TV sources (#75 inc 2)
        src/core/OpdsCatalogStore.cpp src/core/OpdsCatalogStore.h # per-profile OPDS book catalogs, device-local (#146)
        src/ebook/OpdsFeed.h                                     # pure OPDS 1.2 Atom parser (header-only, #146)
        src/core/XmltvGuide.cpp src/core/XmltvGuide.h            # XMLTV EPG parse + source-agnostic model (#75 inc 3)
        src/core/PcGameStore.cpp  src/core/PcGameStore.h
        src/core/DownloadsStore.cpp src/core/DownloadsStore.h
        src/core/RaBrowse.cpp     src/core/RaBrowse.h
        src/core/SteamAchievements.cpp src/core/SteamAchievements.h
        src/core/PlayStats.cpp    src/core/PlayStats.h
        src/core/RomLibrary.cpp   src/core/RomLibrary.h
        src/core/RomRouting.h     # #53 folder-authoritative routing + junk filter (header-only, used by RomLibrary)
        src/core/LocalLibrary.cpp src/core/LocalLibrary.h
        src/core/PhotoLibrary.cpp src/core/PhotoLibrary.h  # #102 local photo library scan (viewer = ComicView photo mode)
        src/core/CatalogMatch.cpp src/core/CatalogMatch.h  # pure local-library id-resolver matcher (id-resolver T1)
        src/core/LocalResolveCache.cpp src/core/LocalResolveCache.h  # persisted resolved-id cache (id-resolver T2)
        src/core/CatalogResolver.cpp src/core/CatalogResolver.h      # async throttled resolver engine (id-resolver T3)
        src/core/LocalMetaMerge.cpp src/core/LocalMetaMerge.h        # pure .nfo>scraped merge for resolved local files (#73)
        src/core/Theme.cpp        src/core/Theme.h
        src/core/CloudSync.cpp    src/core/CloudSync.h
        src/core/DriveSyncBackend.cpp src/core/DriveSyncBackend.h  # Increment B: Drive transport behind the SyncBackend seam
        src/core/ServerSyncBackend.cpp src/core/ServerSyncBackend.h # Increment C: self-hosted server transport behind the SyncBackend seam
        src/core/SyncBackend.h                                     # the abstract seam CloudSync composes (Q_OBJECT for AUTOMOC)
        # Brand migration (rebrand T3): per-step, resumable move of an existing install off the previous name.
        # Split in two TUs on purpose — BrandMigration.cpp is the QtCore-only half (flags + ini + addon ids)
        # that the lean Core-only probes link just to read a flag, while BrandMigrationDrive.cpp holds run()
        # and the Google Drive steps and is the only part that drags CloudSync/Qt6::Network in.
        src/core/BrandMigration.cpp      src/core/BrandMigration.h
        src/core/BrandMigrationDrive.cpp
        src/core/SaveSyncPlan.cpp src/core/SaveSyncPlan.h  # per-file save decision table (save-sync T1)
        src/core/SaveSync.cpp     src/core/SaveSync.h      # save-sync transport (save-sync T2)
        src/core/SaveMeta.cpp     src/core/SaveMeta.h      # save baseline/metadata store (save-sync T4)
        src/core/SteamLibrary.cpp src/core/SteamLibrary.h
        src/core/EpicLibrary.cpp  src/core/EpicLibrary.h
        src/core/GogLibrary.cpp   src/core/GogLibrary.h
        src/core/BattleNetLibrary.cpp src/core/BattleNetLibrary.h  # Blizzard Uninstall-hive detection (bnet T1)
        src/core/PcGameId.cpp     src/core/PcGameId.h      # ONE PC-game entry, many launcher sources (pc-games T1)
        src/core/PcGameRemap.cpp  src/core/PcGameRemap.h   # marks/stats/favourites follow the merged id (pc-games T3)
        src/core/PcScanCache.cpp  src/core/PcScanCache.h   # persist the last good installed-scan per launcher (#62)
        src/core/SystemCatalog.h
        src/ui/SettingsDialog.cpp src/ui/SettingsDialog.h
        src/ui/ControllerRemapDialog.cpp src/ui/ControllerRemapDialog.h
        src/ui/MainWindow.cpp     src/ui/MainWindow.h
        src/ui/BlackFrameWatchdog.cpp src/ui/BlackFrameWatchdog.h
        src/ui/AttractOverlay.cpp src/ui/AttractOverlay.h  # issue #54: the full-screen Ken-Burns slideshow overlay
        src/ui/Notifier.cpp       src/ui/Notifier.h
        src/media/StreamResolver.cpp src/media/StreamResolver.h
        src/media/PlaybackSession.cpp src/media/PlaybackSession.h
        src/launch/GameLauncher.cpp src/launch/GameLauncher.h
        src/core/RomPatch.cpp src/core/RomPatch.h  # #128: GameLauncher::prepareCore links resolvePatchedRom
        src/core/HashVerify.cpp src/core/HashVerify.h  # #97: DAT dump verification (HomeView badge links it)
        resources/appicon.qrc
        $<$<PLATFORM_ID:Windows>:${CMAKE_CURRENT_SOURCE_DIR}/resources/app.rc>
        third_party/miniz.c
        third_party/duktape/duktape.c)

    if(MSVC)
        # MainWindow.cpp crosses MSVC's per-object section limit (C1128). TARGET-SCOPED on purpose:
        # applied project-wide it changes how the probe objects are emitted and probe_cloudmerge fails
        # differently, so widening this is not free -- rebuild every probe target if you ever move it.
        target_compile_options(everythingbox PRIVATE /bigobj)
    endif()
    target_include_directories(everythingbox PRIVATE src src/core src/addons third_party third_party/duktape ${MPV_INCLUDE_DIR} ${EB_SECRETS_GEN_DIR})
    target_compile_definitions(everythingbox PRIVATE MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
    # MainWindow.cpp crossed MSVC's 65,536-section-per-object limit (fatal error C1128) while the Trakt
    # settings rows were being added. The count is of COMDAT sections, which Qt's tr()/lambda/template
    # expansion produces in quantity, so this is a property of how big the file is rather than of anything
    # in particular that was added — the next thing added will cross it again. /bigobj raises the ceiling
    # to 2^32 and changes no code generation.
    #
    # TARGET-SCOPED, and that is not tidiness. Applied project-wide with add_compile_options it makes
    # probe_cloudmerge.cpp fail to compile at all — "error C1075: '{' no matching token found" pointing at
    # its main(), on a file this branch never touches — which is an MSVC defect rather than anything wrong
    # with that probe: removing the flag compiles it unchanged. So the flag goes exactly where the C1128
    # is, and the probe suite keeps building. Anyone widening this should re-run the whole probe build,
    # not just the app.
    if(MSVC)
        target_compile_options(everythingbox PRIVATE /bigobj)
    endif()
    # Qt6::GuiPrivate: the uitest `touch` verb posts synthetic touch via <qpa/qwindowsysteminterface.h>.
    target_link_libraries(everythingbox PRIVATE everythingbox_libretro rcheevos lzma7z Qt6::Widgets Qt6::OpenGLWidgets Qt6::Multimedia Qt6::Network Qt6::Concurrent Qt6::GuiPrivate ${MPV_LIBRARY})
    if(NOT ANDROID)
        target_link_libraries(everythingbox PRIVATE Qt6::Pdf Qt6::PdfWidgets)
    endif()
    # RetroPark (Slice 2a): link the permanent RetroPark static lib into the app itself, not only probe_retropark.
    # The retropark_ext ExternalProject + EB_RETROPARK_LIB + Vulkan::Vulkan are defined unconditionally on
    # desktop/Windows in the block near the top of this file, so on that platform they are always available here.
    # As with the probe, retropark is STATIC: the consumer supplies its PUBLIC transitive link deps by hand
    # (Vulkan + the Windows D3D11/DXGI/XAudio2/OLE/Winsock imports). EB_HAVE_RETROPARK flags it on in app code.
    # Slice 2a links the lib but adds no app code that USES it yet — a linked-but-unused lib is expected and fine.
    if(WIN32 AND NOT ANDROID AND NOT IOS)
        add_dependencies(everythingbox retropark_ext)
        target_link_libraries(everythingbox PRIVATE
            "${EB_RETROPARK_LIB}" Vulkan::Vulkan
            d3d11 dxgi dxguid d3dcompiler xaudio2 ole32 ws2_32)
        target_include_directories(everythingbox PRIVATE ${RETROPARK_DIR}/include ${RETROPARK_DIR}/src)
        target_compile_definitions(everythingbox PRIVATE EB_HAVE_RETROPARK)
        # Slice 2a: the app itself loads the static driven core RetroParkView drives, so it compiles
        # RefCoreDriven.cpp with the exported getter renamed — the same DLL-free static-core path the probes use
        # (RetroParkView.cpp declares refcore_driven_static_get_core_abi + registers it before load). Added here,
        # inside the desktop guard, because it needs the RetroPark headers on the include path above.
        target_sources(everythingbox PRIVATE ${RETROPARK_DIR}/cores/refcore_driven/RefCoreDriven.cpp)
        set_source_files_properties(${RETROPARK_DIR}/cores/refcore_driven/RefCoreDriven.cpp PROPERTIES
            COMPILE_DEFINITIONS "rp_get_core_abi=refcore_driven_static_get_core_abi")
    endif()
    # librashader (issue #99): link the Rust C-ABI crate into the app, put its vendored header on the include
    # path, and — SLICE 4 — compile the shader present pass + ship the curated shader assets. This whole block is
    # gated on EB_HAVE_LIBRASHADER (desktop only: Android/iOS have no Rust toolchain here, see the FetchContent
    # block). The shader TUs (ShaderChain, ShaderRenderer) include librashader.h, so they live INSIDE this gate,
    # not the unconditional app source list — and RetroView's shader code is compiled out on mobile via the
    # EB_HAVE_LIBRASHADER define below, so the emulator display path builds byte-for-byte as before there.
    if(EB_HAVE_LIBRASHADER)
        target_link_libraries(everythingbox PRIVATE ${EB_LIBRASHADER_TARGET})
        target_include_directories(everythingbox PRIVATE ${EB_LIBRASHADER_INCLUDE})
        # slice 3: expose librashader's OpenGL declarations to the shader TUs (the header gates them on
        # LIBRA_RUNTIME_OPENGL). slice 4: EB_HAVE_LIBRASHADER gates RetroView's shader integration in code.
        target_compile_definitions(everythingbox PRIVATE LIBRA_RUNTIME_OPENGL EB_HAVE_LIBRASHADER)
        # The librashader-dependent TUs (slice 3 chain wrapper + slice 4 present pass). Added here, not the
        # unconditional source list, because they cannot compile without the librashader header on the path.
        target_sources(everythingbox PRIVATE
            src/emu/ShaderChain.cpp    src/emu/ShaderChain.h    # slice 3: GL filter-chain RAII wrapper
            src/emu/ShaderRenderer.cpp src/emu/ShaderRenderer.h) # slice 4: off-screen slang-shader present pass
        # Ship the curated shaders. Compiled explicitly (not via target_sources) for the same reason as
        # theme2.qrc: a .qrc added through target_sources after finalization is silently not embedded.
        qt_add_resources(EB_SHADERS_RCC resources/shaders.qrc)
        target_sources(everythingbox PRIVATE ${EB_SHADERS_RCC})
    endif()
    # QML theme engine: link Qt Quick, compile the qrc-embedded QML + ThemeEngine, and flag it on in code.
    if(EB_HAVE_QML)
        # Compile theme2.qrc explicitly (not via target_sources): qt_add_executable only wires up .qrc files
        # present in its initial source list at finalization, so a .qrc added later via target_sources is
        # silently skipped and "qrc:/theme2/ThemeView.qml" isn't found at runtime. This runs rcc directly.
        qt_add_resources(EB_THEME2_RCC resources/theme2.qrc)
        target_sources(everythingbox PRIVATE
            src/theme2/ThemeEngine.cpp src/theme2/ThemeEngine.h
            src/theme2/MpvPreview.cpp src/theme2/MpvPreview.h
            src/theme2/ReaderChromeHost.cpp src/theme2/ReaderChromeHost.h
            src/theme2/ThemedPanelHost.cpp src/theme2/ThemedPanelHost.h
            src/theme2/ThemePickerHost.cpp src/theme2/ThemePickerHost.h
            # FormFactor is now in the unconditional app source list above (Settings/RetroView need it in
            # the no-QML build too). It still drives `form` + TV scale/insets on the QML surfaces here.
            ${EB_THEME2_RCC})
        target_link_libraries(everythingbox PRIVATE Qt6::Qml Qt6::Quick Qt6::QuickWidgets)
        target_compile_definitions(everythingbox PRIVATE EB_HAVE_QML)
    endif()
    set_target_properties(everythingbox PROPERTIES OUTPUT_NAME "EverythingBox") # the on-disk exe is EverythingBox.exe
    # On macOS, produce an EverythingBox.app bundle so macdeployqt can bundle Qt + libmpv into a .dmg.
    set_target_properties(everythingbox PROPERTIES MACOSX_BUNDLE TRUE)
    # macOS app icon. Two halves, and BOTH are required — a bundle with only one of them shows the
    # generic app tile: the .icns has to be COPIED into Contents/Resources (that is what
    # MACOSX_PACKAGE_LOCATION on the source file does; the RESOURCE property would also install the
    # source file itself), and CFBundleIconFile has to NAME it. Qt's stock macOS Info.plist template
    # already carries `<key>CFBundleIconFile</key><string>${MACOSX_BUNDLE_ICON_FILE}</string>`, so
    # setting that property is enough here and no custom plist is needed (iOS is a different story —
    # see the IOS block below). The value is the basename WITHOUT the extension per Apple's plist
    # convention; the loader appends .icns.
    if(APPLE AND NOT IOS)
        target_sources(everythingbox PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/resources/appicon.icns")
        set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/resources/appicon.icns"
            TARGET_DIRECTORY everythingbox
            PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
        set_target_properties(everythingbox PROPERTIES MACOSX_BUNDLE_ICON_FILE "appicon")
    endif()
    # On Windows, build as a GUI-subsystem app so no console window opens alongside the main window.
    # qt_add_executable links Qt's EntryPoint, which supplies the WinMain that forwards to main(); all
    # diagnostics go to the log file instead of the console (see main.cpp's message handler).
    set_target_properties(everythingbox PROPERTIES WIN32_EXECUTABLE TRUE)

    # Console harnesses (desktop-only): standalone executables for headless verification - not built for
    # Android (shared lib + APK) or iOS (app bundle).
    if(NOT ANDROID AND NOT IOS)
    # Crash triage (Windows): symbolize a minidump's faulting-thread stack against the Release .pdb, since there's
    # no cdb/WinDbg on the box. No Qt — just DbgHelp. Usage: symbolize_dump <crash.dmp> C:\EverythingBox-app
    if(WIN32)
        add_executable(symbolize_dump tools/symbolize_dump.cpp)
        target_link_libraries(symbolize_dump PRIVATE dbghelp)
    endif()
    # Headless check of the relay netplay path (two NetplaySession objects through a local relay).
    add_executable(probe_netplay tools/probe_netplay.cpp src/emu/NetplaySession.cpp)
    target_include_directories(probe_netplay PRIVATE src src/emu)
    target_link_libraries(probe_netplay PRIVATE Qt6::Core Qt6::Network)
    # Exercise UPnP-IGD port mapping against the real router.
    add_executable(probe_portmap tools/probe_portmap.cpp src/core/PortMapper.cpp)
    target_include_directories(probe_portmap PRIVATE src src/core)
    target_link_libraries(probe_portmap PRIVATE Qt6::Core Qt6::Network)
    # Exercise the "Both" online orchestration: direct-wins and direct-fail->relay-fallback.
    add_executable(probe_netplay_both tools/probe_netplay_both.cpp src/emu/NetplaySession.cpp)
    target_include_directories(probe_netplay_both PRIVATE src src/emu)
    target_link_libraries(probe_netplay_both PRIVATE Qt6::Core Qt6::Network)
    # Compute the RetroAchievements ROM hash (rc_hash) to triage "unsupported game version".
    add_executable(probe_rahash tools/probe_rahash.cpp)
    target_link_libraries(probe_rahash PRIVATE rcheevos)
    # Console harness for the EPUB parser (Qt Core only) - used for headless verification.
    add_executable(probe_epub tools/probe_epub.cpp src/ebook/EpubBook.cpp third_party/miniz.c)
    target_include_directories(probe_epub PRIVATE src src/ebook third_party)
    target_compile_definitions(probe_epub PRIVATE MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
    target_link_libraries(probe_epub PRIVATE Qt6::Core)

    # Console harness for the MOBI parser (Qt Core only).
    add_executable(probe_mobi tools/probe_mobi.cpp src/ebook/MobiBook.cpp)
    target_include_directories(probe_mobi PRIVATE src src/ebook)
    target_link_libraries(probe_mobi PRIVATE Qt6::Core)

    # FormFactor core: mode resolution + the adaptivity token table + the display/mode setting (D1 Task 1).
    # QtCore-only (no Quick/Widgets) so the probe links lean; Qt6::Test supplies QSignalSpy for the
    # changed()-fires-once assert.
    add_executable(probe_formfactor tools/probe_formfactor.cpp
        src/theme2/FormFactor.cpp src/theme2/FormFactor.h
        src/core/Settings.cpp src/core/Settings.h
        src/core/LifecyclePolicy.h)
    target_include_directories(probe_formfactor PRIVATE src src/theme2 src/core)
    target_link_libraries(probe_formfactor PRIVATE Qt6::Core Qt6::Test)

    # SyncOffsets store: per-file/global audio+subtitle sync offsets, the persistence foundation the player's
    # sync controls (player/sync-controls Task 2) wire onto. QtCore-only (no Quick/Widgets) so the probe links
    # lean and runs under offscreen QPA. SyncOffsets.cpp is self-contained (its own QSettings store() on the
    # shared everythingbox.ini), so the target needs neither Settings.cpp nor FormFactor.cpp — Qt6::Core only.
    add_executable(probe_sync tools/probe_sync.cpp
        src/core/SyncOffsets.cpp src/core/SyncOffsets.h)
    target_include_directories(probe_sync PRIVATE src src/core)
    target_link_libraries(probe_sync PRIVATE Qt6::Core)

    # ThemeRegistry: the pure core of the in-app theme gallery — what a community registry index means, and
    # which remote paths may become filenames. QtCore-only and network-free by construction, which is the
    # whole point: both Appearance surfaces share this one copy, and the probe suite (offline, windowless)
    # can only reach the feature here.
    add_executable(probe_themereg tools/probe_themereg.cpp
        src/core/ThemeRegistry.cpp)
    target_include_directories(probe_themereg PRIVATE src src/core)
    target_link_libraries(probe_themereg PRIVATE Qt6::Core)

    # ItemMarks store: per-profile hidden/completion/tags + tag vocab + pinned shelves, the persistence
    # foundation later library-management tasks (hidden filtering, detail actions, shelves) wire onto.
    # QtCore-only (a QSettings wrapper over the shared everythingbox.ini) so the probe links lean and runs
    # under offscreen QPA. Needs ProfileStore.cpp (marks are namespaced by the active profile id).
    add_executable(probe_marks tools/probe_marks.cpp
        src/core/ItemMarks.cpp src/core/ItemMarks.h
        src/core/Tombstones.cpp src/core/Tombstones.h  # removeTagEverywhere tombstones the retired tag name
        src/core/ProfileStore.cpp src/core/ProfileStore.h)
    target_include_directories(probe_marks PRIVATE src src/core)
    target_link_libraries(probe_marks PRIVATE Qt6::Core)

    # Per-book reading bookmarks (issue #136): the pure ReaderAnchor model (header-only, in src/ebook) and the
    # BookmarkStore round-trip (add/remove/list-sorted + the delete tombstone). QtCore-only (a QSettings wrapper
    # over the shared everythingbox.ini, per-profile like FavoritesStore), so it links lean under offscreen QPA.
    # Needs ProfileStore.cpp (bookmarks are namespaced by the active profile) and Tombstones.cpp (remove()).
    add_executable(probe_bookmarks tools/probe_bookmarks.cpp
        src/core/BookmarkStore.cpp src/core/BookmarkStore.h
        src/ebook/ReaderAnchor.h                       # header-only; the anchor round-trip + comparator
        src/core/Tombstones.cpp src/core/Tombstones.h  # remove() records a delete tombstone
        src/core/ProfileStore.cpp src/core/ProfileStore.h)
    target_include_directories(probe_bookmarks PRIVATE src src/core src/ebook)
    target_link_libraries(probe_bookmarks PRIVATE Qt6::Core)

    # Per-item audio bookmarks (issue #140): the AudioBookmarkStore round-trip (add/remove/list-sorted-by-position
    # + the delete tombstone) and the jump-interval Settings default. QtCore-only (a QSettings wrapper over the
    # shared everythingbox.ini, per-profile like BookmarkStore), so it links lean under offscreen QPA. Needs
    # ProfileStore.cpp (per-profile namespace) + Tombstones.cpp (remove()); Settings.cpp pulls FormFactor.cpp
    # (the QtCore-only mode authority), same pairing as probe_hwdecode.
    add_executable(probe_audiobookmarks tools/probe_audiobookmarks.cpp
        src/core/AudioBookmarkStore.cpp src/core/AudioBookmarkStore.h
        src/core/Tombstones.cpp src/core/Tombstones.h  # remove() records a delete tombstone
        src/core/ProfileStore.cpp src/core/ProfileStore.h
        src/core/Settings.cpp src/core/Settings.h      # §4 asserts audioJumpSeconds() default + clamp
        src/theme2/FormFactor.cpp src/theme2/FormFactor.h)
    target_include_directories(probe_audiobookmarks PRIVATE src src/core src/theme2)
    target_link_libraries(probe_audiobookmarks PRIVATE Qt6::Core)

    # OPDS book catalogs (issue #146): the pure OPDS 1.2 Atom parser (OpdsFeed.h — feed/entry parse, link
    # classification, href resolution, basic-auth encoding; header-only in src/ebook) and the per-profile
    # OpdsCatalogStore round-trip (add/update/remove, credentials round-trip). QtCore-only (a QSettings wrapper
    # over the shared everythingbox.ini, per-profile like the stores above), so it links lean under offscreen QPA.
    add_executable(probe_opds tools/probe_opds.cpp
        src/ebook/OpdsFeed.h                             # header-only unit under test (parser + helpers)
        src/core/OpdsCatalogStore.cpp src/core/OpdsCatalogStore.h
        src/core/ProfileStore.cpp src/core/ProfileStore.h)  # catalogs are namespaced by the active profile
    target_include_directories(probe_opds PRIVATE src src/core src/ebook)
    target_link_libraries(probe_opds PRIVATE Qt6::Core)

    # CB7/CBT comic readers (issue #144): the pure in-memory Tar reader (Tar.h) and the natural page-order
    # collation (ComicPageOrder.h) both readers share. Both header-only and QtCore-only, so this links lean
    # under offscreen QPA; the CB7 7z path rides the already-tested SevenZip and is not exercised here.
    add_executable(probe_tar tools/probe_tar.cpp
        src/comic/Tar.h src/comic/ComicPageOrder.h)  # header-only units under test
    target_include_directories(probe_tar PRIVATE src src/comic)
    target_link_libraries(probe_tar PRIVATE Qt6::Core)

    # Per-game launch overrides (issue #51): the pure resolution (core/emulator/extra-args) that the launch
    # pipeline consults, plus the global husk-on-clear store round-trip. QtCore-only (a QSettings wrapper over
    # the shared everythingbox.ini, global like MetaOverrides), so it links lean and runs under offscreen QPA.
    add_executable(probe_launchopts tools/probe_launchopts.cpp
        src/core/LaunchOptionsStore.cpp src/core/LaunchOptionsStore.h
        # Slice 2a §11 asserts the exact composition prepareCore threads: resolveBackend(Settings::backendFor(id),ov).
        # Settings.cpp pulls FormFactor.cpp (virtualPadEnabled resolves against it), same as probe_softpatch/probe_stats.
        src/core/Settings.cpp     src/core/Settings.h
        src/theme2/FormFactor.cpp src/theme2/FormFactor.h)
    target_include_directories(probe_launchopts PRIVATE src src/core src/theme2)
    target_link_libraries(probe_launchopts PRIVATE Qt6::Core)

    # Pad-to-keyboard (issue #105): the pure model + translate() edge detector + resolveStick() analog hysteresis
    # (src/core/Pad2Key.h, header-only) and the per-game enable+profile store round-trip (src/core/Pad2KeyStore).
    # QtCore-only — NO SDL and NO OS injection (the SendInput side lives in Pad2KeyRuntime and is not headlessly
    # drivable; the probe pins the profile + translation only) — so it links lean and runs under offscreen QPA.
    add_executable(probe_pad2key tools/probe_pad2key.cpp
        src/core/Pad2KeyStore.cpp src/core/Pad2KeyStore.h src/core/Pad2Key.h)
    target_include_directories(probe_pad2key PRIVATE src src/core)
    target_link_libraries(probe_pad2key PRIVATE Qt6::Core)

    # Persisted PC-launcher scan cache (issue #62): the pure merge (an OK scan replaces the cache, an
    # unreadable one falls back to it marked unavailable) + its JSON blob round-trip + the ini-backed
    # reconcile. QtCore-only (a QSettings wrapper over the shared everythingbox.ini, like the stores above),
    # so it links lean and runs under offscreen QPA.
    add_executable(probe_pcscan tools/probe_pcscan.cpp
        src/core/PcScanCache.cpp src/core/PcScanCache.h)
    target_include_directories(probe_pcscan PRIVATE src src/core)
    target_link_libraries(probe_pcscan PRIVATE Qt6::Core)

    # Per-game launch HOOKS (issue #64): the pure argv tokenizer + {rom} substitution (src/core/LaunchHooks.h,
    # header-only) and the device-local, UNSYNCED per-game store (src/core/LaunchHooksStore). QtCore-only — no
    # QProcess is exercised (a live hook run is not headlessly drivable) — so it links lean under offscreen QPA.
    # Cross-emulator graphics settings (issue #103): the pure quartet -> per-emulator INI mapping
    # (src/core/EmuSettings.h, header-only) plus the device-local per-game override store round-trip
    # (src/core/EmuGfxStore). QtCore-only — the mapping is asserted against hand-authored fixtures with no disk
    # and no live emulator — so it links lean under offscreen QPA.
    add_executable(probe_emusettings tools/probe_emusettings.cpp
        src/core/EmuGfxStore.cpp src/core/EmuGfxStore.h src/core/EmuSettings.h)
    target_include_directories(probe_emusettings PRIVATE src src/core)
    target_link_libraries(probe_emusettings PRIVATE Qt6::Core)

    # Slang-shader preset model + store (issue #99, SLICE 1): the PURE preset model + curated registry + scope
    # resolution (src/core/ShaderPreset.h, header-only) plus the device-local per-game/per-system override store
    # round-trip (src/core/ShaderPresetStore). QtCore-only — the model is asserted against hand-authored fixtures
    # with no disk and no librashader (nothing renders yet) — so it links lean under offscreen QPA.
    add_executable(probe_shaderpreset tools/probe_shaderpreset.cpp
        src/core/ShaderPresetStore.cpp src/core/ShaderPresetStore.h src/core/ShaderPreset.h)
    target_include_directories(probe_shaderpreset PRIVATE src src/core)
    target_link_libraries(probe_shaderpreset PRIVATE Qt6::Core)

    # librashader link+call proof (issue #99, SLICE 2): prove librashader-capi — the Rust C-ABI crate compiled
    # from source via corrosion — links and its C ABI is callable from C++ with NO GPU context (version funcs +
    # CPU-only preset load + the error surface). Plain C++17: NOT linked against Qt, and it writes its .slangp/
    # .slang fixtures to an OS temp dir, so it touches neither Qt nor the build folder. Only built when the
    # librashader block above ran (desktop); the runner + CI (which run on desktop) require its binary.
    if(EB_HAVE_LIBRASHADER)
        add_executable(probe_librashader tools/probe_librashader.cpp)
        target_include_directories(probe_librashader PRIVATE ${EB_LIBRASHADER_INCLUDE})
        target_link_libraries(probe_librashader PRIVATE ${EB_LIBRASHADER_TARGET})

        # ShaderChain wrapper proof (issue #99, SLICE 3): the RAII wrapper over librashader's OpenGL
        # filter-chain C API compiles + links against that API, its error paths behave (no crash on a bad
        # preset / no chain), and — when an offscreen GL context is obtainable — a real passthrough preset
        # creates a live chain and filters a frame. GL-CONDITIONAL: it forces offscreen QPA and skips the live
        # chain test (still PASSES) when no GL context exists, so a GPU-less CI runner never fails on it. Needs
        # Qt6::OpenGL(+Widgets) for QOpenGLContext/QOpenGLFunctions and the LIBRA_RUNTIME_OPENGL define so the
        # vendored header exposes the GL declarations ShaderChain.h uses.
        add_executable(probe_shaderchain tools/probe_shaderchain.cpp
            src/emu/ShaderChain.cpp src/emu/ShaderChain.h)
        target_include_directories(probe_shaderchain PRIVATE src src/emu ${EB_LIBRASHADER_INCLUDE})
        target_compile_definitions(probe_shaderchain PRIVATE LIBRA_RUNTIME_OPENGL)
        target_link_libraries(probe_shaderchain PRIVATE ${EB_LIBRASHADER_TARGET}
            Qt6::Gui Qt6::OpenGL)

        # Shipped shader assets + present pass proof (issue #99, SLICE 4): extraction + id->.slangp resolution
        # (context-free, runs on CI) and — GL-conditional — that every SHIPPED .slangp compiles in librashader
        # and ShaderRenderer::render() (the exact RetroView call) filters a frame. Compiles shaders.qrc in so
        # :/eb/shaders exists; EB_ISOLATED_DATA_DIR (applied to every probe_* at the end of this file) sends
        # ensureExtracted()'s writes to this process's scratch dir. Needs the ShaderRenderer + ShaderChain TUs.
        qt_add_resources(EB_SHADERS_RCC_PROBE resources/shaders.qrc)
        add_executable(probe_shaderassets tools/probe_shaderassets.cpp
            src/emu/ShaderRenderer.cpp src/emu/ShaderRenderer.h
            src/emu/ShaderChain.cpp src/emu/ShaderChain.h
            src/core/ShaderPreset.h
            ${EB_SHADERS_RCC_PROBE})
        target_include_directories(probe_shaderassets PRIVATE src src/emu src/core ${EB_LIBRASHADER_INCLUDE})
        target_compile_definitions(probe_shaderassets PRIVATE LIBRA_RUNTIME_OPENGL)
        target_link_libraries(probe_shaderassets PRIVATE ${EB_LIBRASHADER_TARGET}
            Qt6::Gui Qt6::OpenGL)
    endif()

    # Device performance profiles (issue #119): the PURE device classifier + tuned per-emulator defaults table
    # (src/core/DeviceProfile.h, header-only) and the three-layer resolution precedence. QtCore-only — identify()
    # and defaultsFor() touch no hardware and no disk (the platform glue in DeviceProfileDetect is not linked;
    # real detection on a Deck/Ally is not headlessly drivable) — so it links lean under offscreen QPA.
    add_executable(probe_deviceprofile tools/probe_deviceprofile.cpp
        src/core/DeviceProfile.h src/core/EmuSettings.h)
    target_include_directories(probe_deviceprofile PRIVATE src src/core)
    target_link_libraries(probe_deviceprofile PRIVATE Qt6::Core)

    # Multi-seat controller propagation (issue #104): the pure seat model + per-emulator player-N INI mapping
    # (src/core/ControllerSeats.h, header-only). QtCore-only — no SDL, no disk, no live emulator: the seat
    # assignment and each player's config edits are asserted against hand-authored shipped-P1 fixtures — so it
    # links lean under offscreen QPA. Pins that seat 0 reproduces today's Player-1 block byte-for-byte.
    add_executable(probe_seats tools/probe_seats.cpp src/core/ControllerSeats.h)
    target_include_directories(probe_seats PRIVATE src src/core)
    target_link_libraries(probe_seats PRIVATE Qt6::Core)

    add_executable(probe_launchhooks tools/probe_launchhooks.cpp
        src/core/LaunchHooksStore.cpp src/core/LaunchHooksStore.h src/core/LaunchHooks.h)
    target_include_directories(probe_launchhooks PRIVATE src src/core)
    target_link_libraries(probe_launchhooks PRIVATE Qt6::Core)

    # Saved filter presets (issue #63): the pure filter model/evaluator (src/core/GameFilter) and the
    # per-profile preset store (src/core/FilterPresetStore). QtCore-only — the evaluator takes a plain facts
    # snapshot and the store is a QSettings wrapper — so the probe links lean and runs under offscreen QPA.
    # Needs ProfileStore.cpp (presets are namespaced by the active profile id).
    add_executable(probe_filterpreset tools/probe_filterpreset.cpp
        src/core/GameFilter.cpp        src/core/GameFilter.h
        src/core/FilterPresetStore.cpp src/core/FilterPresetStore.h
        src/core/Tombstones.cpp        src/core/Tombstones.h  # issue #184: remove() leaves a delete tombstone
        src/core/ProfileStore.cpp      src/core/ProfileStore.h)
    target_include_directories(probe_filterpreset PRIVATE src src/core)
    target_link_libraries(probe_filterpreset PRIVATE Qt6::Core)

    # Hardware-decode Setting (issue #67): the pure choice->mpv-option map (src/video/HwDecode.h, header-only)
    # plus the Settings default. QtCore-only; Settings.cpp pulls FormFactor.cpp (virtualPadEnabled resolves
    # against it), same as probe_softpatch.
    add_executable(probe_hwdecode tools/probe_hwdecode.cpp
        src/core/Settings.cpp     src/core/Settings.h
        src/video/HwDecode.h
        src/theme2/FormFactor.cpp src/theme2/FormFactor.h)
    target_include_directories(probe_hwdecode PRIVATE src src/core src/video)
    target_link_libraries(probe_hwdecode PRIVATE Qt6::Core)

    # Hardcore RetroAchievements (issue #94): the pure hardcore-affordance policy (src/core/Hardcore.h,
    # header-only, QtCore-free) plus the opt-in Setting default that drives it. QtCore-only; Settings.cpp pulls
    # FormFactor.cpp (virtualPadEnabled resolves against it), same as probe_hwdecode.
    add_executable(probe_hardcore tools/probe_hardcore.cpp
        src/core/Settings.cpp     src/core/Settings.h
        src/core/Hardcore.h
        src/theme2/FormFactor.cpp src/theme2/FormFactor.h)
    target_include_directories(probe_hardcore PRIVATE src src/core)
    target_link_libraries(probe_hardcore PRIVATE Qt6::Core)

    # Subtitle styling (issue #71): the pure Style->mpv-option map (src/video/SubtitleStyle.h, header-only) plus
    # the Settings defaults + round-trip that feed it. QtCore-only; Settings.cpp pulls FormFactor.cpp
    # (virtualPadEnabled resolves against it), same as probe_hwdecode.
    add_executable(probe_substyle tools/probe_substyle.cpp
        src/core/Settings.cpp     src/core/Settings.h
        src/video/SubtitleStyle.h
        src/theme2/FormFactor.cpp src/theme2/FormFactor.h)
    target_include_directories(probe_substyle PRIVATE src src/core src/video)
    target_link_libraries(probe_substyle PRIVATE Qt6::Core)

    # Reader typography (issue #135): the pure Settings->Resolved map + theme palettes + clamps
    # (src/ebook/ReaderTypography.h, header-only) plus the Settings defaults + round-trip that feed it. QtCore-only;
    # Settings.cpp pulls FormFactor.cpp (virtualPadEnabled resolves against it), same as probe_substyle.
    add_executable(probe_readertypography tools/probe_readertypography.cpp
        src/core/Settings.cpp     src/core/Settings.h
        src/ebook/ReaderTypography.h
        src/theme2/FormFactor.cpp src/theme2/FormFactor.h)
    target_include_directories(probe_readertypography PRIVATE src src/core src/ebook)
    target_link_libraries(probe_readertypography PRIVATE Qt6::Core)

    # Refresh-rate matching Tier 1 (issue #70): the pure toggle->mpv-video-sync map + form-factor default
    # (src/video/RefreshSync.h, header-only) plus the Settings default/round-trip. QtCore-only; Settings.cpp pulls
    # FormFactor.cpp (videoRefreshSync()'s form-factor default resolves against it), same as probe_hwdecode.
    add_executable(probe_refreshsync tools/probe_refreshsync.cpp
        src/core/Settings.cpp     src/core/Settings.h
        src/video/RefreshSync.h
        src/theme2/FormFactor.cpp src/theme2/FormFactor.h)
    target_include_directories(probe_refreshsync PRIVATE src src/core src/video)
    target_link_libraries(probe_refreshsync PRIVATE Qt6::Core)

    # HDR output (issue #68): the pure (Mode, Platform)->mpv-option map (src/video/HdrOutput.h, header-only) plus
    # the id<->Mode round-trip and the Settings default/round-trip that feed it. QtCore-only; Settings.cpp pulls
    # FormFactor.cpp (videoRefreshSync()'s form-factor default resolves against it), same as probe_refreshsync.
    add_executable(probe_hdroutput tools/probe_hdroutput.cpp
        src/core/Settings.cpp     src/core/Settings.h
        src/video/HdrOutput.h
        src/theme2/FormFactor.cpp src/theme2/FormFactor.h)
    target_include_directories(probe_hdroutput PRIVATE src src/core src/video)
    target_link_libraries(probe_hdroutput PRIVATE Qt6::Core)

    # Audio output (issue #69): the pure Output->mpv-option map (src/video/AudioOutput.h, header-only), the
    # Settings defaults + round-trip, AND the DEVICE-LOCAL classification asserted against the REAL CloudSync
    # carve-out (isDeviceLocalKey). Linking CloudSync in pulls the same lean subset probe_cloudmerge uses for its
    # carve-out asserts: SettingsTxn (applySettingsJson closes an open txn), BrandMigration (the Drive lookups
    # consult its flags), TraktSync (backfillKeyPrefix — the family the carve-out excludes) and miniz (the bundle
    # zip; unused by the classification asserts but part of CloudSync.cpp's TU). Qt6::Gui is CloudSync's
    # QDesktopServices (OAuth browser open); Qt6::Network its QNetworkAccessManager.
    add_executable(probe_audioout tools/probe_audioout.cpp
        src/core/Settings.cpp        src/core/Settings.h
        src/video/AudioOutput.h
        src/theme2/FormFactor.cpp    src/theme2/FormFactor.h
        src/core/CloudSync.cpp       src/core/CloudSync.h
        src/core/DriveSyncBackend.cpp src/core/DriveSyncBackend.h   # Increment B: Drive transport behind the SyncBackend seam
        src/core/ServerSyncBackend.cpp src/core/ServerSyncBackend.h # Increment C: self-hosted server transport behind the SyncBackend seam
        src/core/SyncBackend.h                                      # the abstract seam CloudSync composes (Q_OBJECT for AUTOMOC)
        src/core/SettingsTxn.cpp     src/core/SettingsTxn.h
        src/core/BrandMigration.cpp  src/core/BrandMigration.h
        src/core/TraktSync.cpp       src/core/TraktSync.h
        src/core/TraktRead.cpp       src/core/TraktRead.h          # TraktSync's id mapping + Trakt time parser
        src/core/PendingPush.cpp     src/core/PendingPush.h        # CloudSync::withAccessToken classifies a refresh
        third_party/miniz.c)
    target_include_directories(probe_audioout PRIVATE src src/core src/video src/theme2 third_party)
    target_compile_definitions(probe_audioout PRIVATE MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
    target_link_libraries(probe_audioout PRIVATE Qt6::Core Qt6::Network Qt6::Gui)

    # Preferred content language (Increment 1): the canonical ISO-639 mapping (src/core/LanguageCodes.h,
    # header-only) plus the Settings content/language accessor, its one-shot migration from the legacy
    # 3-letter subs/language key, and the subtitle-language delegation. Mirrors probe_audioout's dependency
    # set because Settings.cpp drags in the same CloudSync/AppPaths/miniz TU (probe_audioout's comment above
    # explains the lean subset); only the target/source names change and LanguageCodes.h is added.
    add_executable(probe_contentlang tools/probe_contentlang.cpp
        src/core/LanguageCodes.h
        src/core/Settings.cpp        src/core/Settings.h
        src/video/AudioOutput.h
        src/theme2/FormFactor.cpp    src/theme2/FormFactor.h
        src/core/CloudSync.cpp       src/core/CloudSync.h
        src/core/DriveSyncBackend.cpp src/core/DriveSyncBackend.h   # Increment B: Drive transport behind the SyncBackend seam
        src/core/ServerSyncBackend.cpp src/core/ServerSyncBackend.h # Increment C: self-hosted server transport behind the SyncBackend seam
        src/core/SyncBackend.h                                      # the abstract seam CloudSync composes (Q_OBJECT for AUTOMOC)
        src/core/SettingsTxn.cpp     src/core/SettingsTxn.h
        src/core/BrandMigration.cpp  src/core/BrandMigration.h
        src/core/TraktSync.cpp       src/core/TraktSync.h
        src/core/TraktRead.cpp       src/core/TraktRead.h          # TraktSync's id mapping + Trakt time parser
        src/core/PendingPush.cpp     src/core/PendingPush.h        # CloudSync::withAccessToken classifies a refresh
        third_party/miniz.c)
    target_include_directories(probe_contentlang PRIVATE src src/core src/video src/theme2 third_party)
    target_compile_definitions(probe_contentlang PRIVATE MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
    target_link_libraries(probe_contentlang PRIVATE Qt6::Core Qt6::Network Qt6::Gui)

    # ROM soft-patching (src/core/RomPatch): the in-tree IPS/BPS/UPS appliers + the launch-seam resolver that
    # patches into a derived cache file so the original ROM is never touched (issue #128). QtCore-only; needs
    # Settings.cpp (the auto-apply toggle) which pulls FormFactor.cpp (virtualPadEnabled resolves against it).
    add_executable(probe_softpatch tools/probe_softpatch.cpp
        src/core/RomPatch.cpp src/core/RomPatch.h
        src/core/Settings.cpp src/core/Settings.h
        src/theme2/FormFactor.cpp src/theme2/FormFactor.h
        src/core/LifecyclePolicy.h)
    target_include_directories(probe_softpatch PRIVATE src src/core src/theme2)
    target_link_libraries(probe_softpatch PRIVATE Qt6::Core)

    # Per-game core-option and input-remap overrides (issue #95): the pure OverrideLayer folding + the Settings
    # store's no-leak rails (a game delta never mutates opt/* or the global/per-system binding). QtCore-only;
    # Settings.cpp pulls FormFactor.cpp (virtualPadEnabled resolves against it), same as probe_softpatch. The
    # libretro header is used header-only for the RETRO_DEVICE_ID_JOYPAD_* constants.
    add_executable(probe_overrides tools/probe_overrides.cpp
        src/core/OverrideLayer.cpp src/core/OverrideLayer.h
        src/core/Settings.cpp src/core/Settings.h
        src/theme2/FormFactor.cpp src/theme2/FormFactor.h)
    target_include_directories(probe_overrides PRIVATE src src/core src/theme2 src/libretro)
    target_link_libraries(probe_overrides PRIVATE Qt6::Core)

    # Data-driven system catalog (src/core/SystemCatalog, issue #92): the built-in table round-trips through
    # the JSON schema unchanged (the no-regression rail), and <data>/systems/*.json adds/overrides/skip-malformed
    # over it. Header-only + QtCore, so the probe compiles just its own .cpp. EB_SYSCATALOG_EXAMPLE_DIR points at
    # the shipped example so the probe validates that file merges as expected.
    add_executable(probe_syscatalog tools/probe_syscatalog.cpp src/core/SystemCatalog.h)
    target_include_directories(probe_syscatalog PRIVATE src src/core)
    target_link_libraries(probe_syscatalog PRIVATE Qt6::Core)
    target_compile_definitions(probe_syscatalog PRIVATE
        EB_SYSCATALOG_EXAMPLE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/resources/systems")

    # Data-driven standalone-emulator registry (src\core\EmulatorRegistry, issue #52): the built-in emulator
    # table round-trips through the JSON schema unchanged (the no-regression rail), <data>/emulators/*.json
    # adds/overrides/skip-malformed over it, a user entry (absolute binary, no update URL) resolves as
    # installed with NO download. Header-only + QtCore, so the probe compiles just its own .cpp.
    # EB_USEREMU_EXAMPLE_DIR points at the shipped example so the probe validates that file merges as expected.
    add_executable(probe_useremulators tools/probe_useremulators.cpp src/core/EmulatorRegistry.h)
    target_include_directories(probe_useremulators PRIVATE src src/core)
    target_link_libraries(probe_useremulators PRIVATE Qt6::Core)
    target_compile_definitions(probe_useremulators PRIVATE
        EB_USEREMU_EXAMPLE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/resources/emulators")

    # ROM dump verification against No-Intro/Redump DAT files (src/core/HashVerify, issue #97): the Logiqx DAT
    # parser, the payload hasher (iNES header skip), the CHD v5-header SHA1 read, classification and the
    # path+mtime stamp cache. Pure + QtCore-only — no ArchiveRom/Settings dependency (the app-side verify pass
    # owns archive extraction and the on/off toggle), so this probe links just HashVerify.cpp against Qt6::Core.
    add_executable(probe_hashverify tools/probe_hashverify.cpp
        src/core/HashVerify.cpp src/core/HashVerify.h)
    target_include_directories(probe_hashverify PRIVATE src src/core)
    target_link_libraries(probe_hashverify PRIVATE Qt6::Core)

    # ConsumptionStats store: per-profile media (watch/listen seconds) + reading (high-water pages) accrual, the
    # persistence foundation the Stats panel (stats T2) reads and the PlaybackSession/reader seams feed. QtCore-
    # only (a QSettings+JSON wrapper over the shared everythingbox.ini) so the probe links lean and runs under
    # offscreen QPA. Needs ProfileStore.cpp (stats are namespaced by the active profile id).
    # mdsync T3: the accumulators are now device-namespaced (stats/<profile>/<deviceId>/...), so both stores
    # need Settings::deviceId() (which drags FormFactor via Settings.cpp's virtualPadEnabled). PlayStats.cpp is
    # pulled in too — its device-namespaced play history + migration are probed alongside ConsumptionStats.
    add_executable(probe_stats tools/probe_stats.cpp
        src/core/ConsumptionStats.cpp src/core/ConsumptionStats.h
        src/core/PlayStats.cpp        src/core/PlayStats.h
        src/core/Settings.cpp         src/core/Settings.h
        src/theme2/FormFactor.cpp     src/theme2/FormFactor.h
        src/core/ProfileStore.cpp src/core/ProfileStore.h)
    target_include_directories(probe_stats PRIVATE src src/core src/theme2)
    target_link_libraries(probe_stats PRIVATE Qt6::Core)

    # PlaylistStore category migration: the data-safety core of category-scoped playlists — a legacy
    # catalogKey-shaped blob folds to categoryKey (video/audio/game/reading via core/MediaCategories.h), one-shot
    # + idempotent, preserving ids/items/legacyKey (the user's real "Weekend Picks" list must survive). QtCore-
    # only (a QSettings+JSON wrapper) so it links lean and runs under offscreen QPA. Needs ProfileStore.cpp
    # (playlists are namespaced by the active profile). MediaCategories.h is header-only (no source to add).
    add_executable(probe_playlists tools/probe_playlists.cpp
        src/core/PlaylistStore.cpp src/core/PlaylistStore.h
        src/core/Tombstones.cpp src/core/Tombstones.h  # PlaylistStore::remove tombstones the deleted playlist id
        src/core/MediaCategories.h
        src/core/ShuffleBag.h
        src/core/ProfileStore.cpp src/core/ProfileStore.h)
    target_include_directories(probe_playlists PRIVATE src src/core)
    target_link_libraries(probe_playlists PRIVATE Qt6::Core)

    # Multi-device-sync foundation (mdsync T1): device identity (Settings::deviceId), the per-store write
    # timestamps (ItemMarks updatedAt / FavoritesStore ts / PlaylistStore updatedAt), and the deletion
    # tombstones (core/Tombstones) that the generalized CloudMerge pass (T2) reads. QtCore-only (QSettings +
    # JSON over the shared everythingbox.ini) so it links lean and runs under offscreen QPA. Pulls the real
    # stores + their deps: Settings (deviceId) needs FormFactor; FavoritesStore's one-time list() migration
    # references RecentStore/DownloadsStore; every store is namespaced by ProfileStore.
    add_executable(probe_cloudmerge tools/probe_cloudmerge.cpp
        src/core/Settings.cpp        src/core/Settings.h
        src/theme2/FormFactor.cpp    src/theme2/FormFactor.h
        src/core/ItemMarks.cpp       src/core/ItemMarks.h
        src/core/FavoritesStore.cpp  src/core/FavoritesStore.h
        src/core/PlaylistStore.cpp   src/core/PlaylistStore.h
        src/core/MediaCategories.h
        src/core/ShuffleBag.h
        src/core/RecentStore.cpp     src/core/RecentStore.h
        src/core/DownloadsStore.cpp  src/core/DownloadsStore.h
        src/core/Tombstones.cpp      src/core/Tombstones.h
        src/core/ResumeStore.cpp     src/core/ResumeStore.h        # issue #150: the resume tombstone namespace
        # issue #150: the RESUME READER. Section 26 asserts the resume fix through the answer the app
        # actually gives -- a fresh PlaybackSession's pending seek -- and drives it through the real clear
        # site (finishResume), because an assertion that the row is absent passes on the broken build too.
        # PlaybackSession is QtCore-only (QSettings + QCryptographicHash + ConsumptionStats, all already
        # linked here) and AUTOMOC handles its QObject, so this costs the target nothing but one TU.
        src/media/PlaybackSession.cpp src/media/PlaybackSession.h
        src/core/CloudMerge.cpp      src/core/CloudMerge.h
        # issue #184: saved filter presets, now a synced per-profile store. CloudMerge calls
        # FilterPresetStore::syncIdForName (legacy-id back-fill) and §33 asserts through the store accessor;
        # FilterPresetStore parses/serializes a gamefilter::Filter, so GameFilter comes with it. QtCore-only.
        src/core/FilterPresetStore.cpp src/core/FilterPresetStore.h
        src/core/GameFilter.cpp      src/core/GameFilter.h
        src/core/MetaOverrides.cpp   src/core/MetaOverrides.h      # issue #24: mergeAll() carries + invalidates it
        src/core/LaunchOptionsStore.cpp src/core/LaunchOptionsStore.h  # issue #51: mergeAll() carries + invalidates it
        src/core/Pad2KeyStore.cpp    src/core/Pad2KeyStore.h     # issue #105: mergeAll() carries + invalidates it (Pad2Key.h comes along)
        src/core/MissedDismiss.cpp   src/core/MissedDismiss.h      # issue #25: ditto for the "you missed" dismissals
        src/core/TraktMissed.cpp     src/core/TraktMissed.h        # (MissedDismiss::prune -> missedDismissExpired)
        src/core/ConsumptionStats.cpp src/core/ConsumptionStats.h  # mdsync T3: mergeAll() invalidates its cache
        src/core/PlayStats.cpp       src/core/PlayStats.h          # issue #166: the reader §30f asserts through
        # issue #166: the PC-game id repair, played against the merge. It is the last place in the tree that
        # removes a synced per-item row, so what a peer sees while it is mid-repair is a CloudMerge question
        # and belongs here rather than in probe_pcgames (which has no merge linked). PcGameId comes with it:
        # remapTable() calls effectiveItemId, so the symbol is needed even though §30 builds its own table.
        # Neither unit gets EB_PCGAMEID_TEST_SEAM here — un-defined they resolve to AppPaths::dataDir()'s ini,
        # which IS this probe's per-process scratch ini, so the remap and the stores share one file already.
        src/core/PcGameId.cpp        src/core/PcGameId.h
        src/core/PcGameRemap.cpp     src/core/PcGameRemap.h
        src/core/CloudSync.cpp       src/core/CloudSync.h          # mdsync T4: the device-local carve-out + bundle hands-off
        src/core/DriveSyncBackend.cpp src/core/DriveSyncBackend.h   # Increment B: the Drive transport behind the SyncBackend seam
        src/core/ServerSyncBackend.cpp src/core/ServerSyncBackend.h # Increment C: self-hosted server transport behind the SyncBackend seam
        src/core/SyncBackend.h                                      # the abstract seam CloudSync composes (Q_OBJECT for AUTOMOC)
        src/core/SettingsTxn.cpp     src/core/SettingsTxn.h        # CloudSync::applySettingsJson closes an open txn (#26)
        src/core/TraktRead.cpp       src/core/TraktRead.h          # (TraktSync's id mapping + time parser)
        src/core/TraktSync.cpp       src/core/TraktSync.h          # backfillKeyPrefix() — SettingsTxn/CloudSync exclude the family
        src/core/PendingPush.cpp     src/core/PendingPush.h        # #34: the pending-push record + retry policy (§19-22)
        src/core/BrandMigration.cpp src/core/BrandMigration.h  # CloudSync/AddonManager consult its flags (QtCore-only TU)
        third_party/miniz.c                                        # (CloudSync's bundle zip; unused by the exclusion-table asserts)
        src/core/ProfileStore.cpp    src/core/ProfileStore.h)
    target_include_directories(probe_cloudmerge PRIVATE src src/core src/media src/theme2 third_party)
    target_compile_definitions(probe_cloudmerge PRIVATE MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
    target_link_libraries(probe_cloudmerge PRIVATE Qt6::Core Qt6::Network Qt6::Gui) # Qt6::Gui: CloudSync's QDesktopServices (OAuth browser open)

    # First-run onboarding router (onboarding/drive-restore T1): the pure eb::onboardingRoute table plus
    # CloudSync::signInAvailable()'s desktop-true / Android-false platform gate. OnboardingRoute.h is header-only,
    # so the linkage is just CloudSync's TU (for signInAvailable) + its deps (Settings needs FormFactor;
    # CloudSync's bundle zip needs miniz). Same lean Core/Network/Gui set as probe_cloudmerge.
    add_executable(probe_onboarding tools/probe_onboarding.cpp
        src/core/OnboardingRoute.h                                    # the pure router under test (header-only)
        src/core/CloudSync.cpp       src/core/CloudSync.h             # signInAvailable() + the onboarding/done carve-out
        src/core/DriveSyncBackend.cpp src/core/DriveSyncBackend.h      # Increment B: the Drive transport behind the SyncBackend seam
        src/core/ServerSyncBackend.cpp src/core/ServerSyncBackend.h # Increment C: self-hosted server transport behind the SyncBackend seam
        src/core/SyncBackend.h                                         # the abstract seam CloudSync composes (Q_OBJECT for AUTOMOC)
        src/core/SettingsTxn.cpp     src/core/SettingsTxn.h           # CloudSync::applySettingsJson closes an open txn (#26)
        src/core/TraktRead.cpp       src/core/TraktRead.h          # (TraktSync's id mapping + time parser)
        src/core/TraktSync.cpp       src/core/TraktSync.h          # backfillKeyPrefix() — SettingsTxn/CloudSync exclude the family
        src/core/PendingPush.cpp     src/core/PendingPush.h           # CloudSync's token refresh classifies via it (#34)
        src/core/BrandMigration.cpp src/core/BrandMigration.h  # CloudSync/AddonManager consult its flags (QtCore-only TU)
        src/core/Settings.cpp        src/core/Settings.h
        src/theme2/FormFactor.cpp    src/theme2/FormFactor.h          # Settings.cpp references FormFactor::instance()
        third_party/miniz.c)                                          # (CloudSync's bundle zip; unused by these asserts)
    target_include_directories(probe_onboarding PRIVATE src src/core src/theme2 third_party)
    target_compile_definitions(probe_onboarding PRIVATE MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
    target_link_libraries(probe_onboarding PRIVATE Qt6::Core Qt6::Network Qt6::Gui)

    # Headless test for the per-profile theme choice (roadmap #57): the key format, needsPick, resolve's
    # four-step ordering, the XMB->Triple rename, and the migration table incl. idempotence. ThemeChoice.cpp
    # is QtCore-only (its own file-local store(), AppPaths/AppBrand are header-only), and the only other TU it
    # needs is ProfileStore for the profile id list — so this links leaner than any other probe here.
    # It ALSO carries ThemeFormFactors (issue #32) — the `formFactors` manifest declaration judged against the
    # current device — which is the same shape of pure theme decision and is likewise QtCore-only.
    add_executable(probe_theme tools/probe_theme.cpp
        src/core/ThemeChoice.cpp src/core/ThemeChoice.h
        src/core/ThemeFormFactors.cpp src/core/ThemeFormFactors.h
        src/core/ProfileStore.cpp src/core/ProfileStore.h)
    target_include_directories(probe_theme PRIVATE src src/core)
    # The ini-redirect seam exists ONLY here. The app target must never define this: without it the setter is
    # not declared or compiled at all, so an accidental production call is a compile error rather than a
    # silent process-wide redirect of every theme read and write.
    target_compile_definitions(probe_theme PRIVATE EB_THEMECHOICE_TEST_SEAM)
    target_link_libraries(probe_theme PRIVATE Qt6::Core)

    # Headless test for the settings save/discard transaction (issue #26): the scope predicate (incl. the
    # device-local-but-in-scope cases a naive implementation gets wrong), dirty counting by VALUE, rollback
    # fidelity, and — the assertion the whole predicate exists for — that an out-of-scope key written mid
    # transaction survives rollback untouched. SettingsTxn.cpp is QtCore-only with its own file-local
    # store(), so this links against Qt6::Core alone.
    add_executable(probe_settingstxn tools/probe_settingstxn.cpp
        src/core/TraktRead.cpp       src/core/TraktRead.h          # (TraktSync's id mapping + time parser)
        src/core/TraktSync.cpp       src/core/TraktSync.h          # backfillKeyPrefix() — the excluded family
        src/core/SettingsTxn.cpp src/core/SettingsTxn.h)
    target_include_directories(probe_settingstxn PRIVATE src src/core)
    # The ini-redirect seam exists ONLY here (same rule as probe_theme's above): without the macro the setter
    # is not declared or compiled at all, so a production call is a compile error, not a silent redirect.
    target_compile_definitions(probe_settingstxn PRIVATE EB_SETTINGSTXN_TEST_SEAM)
    target_link_libraries(probe_settingstxn PRIVATE Qt6::Core)

    # Headless test for the per-profile passcode core (issue #30): format, hashing + the SALT SEPARATION from
    # the frozen parental-PIN salt, the always-ask-at-launch decision, the recovery-route rule, the rate-limit
    # escalation + cap, the evaluate() lockout-before-compare composite, and the device-local attempt-state
    # persistence. ProfilePasscode.cpp is QtCore-only with its own file-local store() (AppPaths/AppBrand are
    # header-only), so this links against Qt6::Core alone — leaner than every probe except probe_theme.
    add_executable(probe_passcode tools/probe_passcode.cpp
        src/core/ProfilePasscode.cpp src/core/ProfilePasscode.h)
    target_include_directories(probe_passcode PRIVATE src src/core)
    # The ini-redirect seam exists ONLY here (same rule as probe_theme / probe_settingstxn): without the macro
    # the setter is not declared or compiled at all, so a production call is a compile error, not a silent
    # process-wide redirect of every passcode read and write.
    target_compile_definitions(probe_passcode PRIVATE EB_PROFILEPASSCODE_TEST_SEAM)
    target_link_libraries(probe_passcode PRIVATE Qt6::Core)

    # Headless test for the Trakt read layer (issue #23): the tolerant calendar parser and the
    # identity mapper. TraktRead.cpp is QtCore-only (QJson + QDateTime), so this links leanest of all.
    # SingleFlight.h is header-only and Qt-free on purpose: it is the token-refresh queue TraktClient
    # runs, and listing it here is what makes that queue's re-entrancy cases testable without a socket.
    # TraktSync.cpp joins it for the same reason: the watchlist/collection parsers, the paging + rate-limit
    # classifier and the watched-history reconciliation are all pure, so the partial-page, 429 and
    # "local and Trakt disagree" cases are reachable here with no socket and no account.
    # TraktMissed.cpp joins them for the same reason again (#25): "you missed" is a pure join of the
    # calendar, the marks and the per-show dismissal watermarks, with both local-state lookups arriving
    # through callbacks — so every clause of it, and every negative case, is reachable here with no
    # socket, no account and no ini.
    add_executable(probe_trakt tools/probe_trakt.cpp
        src/core/TraktRead.cpp src/core/TraktRead.h
        src/core/TraktSync.cpp src/core/TraktSync.h
        src/core/TraktMissed.cpp src/core/TraktMissed.h
        src/core/SingleFlight.h)
    target_include_directories(probe_trakt PRIVATE src src/core)
    target_link_libraries(probe_trakt PRIVATE Qt6::Core)

    # Headless test for PC-game identity + source selection (the merged PC Games folder), and for the
    # remap that moves the user's per-item records onto the merged id (pc-games T3). PcGameRemap works at
    # the QSettings layer rather than through ItemMarks/ConsumptionStats/PlayStats/FavoritesStore — see the
    # note at the top of PcGameRemap.cpp — so both units stay QtCore-only and this links against Qt6::Core
    # alone, with no ProfileStore/Settings/Tombstones/RecentStore dragged into a probe about identity.
    add_executable(probe_pcgames tools/probe_pcgames.cpp
        src/core/PcGameId.cpp    src/core/PcGameId.h
        src/core/PcGameRemap.cpp src/core/PcGameRemap.h)
    target_include_directories(probe_pcgames PRIVATE src src/core)
    # This define does two jobs. The second is why it still exists: PcGameRemap compiles a SYNC COUNTER
    # under it, and the probe asserts on how many flushes a remap performs — nothing about data-dir
    # isolation can supply that. The first, the ini redirect, is now largely redundant: it was there
    # because the alias override store persists, so an un-redirected run read back what the previous run
    # wrote into the ini next to the exe (issue #42) — and the per-process scratch data dir every probe now
    # gets makes each run start empty on its own. It is kept rather than unpicked from two production
    # translation units for no behaviour change; see the note at the call site in probe_pcgames.cpp.
    target_compile_definitions(probe_pcgames PRIVATE EB_PCGAMEID_TEST_SEAM)
    target_link_libraries(probe_pcgames PRIVATE Qt6::Core)

    # Headless test for the one-shot brand migration: per-step flags, idempotence, resumability, and that the
    # ini step never destroys the legacy file before the new one is verified. AppPaths.h is header-only, so
    # there is no AppPaths.cpp to add. run()'s Drive half lives in BrandMigrationDrive.cpp, which pulls
    # CloudSync (and hence Settings + FormFactor + miniz + Network/Gui) — the same lean set probe_onboarding
    # already uses. The probe makes no network call: it clears the cloud credentials first, so isConfigured()
    # is false and the Drive steps stay dormant.
    add_executable(probe_brand tools/probe_brand.cpp
        src/core/BrandMigration.cpp      src/core/BrandMigration.h
        src/core/BrandMigrationDrive.cpp                              # run() + the Drive folder/file steps
        src/core/CloudSync.cpp       src/core/CloudSync.h
        src/core/DriveSyncBackend.cpp src/core/DriveSyncBackend.h      # Increment B: Drive transport behind the SyncBackend seam
        src/core/ServerSyncBackend.cpp src/core/ServerSyncBackend.h # Increment C: self-hosted server transport behind the SyncBackend seam
        src/core/SyncBackend.h                                         # the abstract seam CloudSync composes (Q_OBJECT for AUTOMOC)
        src/core/SettingsTxn.cpp     src/core/SettingsTxn.h           # CloudSync::applySettingsJson closes an open txn (#26)
        src/core/TraktRead.cpp       src/core/TraktRead.h          # (TraktSync's id mapping + time parser)
        src/core/TraktSync.cpp       src/core/TraktSync.h          # backfillKeyPrefix() — SettingsTxn/CloudSync exclude the family
        src/core/PendingPush.cpp     src/core/PendingPush.h           # CloudSync's token refresh classifies via it (#34)
        src/core/Settings.cpp        src/core/Settings.h
        src/theme2/FormFactor.cpp    src/theme2/FormFactor.h          # Settings.cpp references FormFactor::instance()
        src/addons/AddonContext.cpp  src/addons/AddonContext.h        # the REAL per-addon config lookup (see below)
        # The two stores that persist an add-on id INSIDE a value (#58), linked for the same reason
        # AddonContext is: sections 12/13 read the favourite/playlist back through FavoritesStore::list() and
        # PlaylistStore::get() rather than re-parsing the blob themselves, so the assertion runs against the
        # real deserializer and cannot drift from it. Both are QtCore-only (QSettings + JSON on the shared
        # ini). Their own dependencies come with them: every store is namespaced by ProfileStore;
        # FavoritesStore's one-time list() backfill consults RecentStore/DownloadsStore; both stores tombstone
        # on remove; PlaylistStore's v1->v2 migration classifies through MediaCategories.
        src/core/FavoritesStore.cpp  src/core/FavoritesStore.h
        src/core/PlaylistStore.cpp   src/core/PlaylistStore.h
        src/core/RecentStore.cpp     src/core/RecentStore.h
        src/core/DownloadsStore.cpp  src/core/DownloadsStore.h
        src/core/Tombstones.cpp      src/core/Tombstones.h
        src/core/ProfileStore.cpp    src/core/ProfileStore.h
        src/core/MediaCategories.h
        src/core/ShuffleBag.h
        third_party/miniz.c)                                          # (CloudSync's bundle zip; unused by these asserts)
    # AddonContext is linked so the addon-config sections can assert through AddonContext::readConfig itself
    # rather than a re-spelling of its key format. That distinction is the whole point of those sections: the
    # bug is a RENAME, so a check that builds its own key string would be asserting against a copy of the
    # thing under test and would keep passing if the two ever drifted. Its configStore() is pinned to
    # AppPaths::dataDir(), which under EB_ISOLATED_DATA_DIR is this process's own scratch directory — so the
    # real lookup runs against a fixture and never opens an installed ini. ${EB_SECRETS_GEN_DIR} is for the
    # build-tree BuiltinSecrets.h that AddonContext.cpp includes (empty when no secrets file is present).
    target_include_directories(probe_brand PRIVATE src src/core src/addons src/theme2 third_party ${EB_SECRETS_GEN_DIR})
    target_compile_definitions(probe_brand PRIVATE MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
    target_link_libraries(probe_brand PRIVATE Qt6::Core Qt6::Network Qt6::Gui)

    # ExternalPlayer detection/handoff core (VLC/MPC/custom desktop exe, or the Android ACTION_VIEW intent) —
    # the foundation the external-player settings UI + playback routing (player/external-handoff Task 2) wire
    # onto. QtCore-only (QProcess/QFileInfo/QSettings), so the probe links lean and runs under offscreen QPA.
    # Needs Settings.cpp (externalPlayer()/externalPlayerPath() accessors); FormFactor.cpp because Settings.cpp
    # references FormFactor::instance() (virtualPadEnabled).
    add_executable(probe_extplayer tools/probe_extplayer.cpp
        src/core/ExternalPlayer.cpp src/core/ExternalPlayer.h
        src/core/Settings.cpp src/core/Settings.h
        src/theme2/FormFactor.cpp src/theme2/FormFactor.h)
    target_include_directories(probe_extplayer PRIVATE src src/core src/theme2)
    target_link_libraries(probe_extplayer PRIVATE Qt6::Core)

    # Android asset bootstrap core: first-run extraction of the APK-bundled themes2/ + addons/ into the
    # writable data dir (D2 Task 2). QtCore-only (no Quick/Widgets) like FormFactor, so the probe links lean
    # and runs under offscreen QPA in CI. Desktop-verifiable: run() is a pure fn of (sourceRoot, dataDir, ver).
    # ThemeRegistry.cpp: AssetBootstrap asks it where themes2 lives rather than spelling the path a second
    # time (the picker and the extractor scanning different directories is a silent failure).
    add_executable(probe_bootstrap tools/probe_bootstrap.cpp
        src/core/AssetBootstrap.cpp src/core/AssetBootstrap.h
        src/core/ThemeRegistry.cpp src/core/ThemeRegistry.h)
    target_include_directories(probe_bootstrap PRIVATE src src/core)
    target_link_libraries(probe_bootstrap PRIVATE Qt6::Core)

    # Probe data-dir isolation (issue #42): asserts that this process's AppPaths::dataDir() is NOT the exe's
    # folder, that state seeded in the exe's folder is invisible from it, that a write through the probe's own
    # store leaves that folder byte-identical, and — by re-running itself as a child — that two probe
    # PROCESSES get two different directories and that a directory dies with the process that made it.
    # AppPaths.h and AppBrand.h are header-only, so the target is the single .cpp against Qt6::Core.
    add_executable(probe_isolation tools/probe_isolation.cpp
        src/core/AppPaths.h src/core/AppBrand.h)
    target_include_directories(probe_isolation PRIVATE src src/core)
    target_link_libraries(probe_isolation PRIVATE Qt6::Core)

    # The uitest channel's blocking-command lifetime guard (commit 8da19a3). UiTestServer::handle() runs
    # SYNCHRONOUSLY inside the socket's readyRead handler and can nest an event loop (the "/" key opens the
    # OSK, whose getText spins one), so a client that disappears in there frees the server socket under the
    # suspended frame — the QPointer re-check after handle() is what stops the resumed frame writing to freed
    # memory. This drives the real class over a real QLocalSocket: no window, no OSK, just a hook that nests a
    # loop and a client that dies in it. Needs Qt6::Network for QLocalServer/QLocalSocket; Settings.cpp
    # because UiTestServer::wanted() reads the Settings ▸ Debug toggle, and FormFactor.cpp because
    # Settings.cpp references FormFactor::instance() (virtualPadEnabled).
    add_executable(probe_uitest tools/probe_uitest.cpp
        src/core/UiTestServer.cpp src/core/UiTestServer.h
        src/core/Settings.cpp     src/core/Settings.h
        src/theme2/FormFactor.cpp src/theme2/FormFactor.h)
    target_include_directories(probe_uitest PRIVATE src src/core src/theme2)
    target_link_libraries(probe_uitest PRIVATE Qt6::Core Qt6::Network)

    # Console harness for the PDF -> reflow-text reader (needs QtPdf).
    add_executable(probe_pdftext tools/probe_pdftext.cpp src/ebook/PdfTextBook.cpp)
    target_include_directories(probe_pdftext PRIVATE src src/ebook)
    target_link_libraries(probe_pdftext PRIVATE Qt6::Gui Qt6::Pdf)

    # Renders a few paginated pages to PNG to verify the reader paginates by line (no cut text).
    add_executable(probe_pageview tools/probe_pageview.cpp src/ebook/MobiBook.cpp)
    target_include_directories(probe_pageview PRIVATE src src/ebook)
    target_link_libraries(probe_pageview PRIVATE Qt6::Gui)

    # QML theme engine probe: renders a theme dir through ThemeEngine + the qrc-embedded QML (the app path).
    # Use qt_add_resources (not AUTORCC) for the same reasons as the app: it both reliably registers the
    # resource in the exe and re-runs rcc when the listed .qml files change (AUTORCC only tracks the .qrc).
    if(EB_HAVE_QML)
        qt_add_resources(EB_THEME2_RCC_PROBE resources/theme2.qrc)
        add_executable(probe_theme2 tools/probe_theme2.cpp src/theme2/ThemeEngine.cpp
            src/core/ThemeFormFactors.cpp src/core/ThemeFormFactors.h  # ThemeEngine::themeFormFactorFit (#32)
            src/theme2/MpvPreview.cpp src/theme2/MpvPreview.h
            src/theme2/VideoPreviewBridge.cpp src/theme2/VideoPreviewBridge.h  # ThemeEngine registers `videoPreview` + MpvPreview reports audible (#55)
            src/theme2/FormFactor.cpp src/theme2/FormFactor.h  # D1 Task 2: ThemeEngine now registers `form`
            src/core/Settings.cpp src/core/Settings.h
            src/core/SafeAreaInsets.cpp src/core/SafeAreaInsets.h   # ThemeEngine registers `safeArea`
            # ThemeEngine::themesRoot() defers to ThemeRegistry::themesRoot() so the path has one definition.
            src/core/ThemeRegistry.cpp src/core/ThemeRegistry.h
            src/ui/nav/NavGraph.cpp src/ui/nav/NavGraph.h ${EB_THEME2_RCC_PROBE})
        target_include_directories(probe_theme2 PRIVATE src src/theme2 src/ui src/core src/addons ${MPV_INCLUDE_DIR})
        target_link_libraries(probe_theme2 PRIVATE Qt6::Widgets Qt6::Qml Qt6::Quick Qt6::QuickWidgets Qt6::Multimedia ${MPV_LIBRARY})

        # The theme renderer's PURE decisions (issue #29): Theme.js's viewFor/inkFor/tileImage/tileNeedsTitle,
        # evaluated out of the theme2 qrc in a QML JS engine — the same bytes ThemeView.qml imports, with no
        # scene, no widgets and no host. Its own qt_add_resources call (a target_sources-added .qrc silently
        # no-ops — see the app target's note above) so :/theme2/Theme.js is embedded and readable.
        qt_add_resources(EB_THEME2_RCC_THEMEVIEW resources/theme2.qrc)
        add_executable(probe_themeview tools/probe_themeview.cpp ${EB_THEME2_RCC_THEMEVIEW})
        # Qt6::Quick, for a probe with no scene in it: inkFor resolves a background colour through Qt.color,
        # and the colour provider behind Qt.color is installed by the QtQuick module, not by QtQml. Linked
        # against Core+Qml alone the call throws for EVERY input and inkFor answers "white" to everything —
        # the light-background assertions then fail loudly, which is how this was found, but the point is
        # that the probe must run Theme.js in the environment the app gives it. See probe_themeview.cpp.
        #
        # Declared inside if(EB_HAVE_QML) yet named UNCONDITIONALLY in ci.yml's build-probes line, so on a Qt
        # install without qtdeclarative the BUILD step fails loudly instead of the suite quietly skipping.
        # That is deliberate: probe_navqml §22 is the only end-to-end cover for issue #29, and a §22 that
        # silently does not run is worse than a build that stops and says qtdeclarative is missing.
        target_link_libraries(probe_themeview PRIVATE Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick)

        # Verifies MpvPreview (libmpv software-render) decodes + paints real frames on the software backend,
        # using mpv's built-in lavfi test source so no video file/key is needed.
        add_executable(probe_mpvpreview tools/probe_mpvpreview.cpp
            src/theme2/MpvPreview.cpp src/theme2/MpvPreview.h
            src/theme2/VideoPreviewBridge.cpp src/theme2/VideoPreviewBridge.h)  # MpvPreview reports audible state to it (#55)
        target_include_directories(probe_mpvpreview PRIVATE src src/theme2 ${MPV_INCLUDE_DIR})
        target_link_libraries(probe_mpvpreview PRIVATE Qt6::Widgets Qt6::Qml Qt6::Quick ${MPV_LIBRARY})
    endif()

    # Headless tests for the offline metadata cache: merge-preserves-unknown-keys (future metadata),
    # item/detail round-trips, artwork path resolution, uninstall cleanup — PLUS the per-item override layer
    # (issue #24): the record's canonical spelling, the override-beats-scraped composite, reset-to-scraped, and
    # the fact that the three MetaCache read primitives all composite it. MetaOverrides is QtCore-only
    # (QSettings over the shared ini + header-only AppPaths/AppBrand), so it adds no link dependency here.
    add_executable(probe_meta tools/probe_meta.cpp src/core/MetaCache.cpp src/core/MetaOverrides.cpp
        src/core/MetaCache.h src/core/MetaOverrides.h
        src/core/ScrapedSnapshot.h   # the metadata editor's keyed baseline (header-only, pure)
        src/addons/AddonModels.cpp src/addons/AddonModels.h)
    target_include_directories(probe_meta PRIVATE src src/core src/addons)
    target_link_libraries(probe_meta PRIVATE Qt6::Core Qt6::Network)

    # Attract mode (idle screensaver, issue #54): the pure AttractController — art selection/rotation, the
    # idle-fire gating, and the enter->input->restore round-trip. QtCore-only (AddonModels for MediaArt), no
    # window/player, so it runs as a plain no-argument probe.
    add_executable(probe_attract tools/probe_attract.cpp
        src/core/AttractController.cpp src/core/AttractController.h
        src/addons/AddonModels.cpp src/addons/AddonModels.h)
    target_include_directories(probe_attract PRIVATE src src/core src/addons)
    target_link_libraries(probe_attract PRIVATE Qt6::Core)

    # Miximage compositor (src/core/Miximage, issue #90): composites a game card from cached art roles, and
    # publishes it as the "miximage" role via MetaCache. Links Qt6::Gui for QImage/QPainter; QCoreApplication
    # is enough (raster paint needs no platform plugin), so this runs as a plain no-argument probe.
    add_executable(probe_miximage tools/probe_miximage.cpp
        src/core/Miximage.cpp src/core/Miximage.h
        src/core/MetaCache.cpp src/core/MetaOverrides.cpp src/addons/AddonModels.cpp)
    target_include_directories(probe_miximage PRIVATE src src/core src/addons)
    target_link_libraries(probe_miximage PRIVATE Qt6::Core Qt6::Network Qt6::Gui)

    # EmulationStation / RetroBat gamelist.xml reader (parse + ES-role mapping + local media resolve).
    add_executable(probe_gamelist tools/probe_gamelist.cpp
        src/core/GamelistStore.cpp src/core/GamelistStore.h src/addons/AddonModels.cpp)
    target_include_directories(probe_gamelist PRIVATE src src/core src/addons)
    target_link_libraries(probe_gamelist PRIVATE Qt6::Core Qt6::Network)

    # Headless test for the synthetic browse-catalog builders (Recent/Downloaded/Favorites).
    add_executable(probe_browse tools/probe_browse.cpp
        src/browse/SyntheticCatalogs.cpp src/browse/SyntheticCatalogs.h
        src/core/PcGameId.cpp src/core/PcGameId.h   # SyntheticCatalogs -> pcGamesCatalog's merge key
        # The catalog builder and the record remap must agree on ONE item id. probe_browse is the only
        # probe that can build both sides, so it links the remap purely to assert they do (remapTable is
        # pure; applyRemap, which touches the ini, is never called from here).
        src/core/PcGameRemap.cpp src/core/PcGameRemap.h
        src/core/TraktRead.cpp src/core/TraktRead.h   # SyntheticCatalogs -> trakt::imdbStreamIdFor (#23)
        src/core/TraktSync.cpp src/core/TraktSync.h   # SyntheticCatalogs -> traktListCatalog's TraktListEntry (#23)
        # #25: SyntheticCatalogs needs only the MissedRow TYPE, but probe_browse links the RULE — it is the
        # one probe that can build both Trakt calendar surfaces, so it is the only place the claim that
        # they PARTITION the calendar can be stated at all.
        src/core/TraktMissed.cpp src/core/TraktMissed.h
        src/core/LocalLibrary.cpp src/core/LocalLibrary.h     # SyntheticCatalogs -> LocalLibrary::displayTitle
        src/core/Settings.cpp src/core/Settings.h             # LocalLibrary::root() -> Settings::libraryFolder()
        src/theme2/FormFactor.cpp src/theme2/FormFactor.h     # Settings.cpp -> FormFactor::instance()
        src/core/MetaCache.cpp src/core/MetaOverrides.cpp src/core/SteamLibrary.cpp src/addons/AddonModels.cpp
        src/core/PhotoLibrary.cpp src/core/PhotoLibrary.h)  # SyntheticCatalogs -> photosCatalog's groupByFolder (#102)
    target_include_directories(probe_browse PRIVATE src src/core src/addons src/browse)
    target_link_libraries(probe_browse PRIVATE Qt6::Core Qt6::Network Qt6::Gui)

    # Headless test for the Live TV / IPTV persistent-sources feature (#75 increment 2): IptvSourceStore's
    # per-profile round-trip (add/update/remove, epgUrl preserved) + the two pure synthetic builders
    # (liveTvSourcesCatalog / liveTvChannelsCatalog). Mirrors probe_browse's SyntheticCatalogs dependency set
    # (the builder TU is the same), plus IptvSourceStore.cpp for the store. The builders reference only the
    # IptvSource / M3uEntry / FavoriteItem TYPES (header-only), so no StreamResolver.cpp / FavoritesStore.cpp
    # link is pulled in — the probe hand-builds M3uEntry/FavoriteItem fixtures rather than parsing.
    add_executable(probe_iptv tools/probe_iptv.cpp
        src/core/IptvSourceStore.cpp src/core/IptvSourceStore.h
        src/core/ProfileStore.cpp src/core/ProfileStore.h        # IptvSourceStore keys per active profile
        src/browse/SyntheticCatalogs.cpp src/browse/SyntheticCatalogs.h
        src/core/PcGameId.cpp src/core/PcGameId.h   # SyntheticCatalogs -> pcGamesCatalog's merge key
        src/core/TraktRead.cpp src/core/TraktRead.h   # SyntheticCatalogs -> trakt::imdbStreamIdFor (#23)
        src/core/TraktSync.cpp src/core/TraktSync.h   # SyntheticCatalogs -> traktListCatalog's TraktListEntry (#23)
        src/core/TraktMissed.cpp src/core/TraktMissed.h   # SyntheticCatalogs -> traktMissedCatalog's MissedRow
        src/core/LocalLibrary.cpp src/core/LocalLibrary.h     # SyntheticCatalogs -> LocalLibrary::displayTitle
        src/core/Settings.cpp src/core/Settings.h             # LocalLibrary::root() -> Settings::libraryFolder()
        src/theme2/FormFactor.cpp src/theme2/FormFactor.h     # Settings.cpp -> FormFactor::instance()
        src/core/MetaCache.cpp src/core/MetaOverrides.cpp src/core/SteamLibrary.cpp src/addons/AddonModels.cpp
        src/core/PhotoLibrary.cpp src/core/PhotoLibrary.h)  # SyntheticCatalogs -> photosCatalog's groupByFolder (#102)
    target_include_directories(probe_iptv PRIVATE src src/core src/addons src/browse src/media)
    target_link_libraries(probe_iptv PRIVATE Qt6::Core Qt6::Network Qt6::Gui)

    # Headless test for the XMLTV EPG heart (#75 inc 3): the pure parser/now-next/gunzip (XmltvGuide) + the
    # #EXTM3U url-tvg header parse (StreamResolver) + the display builders (LiveTvGuide). Deliberately does NOT
    # pull SyntheticCatalogs.cpp's dependency chain — the subtitle-override wiring in liveTvChannelsCatalog is
    # asserted in probe_iptv, where that TU is already fully linked. miniz.c backs XmltvGuide::gunzip.
    add_executable(probe_xmltv tools/probe_xmltv.cpp
        src/core/XmltvGuide.cpp src/core/XmltvGuide.h
        src/browse/LiveTvGuide.cpp src/browse/LiveTvGuide.h        # grid + now/next builders over the model
        src/addons/AddonModels.cpp src/addons/AddonModels.h        # LiveTvGuide -> MediaCatalog / MediaItem
        src/media/StreamResolver.cpp src/media/StreamResolver.h    # m3uHeaderTvgUrl + M3uEntry
        src/core/StreamHeaders.cpp                                 # StreamResolver applies proxyHeaders on fetch
        src/core/NetHeaderApply.h                                  # (header-only; named so it is visible here)
        third_party/miniz.c)                                       # XmltvGuide::gunzip -> tinfl_decompress
    target_include_directories(probe_xmltv PRIVATE src src/core src/addons src/browse src/media third_party)
    target_link_libraries(probe_xmltv PRIVATE Qt6::Core Qt6::Network Qt6::Gui)

    # Headless test for the local video library core (parse + NFO + scan + OwnedIndex).
    add_executable(probe_locallib tools/probe_locallib.cpp
        src/core/LocalLibrary.cpp src/core/LocalLibrary.h
        src/browse/SyntheticCatalogs.cpp src/browse/SyntheticCatalogs.h
        src/core/PcGameId.cpp src/core/PcGameId.h   # SyntheticCatalogs -> pcGamesCatalog's merge key
        src/core/TraktRead.cpp src/core/TraktRead.h   # SyntheticCatalogs -> trakt::imdbStreamIdFor (#23)
        src/core/TraktSync.cpp src/core/TraktSync.h   # SyntheticCatalogs -> traktListCatalog's TraktListEntry (#23)
        src/core/MetaCache.cpp src/core/MetaOverrides.cpp src/core/SteamLibrary.cpp src/addons/AddonModels.cpp
        src/core/Settings.cpp src/core/Settings.h                # LocalLibrary::root() -> Settings::libraryFolder()
        src/theme2/FormFactor.cpp src/theme2/FormFactor.h       # Settings.cpp's virtualPadEnabled() -> FormFactor::instance()
        src/core/PhotoLibrary.cpp src/core/PhotoLibrary.h)      # SyntheticCatalogs -> photosCatalog's groupByFolder (#102)
    target_include_directories(probe_locallib PRIVATE src src/core src/addons src/browse)
    target_link_libraries(probe_locallib PRIVATE Qt6::Core Qt6::Network Qt6::Gui)

    # Headless test for the local photo library core (issue #102): PhotoLibrary's isPhotoFile predicate, the
    # recursive image-only scan, natural (numeric-aware) order, non-recursive per-folder listing, and folder
    # grouping. PURE (QtCore only); Settings.cpp/FormFactor.cpp are linked for PhotoLibrary::root() ->
    # Settings::photosFolder() (as probe_locallib does for libraryFolder), though the probe never calls root().
    add_executable(probe_photos tools/probe_photos.cpp
        src/core/PhotoLibrary.cpp src/core/PhotoLibrary.h
        src/core/Settings.cpp     src/core/Settings.h
        src/theme2/FormFactor.cpp src/theme2/FormFactor.h)
    target_include_directories(probe_photos PRIVATE src src/core src/theme2)
    target_link_libraries(probe_photos PRIVATE Qt6::Core Qt6::Gui)

    # Headless test for the subtitle accuracy cores (OSDb hash + match chain + cache identifier + download
    # cache). SubtitleFetcher is linked in for its two PURE statics (buildQueries/cacheIdentifier); that pulls
    # QtNetwork (QNetworkAccessManager) and Settings (the API credentials) -> FormFactor, as probe_locallib does.
    add_executable(probe_subs tools/probe_subs.cpp
        src/core/SubtitleHash.cpp src/core/SubtitleHash.h
        src/core/SubtitleCache.cpp src/core/SubtitleCache.h
        src/core/SubtitleFetcher.cpp src/core/SubtitleFetcher.h
        src/core/Settings.cpp src/core/Settings.h
        src/theme2/FormFactor.cpp src/theme2/FormFactor.h)
    target_include_directories(probe_subs PRIVATE src src/core src/theme2)
    target_link_libraries(probe_subs PRIVATE Qt6::Core Qt6::Network Qt6::Gui)

    # Headless test for the intro/credits segment core: the three providers (.edl / chapters / learned),
    # per-type precedence, series-key derivation, and the tracker. Pure — no mpv, no video file.
    add_executable(probe_segments tools/probe_segments.cpp
        src/core/MediaSegments.cpp src/core/MediaSegments.h
        src/core/SegmentStore.cpp  src/core/SegmentStore.h
        src/core/LocalLibrary.cpp  src/core/LocalLibrary.h
        src/core/Settings.cpp      src/core/Settings.h
        src/theme2/FormFactor.cpp  src/theme2/FormFactor.h)
    target_include_directories(probe_segments PRIVATE src src/core src/theme2)
    target_link_libraries(probe_segments PRIVATE Qt6::Core Qt6::Gui)

    # The listening kit (issue #140): the pure sleep-timer decision (SleepTimer.h — expiry/fade/nudge) and the
    # per-item speed store + resolve (SpeedStore). SleepTimer includes MediaSegments.h for its Chapter struct
    # but calls none of its functions, so no MediaSegments.cpp is needed here; SpeedStore is QtCore-only.
    add_executable(probe_listening tools/probe_listening.cpp
        src/core/SpeedStore.cpp src/core/SpeedStore.h
        src/media/SleepTimer.h)
    target_include_directories(probe_listening PRIVATE src src/core src/media)
    target_link_libraries(probe_listening PRIVATE Qt6::Core)

    # Synced lyrics (issue #142, source 1 — the LRC sidecar): the pure parser + current-line lookup
    # (LrcLyrics.h — parseLrc/lineIndexAtTime). Header-only, QtCore-only, so no .cpp comes along.
    add_executable(probe_lyrics tools/probe_lyrics.cpp
        src/media/LrcLyrics.h)
    target_include_directories(probe_lyrics PRIVATE src src/media)
    target_link_libraries(probe_lyrics PRIVATE Qt6::Core)

    # Headless test for the Stremio protocol translator: manifest/extras/idPrefixes/stream parsing and the
    # catalog classification — plus everything the app does with a stream's proxyHeaders, which is all in one
    # place on purpose. No addon is installed: the QtNetwork half (#59) mostly builds requests and a download
    # queue without running an event loop. Its last section is the exception — two loopback servers on
    # EPHEMERAL ports put a real 302 in front of a real DownloadManager, because the redirect decision and
    # the failure reason it produces cannot be reached from an un-sent request. DownloadManager comes along
    # because it is the one header consumer that PERSISTS, and "the values are not written to disk" is the
    # rule with teeth.
    add_executable(probe_stremio tools/probe_stremio.cpp
        src/addons/StremioTranslate.cpp src/addons/StremioTranslate.h
        src/core/BingeStore.cpp src/core/BingeStore.h
        src/core/DownloadManager.cpp src/core/DownloadManager.h
        src/core/StreamHeaders.cpp src/core/StreamHeaders.h
        src/core/NetHeaderApply.h)   # header-only, but this is one of the four targets that compile it
    target_include_directories(probe_stremio PRIVATE src src/addons src/core)
    target_link_libraries(probe_stremio PRIVATE Qt6::Core Qt6::Network)

    # Headless test for the save-sync decision table (pure) AND for the transport's conflict ordering and
    # baseline rules, which are driven through SaveSync's virtual Drive seam against an in-memory cloud and
    # a scratch dir under TEMP — real plan/execute/baseline code, no network. CloudSync is linked because
    # SaveSync's default seam implementations call its primitives (miniz comes along for CloudSync's bundle
    # zip, which nothing here touches).
    add_executable(probe_savesync tools/probe_savesync.cpp
        src/core/SaveSyncPlan.cpp src/core/SaveSyncPlan.h
        src/core/SaveSync.cpp     src/core/SaveSync.h
        src/core/SaveMeta.cpp     src/core/SaveMeta.h
        src/core/Tombstones.cpp   src/core/Tombstones.h
        src/core/CloudSync.cpp    src/core/CloudSync.h
        src/core/DriveSyncBackend.cpp src/core/DriveSyncBackend.h  # Increment B: Drive transport behind the SyncBackend seam
        src/core/ServerSyncBackend.cpp src/core/ServerSyncBackend.h # Increment C: self-hosted server transport behind the SyncBackend seam
        src/core/SyncBackend.h                                     # the abstract seam CloudSync composes (Q_OBJECT for AUTOMOC)
        src/core/SettingsTxn.cpp  src/core/SettingsTxn.h  # CloudSync::applySettingsJson closes an open txn (#26)
        src/core/TraktRead.cpp       src/core/TraktRead.h          # (TraktSync's id mapping + time parser)
        src/core/TraktSync.cpp       src/core/TraktSync.h          # backfillKeyPrefix() — SettingsTxn/CloudSync exclude the family
        src/core/PendingPush.cpp  src/core/PendingPush.h  # CloudSync's token refresh classifies via it (#34)
        src/core/BrandMigration.cpp src/core/BrandMigration.h  # CloudSync/AddonManager consult its flags (QtCore-only TU)
        src/core/Settings.cpp     src/core/Settings.h
        src/theme2/FormFactor.cpp src/theme2/FormFactor.h
        third_party/miniz.c)
    target_include_directories(probe_savesync PRIVATE src src/core src/theme2 third_party)
    target_compile_definitions(probe_savesync PRIVATE MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
    target_link_libraries(probe_savesync PRIVATE Qt6::Core Qt6::Network Qt6::Gui) # Qt6::Gui: CloudSync's QDesktopServices

    # ServerSyncBackend (Increment C) driven end-to-end against an in-process QTcpServer stub of the object store
    # — no network beyond loopback. Links ONLY the backend and its config deps; CloudSync is deliberately absent,
    # so this is the transport in isolation. Qt6::Network only — no QDesktopServices, no bundle zip.
    add_executable(probe_serversync tools/probe_serversync.cpp
        src/core/ServerSyncBackend.cpp src/core/ServerSyncBackend.h
        src/core/SyncBackend.h                                     # the abstract seam (Q_OBJECT for AUTOMOC)
        src/core/PendingPush.cpp  src/core/PendingPush.h)          # ServerSyncBackend::lastAuth returns a PendingPush::Auth
    target_include_directories(probe_serversync PRIVATE src src/core src/theme2 third_party)
    target_link_libraries(probe_serversync PRIVATE Qt6::Core Qt6::Network)

    # Headless test for the pure save-state slot logic (#93): the reserved save-on-exit slot is never a user
    # slot and the auto-increment search never selects it; the ROM-mtime/size resume-invalidation gate; and the
    # grid pagination arithmetic. StateSlots.h is header-only and QtCore-free, so no Qt link is needed.
    add_executable(probe_stateslots tools/probe_stateslots.cpp
        src/emu/StateSlots.h)
    target_include_directories(probe_stateslots PRIVATE src src/emu)

    # Headless test for the pure bezel decoration units (#106): selection precedence, info-file->viewport
    # parse, and viewport->widget mapping. BezelSelect.h is header-only and Qt-free, so no Qt link.
    add_executable(probe_bezel tools/probe_bezel.cpp
        src/emu/BezelSelect.h)
    target_include_directories(probe_bezel PRIVATE src src/emu)

    # Headless test for the pure cheat-search engine (#96): width/endian/signed/bounds-safe readValue, the
    # exact-value initial scan, the four relational narrowing filters, and successive-step intersection.
    # CheatSearch.h is header-only, QtCore-free and does no I/O, so no Qt link is needed.
    add_executable(probe_cheatsearch tools/probe_cheatsearch.cpp
        src/emu/CheatSearch.h)
    target_include_directories(probe_cheatsearch PRIVATE src src/emu)

    # Headless test for the pure remote-control API core (#76): parseRequest's request-line/query/body split +
    # Content-Length handling, route's whole decision table (/state, /player actions, /input directions,
    # NotFound / BadRequest), stateJson round-trip, and httpResponse framing. RemoteApi.cpp is QtCore-only (it
    # builds request/response bytes and JSON with QByteArray/QJsonDocument), so it links Qt6::Core and nothing
    # heavier — no QTcpServer, no player, no window.
    add_executable(probe_remoteapi tools/probe_remoteapi.cpp
        src/core/RemoteApi.cpp src/core/RemoteApi.h)
    target_include_directories(probe_remoteapi PRIVATE src src/core)
    target_link_libraries(probe_remoteapi PRIVATE Qt6::Core)

    # Headless test for the catalog id-resolver core (pure matcher + persisted cache) AND the pure .nfo>scraped
    # metadata merge for a resolved local file (#73), composed with MetaOverrides to pin override>.nfo>scraped.
    add_executable(probe_resolver tools/probe_resolver.cpp
        src/core/CatalogMatch.cpp src/core/CatalogMatch.h
        src/core/LocalResolveCache.cpp src/core/LocalResolveCache.h
        src/core/LocalLibrary.cpp src/core/LocalLibrary.h
        src/core/LocalMetaMerge.cpp src/core/LocalMetaMerge.h        # #73: pure .nfo>scraped merge
        src/core/MetaOverrides.cpp src/core/MetaOverrides.h          # #73: override>.nfo>scraped composition (QtCore-only)
        src/core/Settings.cpp src/core/Settings.h
        src/theme2/FormFactor.cpp src/theme2/FormFactor.h
        src/addons/AddonModels.cpp)
    target_include_directories(probe_resolver PRIVATE src src/core src/addons src/theme2)
    target_link_libraries(probe_resolver PRIVATE Qt6::Core Qt6::Network Qt6::Gui)

    # Headless test for the game-importers Task 1 (Steam gap-closure) pure logic: owned-games JSON parse +
    # TTL semantics (SteamLibrary), the owned-not-installed console append (SyntheticCatalogs), and the
    # Recent-kind dispatch table (RecentStore::relaunchFor) the app's openRecent switch mirrors. No network /
    # registry / Steam install needed — all in-memory fixtures. RecentStore pulls ProfileStore (profile-namespaced
    # recents) + MetaCache (SyntheticCatalogs' offline-art resolve) + AddonModels (MediaItem/Catalog).
    add_executable(probe_importers tools/probe_importers.cpp
        src/core/SteamLibrary.cpp src/core/SteamLibrary.h
        src/core/EpicLibrary.cpp src/core/EpicLibrary.h
        src/core/GogLibrary.cpp src/core/GogLibrary.h
        src/core/BattleNetLibrary.cpp src/core/BattleNetLibrary.h
        src/browse/SyntheticCatalogs.cpp src/browse/SyntheticCatalogs.h
        src/core/PcGameId.cpp src/core/PcGameId.h   # SyntheticCatalogs -> pcGamesCatalog's merge key
        src/core/TraktRead.cpp src/core/TraktRead.h   # SyntheticCatalogs -> trakt::imdbStreamIdFor (#23)
        src/core/TraktSync.cpp src/core/TraktSync.h   # SyntheticCatalogs -> traktListCatalog's TraktListEntry (#23)
        src/core/LocalLibrary.cpp src/core/LocalLibrary.h     # SyntheticCatalogs -> LocalLibrary::displayTitle
        src/core/Settings.cpp src/core/Settings.h             # LocalLibrary::root() -> Settings::libraryFolder()
        src/theme2/FormFactor.cpp src/theme2/FormFactor.h     # Settings.cpp -> FormFactor::instance()
        src/core/MetaCache.cpp src/core/MetaOverrides.cpp src/core/RecentStore.cpp
        src/core/Tombstones.cpp src/core/Tombstones.h   # RecentStore::remove/clear date the removal (#150)
        src/core/ProfileStore.cpp src/addons/AddonModels.cpp
        src/core/PhotoLibrary.cpp src/core/PhotoLibrary.h)  # SyntheticCatalogs -> photosCatalog's groupByFolder (#102)
    target_include_directories(probe_importers PRIVATE src src/core src/addons src/browse src/theme2)
    target_link_libraries(probe_importers PRIVATE Qt6::Core Qt6::Network Qt6::Gui)

    # Headless test for the PerfTrace span harness (gating, format, overhead budget) plus the
    # component-budget asserts (browse builders, m3u parse, search dedup) — sources mirror
    # probe_browse's builder/search deps + StreamResolver for parseM3u.
    add_executable(probe_perf tools/probe_perf.cpp
        src/core/PerfTrace.cpp src/core/PerfTrace.h
        src/browse/SyntheticCatalogs.cpp src/browse/SyntheticCatalogs.h
        src/core/PcGameId.cpp src/core/PcGameId.h   # SyntheticCatalogs -> pcGamesCatalog's merge key
        src/core/TraktRead.cpp src/core/TraktRead.h   # SyntheticCatalogs -> trakt::imdbStreamIdFor (#23)
        src/core/TraktSync.cpp src/core/TraktSync.h   # SyntheticCatalogs -> traktListCatalog's TraktListEntry (#23)
        src/core/LocalLibrary.cpp src/core/LocalLibrary.h     # SyntheticCatalogs -> LocalLibrary::displayTitle
        src/core/Settings.cpp src/core/Settings.h             # LocalLibrary::root() -> Settings::libraryFolder()
        src/theme2/FormFactor.cpp src/theme2/FormFactor.h     # Settings.cpp's virtualPadEnabled() -> FormFactor::instance()
        src/core/MetaCache.cpp src/core/MetaOverrides.cpp src/core/SteamLibrary.cpp src/addons/AddonModels.cpp
        src/media/StreamResolver.cpp src/media/StreamResolver.h
        src/core/StreamHeaders.cpp    # StreamResolver applies a source's proxyHeaders to its playlist fetch
        src/core/NetHeaderApply.h    # …through this, header-only and listed so it is visible here too
        src/core/PhotoLibrary.cpp src/core/PhotoLibrary.h)  # SyntheticCatalogs -> photosCatalog's groupByFolder (#102)
    target_include_directories(probe_perf PRIVATE src src/core src/addons src/browse src/media)
    target_link_libraries(probe_perf PRIVATE Qt6::Core Qt6::Network Qt6::Gui)

    # Headless tests for the controller-navigation kit (rings, overlays, on-screen keyboard, back routing).
    # Runs under the offscreen QPA in CI so every push proves the "selection can never be lost" invariants.
    add_executable(probe_nav tools/probe_nav.cpp
        src/ui/nav/Nav.cpp src/ui/nav/Nav.h
        src/ui/nav/NavGraph.cpp src/ui/nav/NavGraph.h
        src/ui/nav/NavOverlay.cpp src/ui/nav/NavOverlay.h
        src/ui/nav/Osk.cpp src/ui/nav/Osk.h
        # The 4-digit passcode pad (#30) is a NavOverlay like the OSK, so its D-pad/auto-submit/back
        # behaviour belongs in the nav suite. It needs no ProfilePasscode.cpp: the only thing it takes from
        # that unit is the constexpr kLength, which is header-only.
        src/ui/nav/PasscodePad.cpp src/ui/nav/PasscodePad.h
        # The classic Edit-Profile picker page is asserted against the REAL widget (§24), not a replica: the
        # defect it covers was a GEOMETRY one, and a replica would drift from the layout it is meant to guard.
        src/ui/ProfileDialog.cpp src/ui/ProfileDialog.h
        src/core/ProfileStore.cpp src/core/ProfileStore.h)
    target_include_directories(probe_nav PRIVATE src src/ui src/core)
    target_link_libraries(probe_nav PRIVATE Qt6::Widgets)

    # Headless test for NavGraph (the pure selection model + back stack — no QML, no widgets). Proves the
    # "selection is never null" churn invariants, divider snapping, spatial move() reachability, and the
    # LIFO back stack under the offscreen QPA. When QML is available it ALSO instantiates the themed input
    # components (ThemedTextField/ThemedChoice) in an offscreen QQuickWidget with a REAL NavGraph as the `nav`
    # context property, and asserts the two-state contract (register / select / edit / commit / cancel).
    add_executable(probe_navqml tools/probe_navqml.cpp
        src/ui/nav/NavGraph.cpp src/ui/nav/NavGraph.h
        src/ui/BlackFrameWatchdog.cpp src/ui/BlackFrameWatchdog.h)
    target_include_directories(probe_navqml PRIVATE src src/ui src/theme2 src/core)
    target_link_libraries(probe_navqml PRIVATE Qt6::Core Qt6::Gui)
    if(EB_HAVE_QML)
        # Compile theme2.qrc explicitly for this probe too (a target_sources-added .qrc silently no-ops — see
        # the app target's note above), so qrc:/theme2/elements/ThemedTextField.qml is embedded and loadable.
        qt_add_resources(EB_THEME2_RCC_NAVQML resources/theme2.qrc)
        target_sources(probe_navqml PRIVATE ${EB_THEME2_RCC_NAVQML})
        # §18(e) drives the REAL ThemedPanelHost (not the bare NavGraph) to pin the pop-restore clamp hazard at
        # host level. That drags in the panel host + the nav-kit closure its OSK edit path references
        # (Osk/NavOverlay/Nav) — the exact set probe_nav already links with only Qt6::Widgets, so no MainWindow.
        target_sources(probe_navqml PRIVATE
            src/theme2/ThemedPanelHost.cpp src/theme2/ThemedPanelHost.h
            src/core/SafeAreaInsets.cpp src/core/SafeAreaInsets.h   # the panel host registers `safeArea`
            src/ui/nav/Nav.cpp        src/ui/nav/Nav.h
            src/ui/nav/NavOverlay.cpp src/ui/nav/NavOverlay.h
            src/ui/nav/Osk.cpp        src/ui/nav/Osk.h
            # §19 (D1 Task 2) drives the REAL FormFactor singleton (which reads the display/mode setting) so the
            # `form` context property + the TV scale/insets on the ThemeView surface are pinned end to end.
            src/theme2/FormFactor.cpp src/theme2/FormFactor.h
            src/core/Settings.cpp     src/core/Settings.h
            # §21 (issue #38) drives the REAL ThemeEngine::buildView on a scratch theme.json, so the sidebar
            # assertions ride the whole shipped path — theme parse, graph-shape choice, ThemeBridge write-back
            # and the qrc QML — instead of a probe-local replica of any of it.
            src/theme2/ThemeEngine.cpp src/theme2/ThemeEngine.h
            src/theme2/VideoPreviewBridge.cpp src/theme2/VideoPreviewBridge.h  # ThemeEngine registers `videoPreview` (#55)
            src/core/ThemeFormFactors.cpp src/core/ThemeFormFactors.h  # ThemeEngine::themeFormFactorFit (#32)
            # ThemeEngine::themesRoot() defers to ThemeRegistry::themesRoot() so the path has one definition.
            src/core/ThemeRegistry.cpp src/core/ThemeRegistry.h)
        # Qt6::Test supplies QTest::touchEvent / createTouchDevice for §20 (synthetic touch with real hit-testing).
        # Qt6::Multimedia is ThemeEngine's QSoundEffect (a theme's optional UI sounds; §21's fixture declares none).
        target_link_libraries(probe_navqml PRIVATE Qt6::Widgets Qt6::Qml Qt6::Quick Qt6::QuickWidgets Qt6::Test
                                                   Qt6::Multimedia)
        target_compile_definitions(probe_navqml PRIVATE EB_HAVE_QML)
    endif()

    # Headless test for the Notifier overlay (the single user-feedback channel).
    add_executable(probe_notifier tools/probe_notifier.cpp
        src/ui/Notifier.cpp src/ui/Notifier.h)
    target_include_directories(probe_notifier PRIVATE src src/ui)
    target_link_libraries(probe_notifier PRIVATE Qt6::Widgets)

    # Headless test for StreamResolver's m3u classification (HLS vs IPTV vs multi-disc).
    add_executable(probe_m3u tools/probe_m3u.cpp
        src/media/StreamResolver.cpp src/media/StreamResolver.h
        src/core/StreamHeaders.cpp    # StreamResolver applies a source's proxyHeaders to its playlist fetch
        src/core/NetHeaderApply.h)   # …through this, header-only and listed so it is visible here too
    target_include_directories(probe_m3u PRIVATE src src/media)
    target_link_libraries(probe_m3u PRIVATE Qt6::Core Qt6::Network)

    # Headless test for DiscGroup (#49): multi-disc detection/ordering/grouping + the generated .m3u body.
    # DiscGroup.h is header-only (pure QtCore functions), so this probe compiles just the header + Qt6::Core.
    add_executable(probe_discgroup tools/probe_discgroup.cpp
        src/core/DiscGroup.h)
    target_include_directories(probe_discgroup PRIVATE src src/core)
    target_link_libraries(probe_discgroup PRIVATE Qt6::Core)

    # Headless test for RegionCollapse (#50): region ranking, revision tie-break, same-title grouping and the
    # winner/loser pick. RegionCollapse.h is header-only (reuses DiscGroup.h), so this links just Qt6::Core.
    add_executable(probe_regioncollapse tools/probe_regioncollapse.cpp
        src/core/RegionCollapse.h src/core/DiscGroup.h)
    target_include_directories(probe_regioncollapse PRIVATE src src/core)
    target_link_libraries(probe_regioncollapse PRIVATE Qt6::Core)

    # Headless test for platform-aware ROM routing (#53): the pure junk filter + folder-authoritative accept
    # decision (RomRouting.h), and that PS2/PSP/Dreamcast/Xbox declaring their disc extensions does NOT regress
    # the loose-file extension router (SystemCatalog.h). Both header-only + QtCore, so this links just Qt6::Core.
    add_executable(probe_romrouting tools/probe_romrouting.cpp
        src/core/RomRouting.h src/core/SystemCatalog.h)
    target_include_directories(probe_romrouting PRIVATE src src/core)
    target_link_libraries(probe_romrouting PRIVATE Qt6::Core)

    # Headless test for BulkSelect (the multi-select set + collision-safe reassign target path, issue #65).
    # Header-only unit; links nothing but Qt6::Core.
    add_executable(probe_bulkselect tools/probe_bulkselect.cpp
        src/core/BulkSelect.h)
    target_include_directories(probe_bulkselect PRIVATE src src/core)
    target_link_libraries(probe_bulkselect PRIVATE Qt6::Core)

    # Headless test for PlaybackSession (queue advance + resume persistence).
    add_executable(probe_playback tools/probe_playback.cpp
        src/media/PlaybackSession.cpp src/media/PlaybackSession.h
        # finishResume clears through ResumeStore, which dates the clear in the tombstone store (#150).
        src/core/ResumeStore.cpp      src/core/ResumeStore.h
        src/core/Tombstones.cpp       src/core/Tombstones.h
        src/core/ConsumptionStats.cpp src/core/ConsumptionStats.h
        # ConsumptionStats' device-namespaced accrual (mdsync T3) needs Settings::deviceId(), which drags
        # FormFactor via Settings.cpp's virtualPadEnabled() — mirror probe_stats' link set.
        src/core/Settings.cpp     src/core/Settings.h
        src/theme2/FormFactor.cpp src/theme2/FormFactor.h
        src/core/ProfileStore.cpp src/core/ProfileStore.h)
    target_include_directories(probe_playback PRIVATE src src/media src/core src/theme2)
    target_link_libraries(probe_playback PRIVATE Qt6::Core)

    # Console harness for the QtPdf renderer - round-trips a generated PDF through PDFium.
    add_executable(probe_pdf tools/probe_pdf.cpp)
    target_link_libraries(probe_pdf PRIVATE Qt6::Gui Qt6::Pdf)

    # Console harness: load an image (incl. SVG) into a QPixmap - verifies the bundled console tiles render.
    add_executable(probe_image tools/probe_image.cpp)
    target_link_libraries(probe_image PRIVATE Qt6::Gui)

    # Console harness for libmpv audio decoding (no Qt) - verifies the "now playing" metadata path.
    add_executable(probe_audio tools/probe_audio.cpp)
    target_include_directories(probe_audio PRIVATE ${MPV_INCLUDE_DIR})
    target_link_libraries(probe_audio PRIVATE ${MPV_LIBRARY})

    # Console harness for the addon JS engine (Duktape) + manager discovery/resolution.
    add_executable(probe_addon tools/probe_addon.cpp
        src/addons/JsAddon.cpp src/addons/AddonContext.cpp src/addons/AddonModels.cpp
        src/addons/AddonManager.cpp src/addons/CatalogPrefetcher.cpp
        src/core/BrandMigration.cpp src/core/BrandMigration.h  # CloudSync/AddonManager consult its flags (QtCore-only TU)
        src/addons/StremioTranslate.cpp   # AddonManager parses/routes Stremio through the translator
        src/core/StreamHeaders.cpp        # …and the translator parses behaviorHints.proxyHeaders through this
        src/core/SubtitleHash.cpp src/core/SubtitleHash.h  # AddonManager builds the /subtitles videoHash extra (#79)
        third_party/duktape/duktape.c third_party/miniz.c)
    target_include_directories(probe_addon PRIVATE src src/addons third_party third_party/duktape ${EB_SECRETS_GEN_DIR})
    target_compile_definitions(probe_addon PRIVATE MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
    target_link_libraries(probe_addon PRIVATE Qt6::Core Qt6::Network Qt6::Concurrent)

    # Queued game-metadata aggregator: prefetch-on-console-entry + cache, priority hover, dedup/skip-cached.
    add_executable(probe_gameagg tools/probe_gameagg.cpp
        src/addons/GameMetaAggregator.cpp src/addons/JsAddon.cpp src/addons/AddonContext.cpp
        src/addons/AddonModels.cpp src/addons/AddonManager.cpp src/core/MetaCache.cpp src/core/MetaOverrides.cpp
        src/core/BrandMigration.cpp src/core/BrandMigration.h  # CloudSync/AddonManager consult its flags (QtCore-only TU)
        src/addons/StremioTranslate.cpp   # AddonManager parses/routes Stremio through the translator
        src/core/StreamHeaders.cpp        # …and the translator parses behaviorHints.proxyHeaders through this
        src/core/SubtitleHash.cpp src/core/SubtitleHash.h  # AddonManager builds the /subtitles videoHash extra (#79)
        src/core/GamelistStore.cpp src/core/Settings.cpp
        src/theme2/FormFactor.cpp src/theme2/FormFactor.h  # Settings::virtualPadEnabled() resolves "auto" via FormFactor::instance() (D1)
        third_party/duktape/duktape.c third_party/miniz.c)
    target_include_directories(probe_gameagg PRIVATE src src/addons src/core third_party third_party/duktape ${EB_SECRETS_GEN_DIR})
    target_compile_definitions(probe_gameagg PRIVATE MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
    target_link_libraries(probe_gameagg PRIVATE Qt6::Core Qt6::Network Qt6::Concurrent)

    # On-demand game manual (issue #89): the "manual" art role merges by precedence but is EXCLUDED from
    # saveArt's eager prefetch — fetched only on explicit open. Merge/exclusion/fetch, over a loopback server.
    add_executable(probe_manual tools/probe_manual.cpp
        src/core/MetaCache.cpp src/core/MetaOverrides.cpp src/addons/AddonModels.cpp)
    target_include_directories(probe_manual PRIVATE src src/core src/addons)
    target_link_libraries(probe_manual PRIVATE Qt6::Core Qt6::Network Qt6::Gui)

    # Hermetic integration probe for the CatalogResolver SHOW-dispatch seam (the TV-resolution C1 bug): stands
    # up a REAL AddonManager + Duktape JS engine on a JsLocal series fixture (zero network) and drives a show
    # job through the typed requestCatalog dispatch to the cache. Same addon/JS/miniz set as probe_addon PLUS
    # the resolver/core sources (CatalogResolver/CatalogMatch/LocalResolveCache/LocalLibrary/Settings/FormFactor).
    add_executable(probe_showdispatch tools/probe_showdispatch.cpp
        src/addons/JsAddon.cpp src/addons/AddonContext.cpp src/addons/AddonModels.cpp
        src/addons/AddonManager.cpp src/addons/CatalogPrefetcher.cpp
        src/core/BrandMigration.cpp src/core/BrandMigration.h  # CloudSync/AddonManager consult its flags (QtCore-only TU)
        src/addons/StremioTranslate.cpp   # AddonManager parses/routes Stremio through the translator
        src/core/StreamHeaders.cpp        # …and the translator parses behaviorHints.proxyHeaders through this
        src/core/SubtitleHash.cpp src/core/SubtitleHash.h  # AddonManager builds the /subtitles videoHash extra (#79)
        src/core/CatalogResolver.cpp src/core/CatalogMatch.cpp src/core/LocalResolveCache.cpp
        src/core/LocalLibrary.cpp src/core/Settings.cpp src/theme2/FormFactor.cpp
        src/core/LocalMetaMerge.cpp src/core/MetaCache.cpp src/core/MetaOverrides.cpp   # #73: resolver's meta-fetch phase
        third_party/duktape/duktape.c third_party/miniz.c)
    target_include_directories(probe_showdispatch PRIVATE src src/core src/addons src/theme2 third_party third_party/duktape ${EB_SECRETS_GEN_DIR})
    target_compile_definitions(probe_showdispatch PRIVATE MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
    target_link_libraries(probe_showdispatch PRIVATE Qt6::Core Qt6::Network Qt6::Concurrent)
    endif() # NOT ANDROID (console probes)

    # Gamepad support via SDL2 (optional). Install SDL2 and pass CMAKE_PREFIX_PATH, or
    # -DSDL2_INCLUDE_DIR= and -DSDL2_LIBRARY=. Without it, controller input is simply absent.
    find_path(SDL2_INCLUDE_DIR NAMES SDL.h PATH_SUFFIXES SDL2)
    find_library(SDL2_LIBRARY NAMES SDL2)
    if(SDL2_INCLUDE_DIR AND SDL2_LIBRARY)
        target_include_directories(everythingbox PRIVATE ${SDL2_INCLUDE_DIR})
        target_link_libraries(everythingbox PRIVATE ${SDL2_LIBRARY})
        target_compile_definitions(everythingbox PRIVATE EVERYTHINGBOX_HAVE_SDL)
        message(STATUS "Gamepad: SDL2 found (${SDL2_LIBRARY})")
    else()
        message(STATUS "Gamepad: SDL2 not found - building without controller input")
    endif()

    # ---- Windows: make the binary RUNNABLE WHERE IT IS BUILT (issue #172) ---------------------------------
    # Until now `build/Release/EverythingBox.exe` exited instantly with 0xC0000135 (STATUS_DLL_NOT_FOUND) even
    # with Qt's bin/ on PATH, because libmpv is a LOAD-TIME dependency and lives somewhere else again. The only
    # way to run a build was to copy a real installation and drop the new exe into it — which is how #172's
    # verification gap started: the recipe was undocumented, expensive (a multi-GB copy), and it ran the new
    # code against somebody else's data directory. A build tree that runs is the cheaper, safer default.
    #
    # This is the same deployment the release workflow already performs (.github/workflows/release.yml), just
    # done in place. It costs one windeployqt pass per RELINK of the app (not per build: POST_BUILD only runs
    # when the exe is actually produced), and nothing at all for a probe-only build, since it hangs off the app
    # target. Turn it off with -DEB_WINDEPLOYQT=OFF if you are relinking in a tight loop and don't need to run
    # what you built.
    if(WIN32 AND NOT ANDROID)
        option(EB_WINDEPLOYQT "Stage the Qt + libmpv runtime beside the built app so it runs from the build tree" ON)
        find_program(EB_WINDEPLOYQT_EXE NAMES windeployqt HINTS "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}"
                     "${QT6_INSTALL_PREFIX}/bin")
        if(EB_WINDEPLOYQT AND EB_WINDEPLOYQT_EXE)
            # --qmldir mirrors release.yml: the themed home is QML, and without its module scan the app starts
            # and then renders nothing — the failure mode this whole issue is about (it looks like it worked).
            # Release-ONLY on a multi-config generator (issue #178): windeployqt --release stages RELEASE Qt DLLs,
            # which sit wrong (and windeployqt may reject the binary outright, failing the build) beside a Debug
            # exe. Guarding every token with $<CONFIG:Release> collapses the whole command to empty for Debug/
            # other configs, so CMake skips it; single-config Release (Ninja, CI) still runs it.
            add_custom_command(TARGET everythingbox POST_BUILD
                COMMAND "$<$<CONFIG:Release>:${EB_WINDEPLOYQT_EXE}>"
                        $<$<CONFIG:Release>:--release> $<$<CONFIG:Release>:--no-translations>
                        $<$<CONFIG:Release>:--no-system-d3d-compiler>
                        $<$<CONFIG:Release>:--qmldir> "$<$<CONFIG:Release>:${CMAKE_CURRENT_SOURCE_DIR}/src/theme2/qml>"
                        "$<$<CONFIG:Release>:$<TARGET_FILE:everythingbox>>"
                COMMENT "windeployqt: staging the Qt runtime beside EverythingBox.exe (Release config only)")
            # windeployqt knows only about Qt. libmpv (and SDL2, when found) are linked against import libs
            # whose DLLs live in their own SDK folder, so copy those too — otherwise the deploy is complete
            # except for the one DLL whose absence was the original 0xC0000135.
            set(_eb_runtime_libs "${MPV_LIBRARY}")
            if(SDL2_LIBRARY)
                list(APPEND _eb_runtime_libs "${SDL2_LIBRARY}")
            endif()
            foreach(_lib ${_eb_runtime_libs})
                get_filename_component(_lib_dir "${_lib}" DIRECTORY)
                file(GLOB _lib_dlls "${_lib_dir}/*.dll")
                foreach(_dll ${_lib_dlls})
                    add_custom_command(TARGET everythingbox POST_BUILD
                        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                                "${_dll}" "$<TARGET_FILE_DIR:everythingbox>")
                endforeach()
            endforeach()
        elseif(EB_WINDEPLOYQT)
            message(STATUS "EverythingBox: windeployqt not found - the built exe will need Qt + libmpv on PATH")
        endif()
    endif()

    # -----------------------------------------------------------------------------------------------------
    # Android packaging scaffolding. This sets up the APK metadata; a working build ALSO needs a Qt-for-
    # Android kit and the native deps (libmpv + ffmpeg, SDL2) cross-compiled for the target ABI and pointed
    # at via -DMPV_LIBRARY=/-DSDL2_LIBRARY= so they get bundled. See native/docs/android-port.md.
    # Configure e.g.: <Qt>/android_arm64_v8a/bin/qt-cmake -S native -B build-android -DEVERYTHINGBOX_BUILD_APP=ON
    # qt_add_executable already produces the shared library + APK target on Android.
    # -----------------------------------------------------------------------------------------------------
    if(ANDROID)
        # versionCode is a monotonic integer: major*1000000 + minor*10000 + patch (0.4.1 -> 4040001).
        # Patch capacity 9999 — the per-commit auto-bump hook (native/tools/pre-commit-version-bump.sh)
        # increments patch on every commit, so the old *100 formula would have overflowed into minor.
        math(EXPR EB_ANDROID_VERSION_CODE "${PROJECT_VERSION_MAJOR}*1000000 + ${PROJECT_VERSION_MINOR}*10000 + ${PROJECT_VERSION_PATCH}")
        # Stage the APK package-source overlay in the BUILD tree — never mutate the committed native/android/.
        # We copy the committed overlay verbatim, then add assets/eb/ = the stock themes2/ + every first-party
        # addon dir. androiddeployqt reads QT_ANDROID_PACKAGE_SOURCE_DIR and bundles its assets/ into the APK's
        # read-only assets; AssetBootstrap::run("assets:/eb", ...) extracts them into the writable data dir on
        # first launch (main.cpp). Staged at CONFIGURE time so a fresh configure always mirrors the current tree.
        # NOTE: not testable locally (no Android toolchain) — inert on desktop; Task 4's CI run exercises it.
        set(EB_ANDROID_PKG_DIR "${CMAKE_CURRENT_BINARY_DIR}/android-package")
        file(REMOVE_RECURSE "${EB_ANDROID_PKG_DIR}")
        file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/android/" DESTINATION "${EB_ANDROID_PKG_DIR}")
        file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/themes2" DESTINATION "${EB_ANDROID_PKG_DIR}/assets/eb")
        # Ship every first-party addon dir under native/addons/ (each is a self-contained manifest.json + main.js).
        file(GLOB _eb_addon_dirs LIST_DIRECTORIES true "${CMAKE_CURRENT_SOURCE_DIR}/addons/*")
        foreach(_addon ${_eb_addon_dirs})
            if(IS_DIRECTORY "${_addon}")
                file(COPY "${_addon}" DESTINATION "${EB_ANDROID_PKG_DIR}/assets/eb/addons")
            endif()
        endforeach()
        message(STATUS "Android assets staged at ${EB_ANDROID_PKG_DIR}/assets/eb (themes2 + first-party addons)")

        set_target_properties(everythingbox PROPERTIES
            QT_ANDROID_MIN_SDK_VERSION 24
            QT_ANDROID_TARGET_SDK_VERSION 34
            QT_ANDROID_VERSION_NAME "${PROJECT_VERSION}"
            QT_ANDROID_VERSION_CODE "${EB_ANDROID_VERSION_CODE}"
            QT_ANDROID_PACKAGE_SOURCE_DIR "${EB_ANDROID_PKG_DIR}") # build-staged manifest/res/assets overlay
        # Bundle the app's non-Qt shared libraries into the APK (androiddeployqt copies these). libmpv pulls
        # in its ffmpeg stack (libav*/libsw*), so bundle every .so sitting next to MPV_LIBRARY, not just it.
        if(MPV_LIBRARY)
            get_filename_component(_eb_mpvdir "${MPV_LIBRARY}" DIRECTORY)
            file(GLOB _eb_mpvlibs "${_eb_mpvdir}/*.so")
            set_property(TARGET everythingbox APPEND PROPERTY QT_ANDROID_EXTRA_LIBS ${_eb_mpvlibs})
        endif()
        if(SDL2_LIBRARY)
            set_property(TARGET everythingbox APPEND PROPERTY QT_ANDROID_EXTRA_LIBS "${SDL2_LIBRARY}")
        endif()
        # OpenSSL runtime for TLS. Qt's OpenSSL *plugin* is bundled, but without a per-ABI libssl/libcrypto in
        # the APK, Qt's on-device dlopen of the system libcrypto.so is namespace-denied by Android's linker and
        # ALL HTTPS fails ("No TLS backend is available"). Bundle the prebuilt OpenSSL 3 .so's for this ABI
        # (CI fetches github.com/KDAB/android_openssl and points OPENSSL_ANDROID_LIBS_DIR at ssl_3/<abi>) so
        # Qt's TLS backend resolves them from the app's own lib dir. Globs the whole dir: both the plain and
        # the version-suffixed sonames (libssl.so + libssl_3.so, libcrypto.so + libcrypto_3.so) ride along so
        # whichever name Qt's backend dlopens — and the libssl->libcrypto soname dependency — is satisfied.
        if(OPENSSL_ANDROID_LIBS_DIR)
            file(GLOB _eb_ssllibs "${OPENSSL_ANDROID_LIBS_DIR}/*.so")
            if(_eb_ssllibs)
                set_property(TARGET everythingbox APPEND PROPERTY QT_ANDROID_EXTRA_LIBS ${_eb_ssllibs})
                message(STATUS "Bundling Android OpenSSL runtime: ${_eb_ssllibs}")
            else()
                message(WARNING "OPENSSL_ANDROID_LIBS_DIR=${OPENSSL_ANDROID_LIBS_DIR} has no .so's — TLS will fail on device")
            endif()
        endif()
        message(STATUS "Configuring Android APK target (ABI: ${CMAKE_ANDROID_ARCH_ABI}). "
                       "Cores download to the app data dir at runtime; external emulators are disabled.")
    endif()

    # -----------------------------------------------------------------------------------------------------
    # iOS packaging. Mirrors the Android port: the media hub (video/audio via libmpv, readers, addons)
    # ships; standalone emulators + downloaded libretro cores are gated off (iOS forbids JIT and dlopen of
    # downloaded code). Configure with the Qt-for-iOS qt-cmake; libmpv comes from MPVKit's static
    # xcframeworks (https://github.com/mpvkit/MPVKit) via -DEB_IOS_MPV_DEPS_DIR=<dir of *.xcframework>,
    # plus -DMPV_INCLUDE_DIR=<Libmpv.framework/Headers> -DMPV_LIBRARY=<Libmpv.framework/Libmpv>.
    # -----------------------------------------------------------------------------------------------------
    if(IOS)
        # Real safe-area insets for the edge-to-edge window (UIKit query; the .cpp fallback is
        # Q_OS_IOS-guarded so both compiling is not a duplicate definition).
        target_sources(everythingbox PRIVATE src/core/SafeAreaInsets.mm)
        # Personal-team dev installs need a UNIQUE bundle id (the canonical one is registered to the
        # project's team; Apple refuses to register it to another). Override with
        # -DEB_IOS_BUNDLE_ID=com.yourname.everythingbox for on-device development.
        if(NOT EB_IOS_BUNDLE_ID)
            set(EB_IOS_BUNDLE_ID "com.everythingbox.app")
        endif()
        set_target_properties(everythingbox PROPERTIES
            MACOSX_BUNDLE_GUI_IDENTIFIER "${EB_IOS_BUNDLE_ID}"
            MACOSX_BUNDLE_BUNDLE_NAME "EverythingBox"
            MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")

        # iOS app icon. iOS is the one platform that will NOT take a file: it ignores .icns entirely
        # and ignores loose PNGs in the bundle, and finds the icon only via a COMPILED asset catalog
        # named by CFBundleIconName. So all three of these are load-bearing:
        #   1. the .xcassets is a bundle RESOURCE, so Xcode runs actool over it (adding the bare
        #      .appiconset instead compiles nothing — hence native/ios/Assets.xcassets/);
        #   2. ASSETCATALOG_COMPILER_APPICON_NAME tells actool WHICH set in the catalog is the app
        #      icon (without it the images are compiled but never marked as the icon);
        #   3. CFBundleIconName tells the springboard to look for it — and Qt's stock iOS plist
        #      template has no such key, which is why we hand Qt our own template (it keeps every Qt
        #      key incl. the @qt_ios_launch_screen_plist_entry@ substitution; see the file's header).
        # Miss any one and the app ships with a blank default tile.
        set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/ios/Assets.xcassets"
            TARGET_DIRECTORY everythingbox
            PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
        target_sources(everythingbox PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/ios/Assets.xcassets")
        set_target_properties(everythingbox PROPERTIES
            XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME "AppIcon"
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/ios/Info.plist.in")

        # Stage the stock themes2/ + first-party addons into the bundle (iOS bundles are shallow — resources
        # sit at the bundle root). AssetBootstrap extracts them into the writable data dir on first launch,
        # exactly like the Android assets:/eb flow (see main.cpp).
        add_custom_command(TARGET everythingbox POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_BUNDLE_DIR:everythingbox>/eb"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${CMAKE_CURRENT_SOURCE_DIR}/themes2" "$<TARGET_BUNDLE_DIR:everythingbox>/eb/themes2"
            COMMENT "Staging themes2 + addons into the app bundle")
        file(GLOB _eb_ios_addon_dirs LIST_DIRECTORIES true "${CMAKE_CURRENT_SOURCE_DIR}/addons/*")
        foreach(_addon ${_eb_ios_addon_dirs})
            if(IS_DIRECTORY "${_addon}")
                get_filename_component(_addon_name "${_addon}" NAME)
                add_custom_command(TARGET everythingbox POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_directory
                            "${_addon}" "$<TARGET_BUNDLE_DIR:everythingbox>/eb/addons/${_addon_name}")
            endif()
        endforeach()

        # MPVKit ships libmpv + its whole dependency stack (ffmpeg, libass, gnutls, libplacebo, MoltenVK, ...)
        # as per-library STATIC xcframeworks. Link the slice matching the target (device vs simulator), plus
        # the system frameworks/libs that stack expects.
        if(EB_IOS_MPV_DEPS_DIR)
            if(CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
                set(_eb_ios_slices "ios-arm64_x86_64-simulator" "ios-arm64-simulator")
            else()
                set(_eb_ios_slices "ios-arm64")
            endif()
            foreach(_slice ${_eb_ios_slices})
                file(GLOB _eb_ios_fws "${EB_IOS_MPV_DEPS_DIR}/*.xcframework/${_slice}/*.framework")
                foreach(_fw ${_eb_ios_fws})
                    get_filename_component(_fw_name "${_fw}" NAME_WE)
                    if(EXISTS "${_fw}/${_fw_name}")
                        target_link_libraries(everythingbox PRIVATE "${_fw}/${_fw_name}")
                    endif()
                endforeach()
                # Some deps ship a bare static lib in the slice instead of a framework (e.g. MoltenVK).
                file(GLOB _eb_ios_libs "${EB_IOS_MPV_DEPS_DIR}/*.xcframework/${_slice}/*.a")
                foreach(_lib ${_eb_ios_libs})
                    target_link_libraries(everythingbox PRIVATE "${_lib}")
                endforeach()
            endforeach()
            target_link_libraries(everythingbox PRIVATE
                "-framework AudioToolbox" "-framework AVFoundation" "-framework CoreAudio"
                "-framework CoreMedia" "-framework CoreVideo" "-framework VideoToolbox"
                "-framework Metal" "-framework MetalKit" "-framework CoreImage" "-framework QuartzCore"
                "-framework IOSurface" "-framework Security" "-framework SystemConfiguration"
                "-framework CoreText" "-framework UIKit"
                iconv bz2 z c++ resolv xml2)
            message(STATUS "iOS: linking MPVKit static stack from ${EB_IOS_MPV_DEPS_DIR}")
        endif()
        message(STATUS "Configuring iOS app bundle. Downloaded libretro cores + external emulators are "
                       "disabled on this platform.")
    endif()
endif()

# ---------------------------------------------------------------------------
# Probe data-dir isolation (issue #42) — applied to EVERY probe, by name, automatically.
#
# On desktop AppPaths::dataDir() is the executable's own folder (the app is portable), and every probe binary
# is built into that same folder as the GUI exe. One everythingbox.ini, one addons/, one metadata/, shared
# between the app, the probes, and whatever a developer dropped in build/Release. Three separate debugging
# sessions in one day went into suite failures that turned out to be that collision rather than the branch
# under test — and each one looked like a real regression, which is the expensive part: a gate that cries
# wolf teaches people to discount it.
#
# EB_ISOLATED_DATA_DIR redirects dataDir() at a per-PROCESS scratch directory that removes itself on exit
# (native/src/core/AppPaths.h). It is applied HERE, over every already-declared target whose name begins
# probe_, rather than at each add_executable, for one reason: a probe added next month has to get isolation
# without its author knowing this issue ever existed. Opt-in isolation would only move the trap.
#
# Four properties this placement depends on. The first two are about WHERE the sweep sits; the last two are
# constraints on everything else, and they are recorded here because nothing enforces them:
#   * it must stay at the END of the file — BUILDSYSTEM_TARGETS only sees targets declared above it;
#   * the app target must never match — `everythingbox` does not begin with probe_, and the runner's
#     "probe data-dir isolation wiring" gate fails on ANY mention of EB_ISOLATED_DATA_DIR in any CMake file
#     under native/ other than the one grant below;
#   * PROBE TARGETS MUST BE DECLARED IN THIS FILE. BUILDSYSTEM_TARGETS is per-DIRECTORY: a probe declared in
#     an add_subdirectory() CMakeLists is not in this list and would be built with no isolation at all,
#     writing its ini and add-ons straight into build/Release like before the fix. If a probe ever has to
#     live in a subdirectory, the sweep has to walk SUBDIRECTORIES too — it does not today;
#   * A PROBE'S LINK DEPENDENCIES MUST NOT CALL AppPaths::dataDir(). The define is PRIVATE, so a library
#     target linked into a probe compiles WITHOUT it — and dataDir() is an `inline` function whose body
#     differs between the two settings (native/src/core/AppPaths.h). Linking both definitions into one
#     binary is an ODR violation: the linker keeps one COMDAT and may keep the UN-isolated one, at which
#     point that probe silently reads build/Release again. Nothing catches this — probe_isolation only tests
#     its OWN binary, and the runner's contamination gate only sees WRITES, not reads. It is not live today:
#     probes compile the production sources directly, and the library targets they do link
#     (everythingbox_libretro, rcheevos, lzma7z) never touch AppPaths. Keep it that way, or give the define
#     to those libraries too so both sides agree.
# Deleting the block does not silently un-isolate the suite: probe_isolation asserts the outcome at runtime.
get_property(_eb_dir_targets DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY BUILDSYSTEM_TARGETS)
set(_eb_isolated_probes "")
foreach(_eb_t ${_eb_dir_targets})
    get_target_property(_eb_t_type ${_eb_t} TYPE)
    if(_eb_t MATCHES "^probe_" AND _eb_t_type STREQUAL "EXECUTABLE")
        target_compile_definitions(${_eb_t} PRIVATE EB_ISOLATED_DATA_DIR)
        list(APPEND _eb_isolated_probes ${_eb_t})
    endif()
endforeach()
list(LENGTH _eb_isolated_probes _eb_isolated_count)
message(STATUS "EverythingBox: data-dir isolation applied to ${_eb_isolated_count} probe target(s)")
