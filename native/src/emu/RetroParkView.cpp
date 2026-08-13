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
    // runtime; a driven target (refcore / shim, the only kind 3a actually launches) keeps the proven D3D11 path.
    // The mapping lives once in rpapi::runtimeApiForCore (unit-tested in probe_retropark_apiselect).
    const rp_graphics_api runtimeApi =
        rpapi::runtimeApiForCore(presenting ? rpapi::CoreKind::Presenting : rpapi::CoreKind::Driven);
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
    rpW_ = presentingContent ? kGcW : (realContent ? kContentW : kRpW);
    rpH_ = presentingContent ? kGcH : (realContent ? kContentH : kRpH);
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
        // <coresDir>/libretro_shim is a DIRECTORY holding core.json + LibretroShim.dll + fceumm_libretro.dll,
        // staged there by the build (native/CMakeLists.txt Slice-2b POST_BUILD). coresDir() is EB's own resolver
        // (<exeDir>/cores on desktop) — the exact location the shim is staged into — so we never hardcode a path.
        const QString shimDir = CoreManager::coresDir() + QStringLiteral("/libretro_shim");

        // fceumm self-heal (removes the build/deploy-order hazard). The FCEUmm shim LoadLibrary's
        // fceumm_libretro.dll ONLY from its own directory. Build-time staging copies fceumm there only if it
        // already sat in <exeDir>/cores at POST_BUILD, so a fresh build tree — or a deploy that ships only the
        // shim's 2 committed files without EB's runtime-downloaded fceumm — leaves the shim dir without it, and
        // rp_runtime_load_core then fails: RetroPark NES is dead. Guarantee it here, at the moment of use: if the
        // shim dir's fceumm is missing or a different SIZE from EB's own copy (a stale/partial mirror), (re)copy
        // EB's copy in. coresDir()/corePath resolve the exact same <exeDir>/cores EB uses for its libretro DLLs.
        const QString shimFceumm = shimDir + QStringLiteral("/fceumm_libretro.dll");
        const QString ebFceumm   = CoreManager::corePath(QStringLiteral("fceumm"));
        const QFileInfo shimFi(shimFceumm), ebFi(ebFceumm);
        if (!shimFi.exists() || (ebFi.exists() && shimFi.size() != ebFi.size())) {
            if (!ebFi.exists()) {
                // Neither the shim dir nor EB has fceumm — EB has never downloaded it. Fail gracefully with a
                // clear next step; do NOT proceed into load_core (which would fail with a muddier message).
                if (error) *error = tr("RetroPark needs the FCEUmm core — open a NES game once on the default "
                                       "backend to download it.");
                rp_runtime_destroy(rt_); rt_ = nullptr; rpW_ = rpH_ = 0;
                return;
            }
            QDir().mkpath(shimDir);                                   // create the shim dir if this is a fresh tree
            if (shimFi.exists()) QFile::remove(shimFceumm);           // QFile::copy won't overwrite an existing file
            if (!QFile::copy(ebFceumm, shimFceumm)) {
                if (error) *error = tr("RetroPark could not install its NES core into the shim directory.");
                rp_runtime_destroy(rt_); rt_ = nullptr; rpW_ = rpH_ = 0;
                return;
            }
        }

        if (rp_runtime_load_core(rt_, shimDir.toUtf8().constData()) != RP_OK) {
            if (error) *error = tr("RetroPark could not load its NES core (the libretro shim under "
                                   "cores/libretro_shim is missing or failed to initialise).");
            rp_runtime_unload_core(rt_); rp_runtime_destroy(rt_); rt_ = nullptr; rpW_ = rpH_ = 0;
            return;
        }
        if (rp_runtime_load_content(rt_, romPath.toUtf8().constData()) != RP_OK) {
            if (error) *error = tr("RetroPark could not load this game — the FCEUmm shim runs NES ROMs only in "
                                   "this build.");
            rp_runtime_unload_core(rt_); rp_runtime_destroy(rt_); rt_ = nullptr; rpW_ = rpH_ = 0;
            return;
        }
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
    // single-shot; a paused menu (timer stopped) or stop()/teardown simply never re-enters this slot.
    scheduleNextFrame();

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

void RetroParkView::feedInput()
{
#ifdef EB_HAVE_RETROPARK
    // Only real content (the dynamic FCEUmm shim) consumes input; the static reference core ignores it, so we skip
    // the per-frame work (and the pad poll) on the no-ROM fallback.
    if (!rt_ || !realContent_) return;

    rp_input_state in{};   // zero: keys[]=0, pad_axes[]=0, pad_buttons=0

    // Keyboard: copy the held NES virtual-key flags into keys[]. The shim reads keys[VK_UP], keys['X'], etc.
    for (int vk = 0; vk < 256; ++vk)
        if (keyHeld_[(size_t)vk]) in.keys[vk] = 1;

    // Physical controller (single-player / port 0 in 2b): poll once, then OR each held RetroPad button into the
    // SAME NES key bytes via the shared mapper, so a pad and the keyboard drive identical controls. A no-op when
    // no controller is connected or SDL isn't compiled in.
    pad_.poll();
    for (unsigned id = 0; id < (unsigned)Gamepad::kRetroPadButtons; ++id) {
        int vk = 0;
        if (rpinput::nesVkForRetroPad(id, vk) && pad_.button(0, id))
            in.keys[vk] = 1;
    }

    rp_runtime_set_input(rt_, 0, &in);   // port 0 only in 2b
#endif
}

void RetroParkView::clearHeldKeys()
{
    keyHeld_.fill(false);
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

    // Gameplay: if this key is one of the NES controls the shim reads, mark it held (feedInput() copies keyHeld_
    // into keys[] each tick). Auto-repeat presses are no-ops — the key is already held from the first press, and
    // release clears it. We do NOT consume other keys, so anything else still reaches the base class.
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

void RetroParkView::showMenu()
{
    if (!running_) return;
#ifdef EB_HAVE_RETROPARK
    if (rt_) rp_runtime_pause(rt_);   // driven: advancing stops; the last frame stays composited
#endif
    if (timer_) timer_->stop();        // stop pumping present() while paused
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
    romPath_.clear();
    clearHeldKeys();
    update();
    if (was) emit gameStopped();  // only when a game was actually running (stop() is safe to call when idle)
}
