cmake_minimum_required(VERSION 3.21)
project(MyMediaVaultNative VERSION 0.3.0 LANGUAGES C CXX) # keep in sync with kAppVersion in src/main.cpp
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# MSVC: emit a .pdb for Release too, so a crash's WER fault offset can be mapped back to a function/line (the
# Release build otherwise ships no symbols — we had to reverse-engineer a Qt crash from a bare module offset).
# /Zi only writes symbols to a side .pdb (the exe stays fully optimized); /DEBUG makes the linker emit the .pdb,
# and /OPT:REF,/OPT:ICF restore the dead-code/COMDAT-folding size optimizations that a bare /DEBUG turns off.
# Set before any target so the static libs (our libretro frontend, etc.) contribute symbols to the final .pdb.
if(MSVC)
    add_compile_options($<$<CONFIG:Release>:/Zi>)
    add_link_options($<$<CONFIG:Release>:/DEBUG>)
    add_link_options($<$<CONFIG:Release>:/OPT:REF>)
    add_link_options($<$<CONFIG:Release>:/OPT:ICF>)
endif()

# ---------------------------------------------------------------------------
# libretro frontend - no external deps. Loads/runs any libretro core via the
# C ABI and exposes BGRA frames. VERIFIED: builds with MSVC, loads + inits mGBA.
# ---------------------------------------------------------------------------
add_library(mymediavault_libretro STATIC src/libretro/LibretroCore.cpp)
target_include_directories(mymediavault_libretro PUBLIC src/libretro)

# A console probe that exercises the frontend (no Qt) - used for verification. Desktop-only: Android builds
# the app as a shared library packaged into an APK, so standalone console executables don't fit.
if(NOT ANDROID)
    add_executable(probe_core tools/probe_core.cpp)
    target_link_libraries(probe_core PRIVATE mymediavault_libretro)
endif()

# ---------------------------------------------------------------------------
# The Qt application (UI + libmpv video). Needs Qt6 + libmpv installed, so it
# is OFF by default; the libretro lib + probe above build without them.
#   Qt6:    https://www.qt.io/download-qt-installer  (or `aqt`)
#   libmpv: vcpkg install libmpv   (or distro mpv-dev / a prebuilt SDK)
# Configure with: cmake -S native -B build -DMYMEDIAVAULT_BUILD_APP=ON -DCMAKE_PREFIX_PATH=<Qt>
# ---------------------------------------------------------------------------
option(MYMEDIAVAULT_BUILD_APP "Build the Qt application (requires Qt6 + libmpv)" OFF)
if(MYMEDIAVAULT_BUILD_APP)
    # QtPdf (PDFium) isn't shipped for Android in the open-source packages, so it's desktop-only; PdfView is
    # stubbed on Android (see src/pdf/PdfView.cpp).
    set(MMV_QT_COMPONENTS Widgets OpenGLWidgets Multimedia Network Concurrent)
    if(NOT ANDROID)
        list(APPEND MMV_QT_COMPONENTS Pdf PdfWidgets)
    endif()
    find_package(Qt6 REQUIRED COMPONENTS ${MMV_QT_COMPONENTS})
    qt_standard_project_setup()  # enables AUTOMOC etc.

    # QML theme engine (declarative, fully themeable front end) - desktop-first. OPTIONAL so a Qt install
    # without qtdeclarative still builds the app (the theme engine just isn't compiled). Gate its targets on
    # TARGET Qt6::Quick. The Android build keeps the Qt-Widgets UI until QML is added to its provisioning.
    if(NOT ANDROID)
        find_package(Qt6 QUIET OPTIONAL_COMPONENTS Quick Qml QuickWidgets)
    endif()
    if(TARGET Qt6::Quick AND TARGET Qt6::Qml AND TARGET Qt6::QuickWidgets)
        set(MMV_HAVE_QML ON)
        message(STATUS "MyMediaVault: QML theme engine enabled")
    else()
        message(STATUS "MyMediaVault: QML theme engine OFF (qtdeclarative not found)")
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

    qt_add_executable(mymediavault
        src/main.cpp
        src/video/MpvWidget.cpp   src/video/MpvWidget.h
        src/emu/RetroView.cpp     src/emu/RetroView.h
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
        src/addons/GameMetaAggregator.cpp src/addons/GameMetaAggregator.h
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
        src/ui/nav/NavOverlay.cpp src/ui/nav/NavOverlay.h
        src/ui/nav/Osk.cpp        src/ui/nav/Osk.h
        src/input/Gamepad.cpp     src/input/Gamepad.h
        src/input/Keymap.cpp      src/input/Keymap.h
        src/core/Settings.cpp     src/core/Settings.h
        src/core/BackgroundMusic.cpp src/core/BackgroundMusic.h
        src/core/Achievements.cpp src/core/Achievements.h
        src/core/CoreManager.cpp  src/core/CoreManager.h
        src/core/EmulatorManager.cpp src/core/EmulatorManager.h
        src/core/AppUpdater.cpp   src/core/AppUpdater.h
        src/core/SubtitleFetcher.cpp src/core/SubtitleFetcher.h
        src/core/CastManager.cpp  src/core/CastManager.h
        src/core/PortMapper.cpp   src/core/PortMapper.h
        src/core/TraktClient.cpp  src/core/TraktClient.h
        src/core/DownloadManager.cpp src/core/DownloadManager.h
        src/core/EmulatorRegistry.h
        src/core/BiosCatalog.h
        src/core/SevenZip.cpp     src/core/SevenZip.h
        src/core/ArchiveRom.cpp   src/core/ArchiveRom.h
        src/core/RecentStore.cpp  src/core/RecentStore.h
        src/core/MetaCache.cpp    src/core/MetaCache.h
        src/core/GamelistStore.cpp src/core/GamelistStore.h
        src/core/UiTestServer.cpp src/core/UiTestServer.h
        src/core/ProfileStore.cpp src/core/ProfileStore.h
        src/core/FavoritesStore.cpp src/core/FavoritesStore.h
        src/core/PlaylistStore.cpp src/core/PlaylistStore.h
        src/core/PcGameStore.cpp  src/core/PcGameStore.h
        src/core/DownloadsStore.cpp src/core/DownloadsStore.h
        src/core/RaBrowse.cpp     src/core/RaBrowse.h
        src/core/SteamAchievements.cpp src/core/SteamAchievements.h
        src/core/PlayStats.cpp    src/core/PlayStats.h
        src/core/RomLibrary.cpp   src/core/RomLibrary.h
        src/core/Theme.cpp        src/core/Theme.h
        src/core/CloudSync.cpp    src/core/CloudSync.h
        src/core/SteamLibrary.cpp src/core/SteamLibrary.h
        src/core/SystemCatalog.h
        src/ui/SettingsDialog.cpp src/ui/SettingsDialog.h
        src/ui/ControllerRemapDialog.cpp src/ui/ControllerRemapDialog.h
        src/ui/MainWindow.cpp     src/ui/MainWindow.h
        resources/appicon.qrc
        $<$<PLATFORM_ID:Windows>:${CMAKE_CURRENT_SOURCE_DIR}/resources/app.rc>
        third_party/miniz.c
        third_party/duktape/duktape.c)
    target_include_directories(mymediavault PRIVATE src third_party third_party/duktape ${MPV_INCLUDE_DIR})
    target_compile_definitions(mymediavault PRIVATE MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
    target_link_libraries(mymediavault PRIVATE mymediavault_libretro rcheevos lzma7z Qt6::Widgets Qt6::OpenGLWidgets Qt6::Multimedia Qt6::Network Qt6::Concurrent ${MPV_LIBRARY})
    if(NOT ANDROID)
        target_link_libraries(mymediavault PRIVATE Qt6::Pdf Qt6::PdfWidgets)
    endif()
    # QML theme engine: link Qt Quick, compile the qrc-embedded QML + ThemeEngine, and flag it on in code.
    if(MMV_HAVE_QML)
        # Compile theme2.qrc explicitly (not via target_sources): qt_add_executable only wires up .qrc files
        # present in its initial source list at finalization, so a .qrc added later via target_sources is
        # silently skipped and "qrc:/theme2/ThemeView.qml" isn't found at runtime. This runs rcc directly.
        qt_add_resources(MMV_THEME2_RCC resources/theme2.qrc)
        target_sources(mymediavault PRIVATE
            src/theme2/ThemeEngine.cpp src/theme2/ThemeEngine.h
            src/theme2/MpvPreview.cpp src/theme2/MpvPreview.h
            ${MMV_THEME2_RCC})
        target_link_libraries(mymediavault PRIVATE Qt6::Qml Qt6::Quick Qt6::QuickWidgets)
        target_compile_definitions(mymediavault PRIVATE MMV_HAVE_QML)
    endif()
    set_target_properties(mymediavault PROPERTIES OUTPUT_NAME "MyMediaVault") # the on-disk exe is MyMediaVault.exe
    # On macOS, produce a MyMediaVault.app bundle so macdeployqt can bundle Qt + libmpv into a .dmg.
    set_target_properties(mymediavault PROPERTIES MACOSX_BUNDLE TRUE)
    # On Windows, build as a GUI-subsystem app so no console window opens alongside the main window.
    # qt_add_executable links Qt's EntryPoint, which supplies the WinMain that forwards to main(); all
    # diagnostics go to the log file instead of the console (see main.cpp's message handler).
    set_target_properties(mymediavault PROPERTIES WIN32_EXECUTABLE TRUE)

    # Console harnesses (desktop-only): standalone executables for headless verification - not built for
    # Android, where the app is a shared lib + APK.
    if(NOT ANDROID)
    # Crash triage (Windows): symbolize a minidump's faulting-thread stack against the Release .pdb, since there's
    # no cdb/WinDbg on the box. No Qt — just DbgHelp. Usage: symbolize_dump <crash.dmp> C:\MyMediaVault-app
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
    if(MMV_HAVE_QML)
        qt_add_resources(MMV_THEME2_RCC_PROBE resources/theme2.qrc)
        add_executable(probe_theme2 tools/probe_theme2.cpp src/theme2/ThemeEngine.cpp
            src/theme2/MpvPreview.cpp src/theme2/MpvPreview.h ${MMV_THEME2_RCC_PROBE})
        target_include_directories(probe_theme2 PRIVATE src src/theme2 ${MPV_INCLUDE_DIR})
        target_link_libraries(probe_theme2 PRIVATE Qt6::Widgets Qt6::Qml Qt6::Quick Qt6::QuickWidgets Qt6::Multimedia ${MPV_LIBRARY})

        # Verifies MpvPreview (libmpv software-render) decodes + paints real frames on the software backend,
        # using mpv's built-in lavfi test source so no video file/key is needed.
        add_executable(probe_mpvpreview tools/probe_mpvpreview.cpp
            src/theme2/MpvPreview.cpp src/theme2/MpvPreview.h)
        target_include_directories(probe_mpvpreview PRIVATE src src/theme2 ${MPV_INCLUDE_DIR})
        target_link_libraries(probe_mpvpreview PRIVATE Qt6::Widgets Qt6::Qml Qt6::Quick ${MPV_LIBRARY})
    endif()

    # Headless tests for the offline metadata cache: merge-preserves-unknown-keys (future metadata),
    # item/detail round-trips, artwork path resolution, uninstall cleanup.
    add_executable(probe_meta tools/probe_meta.cpp src/core/MetaCache.cpp src/core/MetaCache.h
        src/addons/AddonModels.cpp src/addons/AddonModels.h)
    target_include_directories(probe_meta PRIVATE src src/core src/addons)
    target_link_libraries(probe_meta PRIVATE Qt6::Core Qt6::Network)

    # EmulationStation / RetroBat gamelist.xml reader (parse + ES-role mapping + local media resolve).
    add_executable(probe_gamelist tools/probe_gamelist.cpp
        src/core/GamelistStore.cpp src/core/GamelistStore.h src/addons/AddonModels.cpp)
    target_include_directories(probe_gamelist PRIVATE src src/core src/addons)
    target_link_libraries(probe_gamelist PRIVATE Qt6::Core Qt6::Network)

    # Headless tests for the controller-navigation kit (rings, overlays, on-screen keyboard, back routing).
    # Runs under the offscreen QPA in CI so every push proves the "selection can never be lost" invariants.
    add_executable(probe_nav tools/probe_nav.cpp
        src/ui/nav/Nav.cpp src/ui/nav/Nav.h
        src/ui/nav/NavOverlay.cpp src/ui/nav/NavOverlay.h
        src/ui/nav/Osk.cpp src/ui/nav/Osk.h)
    target_include_directories(probe_nav PRIVATE src src/ui)
    target_link_libraries(probe_nav PRIVATE Qt6::Widgets)

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
        src/addons/AddonManager.cpp
        third_party/duktape/duktape.c third_party/miniz.c)
    target_include_directories(probe_addon PRIVATE src src/addons third_party third_party/duktape)
    target_compile_definitions(probe_addon PRIVATE MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
    target_link_libraries(probe_addon PRIVATE Qt6::Core Qt6::Network Qt6::Concurrent)

    # Queued game-metadata aggregator: prefetch-on-console-entry + cache, priority hover, dedup/skip-cached.
    add_executable(probe_gameagg tools/probe_gameagg.cpp
        src/addons/GameMetaAggregator.cpp src/addons/JsAddon.cpp src/addons/AddonContext.cpp
        src/addons/AddonModels.cpp src/addons/AddonManager.cpp src/core/MetaCache.cpp
        src/core/GamelistStore.cpp src/core/Settings.cpp
        third_party/duktape/duktape.c third_party/miniz.c)
    target_include_directories(probe_gameagg PRIVATE src src/addons src/core third_party third_party/duktape)
    target_compile_definitions(probe_gameagg PRIVATE MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
    target_link_libraries(probe_gameagg PRIVATE Qt6::Core Qt6::Network Qt6::Concurrent)
    endif() # NOT ANDROID (console probes)

    # Gamepad support via SDL2 (optional). Install SDL2 and pass CMAKE_PREFIX_PATH, or
    # -DSDL2_INCLUDE_DIR= and -DSDL2_LIBRARY=. Without it, controller input is simply absent.
    find_path(SDL2_INCLUDE_DIR NAMES SDL.h PATH_SUFFIXES SDL2)
    find_library(SDL2_LIBRARY NAMES SDL2)
    if(SDL2_INCLUDE_DIR AND SDL2_LIBRARY)
        target_include_directories(mymediavault PRIVATE ${SDL2_INCLUDE_DIR})
        target_link_libraries(mymediavault PRIVATE ${SDL2_LIBRARY})
        target_compile_definitions(mymediavault PRIVATE MYMEDIAVAULT_HAVE_SDL)
        message(STATUS "Gamepad: SDL2 found (${SDL2_LIBRARY})")
    else()
        message(STATUS "Gamepad: SDL2 not found - building without controller input")
    endif()

    # -----------------------------------------------------------------------------------------------------
    # Android packaging scaffolding. This sets up the APK metadata; a working build ALSO needs a Qt-for-
    # Android kit and the native deps (libmpv + ffmpeg, SDL2) cross-compiled for the target ABI and pointed
    # at via -DMPV_LIBRARY=/-DSDL2_LIBRARY= so they get bundled. See native/docs/android-port.md.
    # Configure e.g.: <Qt>/android_arm64_v8a/bin/qt-cmake -S native -B build-android -DMYMEDIAVAULT_BUILD_APP=ON
    # qt_add_executable already produces the shared library + APK target on Android.
    # -----------------------------------------------------------------------------------------------------
    if(ANDROID)
        # versionCode is a monotonic integer: major*10000 + minor*100 + patch (0.2.0 -> 200).
        math(EXPR MMV_ANDROID_VERSION_CODE "${PROJECT_VERSION_MAJOR}*10000 + ${PROJECT_VERSION_MINOR}*100 + ${PROJECT_VERSION_PATCH}")
        set_target_properties(mymediavault PROPERTIES
            QT_ANDROID_MIN_SDK_VERSION 24
            QT_ANDROID_TARGET_SDK_VERSION 34
            QT_ANDROID_VERSION_NAME "${PROJECT_VERSION}"
            QT_ANDROID_VERSION_CODE "${MMV_ANDROID_VERSION_CODE}"
            QT_ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/android") # custom manifest/res/assets overlay
        # Bundle the app's non-Qt shared libraries into the APK (androiddeployqt copies these). libmpv pulls
        # in its ffmpeg stack (libav*/libsw*), so bundle every .so sitting next to MPV_LIBRARY, not just it.
        if(MPV_LIBRARY)
            get_filename_component(_mmv_mpvdir "${MPV_LIBRARY}" DIRECTORY)
            file(GLOB _mmv_mpvlibs "${_mmv_mpvdir}/*.so")
            set_property(TARGET mymediavault APPEND PROPERTY QT_ANDROID_EXTRA_LIBS ${_mmv_mpvlibs})
        endif()
        if(SDL2_LIBRARY)
            set_property(TARGET mymediavault APPEND PROPERTY QT_ANDROID_EXTRA_LIBS "${SDL2_LIBRARY}")
        endif()
        message(STATUS "Configuring Android APK target (ABI: ${CMAKE_ANDROID_ARCH_ABI}). "
                       "Cores download to the app data dir at runtime; external emulators are disabled.")
    endif()
endif()
