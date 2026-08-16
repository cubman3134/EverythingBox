#include "RetroParkView.h"

#include <QTimer>
#include <QPainter>
#include <QFrame>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QKeyEvent>
#include <QFocusEvent>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include "RetroParkInput.h"        // pure Qt-key/RetroPad -> shim VK mapper (unit-tested in probe_retropark_input)
#include "RetroParkState.h"        // pure state-path derivation (non-collision asserted in probe_retropark_content)
#include "RetroParkPace.h"         // pure fractional-frame pacing (interval derivation asserted in probe_retropark_content)
#include "../core/AppPaths.h"      // dataDir() — the base for the RetroPark-namespaced states/retropark/ subdir
#include "../core/Settings.h"      // retroParkDrivenBackend() + per-core/per-game option persistence (Task B4)
#include "../core/EmuBackend.h"    // retroParkSystemUsesGamepad() — which systems feed the abstract pad vs keys[]
#include "../core/SystemCatalog.h" // byId() — the system's default core (cores[0]), coreName_'s fallback (Task B4)
#include "RetroParkOptions.h"      // parse() — rp_runtime_core_options_json array -> EB CoreOption structs (Task B4)

// The RetroPark runtime is a desktop/Windows static lib linked into the app under this define (see the RetroPark
// block in native/CMakeLists.txt). Everything that touches rp_runtime_* lives behind it, so the widget still
// compiles on a build without RetroPark — openGame() just fails gracefully there.
#include "../core/CoreManager.h"   // coresDir() — the same <exeDir>/cores resolver EB uses for its libretro DLLs;
                                   // the Slice-2b build stages the shim into <coresDir>/libretro_shim (Task 1).

#ifdef EB_HAVE_RETROPARK
#include <retropark/retropark.h>
#include "RetroParkRuntimeApi.h"   // rpapi::runtimeApiForCore — core kind -> the rp_graphics_api to create with
#include "loader/StaticCoreRegistry.h"
// The driven reference core's getter, renamed at compile time by native/CMakeLists.txt so its RefCoreDriven.cpp
// links straight into the app without colliding with the ABI's canonical rp_get_core_abi symbol name — the same
// DLL-free static-core path probe_retropark / probe_retropark_loop use.
extern "C" const rp_core_abi* refcore_driven_static_get_core_abi(void);
#endif

// AUDIO (Slice 2b, Task 4): RetroPark owns audio end to end. Once real content is loaded, the runtime opens its
// OWN XAudio2 device and the driven core pushes samples straight through it — there is nothing to wire on the EB
// side, so this view sets up no QAudioSink and touches no sample buffer (the RetroPark ABI intentionally exposes
// only the diagnostic rp_runtime_audio_stats, never a pull/push hook to route audio through EB). KNOWN 2b
// LIMITATION: EB's own volume/mute does NOT govern RetroPark audio, because the runtime — not EB — drives XAudio2;
// routing RetroPark audio through EB's mixer would need an ABI extension and is deferred to a later slice.

namespace {
// The runtime's internal render resolution for the driven REFERENCE surface (the no-ROM fallback). The driven
// refcore paints a 64×64 field that the compositor upscales into this target; paintEvent then aspect-fits the
// read-back image into the widget. Fixed (not re-sized with the widget) so the per-frame read-back buffer never
// reallocates inside the present loop.
constexpr uint32_t kRpW = 512, kRpH = 448;
// NES native geometry for the real-content path. The Slice-2b libretro shim is FCEUmm — NES only — so its frames
// are 256×240. We size the runtime's OUTPUT surface (the read-back buffer we composite into) to that: the RetroPark
// host ABI exposes no post-load av-info/geometry getter, and the runtime is a read-only submodule we do not extend,
// so rather than querying the loaded core's geometry we size to the shim's known NES resolution. The driven
// compositor presents the core's 256×240 frame 1:1 into this surface, and paintEvent aspect-fits it into the widget.
constexpr uint32_t kContentW = 256, kContentH = 240;
// GameCube output geometry for the PRESENTING (Dolphin) path (Slice 3b). Dolphin renders on the GPU at its own
// (scaled) internal resolution; rp_runtime_present composites+reads that frame back into the surface we sized via
// rp_runtime_resize, so the read-back geometry is OURS to choose, not the core's. As with the NES surface above,
// the RetroPark host ABI exposes no post-load av-info/geometry getter (rp_runtime_status carries only a measured
// fps, no width/height), so we size to the GameCube's native NTSC 4:3 resolution (640×480) rather than querying
// it; paintEvent aspect-fits the read-back frame into the widget, so the display scales cleanly regardless.
constexpr uint32_t kGcW = 640, kGcH = 480;
// Frame rates for pacing (see RetroParkPace.h). The RetroPark host ABI exposes no per-core declared-fps getter:
// rp_runtime_get_status().fps is a MEASURED present rate ("0 until measured") — i.e. it reflects the rate WE are
// already pumping present() at, so it is circular and useless as a pacing SOURCE (and it is 0 right after load).
// So, exactly as we size the real-content surface to the shim's known NES geometry (256x240) rather than querying
// av-info, we pace real content to the NES/FCEUmm true rate — the shim is NES-only in 2b. NTSC NES is 60.0988 Hz;
// pacing to that (with a fractional accumulator) is what stops the ~4% fast-run and the resulting XAudio2 crackle.
constexpr double kNesFps      = 60.0988;   // real content (FCEUmm shim, NES-only in 2b)
constexpr double kRefcoreFps  = 60.0;      // static driven reference core (no meaningful declared fps)
// The PRESENTING (Dolphin) core simulates the GC on its OWN thread; our present() only polls the composited
// read-back, so this is a display/poll cadence, not a simulation driver — NTSC GC (~59.94/60 Hz) is right, and a
// slightly-off poll rate only re-reads or skips a frame, it never runs the game fast. No fractional carry needed.
constexpr double kGcFps       = 60.0;      // presenting Dolphin GC readback cadence
}

RetroParkView::RetroParkView(QWidget* parent) : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    // Painted black in paintEvent; opaque so no compositor cost from a transparent surface.
    setAttribute(Qt::WA_OpaquePaintEvent, true);

    timer_ = new QTimer(this);
    // Single-shot + PreciseTimer: tick() re-arms the timer for the NEXT frame via scheduleNextFrame(), which pays
    // out the core's fractional frame period as alternating integer-ms intervals. A repeating coarse timer at a
    // flat 16 ms would run ~4% fast for NES; PreciseTimer minimises the per-frame wakeup jitter on top of that.
    timer_->setSingleShot(true);
    timer_->setTimerType(Qt::PreciseTimer);
    connect(timer_, &QTimer::timeout, this, &RetroParkView::tick);

    buildMenu();
}

RetroParkView::~RetroParkView()
{
    // Free the runtime directly here rather than via stop(), so no gameStopped() is emitted into a half-destroyed
    // object during teardown.
    if (timer_) timer_->stop();
    running_ = false;
#ifdef EB_HAVE_RETROPARK
    if (rt_) { rp_runtime_unload_core(rt_); rp_runtime_destroy(rt_); rt_ = nullptr; }
#endif
}

void RetroParkView::buildMenu()
{
    // A styled QFrame child overlay — the same shape RetroView::buildMenu uses (NOT a QDialog/QMessageBox, which
    // the nav-kit rule forbids). Minimal for 2a: Resume and Exit.
    menu_ = new QFrame(this);
    menu_->setObjectName(QStringLiteral("rpMenu"));
    menu_->setStyleSheet(QStringLiteral(
        "#rpMenu { background: rgba(20,20,24,0.94); border: 1px solid rgba(255,255,255,0.15); border-radius: 12px; }"
        "#rpMenu QPushButton { padding: 9px 18px; font-size: 15px; color:#e8e8e8; background: transparent;"
        " border: 1px solid transparent; border-radius: 6px; }"
        "#rpMenu QPushButton:focus { background: rgba(90,140,255,0.85); border: 1px solid rgba(255,255,255,0.6); }"
        "#rpMenu QPushButton:hover { background: rgba(90,140,255,0.35); }"
        "#rpMenu QLabel { color: #e8e8e8; }"));
    auto* v = new QVBoxLayout(menu_);
    v->setContentsMargins(20, 18, 20, 18);
    v->setSpacing(8);

    menuTitle_ = new QLabel(tr("Paused"), menu_);
    menuTitle_->setAlignment(Qt::AlignCenter);
    menuTitle_->setStyleSheet(QStringLiteral("font-size:18px; font-weight:600;"));
    v->addWidget(menuTitle_);

    resumeBtn_ = new QPushButton(tr("Resume"), menu_);
    saveBtn_   = new QPushButton(tr("Save State"), menu_);
    loadBtn_   = new QPushButton(tr("Load State"), menu_);
    exitBtn_   = new QPushButton(tr("Exit"), menu_);
    v->addWidget(resumeBtn_);
    v->addWidget(saveBtn_);
    v->addWidget(loadBtn_);
    v->addWidget(exitBtn_);
    // Focus-cycle order for Up/Down navigation (keyPressEvent walks this list).
    menuButtons_ = { resumeBtn_, saveBtn_, loadBtn_, exitBtn_ };

    connect(resumeBtn_, &QPushButton::clicked, this, &RetroParkView::hideMenu);
    connect(saveBtn_, &QPushButton::clicked, this, [this] {
        // Save in place; keep the menu up and echo the result on the title label so the user sees it happened.
        QString err;
        menuTitle_->setText(saveState(&err) ? tr("State saved") : err);
    });
    connect(loadBtn_, &QPushButton::clicked, this, [this] {
        QString err;
        if (loadState(&err)) hideMenu();          // resume straight into the restored state (RetroView parity)
        else menuTitle_->setText(err);
    });
    connect(exitBtn_,   &QPushButton::clicked, this, [this] {
        menu_->hide();
        stop();                 // tear down the runtime (emits gameStopped)
        emit exitRequested();   // …then the host returns Home, mirroring RetroView's Exit
    });

    menu_->hide();
}

void RetroParkView::openGame(const QString& coreOrId, const QString& romPath, const QString& title,
                             const QString& systemId, const QString& gameKey, QString* error, bool presenting)
{
    Q_UNUSED(coreOrId);  // the shim (FCEUmm) is picked by the content branch below; the resolved libretro core id
                         // is carried for identity/parity but the driven shim path does not consult it in 2b.
    stop();              // tear down anything already running (openGame restarts cleanly)

    title_ = title; systemId_ = systemId; gameKey_ = gameKey; romPath_ = romPath;

    // Core-option identity (Task B4). coreName_ namespaces the persisted option keyspace (opt/<core>/* + the
    // optdesc/<core> descriptor cache) — resolved exactly as MainWindow resolves a system's launch core:
    // the user's per-system choice, else the system's default core (cores[0]). overrideToken_ mirrors
    // RetroView::openGame — the stable game key when present, else the ROM path, hashed to the per-game token
    // that keys the per-game option delta. Both are pure Settings/SystemCatalog reads (no runtime), so they
    // live outside the EB_HAVE_RETROPARK guard and are ready when the harvest+apply below runs.
    coreName_ = Settings::coreFor(systemId_);
    if (coreName_.isEmpty()) {
        const GameSystem* sys = SystemCatalog::byId(systemId_);
        if (sys) coreName_ = sys->cores.value(0);
    }
    overrideToken_ = Settings::gameToken(gameKey.isEmpty() ? romPath : gameKey);

#ifdef EB_HAVE_RETROPARK
    // Two load paths share this widget. A real game (romPath non-empty) drives the DYNAMIC libretro shim
    // (rp_runtime_load_core on <coresDir>/libretro_shim, then rp_runtime_load_content with the ROM — FCEUmm/NES).
    // No ROM (the Slice-2a live-surface behaviour) falls back to the static driven reference core. Keeping the
    // fallback means a bare openGame still paints the animated test field exactly as 2a did.
    const bool realContent = !romPath.isEmpty();
    // Slice 3b: the PRESENTING path — a real GameCube ISO on the heavy-app Dolphin core, which renders on the GPU
    // and is composited + read back over a headless VULKAN runtime (vs the driven NES shim / static refcore, both
    // on D3D11). presenting is set by the launcher from the system's core KIND (gc → presenting). It REQUIRES
    // content (Dolphin is requires_content: a bare presenting openGame has nothing to boot), so the presenting
    // load branch is gated on realContent too; presenting with no ROM falls through to the driven fallback below.
    const bool presentingContent = presenting && realContent;
    presenting_ = presenting;

    // Register the statically-compiled-in driven core once, under the id the runtime resolves with no DLL. Used by
    // the fallback path; harmless to register even when the content path is taken.
    static bool registered = false;
    if (!registered) {
        rp::StaticCoreRegistry::register_core("refcore_driven", &refcore_driven_static_get_core_abi);
        registered = true;
    }

    // Choose the runtime's graphics API from the core's KIND — it MUST be picked here, before load_core, since it
    // can't be read off the core after create. A presenting target (heavy-app path) needs a headless Vulkan
    // runtime; a driven target (refcore / shim) keeps the proven D3D11 path by default, or the user-selected
    // OpenGL host runtime when the global "RetroPark driven backend" setting is switched to OpenGL. The chosen
    // driven api is IGNORED for a presenting core (it always forces Vulkan). The mapping lives once in
    // rpapi::runtimeApiForCore (unit-tested in probe_retropark_apiselect).
    const rp_graphics_api drivenApi =
        (Settings::retroParkDrivenBackend() == QStringLiteral("opengl")) ? RP_GFX_OPENGL : RP_GFX_D3D11;
    const rp_graphics_api runtimeApi =
        rpapi::runtimeApiForCore(presenting ? rpapi::CoreKind::Presenting : rpapi::CoreKind::Driven, drivenApi);
    rt_ = rp_runtime_create(runtimeApi, nullptr);
    if (!rt_) {
        if (error) *error = tr("RetroPark could not create a graphics device.");
        return;
    }
    // Size the output surface up front: the presenting (Dolphin/GC) path uses the GameCube geometry, the driven
    // content path the shim's NES geometry, the no-ROM fallback the refcore's. resize() also brings up the
    // runtime's graphics device (its first call initialises the backend), which rp_runtime_load_core requires, so
    // it must precede the load below. (Proven order for presenting too: probe_retropark_present resizes before
    // rp_runtime_load_core on the Vulkan runtime.)
    // N64 (HW-render libretro via Mupen) renders 4:3 like GameCube, so size its driven surface to the same
    // 640x480 (kGc*), NOT the NES 256x240 — else the higher-res N64 frame is downscaled + aspect-squished.
    const bool n64Content = realContent && systemId_ == QStringLiteral("n64");
    rpW_ = presentingContent ? kGcW : (n64Content ? kGcW : (realContent ? kContentW : kRpW));
    rpH_ = presentingContent ? kGcH : (n64Content ? kGcH : (realContent ? kContentH : kRpH));
    if (rp_runtime_resize(rt_, rpW_, rpH_) != RP_OK) {
        if (error) *error = tr("RetroPark could not size its output.");
        rp_runtime_destroy(rt_); rt_ = nullptr; rpW_ = rpH_ = 0;
        return;
    }

    if (presentingContent) {
        // PRESENTING (Dolphin/GC) load path (Slice 3b). <coresDir>/dolphin_present is a DIRECTORY holding
        // core.json + dolphin_present.dll (a full LOCAL Dolphin build). coresDir() is EB's own <exeDir>/cores
        // resolver — the exact dir the build's POST_BUILD stages the vehicle into — so no path is hardcoded.
        //
        // The vehicle is LOCAL-ONLY: dolphin_present.dll is git-ignored, not in the submodule, unbuildable by EB,
        // and NEVER present on CI or a fresh clone (only core.json is tracked). So we MUST degrade gracefully when
        // the DLL is absent: a clear "install the vehicle" message and a clean teardown, with the host never
        // switching to this page. Every other backend (standalone Dolphin, NES, refcore) is unaffected.
        const QString dolphinDir = CoreManager::coresDir() + QStringLiteral("/dolphin_present");
        const QString dolphinDll = dolphinDir + QStringLiteral("/dolphin_present.dll");
        if (!QFileInfo::exists(dolphinDll)) {
            if (error) *error = tr("Dolphin core not installed — build/stage the RetroPark Dolphin vehicle "
                                   "(EB_DOLPHIN_VEHICLE_DIR).");
            rp_runtime_destroy(rt_); rt_ = nullptr; rpW_ = rpH_ = 0;
            return;
        }
        // Load the Dolphin presenting core (its manifest graphics_api is "vulkan"; the runtime's api-match gate
        // accepts it because we created RP_GFX_VULKAN above). Dolphin locates its Sys data beside the MAIN process
        // exe (GetModuleFileNameW(nullptr) → EverythingBox.exe), which the build stages as <exeDir>/Sys — no code
        // here handles Sys; a missing Sys surfaces as a load_content failure below, handled gracefully.
        if (rp_runtime_load_core(rt_, dolphinDir.toUtf8().constData()) != RP_OK) {
            if (error) *error = tr("RetroPark could not load the Dolphin core (dolphin_present under "
                                   "cores/dolphin_present is present but failed to initialise).");
            rp_runtime_unload_core(rt_); rp_runtime_destroy(rt_); rt_ = nullptr; rpW_ = rpH_ = 0;
            return;
        }
        // requires_content: the GC ISO must load or the core won't run. .iso/.rvz/.gcm etc. are Dolphin's to parse.
        if (rp_runtime_load_content(rt_, romPath.toUtf8().constData()) != RP_OK) {
            if (error) *error = tr("RetroPark could not boot this GameCube game (Dolphin rejected the file, or its "
                                   "Sys data is missing beside the app).");
            rp_runtime_unload_core(rt_); rp_runtime_destroy(rt_); rt_ = nullptr; rpW_ = rpH_ = 0;
            return;
        }
    } else if (realContent) {
        // DRIVEN libretro-shim path. The shim is a DIRECTORY holding core.json + LibretroShim.dll + the libretro
        // core DLL it LoadLibrary's; which core depends on the system: NES -> FCEUmm (dir <coresDir>/libretro_shim,
        // build-staged), N64 -> Mupen64Plus-Next (dir <coresDir>/libretro_shim_n64, self-healed on first use).
        // ensureShimDir guarantees the dir (shim + manifest + core DLL) at the moment of use — idempotent + non-
        // destructive, so the build-staged NES dir is left byte-identical (it only re-heals a missing/stale core).
        QString subdir, ebCoreId, coreDllName;
        if (systemId == QStringLiteral("n64")) {
            subdir = QStringLiteral("libretro_shim_n64");
            ebCoreId = QStringLiteral("mupen64plus_next");
            coreDllName = QStringLiteral("mupen64plus_next_libretro.dll");
        } else {   // "nes" — the only other driven-shim system today (FCEUmm)
            subdir = QStringLiteral("libretro_shim");
            ebCoreId = QStringLiteral("fceumm");
            coreDllName = QStringLiteral("fceumm_libretro.dll");
        }
        QString shimErr;
        const QString shimDir = ensureShimDir(subdir, ebCoreId, coreDllName, &shimErr);
        if (shimDir.isEmpty()) {
            if (error) *error = shimErr;
            rp_runtime_destroy(rt_); rt_ = nullptr; rpW_ = rpH_ = 0;
            return;
        }

        if (rp_runtime_load_core(rt_, shimDir.toUtf8().constData()) != RP_OK) {
            if (error) *error = tr("RetroPark could not load its core (the libretro shim under "
                                   "cores/%1 is missing or failed to initialise).").arg(subdir);
            rp_runtime_unload_core(rt_); rp_runtime_destroy(rt_); rt_ = nullptr; rpW_ = rpH_ = 0;
            return;
        }
        if (rp_runtime_load_content(rt_, romPath.toUtf8().constData()) != RP_OK) {
            if (error) *error = tr("RetroPark could not load this game — the libretro shim rejected the ROM for "
                                   "this system.");
            rp_runtime_unload_core(rt_); rp_runtime_destroy(rt_); rt_ = nullptr; rpW_ = rpH_ = 0;
            return;
        }
        // Core options (Task B4). CRITICAL timing: fceumm-class shim cores declare their options only during
        // retro_load_game, so ONLY now — after load_content — does the running core expose any options. Harvest
        // them off the LIVE runtime (rt_), cache the raw descriptors so the global editor (B3) can show this
        // late-declaring core's options before the next launch, then push the user's persisted effective value
        // for each option (per-core baseline, or the option's own default, with the per-game delta winning).
        // Values differing from the core default are set live; the shim's dirty latch picks them up within a
        // frame (load_game-only options like region take effect on the next reset — acceptable).
        {
            const char* jsonC = rp_runtime_core_options_json(rt_);
            const QByteArray json(jsonC ? jsonC : "[]");
            const std::vector<CoreOption> opts = RetroParkOptions::parse(json);
            if (!opts.empty()) {
                Settings::setCoreOptionDescriptors(coreName_, QString::fromUtf8(json));
                const QMap<QString, QString> delta = overrideToken_.isEmpty()
                    ? QMap<QString, QString>() : Settings::gameOptionDelta(overrideToken_, coreName_);
                for (const CoreOption& o : opts) {
                    const QString key = QString::fromStdString(o.key);
                    const QString baseline = Settings::optionValue(coreName_, key);
                    QString val = baseline.isEmpty() ? QString::fromStdString(o.defaultValue) : baseline;
                    if (delta.contains(key)) val = delta.value(key);   // per-game override wins
                    if (val != QString::fromStdString(o.defaultValue))
                        rp_runtime_core_option_set(rt_, o.key.c_str(), val.toUtf8().constData());
                }
            }
        }
    } else if (presenting) {
        // presenting && !realContent (M2, belt-and-suspenders): a presenting target had its runtime created on
        // Vulkan (runtimeApi above keys off `presenting` alone), but there is no content to boot. Do NOT fall
        // through to the DRIVEN static refcore below — that core is a driven (D3D11-kind) core and would run on a
        // Vulkan runtime, a presenting/driven mismatch. Fail gracefully instead (clear error, clean teardown, no
        // page switch). Currently unreachable — a gc launch always carries an ISO (Dolphin is requires_content) —
        // but this guarantees the mismatch can never occur even if a bare presenting openGame is ever issued.
        if (error) *error = tr("RetroPark presenting cores require game content.");
        rp_runtime_destroy(rt_); rt_ = nullptr; rpW_ = rpH_ = 0;
        return;
    } else if (rp_runtime_load_static_core(rt_, "refcore_driven") != RP_OK) {
        if (error) *error = tr("RetroPark could not load its reference core.");
        rp_runtime_unload_core(rt_); rp_runtime_destroy(rt_); rt_ = nullptr; rpW_ = rpH_ = 0;
        return;
    }

    buf_.assign((size_t)rpW_ * rpH_ * 4, 0);
    running_ = true;
    realContent_ = realContent;   // only the dynamic shim (a real ROM) consumes input; the static refcore ignores it
    rewinding_ = false;
    // Rewind: enable the runtime's frame-by-frame ring ONLY for a serialize-capable DRIVEN real-content core. The
    // static refcore is not serialize-capable (serialize_size==0), so we never arm rewind for it. And the
    // PRESENTING (Dolphin) core is explicitly EXCLUDED even though it serializes: its savestate is ~94 MB, so a
    // per-frame ring is impractical (memory + capture cost) — presenting cores never arm rewind (Slice 3b). F2/F4
    // savestate still works for them (see below). max_snapshots=0 lets the runtime pick its own default cap.
    rewindEnabled_ = false;
    if (realContent && !presenting && rp_runtime_serialize_size(rt_) > 0)
        rewindEnabled_ = (rp_runtime_set_rewind(rt_, 1, 0) == RP_OK);
    clearHeldKeys();              // start from a clean input state (no key carried in from a previous game)
    exitComboHeld_ = false;       // fresh game: no Start+Select carried in (NOT reset by clearHeldKeys — see the member)
    setFocus();
    // Pace at the loaded core's TRUE frame period. Real content is the NES/FCEUmm shim (60.0988 Hz); the static
    // refcore has no meaningful declared fps, so ~60. frameAccumMs_ starts fresh so the fractional carry is clean.
    frameIntervalMs_ = 1000.0 / (presentingContent ? kGcFps : (realContent ? kNesFps : kRefcoreFps));
    frameAccumMs_ = 0.0;
    scheduleNextFrame();
    update();
#else
    Q_UNUSED(romPath); Q_UNUSED(title); Q_UNUSED(systemId); Q_UNUSED(gameKey); Q_UNUSED(presenting);
    if (error) *error = tr("RetroPark is not available in this build.");
#endif
}

QString RetroParkView::ensureShimDir(const QString& subdir, const QString& ebCoreId,
                                     const QString& coreDllName, QString* err)
{
    // coresDir() is EB's own <exeDir>/cores resolver — the exact dir the build stages the shim into and EB
    // downloads its libretro DLLs into — so no path is hardcoded. The per-system shim dir is a child of it.
    const QString coresDir = CoreManager::coresDir();
    const QString dir = coresDir + QStringLiteral("/") + subdir;
    QDir().mkpath(dir);   // create it if this is a fresh tree / a not-yet-staged system (e.g. libretro_shim_n64)

    // LibretroShim.dll: the shim host the runtime loads. The build stages ONE copy into <coresDir>/libretro_shim;
    // copy it across into this per-system dir when absent so a self-healed dir (N64) gets the shim with no build.
    const QString shimDll = dir + QStringLiteral("/LibretroShim.dll");
    if (!QFileInfo::exists(shimDll)) {
        const QString stagedShim = coresDir + QStringLiteral("/libretro_shim/LibretroShim.dll");
        if (!QFileInfo::exists(stagedShim)) {
            if (err) *err = tr("RetroPark could not find its libretro shim (LibretroShim.dll).");
            return {};
        }
        if (!QFile::copy(stagedShim, shimDll)) {
            if (err) *err = tr("RetroPark could not install its libretro shim into the core directory.");
            return {};
        }
    }

    // core.json: the shim manifest naming the libretro core to LoadLibrary. NON-DESTRUCTIVE — write it only when
    // absent, NEVER overwrite an existing one (the NES dir's core.json is build-staged and must be left as-is).
    const QString coreJson = dir + QStringLiteral("/core.json");
    if (!QFileInfo::exists(coreJson)) {
        const QByteArray json =
            QStringLiteral("{ \"id\":\"libretro_shim\", \"name\":\"libretro Shim\", \"type\":\"driven\", "
                           "\"abi_version\":4, \"graphics_api\":\"none\", \"entry\":\"LibretroShim.dll\", "
                           "\"libretro_core\":\"%1\" }").arg(coreDllName).toUtf8();
        QSaveFile jf(coreJson);   // atomic: never leaves a torn manifest behind
        if (!jf.open(QIODevice::WriteOnly) || jf.write(json) != json.size() || !jf.commit()) {
            jf.cancelWriting();
            if (err) *err = tr("RetroPark could not write its core manifest.");
            return {};
        }
    }

    // The libretro core DLL itself — the fceumm self-heal generalised to any (ebCoreId, coreDllName). The shim
    // LoadLibrary's <coreDllName> ONLY from its own directory; the build stages it only if EB had already
    // downloaded it, so a fresh tree — or a deploy shipping only the shim's committed files — can lack it and
    // rp_runtime_load_core would then fail. Guarantee it at the moment of use: if the shim dir's copy is missing
    // or a different SIZE from EB's own (a stale/partial mirror), (re)copy EB's copy in. corePath resolves the
    // same <exeDir>/cores EB uses for its libretro DLLs.
    const QString shimCore = dir + QStringLiteral("/") + coreDllName;
    const QString ebCore   = CoreManager::corePath(ebCoreId);
    const QFileInfo shimFi(shimCore), ebFi(ebCore);
    if (!shimFi.exists() || (ebFi.exists() && shimFi.size() != ebFi.size())) {
        if (!ebFi.exists()) {
            // Neither the shim dir nor EB has the core — EB has never downloaded it. Fail with a clear next step
            // rather than proceeding into a muddier load_core failure.
            if (err) *err = tr("RetroPark needs the %1 core — open a game for this system once on the default "
                               "backend to download it.").arg(ebCoreId);
            return {};
        }
        if (shimFi.exists()) QFile::remove(shimCore);   // QFile::copy won't overwrite an existing file
        if (!QFile::copy(ebCore, shimCore)) {
            if (err) *err = tr("RetroPark could not install its core into the shim directory.");
            return {};
        }
    }
    return dir;
}

void RetroParkView::scheduleNextFrame()
{
    // Re-arm the single-shot timer for the next frame at the core's fractional period (see RetroParkPace.h). The
    // accumulator pays out e.g. 16.639 ms as alternating 16/17 ms intervals so the long-run average matches fps.
    if (timer_) timer_->start(rppace::nextFrameIntervalMs(frameIntervalMs_, frameAccumMs_));
}

void RetroParkView::tick()
{
#ifdef EB_HAVE_RETROPARK
    if (!rt_ || buf_.empty()) return;

    // Re-arm the next frame FIRST — before any early-return path below — so pacing never stalls. The timer is
    // single-shot; stop()/teardown simply never re-enters this slot.
    scheduleNextFrame();

    // Pause menu up: the pad drives the MENU, not the game. showMenu() paused the runtime (rp_runtime_pause) but
    // deliberately KEEPS timer_ running so tick() keeps firing — here we route it to handleMenuPad and return
    // WITHOUT advancing/presenting the (paused) game, so the game stays frozen behind the overlay. Mirrors
    // RetroView (RetroView.cpp: "menu up: the pad drives it, not the game"). MainWindow's pollMenuPad is
    // suppressed while RetroPark is current, so this is the sole thing driving the menu with the controller.
    if (menu_ && menu_->isVisible()) { handleMenuPad(); return; }

    // Rewind: while the rewind key is held (real-content, serialize-capable core only), step one frame into the
    // past instead of advancing. rp_runtime_rewind restores the previous frame's pre-state; the following present()
    // re-renders it (and does not re-grow the ring). RP_ERR_NOT_FOUND means the history is exhausted — hold the
    // current frame (repaint the last buffer, do not advance). Any other non-OK (rewind somehow disabled) falls
    // through to a normal advance so play never freezes.
    if (rewinding_ && rewindEnabled_) {
        const rp_result rr = rp_runtime_rewind(rt_);
        if (rr == RP_ERR_NOT_FOUND) { update(); return; }   // no more history — hold the frame
        if (rr == RP_OK) {
            if (rp_runtime_present(rt_, buf_.data()) != RP_OK) return;
            update();
            return;
        }
        // else: fall through to a normal advance
    }

    // Push this frame's input BEFORE advancing: present() runs the core, which polls the input snapshot we set.
    feedInput();
    // Composite the driven core's advanced frame into our RGBA8 read-back buffer, then repaint. present() advances
    // the core one frame unless it is paused (in which case the timer is stopped and tick() does not run anyway).
    if (rp_runtime_present(rt_, buf_.data()) != RP_OK) return;
    update();
#endif
}

// libretro analog index/id constants for Gamepad::axis (RETRO_DEVICE_INDEX_ANALOG_LEFT/RIGHT,
// RETRO_DEVICE_ID_ANALOG_X/Y) — spelled here so this .cpp needs no libretro.h, matching Gamepad's own contract.
namespace {
constexpr unsigned kAnalogLeft = 0, kAnalogRight = 1, kAnalogX = 0, kAnalogY = 1;
// Qt keys for the four arrow directions, indexed to match padKeyArrows_ ([0]=Up [1]=Down [2]=Left [3]=Right), so
// feedInput can map each held arrow back through the pure gcPadAxisForQtKey mapper to its analog-stick axis+value.
constexpr int kArrowKeys[4] = { Qt::Key_Up, Qt::Key_Down, Qt::Key_Left, Qt::Key_Right };
}

void RetroParkView::feedInput()
{
#ifdef EB_HAVE_RETROPARK
    // Only real content (the dynamic FCEUmm shim, or the Dolphin presenting core) consumes input; the static
    // reference core ignores it, so we skip the per-frame work (and the pad poll) on the no-ROM fallback.
    if (!rt_ || !realContent_) return;

    // ONE physical-controller poll per tick, off the app's SHARED Gamepad (see setGamepad()). RetroParkView owns
    // no Gamepad of its own — a second instance would fight the first for SDL's single event queue and miss the
    // hot-plug ADDED event, so a controller connected AFTER launch never opened here. Poll BEFORE reading below.
    if (sharedPad_) sharedPad_->poll();

    // Controller exit route: Start+Select together opens the pause menu, so a pad-only player can reach Exit with
    // no keyboard. Edge-detected via the pure exitComboRising helper (exitComboHeld_ debounces the hold, so it
    // fires once per press). On the trigger frame we open the menu and feed NO input this tick, so the combo never
    // also registers Start/Select in the game. showMenu() stops the timer + pauses the runtime; the trailing
    // present() in tick() is harmless (a paused core does not advance).
    if (sharedPad_) {
        const bool bothHeld =
            sharedPad_->button(0, rpinput::kJoyStart) && sharedPad_->button(0, rpinput::kJoySelect);
        if (rpinput::exitComboRising(bothHeld, exitComboHeld_)) { showMenu(); return; }
    }

    if (presenting_ || retroParkSystemUsesGamepad(systemId_)) {
        // ABSTRACT-PAD path — the GC (Dolphin PRESENTING) core AND the N64 (Mupen64Plus-Next driven shim), both of
        // which read the ABSTRACT PAD out of pad_buttons + pad_axes (see RetroParkInput.h + rp_dolphin.cpp / the
        // shim), carrying the analog stick + the full button/trigger cluster keys[] cannot. The pure mappers
        // (gcPadButtonForQtKey / gcPadAxisForQtKey / gcPadButtonForRetroPad / gcStickY) are unit-tested +
        // mutation-killed in probe_retropark_input. Single-player, port 0.
        rp_input_state in{};                       // zero: keys[]=0, pad_axes[]=0, pad_buttons=0
        uint16_t buttons = padKeyButtons_;         // digital pad bits held from the keyboard (GC keyboard scheme)

        // N64 (DRIVEN shim, not presenting): the keyboard is routed through the NES-mapped keys[] path
        // (keyPressEvent's driven branch fills keyHeld_ since presenting_ is false), which the N64 shim ORs with
        // the abstract pad. Copy those held bytes in so a keyboard player keeps the NES-subset controls while the
        // abstract pad below carries the analog stick + full gamepad. A no-op for the presenting GC path (Dolphin
        // ignores keys[], and keyHeld_ is never written on the GC keyboard branch).
        if (!presenting_)
            for (int vk = 0; vk < 256; ++vk)
                if (keyHeld_[(size_t)vk]) in.keys[vk] = 1;

        // Keyboard arrows -> analog LEFT stick (GC main stick), full deflection. Y is UP-positive per the ABI.
        for (int i = 0; i < 4; ++i) {
            if (!padKeyArrows_[(size_t)i]) continue;
            int ax = 0, val = 0;
            if (rpinput::gcPadAxisForQtKey(kArrowKeys[i], ax, val)) in.pad_axes[ax] = (int16_t)val;
        }

        // Physical controller (port 0), off the SHARED gamepad (already polled above). OR each held RetroPad button
        // into the abstract-pad bitmask, then the analog sticks/triggers. A no-op when no controller is connected.
        if (sharedPad_) {
            for (unsigned id = 0; id < (unsigned)Gamepad::kRetroPadButtons; ++id) {
                const int bit = rpinput::gcPadButtonForRetroPad(id);
                if (bit >= 0 && sharedPad_->button(0, id)) buttons |= (uint16_t)(1u << bit);
            }
            // Analog sticks: left = GC main stick, right = GC C-stick. X passes through; Y is negated from SDL's
            // down-positive to the ABI's up-positive (gcStickY). A non-zero stick overrides the keyboard-arrow
            // value for that axis (a real controller wins over the keyboard fallback). NOTE: Gamepad::button
            // conflates the left stick with the d-pad (a fully-deflected stick also reads as a d-pad press) — a
            // known pre-existing Gamepad behaviour; here the main stick is the important GC control and it is fed
            // cleanly from the analog axis below regardless.
            const int16_t lx = sharedPad_->axis(0, kAnalogLeft,  kAnalogX);
            const int16_t ly = sharedPad_->axis(0, kAnalogLeft,  kAnalogY);
            const int16_t rx = sharedPad_->axis(0, kAnalogRight, kAnalogX);
            const int16_t ry = sharedPad_->axis(0, kAnalogRight, kAnalogY);
            if (lx != 0) in.pad_axes[rpinput::kAxisLeftX]  = lx;
            if (ly != 0) in.pad_axes[rpinput::kAxisLeftY]  = (int16_t)rpinput::gcStickY(ly);
            if (rx != 0) in.pad_axes[rpinput::kAxisRightX] = rx;
            if (ry != 0) in.pad_axes[rpinput::kAxisRightY] = (int16_t)rpinput::gcStickY(ry);
            // Analog triggers: Gamepad exposes no analog-trigger getter, so derive full deflection from the digital
            // L2/R2 read, and OR the digital shoulder so Dolphin's (RP_PAD_L || LEFT_TRIGGER>0.5) always fires.
            if (sharedPad_->button(0, rpinput::kJoyL2)) {
                in.pad_axes[rpinput::kAxisLeftTrigger]  = rpinput::kAxisFull;  buttons |= (uint16_t)(1u << rpinput::kPadL);
            }
            if (sharedPad_->button(0, rpinput::kJoyR2)) {
                in.pad_axes[rpinput::kAxisRightTrigger] = rpinput::kAxisFull;  buttons |= (uint16_t)(1u << rpinput::kPadR);
            }
        }
        in.pad_buttons = buttons;
        rp_runtime_set_input(rt_, 0, &in);         // port 0 only
        return;
    }

    // DRIVEN (NES / FCEUmm shim) path — unchanged from Slice 2b. Reads keys[] only.
    rp_input_state in{};   // zero: keys[]=0, pad_axes[]=0, pad_buttons=0

    // Keyboard: copy the held NES virtual-key flags into keys[]. The shim reads keys[VK_UP], keys['X'], etc.
    for (int vk = 0; vk < 256; ++vk)
        if (keyHeld_[(size_t)vk]) in.keys[vk] = 1;

    // Physical controller (single-player / port 0), off the SHARED gamepad (already polled above): OR each held
    // RetroPad button into the SAME NES key bytes via the shared mapper, so a pad and the keyboard drive identical
    // controls. A no-op when no controller is connected or SDL isn't compiled in.
    if (sharedPad_) {
        for (unsigned id = 0; id < (unsigned)Gamepad::kRetroPadButtons; ++id) {
            int vk = 0;
            if (rpinput::nesVkForRetroPad(id, vk) && sharedPad_->button(0, id))
                in.keys[vk] = 1;
        }
    }

    rp_runtime_set_input(rt_, 0, &in);   // port 0 only in 2b
#endif
}

void RetroParkView::clearHeldKeys()
{
    keyHeld_.fill(false);
    padKeyButtons_ = 0;         // presenting (GC) keyboard state (Slice 3b) — cleared with the NES keys[] flags
    padKeyArrows_.fill(false);
}

QString RetroParkView::statePath() const
{
    // RetroPark-namespaced, deliberately distinct from RetroView's libretro states/<base>.state (subdir AND
    // suffix differ) so the same ROM played on both backends keeps separate state files. Derivation is the pure
    // rpstate::retroParkStatePath (asserted non-colliding in probe_retropark_content).
    const QString base = QFileInfo(romPath_).completeBaseName();
    const std::string p = rpstate::retroParkStatePath(AppPaths::dataDir().toStdString(), base.toStdString());
    return QString::fromStdString(p);
}

bool RetroParkView::saveState(QString* error)
{
#ifdef EB_HAVE_RETROPARK
    if (!rt_ || !realContent_) { if (error) *error = tr("No game is running."); return false; }
    const size_t sz = rp_runtime_serialize_size(rt_);
    if (sz == 0) {   // core has no savestate — user-visible, no crash
        if (error) *error = tr("Save states aren’t supported for this game.");
        return false;
    }
    std::vector<uint8_t> buf(sz);
    if (rp_runtime_save_state(rt_, buf.data(), sz) != RP_OK) {
        if (error) *error = tr("Couldn’t capture the save state.");
        return false;
    }
    const QString path = statePath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);   // atomic: WriteOnly truncates only on commit, so a torn write never destroys the old slot
    if (!f.open(QIODevice::WriteOnly) ||
        f.write(reinterpret_cast<const char*>(buf.data()), (qint64)buf.size()) != (qint64)buf.size() ||
        !f.commit()) {
        f.cancelWriting();
        if (error) *error = tr("Couldn’t write the save-state file.");
        return false;
    }
    emit statusMessage(tr("State saved"));
    return true;
#else
    if (error) *error = tr("RetroPark is not available in this build.");
    return false;
#endif
}

bool RetroParkView::loadState(QString* error)
{
#ifdef EB_HAVE_RETROPARK
    if (!rt_ || !realContent_) { if (error) *error = tr("No game is running."); return false; }
    const QString path = statePath();
    QFile f(path);
    if (!f.exists()) { if (error) *error = tr("No saved state for this game yet."); return false; }
    if (!f.open(QIODevice::ReadOnly)) { if (error) *error = tr("Couldn’t read the save-state file."); return false; }
    const QByteArray bytes = f.readAll();
    if (rp_runtime_load_state(rt_, bytes.constData(), (size_t)bytes.size()) != RP_OK) {
        if (error) *error = tr("This save state couldn’t be restored (it may be from a different game).");
        return false;
    }
    emit statusMessage(tr("State loaded"));
    return true;
#else
    if (error) *error = tr("RetroPark is not available in this build.");
    return false;
#endif
}

void RetroParkView::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), Qt::black);
    if (!running_ || buf_.empty() || rpW_ == 0 || rpH_ == 0) return;

    // Wrap the read-back buffer (no copy). RetroPark hands back RGBA8 top-down, which is QImage::Format_RGBA8888.
    QImage img(buf_.data(), (int)rpW_, (int)rpH_, (int)rpW_ * 4, QImage::Format_RGBA8888);

    // Aspect-fit, centred — the flat-mode fit math duplicated from RetroView::paintEvent (deliberately a copy of
    // the two lines, NOT an include of RetroView, which owns a libretro core we must never pull in here).
    const QSize t = img.size().scaled(size(), Qt::KeepAspectRatio);
    const QRect dst(QPoint((width() - t.width()) / 2, (height() - t.height()) / 2), t);
    p.setRenderHint(QPainter::SmoothPixmapTransform, false); // crisp, non-blurry pixels
    p.drawImage(dst, img);
}

void RetroParkView::resizeEvent(QResizeEvent*)
{
    if (menu_ && menu_->isVisible())
        menu_->move((width() - menu_->width()) / 2, (height() - menu_->height()) / 2);
}

void RetroParkView::keyPressEvent(QKeyEvent* e)
{
    // Esc / Back opens and closes the pause menu (Qt::Key_Back is the Android/TV-remote Back). Handled first, and
    // even on auto-repeat below, so the menu key always works and never becomes a stuck NES button.
    if (!e->isAutoRepeat() && (e->key() == Qt::Key_Escape || e->key() == Qt::Key_Back)) { toggleMenu(); return; }

    // While the menu is up, Up/Down move between its two buttons and Enter activates one; nothing reaches the
    // (paused) game. Auto-repeat here is harmless (it just re-navigates), so no special guard is needed.
    if (menu_ && menu_->isVisible())
    {
        const int k = e->key();
        if ((k == Qt::Key_Up || k == Qt::Key_Down) && !menuButtons_.empty())
        {
            // Cycle focus through the button list (Down = next, Up = previous), wrapping at the ends.
            int idx = 0;
            for (int i = 0; i < (int)menuButtons_.size(); ++i)
                if (menuButtons_[(size_t)i] == focusWidget()) { idx = i; break; }
            const int n = (int)menuButtons_.size();
            const int step = (k == Qt::Key_Down) ? 1 : n - 1;
            for (int hops = 0; hops < n; ++hops) {          // advance to the next ENABLED button (skip greyed-out)
                idx = (idx + step) % n;
                if (menuButtons_[(size_t)idx]->isEnabled()) break;
            }
            menuButtons_[(size_t)idx]->setFocus(Qt::TabFocusReason);
            return;
        }
        if (k == Qt::Key_Return || k == Qt::Key_Enter || k == Qt::Key_Select)
        {
            if (auto* b = qobject_cast<QPushButton*>(focusWidget())) b->click();
            else if (resumeBtn_) resumeBtn_->click();
            return;
        }
        return;
    }

    // Quick save/load state — F2/F4, mirroring RetroView's bindings. Handled before the NES keymap so they never
    // become a stuck button; feedback goes to the status bar via statusMessage. Only meaningful for real content.
    if (!e->isAutoRepeat() && realContent_ && e->key() == Qt::Key_F2) {
        QString err; if (!saveState(&err)) emit statusMessage(err); e->accept(); return;
    }
    if (!e->isAutoRepeat() && realContent_ && e->key() == Qt::Key_F4) {
        QString err; if (!loadState(&err)) emit statusMessage(err); e->accept(); return;
    }
    // Rewind — hold R (RetroView parity). While held, tick() steps back one frame instead of advancing. Armed only
    // when the runtime accepted set_rewind for this (serialize-capable, real-content) core.
    if (realContent_ && e->key() == Qt::Key_R) {
        if (rewindEnabled_) rewinding_ = true;
        else if (!e->isAutoRepeat()) emit statusMessage(tr("Rewind unavailable"));  // brief feedback, RetroView parity
        e->accept(); return;                                                        // (guard auto-repeat so a held R doesn't spam)
    }

    // Gameplay (PRESENTING / GameCube): mark the abstract-pad control held. A face/shoulder/start/d-pad key sets
    // its RP_PAD_* bit; an arrow key marks its direction (feedInput turns it into an analog left-stick value).
    // feedInput reads these each tick. Auto-repeat is a no-op (already held). Other keys reach the base class.
    if (presenting_) {
        const int bit = rpinput::gcPadButtonForQtKey(e->key());
        if (bit >= 0) { padKeyButtons_ |= (uint16_t)(1u << bit); e->accept(); return; }
        int ax = 0, val = 0;
        if (rpinput::gcPadAxisForQtKey(e->key(), ax, val)) {
            switch (e->key()) {
                case Qt::Key_Up:    padKeyArrows_[0] = true; break;
                case Qt::Key_Down:  padKeyArrows_[1] = true; break;
                case Qt::Key_Left:  padKeyArrows_[2] = true; break;
                case Qt::Key_Right: padKeyArrows_[3] = true; break;
            }
            e->accept(); return;
        }
        QWidget::keyPressEvent(e);
        return;
    }

    // Gameplay (DRIVEN / NES): if this key is one of the NES controls the shim reads, mark it held (feedInput()
    // copies keyHeld_ into keys[] each tick). Auto-repeat presses are no-ops — the key is already held from the
    // first press, and release clears it. We do NOT consume other keys, so anything else still reaches the base.
    int vk = 0;
    if (rpinput::nesVkForQtKey(e->key(), vk)) { keyHeld_[(size_t)vk] = true; e->accept(); return; }

    QWidget::keyPressEvent(e);
}

void RetroParkView::keyReleaseEvent(QKeyEvent* e)
{
    // A real key-up clears the held flag. Auto-repeat generates spurious release/press pairs while a key is held,
    // so ignore auto-repeat releases — otherwise a held direction would flicker off every repeat interval.
    if (e->isAutoRepeat()) { QWidget::keyReleaseEvent(e); return; }

    if (e->key() == Qt::Key_R) { rewinding_ = false; e->accept(); return; }   // stop stepping back on key-up

    // PRESENTING (GameCube): clear the held abstract-pad bit / arrow direction, mirroring keyPressEvent.
    if (presenting_) {
        const int bit = rpinput::gcPadButtonForQtKey(e->key());
        if (bit >= 0) { padKeyButtons_ &= (uint16_t)~(1u << bit); e->accept(); return; }
        int ax = 0, val = 0;
        if (rpinput::gcPadAxisForQtKey(e->key(), ax, val)) {
            switch (e->key()) {
                case Qt::Key_Up:    padKeyArrows_[0] = false; break;
                case Qt::Key_Down:  padKeyArrows_[1] = false; break;
                case Qt::Key_Left:  padKeyArrows_[2] = false; break;
                case Qt::Key_Right: padKeyArrows_[3] = false; break;
            }
            e->accept(); return;
        }
        QWidget::keyReleaseEvent(e);
        return;
    }

    int vk = 0;
    if (rpinput::nesVkForQtKey(e->key(), vk)) { keyHeld_[(size_t)vk] = false; e->accept(); return; }

    QWidget::keyReleaseEvent(e);
}

void RetroParkView::focusOutEvent(QFocusEvent* e)
{
    // Losing focus (menu overlay taking focus, app switch, ...) drops every held key so a direction can't stick
    // down with no key-up ever arriving. The rewind hold is released for the same reason.
    clearHeldKeys();
    rewinding_ = false;
    QWidget::focusOutEvent(e);
}

void RetroParkView::toggleMenu()
{
    if (!running_) return;
    if (menu_ && menu_->isVisible()) hideMenu();
    else showMenu();
}

// Held state of the menu-relevant pad buttons on port 0 (no poll here — the caller polls). Up/Down move the
// selection; A confirms; B backs out. Bit layout mirrors RetroView::menuPadMask (1=Up 2=Down 4=A 8=B).
int RetroParkView::menuPadMask() const
{
#ifdef EB_HAVE_RETROPARK
    if (!sharedPad_) return 0;
    int m = 0;
    if (sharedPad_->button(0, rpinput::kJoyUp))   m |= 1;
    if (sharedPad_->button(0, rpinput::kJoyDown)) m |= 2;
    if (sharedPad_->button(0, rpinput::kJoyA))    m |= 4;   // A / South confirms
    if (sharedPad_->button(0, rpinput::kJoyB))    m |= 8;   // B backs out (resume)
    return m;
#else
    return 0;
#endif
}

// Controller navigation for the open pause menu (mirrors RetroView::handleMenuPad): d-pad Up/Down move the
// selection (wrapping, skipping disabled buttons), A/South activates the selected button, B resumes. Rising-edge
// only (menuPadPrev_) so a held direction doesn't race through the short list. tick() calls this — and returns —
// while the menu is visible, so the game stays paused. Keyboard menu nav (keyPressEvent) still works alongside it.
void RetroParkView::handleMenuPad()
{
#ifdef EB_HAVE_RETROPARK
    if (!sharedPad_) return;
    sharedPad_->poll();   // the menu is up: tick() early-returns before feedInput(), so poll the shared pad here
    const int cur = menuPadMask();
    const int pressed = cur & ~menuPadPrev_;   // rising edge only
    menuPadPrev_ = cur;
    if (menuButtons_.empty()) return;
    const int n = (int)menuButtons_.size();

    // B backs out of the menu -> resume the game (RetroView parity). Handle before A so a stray simultaneous
    // press resolves to "back", and return so we don't also activate a button this frame.
    if (pressed & 8) { hideMenu(); return; }

    // Current selection = the focused button (default the first if focus is elsewhere).
    int idx = 0;
    for (int i = 0; i < n; ++i)
        if (menuButtons_[(size_t)i] == focusWidget()) { idx = i; break; }

    // Up = previous, Down = next; wrap and skip disabled (greyed-out Save/Load on the no-ROM refcore fallback).
    if ((pressed & 1) || (pressed & 2)) {
        const bool down = (pressed & 2) != 0;
        idx = rpinput::nextMenuIndex(idx, n, down,
                                     [this](int i){ return menuButtons_[(size_t)i]->isEnabled(); });
        menuButtons_[(size_t)idx]->setFocus(Qt::TabFocusReason);   // move the visible selection (matches keyboard nav)
    }

    // A / South confirms: click the focused button (Resume/Save/Load/Exit).
    if (pressed & 4) {
        if (auto* b = qobject_cast<QPushButton*>(focusWidget())) b->click();
        else menuButtons_.front()->click();
    }
#endif
}

void RetroParkView::showMenu()
{
    if (!running_) return;
#ifdef EB_HAVE_RETROPARK
    if (rt_) rp_runtime_pause(rt_);   // driven: advancing stops; the last frame stays composited
#endif
    // Do NOT stop timer_ here. The runtime is paused (above), so tick() will NOT advance/present the game — but
    // tick() must keep firing to drive the pause menu with the controller (handleMenuPad), because MainWindow's
    // pollMenuPad is suppressed while RetroPark is the current page (RetroView keeps its timer running for the
    // same reason). tick() early-returns to handleMenuPad while the menu is visible. Re-arm defensively in case
    // the timer was somehow idle at open (normally it is armed from the last gameplay tick).
    if (running_ && timer_ && !timer_->isActive()) scheduleNextFrame();
    menuPadPrev_ = menuPadMask();      // seed rising-edge state so a button held at open doesn't auto-activate
    rewinding_ = false;                // the rewind hold can't span a pause (no key-up would arrive)
    clearHeldKeys();                   // drop every held D-pad/button explicitly, so pausing can never leave one
                                       // stuck down (do NOT lean on the resume button stealing focus -> focusOut)
    if (menuTitle_) menuTitle_->setText(tr("Paused"));   // clear any stale save/load feedback from last time
    // Save/Load only make sense for real content; grey them out on the static-refcore fallback (no ROM).
    if (saveBtn_) saveBtn_->setEnabled(realContent_);
    if (loadBtn_) loadBtn_->setEnabled(realContent_);
    menu_->show();
    menu_->raise();
    menu_->move((width() - menu_->width()) / 2, (height() - menu_->height()) / 2);
    if (resumeBtn_) resumeBtn_->setFocus(Qt::TabFocusReason);
}

void RetroParkView::hideMenu()
{
    if (menu_) menu_->hide();
#ifdef EB_HAVE_RETROPARK
    if (rt_) rp_runtime_resume(rt_);
#endif
    if (running_ && timer_) scheduleNextFrame();   // resume pacing at the core's true frame period
    setFocus(); // keep Esc / gameplay keys coming to the view
}

void RetroParkView::stop()
{
    if (timer_) timer_->stop();
    const bool was = running_;
    running_ = false;
    if (menu_) menu_->hide();
#ifdef EB_HAVE_RETROPARK
    if (rt_) { rp_runtime_unload_core(rt_); rp_runtime_destroy(rt_); rt_ = nullptr; }
#endif
    buf_.clear();
    rpW_ = rpH_ = 0;
    realContent_ = false;
    presenting_ = false;
    rewindEnabled_ = false;
    rewinding_ = false;
    exitComboHeld_ = false;   // combo debounce reset on teardown (clearHeldKeys intentionally leaves it alone)
    romPath_.clear();
    clearHeldKeys();
    update();
    if (was) emit gameStopped();  // only when a game was actually running (stop() is safe to call when idle)
}
