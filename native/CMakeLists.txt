cmake_minimum_required(VERSION 3.21)
project(EverythingBoxNative VERSION 0.5.224 LANGUAGES C CXX) # keep in sync with kAppVersion in src/main.cpp
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
    # so this tracks whatever Qt configured this build and stays portable: this machine
    # resolves to C:/Qt/6.8.3/msvc2022_64/plugins, the CI Linux runner resolves ITS own Qt's
    # plugins dir. This only affects build-tree runs; the DEPLOYED app already has plugins
    # deployed beside it, so the deploy path is left untouched.
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
    # native/secrets/screenscraper.secrets, obfuscates it, and generates BuiltinSecrets.h into the
    # BUILD TREE only. Absent secrets file -> empty header + a loud STATUS line (addon falls back to
    # user settings). The generated dir is added to the include path of every target that compiles
    # AddonContext.cpp (the app + the addon probes).
    set(EB_SECRETS_FILE     "${CMAKE_CURRENT_SOURCE_DIR}/secrets/screenscraper.secrets")
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
        src/emu/VirtualPad.cpp    src/emu/VirtualPad.h
        src/emu/NetplaySession.cpp src/emu/NetplaySession.h
        src/ebook/EbookSource.h
        src/ebook/EpubBook.cpp    src/ebook/EpubBook.h
        src/ebook/MobiBook.cpp    src/ebook/MobiBook.h
        src/ebook/PdfTextBook.cpp src/ebook/PdfTextBook.h
        src/ebook/EbookView.cpp   src/ebook/EbookView.h
        src/pdf/PdfView.cpp       src/pdf/PdfView.h
        src/comic/ComicView.cpp   src/comic/ComicView.h
        src/addons/AddonModels.cpp  src/addons/AddonModels.h
        src/addons/AddonContext.cpp src/addons/AddonContext.h
        src/addons/JsAddon.cpp      src/addons/JsAddon.h
        src/addons/AddonManager.cpp src/addons/AddonManager.h
        src/addons/CatalogPrefetcher.cpp src/addons/CatalogPrefetcher.h
        src/addons/GameMetaAggregator.cpp src/addons/GameMetaAggregator.h
        src/browse/SyntheticCatalogs.cpp src/browse/SyntheticCatalogs.h
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
        src/core/SyncOffsets.cpp  src/core/SyncOffsets.h  # player audio/sub sync-offset store (Task 2 wires the card onto it)
        src/core/ExternalPlayer.cpp src/core/ExternalPlayer.h # external-player handoff (VLC/MPC/custom/Android intent); Task 2 routes onto it
        # FormFactor: QtCore-only mode authority. UNCONDITIONAL — Settings.cpp + RetroView.cpp call
        # FormFactor::instance() and both compile in every configuration, incl. the no-QML Android kit.
        src/theme2/FormFactor.cpp src/theme2/FormFactor.h
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
        src/core/MetaCache.cpp src/core/MetaOverrides.cpp    src/core/MetaCache.h
        src/core/ScrapedSnapshot.h   # issue #24: the metadata editor's baseline, keyed to its item
        src/core/GamelistStore.cpp src/core/GamelistStore.h
        src/core/UiTestServer.cpp src/core/UiTestServer.h
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
        src/core/ItemMarks.cpp    src/core/ItemMarks.h  # per-profile hidden/completion/tags store (marks/shelves T1)
        src/core/Tombstones.cpp   src/core/Tombstones.h  # per-store deletion tombstones for multi-device merge (mdsync T1)
        src/core/CloudMerge.cpp   src/core/CloudMerge.h  # generalized per-item merge document serialize/merge (mdsync T2)
        src/core/ConsumptionStats.cpp src/core/ConsumptionStats.h  # per-profile media/reading accrual store (stats T1)
        src/core/PlaylistStore.cpp src/core/PlaylistStore.h
        src/core/PcGameStore.cpp  src/core/PcGameStore.h
        src/core/DownloadsStore.cpp src/core/DownloadsStore.h
        src/core/RaBrowse.cpp     src/core/RaBrowse.h
        src/core/SteamAchievements.cpp src/core/SteamAchievements.h
        src/core/PlayStats.cpp    src/core/PlayStats.h
        src/core/RomLibrary.cpp   src/core/RomLibrary.h
        src/core/LocalLibrary.cpp src/core/LocalLibrary.h
        src/core/CatalogMatch.cpp src/core/CatalogMatch.h  # pure local-library id-resolver matcher (id-resolver T1)
        src/core/LocalResolveCache.cpp src/core/LocalResolveCache.h  # persisted resolved-id cache (id-resolver T2)
        src/core/CatalogResolver.cpp src/core/CatalogResolver.h      # async throttled resolver engine (id-resolver T3)
        src/core/Theme.cpp        src/core/Theme.h
        src/core/CloudSync.cpp    src/core/CloudSync.h
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
        src/core/SystemCatalog.h
        src/ui/SettingsDialog.cpp src/ui/SettingsDialog.h
        src/ui/ControllerRemapDialog.cpp src/ui/ControllerRemapDialog.h
        src/ui/MainWindow.cpp     src/ui/MainWindow.h
        src/ui/BlackFrameWatchdog.cpp src/ui/BlackFrameWatchdog.h
        src/ui/Notifier.cpp       src/ui/Notifier.h
        src/media/StreamResolver.cpp src/media/StreamResolver.h
        src/media/PlaybackSession.cpp src/media/PlaybackSession.h
        src/launch/GameLauncher.cpp src/launch/GameLauncher.h
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
        src/core/MetaOverrides.cpp   src/core/MetaOverrides.h      # issue #24: mergeAll() carries + invalidates it
        src/core/MissedDismiss.cpp   src/core/MissedDismiss.h      # issue #25: ditto for the "you missed" dismissals
        src/core/TraktMissed.cpp     src/core/TraktMissed.h        # (MissedDismiss::prune -> missedDismissExpired)
        src/core/ConsumptionStats.cpp src/core/ConsumptionStats.h  # mdsync T3: mergeAll() invalidates its cache
        src/core/CloudSync.cpp       src/core/CloudSync.h          # mdsync T4: the device-local carve-out + bundle hands-off
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
            src/theme2/MpvPreview.cpp src/theme2/MpvPreview.h)
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
        src/core/MetaCache.cpp src/core/MetaOverrides.cpp src/core/SteamLibrary.cpp src/addons/AddonModels.cpp)
    target_include_directories(probe_browse PRIVATE src src/core src/addons src/browse)
    target_link_libraries(probe_browse PRIVATE Qt6::Core Qt6::Network Qt6::Gui)

    # Headless test for the local video library core (parse + NFO + scan + OwnedIndex).
    add_executable(probe_locallib tools/probe_locallib.cpp
        src/core/LocalLibrary.cpp src/core/LocalLibrary.h
        src/browse/SyntheticCatalogs.cpp src/browse/SyntheticCatalogs.h
        src/core/PcGameId.cpp src/core/PcGameId.h   # SyntheticCatalogs -> pcGamesCatalog's merge key
        src/core/TraktRead.cpp src/core/TraktRead.h   # SyntheticCatalogs -> trakt::imdbStreamIdFor (#23)
        src/core/TraktSync.cpp src/core/TraktSync.h   # SyntheticCatalogs -> traktListCatalog's TraktListEntry (#23)
        src/core/MetaCache.cpp src/core/MetaOverrides.cpp src/core/SteamLibrary.cpp src/addons/AddonModels.cpp
        src/core/Settings.cpp src/core/Settings.h                # LocalLibrary::root() -> Settings::libraryFolder()
        src/theme2/FormFactor.cpp src/theme2/FormFactor.h)       # Settings.cpp's virtualPadEnabled() -> FormFactor::instance()
    target_include_directories(probe_locallib PRIVATE src src/core src/addons src/browse)
    target_link_libraries(probe_locallib PRIVATE Qt6::Core Qt6::Network Qt6::Gui)

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

    # Headless test for the catalog id-resolver core (pure matcher + persisted cache).
    add_executable(probe_resolver tools/probe_resolver.cpp
        src/core/CatalogMatch.cpp src/core/CatalogMatch.h
        src/core/LocalResolveCache.cpp src/core/LocalResolveCache.h
        src/core/LocalLibrary.cpp src/core/LocalLibrary.h
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
        src/core/ProfileStore.cpp src/addons/AddonModels.cpp)
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
        src/core/NetHeaderApply.h)   # …through this, header-only and listed so it is visible here too
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
        src/core/GamelistStore.cpp src/core/Settings.cpp
        src/theme2/FormFactor.cpp src/theme2/FormFactor.h  # Settings::virtualPadEnabled() resolves "auto" via FormFactor::instance() (D1)
        third_party/duktape/duktape.c third_party/miniz.c)
    target_include_directories(probe_gameagg PRIVATE src src/addons src/core third_party third_party/duktape ${EB_SECRETS_GEN_DIR})
    target_compile_definitions(probe_gameagg PRIVATE MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
    target_link_libraries(probe_gameagg PRIVATE Qt6::Core Qt6::Network Qt6::Concurrent)

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
        src/core/CatalogResolver.cpp src/core/CatalogMatch.cpp src/core/LocalResolveCache.cpp
        src/core/LocalLibrary.cpp src/core/Settings.cpp src/theme2/FormFactor.cpp
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
