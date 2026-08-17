// Runs a libretro core and paints its frames into this widget. Keyboard -> RetroPad input.
// Software blit (QImage) - fine for 2D cores; a GL/texture path comes with hardware cores later.
#pragma once
#include <QWidget>
#include <QByteArray>
#include <QImage>
#include <QRect>
#include <QMutex>
#include <QVector>
#include <QElapsedTimer>
#include <set>
#include <deque>
#include <vector>
#include <functional>
#include <cstdint>
#include <QHash>
#include "LibretroCore.h"   // everythingbox_libretro PUBLIC include dir (src/libretro)
#include "../input/Gamepad.h"
#include "../input/Keymap.h"
#include "CheatSearch.h"    // pure cheat-search engine (#96): snapshot RAM -> narrow candidates -> freeze
#include "../core/Hardcore.h" // the ONE hardcore-mode policy (#94): which affordances a hardcore session forbids

class QTimer;
class QThread;
class QAudioSink;
class QIODevice;
class QFrame;
class QLabel;
class QPushButton;
class QVBoxLayout;
class QScrollArea;
class QOpenGLContext;
class QOffscreenSurface;
class QOpenGLFramebufferObject;
class QNetworkAccessManager;
class NetplaySession;
class PortMapper;
class VirtualPad;
class ShaderRenderer;

class RetroView : public QWidget
{
    Q_OBJECT
public:
    explicit RetroView(QWidget* parent = nullptr);
    ~RetroView() override;

    // coreName is the bare core id (e.g. "mgba") used to look up the user's saved per-core options.
    //
    // title/systemId are the launching catalog item's display name and the system it was launched FROM. They
    // are parameters rather than a setter called beforehand because openGame() begins by stop()ping whatever
    // is running, and stop() writes the OUTGOING game's battery RAM: identity set before the call would file
    // that write under the incoming game's system. Both are optional — an omitted systemId falls back to the
    // extension lookup (ambiguous for extensions several systems share, which is why the launcher passes its
    // resolved one) and an omitted title to the ROM's base name.
    // gameKey is the launching catalog item's stable identity (its addon item id, else "") — the same key
    // PlayStats/RecentStore de-dup on. It keys this game's per-game overrides (issue #95: core-option and
    // input-remap deltas); an empty key falls back to the ROM path, matching PlayStats::identity.
    bool openGame(const QString& corePath, const QString& romPath,
                  const QString& coreName = QString(), QString* error = nullptr,
                  const QString& title = QString(), const QString& systemId = QString(),
                  const QString& gameKey = QString());
    void stop();
    bool running() const { return running_; }
    bool paused()  const { return paused_; }   // freeze state (Esc menu + OS-lifecycle pause query)

    // Quick save/load (F2/F4) to the current slot under <app>/states. Return false (with *error set) if the
    // core can't serialize, nothing is running, or file I/O fails.
    bool saveState(QString* error = nullptr);
    bool loadState(QString* error = nullptr);
    // Save/load a specific numbered slot (1..kStateSlots). saveState also writes a PNG thumbnail of the frame.
    bool saveState(int slot, QString* error = nullptr);
    bool loadState(int slot, QString* error = nullptr);
    // The user-slot ceiling. Raised well past the old 6 (#93) so a long playthrough never has to sacrifice a
    // slot; the grid PAGINATES (kSlotsPerPage a page) rather than growing without bound. The state format does
    // not care about the count — this is purely a UI ceiling.
    static constexpr int kStateSlots = 50;
    static constexpr int kSlotsPerPage = 10;

    Gamepad* gamepad() { return &pad_; }  // for the controller-remapping UI
    Keymap*  keymap()  { return &keymap_; } // for the keyboard-remapping UI
    // The running game's per-game-override token (#95), "" when nothing is running. Lets the input-mapping
    // panel offer a "This game" remap scope keyed to the live game, and label it.
    QString  overrideToken() const { return running_ ? overrideToken_ : QString(); }
    QString  currentGameTitle() const { return running_ ? gameTitle_ : QString(); }

    void setPaused(bool paused);          // freeze/resume emulation (used by the Esc menu)
    void setVolume(qreal v);              // 0.0..1.0 audio level (per-pane mixing in split screen)
    void setInputActive(bool active);     // when false, ignore controller/keyboard (unfocused split pane)
    void setVirtualPadMask(quint32 m) { virtualPad_ = m; } // on-screen pad -> held RetroPad bits (port 0)
    void setAchievements(class Achievements* a) { ach_ = a; } // RetroAchievements (full-screen emulator only)
    // Show a RetroAchievements unlock toast (badge + title + points) over the game. Queues if one is already up.
    void showAchievement(const QString& title, const QString& description, int points, const QString& badgeUrl);
    // Run emulation on a dedicated worker thread instead of the GUI timer. Used for split-screen panes so the
    // game isn't throttled by the other pane's video rendering on the shared GUI thread. Call before openGame.
    void setThreaded(bool on) { threaded_ = on; }
    // Mark this instance as a split-screen pane. Disables the user-facing feature restrictions that key off
    // being a split pane (save states, auto-resume, save-on-exit) — NOT the worker-thread mechanics, which
    // stay on setThreaded(). Split screen calls both; a threaded single-player will call only setThreaded().
    void setSplitPane(bool on) { splitPane_ = on; }

signals:
    void statusMessage(const QString& text); // surfaced by the main window (save/load feedback) — ambient 3000 ms
    void coreError(const QString& text);     // a hard core error (crash) — error-class notice, kFeedbackLong (J10)
    void exitRequested();                    // the Esc menu's "Exit" - main window stops + returns Home
    void gameStopped();                      // a running game was torn down (main window records playtime)
    // A save file or save state was just written to disk (save-sync T5). `relPath` is relative to
    // AppPaths::dataDir() and INCLUDES the "saves/"|"states/" prefix — it is the exact name
    // SaveSync::scanLocal produces and the only shape SaveSync::markDirty matches; a bare file name would
    // match nothing on disk, so the push would find no such file and the save would never retire from the
    // dirty set. Emitted from noteSaveMeta(), which already derives that name for the sidecar.
    // In split-screen the battery-RAM autosave runs on the emulation WORKER thread, so this crosses threads:
    // the receiver is the main window, so an auto connection queues it (QString is a registered metatype).
    void saveWritten(const QString& relPath);

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void keyReleaseEvent(QKeyEvent*) override;

private slots:
    void tick();          // GUI-thread frame step (non-threaded mode)
    void stepWorker();    // worker-thread frame step (threaded mode; runs on emuThread_)
    void pollInput();     // GUI-thread input poll -> snapshot (threaded mode)

private:
    // Retro post-process filters drawn over the emulator image (a cached translucent overlay). Since #99 slice 4
    // the CPU overlay is the GPU-UNAVAILABLE fallback for a shader preset, not a standalone control — filter_ is
    // set each paint from the resolved preset (cpuFilterForPreset), and the pause-menu button now opens the SHADER
    // preset picker (showShaderPicker). The overlay machinery itself is unchanged.
    enum VideoFilter { FilterOff, FilterScanlines, FilterCrt, FilterLcd };
    void loadVideoFilter();               // read the persisted choice into filter_
    void cycleVideoFilter();              // Off -> Scanlines -> CRT -> LCD -> Off, persisted + repainted (legacy)
    QString videoFilterLabel() const;     // "Video Filter: <name>" for the menu button (legacy)
    void applyVideoFilter(QPainter& p, const QRect& dst, int srcW, int srcH); // composite the overlay over dst
    static QImage buildFilterOverlay(QSize dst, int srcW, int srcH, VideoFilter f);
    static VideoFilter cpuFilterForPreset(const QString& presetId); // shader id -> nearest CPU overlay (fallback)

    // ---- slang-shader present pass (issue #99 slice 4/5) --------------------------------------------------
    // The resolved preset (per-game > per-system > global), recomputed on openGame and when the choice changes;
    // read every paint. "off"/"" means the GL path is entirely skipped and the frame draws as before this slice.
    // Since slice 5 the pause-menu button opens a preset PICKER (showShaderPicker) that writes any of the three
    // scopes and can load a user's own .slangp; refreshShaderPreset() then repaints so the choice is the preview.
    void    refreshShaderPreset();        // recompute resolvedShaderPreset_ from the three scopes
    QString shaderPresetLabel() const;    // "Shader: <resolved name>" for the menu button
    void    logShaderFrame(const QString& presetId, double ms); // throttled video-log line with the GPU pass time
    void    showShaderPicker();           // pause sub-page: pick a preset (Off / built-ins / user .slangp) + scope
    // Which store showShaderPicker() writes: per-game override, per-system default, or the global default. Default
    // is Game (the most useful, most specific scope). Persisted only within the running session (a menu state).
    enum class ShaderScope { Game, System, Global };
    ShaderScope shaderScope_ = ShaderScope::Game;
    ShaderRenderer* shaderRenderer_ = nullptr;  // built lazily, ONLY while a non-off preset is active
    QString  resolvedShaderPreset_ = QStringLiteral("off"); // cached resolution; drives the paint path
    QString  shaderGameKey_;              // raw per-game identity (item id, else ROM path) for ShaderPresetStore
    std::size_t shaderFrame_ = 0;         // running frame count handed to the shader (FrameCount effects)
    int      shaderLogTick_ = 0;          // throttles logShaderFrame

    void buildMenu();          // the in-game Esc overlay (Resume / Save / Load / Exit)
    bool runOneCoreFrame();    // advance the core one frame (hw or sw), returns false if it crashed + stopped
    void captureRewind();      // snapshot the current state into the rewind ring buffer (bounded by bytes)

    // Hardcore-mode gate (#94): true when a hardcore RetroAchievements session is active (Achievements::
    // hardcoreActive()) AND the ONE policy (Hardcore.h) forbids `f`. Every gated affordance — save/load state,
    // rewind, fast-forward, the cheat editor and cheat search — reads THIS one predicate. It is false whenever
    // hardcore is off (no game / softcore), so the non-hardcore path stays byte-for-byte unchanged.
    bool blockedInHardcore(hardcore::Feature f) const;

    // ---- on-screen RetroAchievements unlock toast (badge + title + points, fades in/out over the game) ----
    struct AchToast { QString title; QString sub; QString badgeUrl; };
    void startNextToast();                 // pop the queue -> show the next toast (fetch its badge)
    void paintAchievementToast(QPainter& p);
    std::deque<AchToast> achQueue_;        // pending unlocks (shown one at a time)
    AchToast achCur_;                      // the toast currently on screen
    QImage achBadge_;                      // fetched badge for achCur_ (empty until it loads / on failure)
    QElapsedTimer achClock_;               // drives the fade-in / hold / fade-out timing
    QTimer* achTimer_ = nullptr;           // ~30fps repaint while a toast is up (game may be paused)
    QNetworkAccessManager* achNam_ = nullptr;
    bool achActive_ = false;

    // ---- netplay (2-player LAN lockstep; full-screen only) ----
    void showNetplay();                 // pause-menu sub-page: Host / Join
    void startNetplay(bool asHost, const QString& hostAddr = QString());     // LAN (direct)
    void startNetplayOnline(bool asHost, const QString& code);               // online via the relay (room code)
    void ensureNetSession();            // build net_ + wire its callbacks/signals once
    void netTick();                     // the frame loop while netplay is active
    quint16 captureLocalButtons();      // this peer's RetroPad button mask (port-0 controls)
    NetplaySession* net_ = nullptr;
    PortMapper* portMapper_ = nullptr;  // UPnP-IGD auto port-forward for online hosting (best-effort)
    bool netActive_ = false;
    unsigned netLocalPort_ = 0, netRemotePort_ = 1;
    quint32 netFrame_ = 0, netGenFrame_ = 0;
    quint16 netCurLocal_ = 0, netCurRemote_ = 0;
    QHash<quint32, quint16> netLocalInputs_;
    static constexpr int kNetDelay = 3; // input-delay frames
    void startEmu();           // begin the frame loop after a game loads (GUI timer or worker thread)
    void stopEmu();            // stop the loop / tear down the worker thread
    // Serialize a GUI-initiated core touch with the worker's frame loop. When the core lives on emuThread_ this
    // runs fn there via a BLOCKING queued call (between stepWorker frames, never concurrent with core_.runFrame);
    // otherwise (single-player-not-threaded / any non-worker case) it runs fn inline. Call only from the GUI thread.
    void runOnCore(const std::function<void()>& fn);
    void publishFrame();       // copy the core's frame for the GUI to paint (threaded mode)
    int16_t resolveInput(unsigned port, unsigned device, unsigned index, unsigned id); // raw input resolve (GUI)
    void toggleMenu();
    void showMenu();
    void hideMenu();
    void showMainMenu();                // pause menu: main page (Resume / Save / Load / Exit)
    void showStateSlots(bool saveMode); // pause menu: the slot grid, in save or load mode
    void showDisk();                    // pause menu: disk control (eject / insert / switch side)
    void showCoreOptions();             // pause menu: live libretro core options (cycle each value)
    void showCheats();                  // pause menu: the per-game cheat list (toggle / add / remove)
    void addCheatDialog();              // prompt for a new cheat code + description
    // ---- Cheat Search (#96): scan system RAM to CREATE a cheat, rather than enter a known code. ----
    void showCheatSearch();             // pause menu: the search sub-page (peer of showCheats)
    QByteArray snapshotSystemRam(); // copy of core system RAM now (empty if the core exposes none); marshals via runOnCore
    void csStart();                     // begin a search: snapshot RAM, seed the candidate universe
    void csReset();                     // abandon the current search (back to width/sign selection)
    void csDoExact(std::int64_t value); // narrow: keep addresses whose current value == `value`
    void csDoRelational(cheatsearch::Filter f); // narrow: keep addresses matching a change since last snapshot
    void csFreeze(std::size_t addr);    // freeze a survivor: add an address-freeze cheat + save it named
    void applyFreezeCheats();           // per-frame: write each enabled address-freeze into system RAM
    QString cheatsPath() const;         // <app>/cheats/<romBaseName>.json
    void loadCheats();                  // read this game's cheats from disk
    void saveCheats();                  // persist this game's cheats
    void applyCheats();                 // push the enabled cheats into the running core
    QImage currentFrameImage();         // a copy of the frame currently on screen, for a slot thumbnail
    QString captureScreenshot();        // save the current (clean, unscaled) frame to <app>/screenshots; "" on fail
    QString statePath() const;          // <app>/states/<romBaseName>.state  (legacy single slot)
    QString statePath(int slot) const;  // <app>/states/<romBaseName>.stateN
    QString thumbPath(int slot) const;  // <app>/states/<romBaseName>.stateN.png

    // ---- Save-on-exit / resume (#93). The RESERVED auto-slot lives at its OWN path, never a numbered slot,
    // so a save-on-exit can never clobber a user's manual save and the user grid never offers it. ----
    QString autoStatePath() const;      // <app>/states/<romBaseName>.state.auto  (reserved auto-slot)
    QString autoStateMetaPath() const;  // <app>/states/<romBaseName>.state.auto.json (ROM mtime+size sidecar)
    bool writeAutoState();              // serialize the core into the reserved auto-slot + stamp the ROM's mtime/size
    bool autoStateResumable() const;    // an auto-state exists AND its sidecar still matches THIS ROM dump (mtime/size)
    bool loadAutoState(QString* error); // restore the reserved auto-slot into the running core
    void offerResume();                 // on launch: resume silently / prompt / do nothing, per Settings::resumeMode()
    void showResumePrompt();            // the "Resume where you left off?" overlay page (own menu, not a QDialog)
    QString savesRoot() const; // <app>/saves
    // <app>/saves/<system>/<romBaseName>.srm for a NEW save, or the legacy flat <app>/saves/<romBaseName>.srm
    // when that file already exists — see SaveMeta::resolvePath. Never migrates an existing save.
    QString sramPath() const;
    void loadSram();           // restore battery RAM after a game loads
    void saveSram();           // persist battery RAM (on stop, exit, and periodically)
    // Record which GAME a save/state file belongs to, keyed the way SaveSync names it (a path relative to the
    // data dir, "saves/…" or "states/…"). Cheap, best-effort, and the only thing that makes a 40-hex ROM
    // hash's save identifiable later. Also emits saveWritten() with that same key — the sidecar and the sync
    // must never disagree about what a save is called, so both names come from this one derivation.
    // NOT const (it was): it emits, and every caller is already a non-const write path.
    void noteSaveMeta(const QString& absPath);
    int16_t inputState(unsigned port, unsigned device, unsigned index, unsigned id);
    void updateControllerPorts(); // enable/disable core ports 0..3 as controllers come and go
    void loadTurbo();             // read turbo/autofire config from Settings
    void startAudio(int sampleRate);
    void stopAudio();
    void pushAudio(const int16_t* data, size_t frames);

    // ---- hardware (OpenGL) rendering: a GL core renders into an offscreen FBO, which we read back into hwImg_
    // and paint through the normal software path (keeps the compositor happy — no native GL child surface). ----
    void setupHwRender();     // create the offscreen GL context + FBO, wire the core's hooks, call context_reset
    void teardownHwRender();  // context_destroy + tear down the GL objects
    void readbackHwFrame();   // glReadPixels the FBO's used region into hwImg_ (flipped to top-down)
    bool hwMode_ = false;
    QOpenGLContext* glCtx_ = nullptr;
    QOffscreenSurface* glSurface_ = nullptr;
    QOpenGLFramebufferObject* glFbo_ = nullptr;
    QImage hwImg_;            // last HW frame read back from the FBO, ready to paint

    LibretroCore core_;
    Gamepad pad_;             // physical controller (SDL2); merged with the keyboard
    Keymap keymap_;           // keyboard -> RetroPad (remappable)
    QString romPath_;         // current content, for naming its save-state slot
    QString coreName_;        // the bare core id of the running game (for the netplay handshake)
    QString overrideToken_;   // Settings::gameToken of the running game's identity; keys its per-game overrides (#95)
    QString systemId_;        // the running game's system ("nes", "snes", …); namespaces NEW save files
    QString gameTitle_;       // the running game's display name, recorded in the saves-meta sidecar
    QTimer* timer_ = nullptr;
    std::set<int> pressedKeys_; // Qt key codes currently held (resolved per-port via keymap_)
    quint32 virtualPad_ = 0;    // held RetroPad bitmask from the on-screen virtual gamepad (port 0), OR'd in resolveInput
    bool running_ = false;
    bool paused_ = false;
    bool inputActive_ = true; // false = a backgrounded split pane (no controller/keyboard)

    // Black-screen diagnostics: one-shot log when a game first produces video, and a warning if it never does.
    bool firstFrameLogged_ = false;
    int noVideoTicks_ = 0;

    // ---- threaded mode (split-screen panes): emulation runs on emuThread_, painted on the GUI thread ----
    bool threaded_ = false;
    // Split-screen pane marker. Distinct from threaded_: threaded_ means "the core runs on a worker thread"
    // (a mechanism), while splitPane_ means "this is a split-screen pane" (a user-facing feature restriction —
    // save states, auto-resume and save-on-exit are disabled). Today a split pane sets BOTH; a future
    // single-player-on-a-worker-thread mode will set threaded_ only, and must keep save states working.
    bool splitPane_ = false;
    QThread* emuThread_ = nullptr;   // owns emuTimer_ + the audio sink; runs stepWorker()
    QTimer* emuTimer_ = nullptr;     // frame pacer, lives on emuThread_
    QTimer* inputTimer_ = nullptr;   // GUI: poll the pad + build the input snapshot
    QMutex frameMutex_;              // guards frameImg_ (worker writes, GUI paints)
    QImage frameImg_;                // last frame handed from the worker to the GUI
    QMutex inputMutex_;              // guards the input snapshot below
    int snapBtn_[4] = { 0, 0, 0, 0 };        // per-port RetroPad button bitmask (worker reads)
    int16_t snapAxis_[4][2][2] = {};         // per-port analog [index L/R][id X/Y]
    qreal volume_ = 1.0;      // audio mix level for this instance
    class Achievements* ach_ = nullptr; // set only on the full-screen emulator
    int sramAutosaveCounter_ = 0;       // frames since the last battery-RAM autosave
    QByteArray sramSnapshot_;           // exact bytes of the last SRAM we persisted; skip the autosave write when unchanged
    int audioUnderruns_ = 0;            // sink-empty events since the last report (diagnostic; rate-limited log)
    int audioUnderrunTick_ = 0;         // frames since the last underrun report
    int frameIntervalMs_ = 16;
    // Clock-driven frame pacing: a QTimer interval is a whole millisecond, but the true frame period is
    // fractional (NES = 16.639ms), so a fixed 17ms timer runs the game ~2% slow. reschedulePace() targets the
    // exact fractional period against a monotonic clock and picks the integer interval that keeps the long-run
    // average on schedule (16,17,16,17… averaging 16.639). See RetroView.cpp.
    double frameIntervalMsF_ = 16.6667; // exact 1000/fps
    QElapsedTimer paceClock_;           // monotonic reference for the single-player frame loop
    double nextFrameMs_ = 0.0;          // ideal wall-clock time (ms since paceClock_ start) of the next tick
    void reschedulePace();              // recompute + restart timer_ each tick to hold the true frame rate
    int portsMask_ = -1;      // bitmask of player ports currently enabled on the core (-1 = unset)

    QFrame* menu_ = nullptr;        // Esc pause menu overlay
    QLabel* menuTitle_ = nullptr;   // "Paused" / "Save State" / "Load State"
    QLabel* menuStatus_ = nullptr;  // save/load feedback inside the menu
    QWidget* mainPage_ = nullptr;   // the Resume/Save/Load/Exit page
    QWidget* slotsPage_ = nullptr;  // the state-slot grid page (rebuilt each time it's shown)
    QVBoxLayout* menuBody_ = nullptr; // holds mainPage_ then slotsPage_
    bool slotsMode_ = false;        // true while the slot grid (not the main page) is showing
    int currentSlot_ = 1;           // slot F2/F4 act on; follows the last slot used in the visual menu
    int slotPage_ = 0;              // which page of the paginated slot grid is showing (0-based)
    QVector<QPushButton*> mainButtons_; // Resume/Save/Load/Filter/Exit (fixed, on the main page)
    QVector<QPushButton*> menuButtons_; // the current page's buttons, in order, for arrow-key + Enter navigation
    QPushButton* filterBtn_ = nullptr;  // the "Video Filter: X" cycle button on the main page
    QPushButton* diskBtn_ = nullptr;    // "Disk" entry, shown only when the core has a disk-control interface
    QPushButton* optBtn_ = nullptr;     // "Core Options" entry, shown only when the core exposes options
    // Held so showMainMenu() can grey them (disable + drop from nav) while a hardcore session forbids them (#94).
    QPushButton* saveBtn_ = nullptr;        // "Save State"
    QPushButton* loadBtn_ = nullptr;        // "Load State"
    QPushButton* cheatsBtn_ = nullptr;      // "Cheats"
    QPushButton* cheatSearchBtn_ = nullptr; // "Cheat Search" (#96)
    QScrollArea* subScroll_ = nullptr;  // the scroll area of a scrollable sub-page (core options), for focus-follow
    bool coreOptGameScope_ = false;     // Core Options editor scope (#95): false = "this core", true = "this game"

    // ---- on-screen virtual gamepad (touch form factors) ----
    VirtualPad* vpad_ = nullptr;        // child overlay; emits maskChanged -> setVirtualPadMask
    void ensureVirtualPad();            // build vpad_ on demand and wire it
    void updateVirtualPad();            // apply visibility (enabled || Mobile) + opacity, resize to fill
    bool virtualPadShouldShow() const;  // Settings override on, or auto + Mobile form factor
    QPushButton* vpadBtn_ = nullptr;    // Esc-menu "Virtual Pad: Auto/On/Off" cycle row
    QPushButton* vpadOpacityBtn_ = nullptr; // Esc-menu "Pad Opacity: N%" cycle row

    // One per-game cheat. Two flavours share the struct: a code cheat (Game Genie / Action Replay, pushed
    // through the libretro cheat API in applyCheats) and — new in #96 — an address-freeze created by Cheat
    // Search, which writes `value` (width bytes, little-endian) into system RAM at `address` every frame.
    // A freeze carries an empty `code`; a code cheat has isFreeze=false.
    struct Cheat {
        QString desc; QString code; bool enabled = true;
        bool isFreeze = false;          // true = address-freeze (RAM write), false = code cheat
        quint32 address = 0;            // freeze: system-RAM byte offset
        qint64  value = 0;              // freeze: the frozen value (interpreted at `width`, signedness folded in)
        quint8  width = 1;              // freeze: value width in bytes (1/2/4)
    };
    QVector<Cheat> cheats_;             // this game's cheats (persisted per ROM)

    // ---- Cheat Search session state (#96). Empty/inactive until csStart(). ----
    bool csActive_ = false;             // a search is in progress (candidates are being narrowed)
    QByteArray csPrevSnap_;             // the previous RAM snapshot, for relational comparisons
    std::vector<std::size_t> csCands_;  // surviving candidate addresses (sorted ascending)
    cheatsearch::Width csWidth_ = cheatsearch::Width::W8; // scan value width; fixed for the life of a search
    bool csSigned_ = false;             // scan values as signed; fixed for the life of a search
    static constexpr int kCheatSearchListCap = 20; // list survivors (and allow freezing) once <= this many

    VideoFilter filter_ = FilterOff;    // active retro filter
    QImage crtOverlay_;                 // cached filter overlay (rebuilt on size/source/filter change)
    QString crtKey_;                    // cache key for crtOverlay_ (dst size + source dims + filter)
    QImage bezel_;                      // bezel/border art (empty = none/disabled); drawn behind the game
                                        // in flat mode, or ON TOP in viewport mode (see paintEvent)
    QRect  bezelViewport_;              // #106 screen cutout in bezel-native px (invalid = flat overlay,
                                        // exactly the pre-#106 behaviour; valid = scale the game into it)
    int menuPadPrev_ = 0;               // previous frame's menu d-pad/confirm mask (edge detection)
    bool menuComboPrev_ = false;        // previous frame's Start+Select state (toggles the menu)
    int menuPadMask() const;            // bit0=Up bit1=Down bit2=confirm(A/B), held across any connected pad
    void handleMenuPad();               // drive the pause menu from the controller while it's open
    bool menuComboHeld();               // Start+Select held on any connected pad (opens/closes the menu)

    int turboMask_[4] = { 0, 0, 0, 0 }; // per port: bit set = that RetroPad button auto-fires
    int turboHalfPeriod_ = 3; // frames the autofire stays on (and off) each cycle
    int turboCounter_ = 0;
    bool turboOn_ = true;     // current autofire phase (recomputed each frame)

    // Fast-forward (hold Tab / pad Select+R2): run several core frames per tick. Rewind (hold R / pad
    // Select+L2): replay states from a bounded ring buffer captured each frame. Both are full-screen only
    // (disabled in threaded/split-pane mode, like save states).
    bool ffKey_ = false, rewindKey_ = false;   // keyboard hold state
    bool fastForward_ = false, rewinding_ = false; // resolved each frame from keyboard + pad
    std::deque<std::vector<uint8_t>> rewindBuf_;   // recent states, oldest at front
    size_t rewindBytes_ = 0;                       // total bytes held in rewindBuf_
    static constexpr size_t kRewindMaxBytes = 96u * 1024 * 1024; // ~96 MB cap (fewer seconds for big-state cores)
    static constexpr int kFfSpeed = 4;             // fast-forward multiplier

    QAudioSink* audioSink_ = nullptr;
    QIODevice* audioIo_ = nullptr; // push-mode sink input (owned by audioSink_)
    QByteArray pendingAudio_;      // interleaved S16 stereo not yet written
    int audioBytesPerSec_ = 0;
    // Resample the core's (often odd, e.g. SNES 32040 Hz) output to the device's native rate; feeding an
    // unsupported rate to QAudioSink on Windows produces static. Linear interp with carried state.
    int audioSrcRate_ = 0;         // the core's reported sample rate
    int audioOutRate_ = 0;         // the QAudioSink's rate (device native)
    double rsStep_ = 1.0;          // input frames per output frame (src/out); nudged by dynamic rate control
    double rsStepBase_ = 1.0;      // the nominal ratio (src/out); rsStep_ oscillates around this
    double rsIntegral_ = 0.0;      // DRC integral term: accumulates buffer error to cancel steady timer drift
    double rsPos_ = 0.0;           // carried fractional read position
    int16_t rsPrev_[2] = { 0, 0 }; // last input frame from the previous push (for cross-buffer interpolation)
    void resampleAppend(const int16_t* in, size_t frames); // src-rate -> out-rate, appends to pendingAudio_
};
