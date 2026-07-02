#include "RetroView.h"
#include "../core/AppPaths.h"
#include "../core/CoreManager.h"
#include "../core/Settings.h"
#include "../core/Achievements.h"
#include <QTimer>
#include <QPainter>
#include <QKeyEvent>
#include <QAudioSink>
#include <QAudioFormat>
#include <QAudioDevice>
#include <QMediaDevices>
#include <QIODevice>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QCoreApplication>
#include <QFrame>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QThread>
#include <cstring>

RetroView::RetroView(QWidget* parent) : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, Qt::black);
    setPalette(pal);

    timer_ = new QTimer(this);
    connect(timer_, &QTimer::timeout, this, &RetroView::tick);

    buildMenu();
}

void RetroView::buildMenu()
{
    menu_ = new QFrame(this);
    menu_->setObjectName(QStringLiteral("emuMenu"));
    menu_->setStyleSheet(QStringLiteral(
        "#emuMenu { background: rgba(20,20,24,0.94); border: 1px solid rgba(255,255,255,0.15); border-radius: 12px; }"
        "#emuMenu QPushButton { padding: 9px 18px; font-size: 15px; }"
        "#emuMenu QLabel { color: #e8e8e8; }"));
    auto* v = new QVBoxLayout(menu_);
    v->setContentsMargins(20, 18, 20, 18);
    v->setSpacing(8);

    auto* title = new QLabel(tr("Paused"), menu_);
    title->setAlignment(Qt::AlignCenter);
    title->setStyleSheet(QStringLiteral("font-size:18px; font-weight:600;"));
    v->addWidget(title);

    auto* resume = new QPushButton(tr("Resume"), menu_);
    auto* save   = new QPushButton(tr("Save State"), menu_);
    auto* load   = new QPushButton(tr("Load State"), menu_);
    auto* exit   = new QPushButton(tr("Exit Emulator"), menu_);
    for (QPushButton* b : { resume, save, load, exit }) v->addWidget(b);

    menuStatus_ = new QLabel(QString(), menu_);
    menuStatus_->setAlignment(Qt::AlignCenter);
    menuStatus_->setStyleSheet(QStringLiteral("color:#aaa; font-size:12px;"));
    v->addWidget(menuStatus_);

    connect(resume, &QPushButton::clicked, this, &RetroView::hideMenu);
    connect(exit,   &QPushButton::clicked, this, [this] { hideMenu(); emit exitRequested(); });
    connect(save,   &QPushButton::clicked, this, [this] {
        QString e; menuStatus_->setText(saveState(&e) ? tr("State saved") : e); });
    connect(load,   &QPushButton::clicked, this, [this] {
        QString e; menuStatus_->setText(loadState(&e) ? tr("State loaded") : e); });

    menu_->hide();
}

RetroView::~RetroView() { stop(); }

bool RetroView::openGame(const QString& corePath, const QString& romPath,
                         const QString& coreName, QString* error)
{
    stop();
    // Point the core at <data>/system for BIOS / firmware before it loads (cores read the system directory
    // during set_environment). MainWindow has already fetched any required BIOS there (CoreManager::ensureBios).
    core_.systemDir = CoreManager::systemDir().toStdString();
    std::string err;
    if (!core_.loadCore(corePath.toStdString(), &err))
    {
        if (error) *error = QString::fromStdString(err);
        return false;
    }
    // Apply the user's saved per-core options before the game loads, so the core picks them up the
    // first time it reads them (resolution, BIOS, region, ...).
    if (!coreName.isEmpty())
        for (const CoreOption& opt : core_.options())
        {
            const QString v = Settings::optionValue(coreName, QString::fromStdString(opt.key));
            if (!v.isEmpty())
                core_.setOptionValue(opt.key, v.toStdString());
        }
    core_.onInput = [this](unsigned p, unsigned d, unsigned i, unsigned id) { return inputState(p, d, i, id); };
    core_.onAudio = [this](const int16_t* data, size_t frames) { pushAudio(data, frames); };
    core_.onRumble = [this](unsigned port, unsigned effect, uint16_t strength) {
        // In threaded mode the core fires this on the worker thread; the pad (SDL) is owned by the GUI thread.
        if (threaded_) QMetaObject::invokeMethod(this, [this, port, effect, strength] { pad_.setRumble(port, effect, strength); }, Qt::QueuedConnection);
        else pad_.setRumble(port, effect, strength);
    };
    if (!core_.loadGame(romPath.toStdString(), &err))
    {
        if (error) *error = QString::fromStdString(err);
        core_.unload();
        return false;
    }
    romPath_ = romPath;
    loadSram(); // restore battery-backed in-game saves before the game starts
    double fps = core_.avInfo().timing.fps;
    if (fps <= 0.0) fps = 60.0;
    portsMask_ = -1;            // force a fresh port setup for this game (done here while nothing else runs)
    updateControllerPorts();
    loadTurbo();
    frameIntervalMs_ = qMax(1, static_cast<int>(1000.0 / fps));
    paused_ = false;
    running_ = true;
    startEmu();                 // GUI timer, or a dedicated worker thread in threaded (split-pane) mode
    setFocus();
    // RetroAchievements: identify this game and start watching memory (no-op if not logged in / unsupported).
    if (ach_)
        ach_->loadGame(&core_, Achievements::consoleIdForExtension(QFileInfo(romPath).suffix().toLower()), romPath);
    return true;
}

void RetroView::startEmu()
{
    if (!threaded_)
    {
        startAudio(static_cast<int>(core_.avInfo().timing.sample_rate));
        timer_->start(frameIntervalMs_);
        return;
    }
    // Threaded: emulate on a worker thread so the other split pane's video rendering on the GUI thread can't
    // throttle the game. The pacer + audio live on the worker; input is snapshotted from the GUI; frames are
    // handed back for the GUI to paint.
    const int sr = static_cast<int>(core_.avInfo().timing.sample_rate);
    emuThread_ = new QThread(this);
    emuTimer_ = new QTimer();                 // no parent; affined to the worker thread below
    emuTimer_->setInterval(frameIntervalMs_);
    emuTimer_->moveToThread(emuThread_);
    connect(emuTimer_, &QTimer::timeout, this, &RetroView::stepWorker, Qt::DirectConnection); // runs on emuThread_
    connect(emuThread_, &QThread::started, this, [this, sr] { startAudio(sr); emuTimer_->start(); }, Qt::DirectConnection);
    if (!inputTimer_) { inputTimer_ = new QTimer(this); connect(inputTimer_, &QTimer::timeout, this, &RetroView::pollInput); }
    inputTimer_->start(frameIntervalMs_);
    emuThread_->start();
}

void RetroView::stopEmu()
{
    if (!threaded_)
    {
        if (timer_) timer_->stop();
        stopAudio();
        return;
    }
    if (inputTimer_) inputTimer_->stop();
    if (emuThread_)
    {
        // Stop the pacer + audio ON the worker thread, then join so no frame is in flight before we unload.
        QMetaObject::invokeMethod(emuTimer_, [this] { emuTimer_->stop(); }, Qt::BlockingQueuedConnection);
        QMetaObject::invokeMethod(emuTimer_, [this] { stopAudio(); }, Qt::BlockingQueuedConnection);
        emuThread_->quit();
        emuThread_->wait();
        delete emuTimer_;  emuTimer_ = nullptr;
        delete emuThread_; emuThread_ = nullptr;
    }
}

void RetroView::stepWorker() // runs on emuThread_
{
    if (!running_ || paused_) return;
    core_.runFrame();
    if (core_.crashed())
    {
        running_ = false;
        QMetaObject::invokeMethod(this, [this] {
            stop(); emit statusMessage(tr("The emulator core crashed and was stopped.")); }, Qt::QueuedConnection);
        return;
    }
    publishFrame();
    if (++sramAutosaveCounter_ >= 600) { sramAutosaveCounter_ = 0; saveSram(); } // worker owns the core here
}

void RetroView::publishFrame() // worker -> GUI handoff
{
    const unsigned w = core_.frameWidth(), h = core_.frameHeight();
    if (!core_.frameBGRA() || !w || !h) return;
    {
        QMutexLocker lk(&frameMutex_);
        frameImg_ = QImage(core_.frameBGRA(), int(w), int(h), int(w * 4), QImage::Format_RGB32).copy();
    }
    QMetaObject::invokeMethod(this, [this] { update(); }, Qt::QueuedConnection);
}

void RetroView::pollInput() // GUI: poll the pad + keyboard, resolve, and publish a snapshot for the worker
{
    pad_.poll();
    if (++turboCounter_ >= 2 * turboHalfPeriod_) turboCounter_ = 0;
    turboOn_ = turboCounter_ < turboHalfPeriod_;
    int btn[4] = { 0, 0, 0, 0 }; int16_t ax[4][2][2] = {};
    for (unsigned p = 0; p < Gamepad::kMaxPlayers && p < 4; ++p)
    {
        for (unsigned id = 0; id < Gamepad::kRetroPadButtons && id < 32; ++id)
            if (resolveInput(p, RETRO_DEVICE_JOYPAD, 0, id)) btn[p] |= (1 << id);
        for (unsigned idx = 0; idx < 2; ++idx)
            for (unsigned id = 0; id < 2; ++id) ax[p][idx][id] = resolveInput(p, RETRO_DEVICE_ANALOG, idx, id);
    }
    QMutexLocker lk(&inputMutex_);
    for (int p = 0; p < 4; ++p) { snapBtn_[p] = btn[p];
        for (int i = 0; i < 2; ++i) for (int j = 0; j < 2; ++j) snapAxis_[p][i][j] = ax[p][i][j]; }
}

void RetroView::stop()
{
    if (core_.gameLoaded()) saveSram(); // persist battery RAM before tearing the core down
    if (ach_) ach_->unloadGame();
    running_ = false;
    stopEmu();          // stop the GUI timer or the worker thread (+ its audio); no core access afterward
    paused_ = false;
    hideMenu();
    pressedKeys_.clear();
    pad_.stopRumble();
    core_.unload();
    update();
}

void RetroView::setPaused(bool paused)
{
    paused_ = paused;
    if (threaded_)
    {
        if (emuTimer_)
            QMetaObject::invokeMethod(emuTimer_, [this, paused] {
                if (paused) emuTimer_->stop(); else if (running_) emuTimer_->start(frameIntervalMs_); }, Qt::QueuedConnection);
        return;
    }
    if (!timer_) return;
    if (paused) timer_->stop();
    else if (running_) timer_->start(frameIntervalMs_);
}

void RetroView::toggleMenu()
{
    if (!running_) return;
    if (menu_->isVisible()) hideMenu();
    else showMenu();
}

void RetroView::showMenu()
{
    if (!running_) return;
    setPaused(true);
    menuStatus_->clear();
    menu_->adjustSize();
    menu_->move((width() - menu_->width()) / 2, (height() - menu_->height()) / 2);
    menu_->show();
    menu_->raise();
}

void RetroView::hideMenu()
{
    menu_->hide();
    setPaused(false);
    setFocus(); // keep Esc / gameplay keys coming to the view
}

void RetroView::resizeEvent(QResizeEvent*)
{
    if (menu_ && menu_->isVisible())
        menu_->move((width() - menu_->width()) / 2, (height() - menu_->height()) / 2);
}

void RetroView::tick()
{
    if (!running_) return;
    pad_.poll();        // refresh controller state + handle hot-plug before the core reads input
    updateControllerPorts(); // pick up controllers plugged in/out mid-game
    // Advance the autofire phase: on for turboHalfPeriod_ frames, then off for the same.
    if (++turboCounter_ >= 2 * turboHalfPeriod_) turboCounter_ = 0;
    turboOn_ = turboCounter_ < turboHalfPeriod_;
    core_.runFrame();   // audio is pushed via core_.onAudio
    if (core_.crashed()) // a hard fault inside the core was caught; stop instead of faulting every frame
    {
        stop();
        emit statusMessage(tr("The emulator core crashed and was stopped."));
        return;
    }
    if (ach_ && !paused_) ach_->doFrame(); // evaluate RetroAchievements against this frame's memory
    if (++sramAutosaveCounter_ >= 600) { sramAutosaveCounter_ = 0; saveSram(); } // ~10s autosave (crash safety)
    update();
}

QString RetroView::statePath() const
{
    const QString dir = AppPaths::dataDir() + QStringLiteral("/states");
    QDir().mkpath(dir);
    return dir + QStringLiteral("/") + QFileInfo(romPath_).completeBaseName() + QStringLiteral(".state");
}

QString RetroView::sramPath() const
{
    const QString dir = AppPaths::dataDir() + QStringLiteral("/saves");
    QDir().mkpath(dir);
    return dir + QStringLiteral("/") + QFileInfo(romPath_).completeBaseName() + QStringLiteral(".srm");
}

// Battery-backed RAM (in-game saves) is frontend-managed: restore it into the core's SAVE_RAM after loading,
// and write it back out so progress survives closing the game (and can sync to Drive).
void RetroView::loadSram()
{
    void* dst = core_.memoryData(RETRO_MEMORY_SAVE_RAM);
    const size_t sz = core_.memorySize(RETRO_MEMORY_SAVE_RAM);
    if (!dst || sz == 0) return; // this game has no battery save
    QFile f(sramPath());
    if (!f.open(QIODevice::ReadOnly)) return;
    const QByteArray bytes = f.readAll();
    std::memcpy(dst, bytes.constData(), qMin(size_t(bytes.size()), sz));
}

void RetroView::saveSram()
{
    const void* src = core_.memoryData(RETRO_MEMORY_SAVE_RAM);
    const size_t sz = core_.memorySize(RETRO_MEMORY_SAVE_RAM);
    if (!src || sz == 0) return;
    QFile f(sramPath());
    if (f.open(QIODevice::WriteOnly))
        f.write(reinterpret_cast<const char*>(src), qint64(sz));
}

bool RetroView::saveState(QString* error)
{
    if (!running_) { if (error) *error = tr("No game is running."); return false; }
    if (threaded_) { if (error) *error = tr("Save states aren’t available in split screen."); return false; }
    std::vector<uint8_t> data;
    if (!core_.saveState(data))
    {
        if (error) *error = tr("This core doesn't support save states for this game.");
        return false;
    }
    QFile f(statePath());
    if (!f.open(QIODevice::WriteOnly) ||
        f.write(reinterpret_cast<const char*>(data.data()), static_cast<qint64>(data.size())) != static_cast<qint64>(data.size()))
    {
        if (error) *error = tr("Couldn't write the save-state file.");
        return false;
    }
    emit statusMessage(tr("State saved"));
    return true;
}

bool RetroView::loadState(QString* error)
{
    if (!running_) { if (error) *error = tr("No game is running."); return false; }
    if (threaded_) { if (error) *error = tr("Save states aren’t available in split screen."); return false; }
    QFile f(statePath());
    if (!f.exists()) { if (error) *error = tr("No saved state for this game yet."); return false; }
    if (!f.open(QIODevice::ReadOnly)) { if (error) *error = tr("Couldn't read the save-state file."); return false; }
    const QByteArray bytes = f.readAll();
    if (!core_.loadState(reinterpret_cast<const uint8_t*>(bytes.constData()), static_cast<size_t>(bytes.size())))
    {
        if (error) *error = tr("The saved state couldn't be restored (it may be from a different core).");
        return false;
    }
    emit statusMessage(tr("State loaded"));
    return true;
}

void RetroView::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), Qt::black);

    if (threaded_) // paint the worker's last handed-off frame (never touch the core from the GUI thread)
    {
        QMutexLocker lk(&frameMutex_);
        if (frameImg_.isNull()) return;
        const QSize t = frameImg_.size().scaled(size(), Qt::KeepAspectRatio);
        const QRect dst(QPoint((width() - t.width()) / 2, (height() - t.height()) / 2), t);
        p.setRenderHint(QPainter::SmoothPixmapTransform, false);
        p.drawImage(dst, frameImg_);
        return;
    }

    if (!core_.hasFrame()) return;

    const unsigned w = core_.frameWidth(), h = core_.frameHeight();
    // frameBGRA() is tightly packed BGRA == QImage::Format_RGB32 byte order on little-endian.
    QImage img(core_.frameBGRA(), static_cast<int>(w), static_cast<int>(h),
               static_cast<int>(w * 4), QImage::Format_RGB32);

    const QSize target = img.size().scaled(size(), Qt::KeepAspectRatio);
    const QRect dst(QPoint((width() - target.width()) / 2, (height() - target.height()) / 2), target);
    p.setRenderHint(QPainter::SmoothPixmapTransform, false); // crisp, non-blurry pixels
    p.drawImage(dst, img);
}

void RetroView::keyPressEvent(QKeyEvent* e)
{
    if (e->isAutoRepeat()) return;

    // Esc toggles the in-game pause menu (Resume / Save / Load / Exit).
    if (e->key() == Qt::Key_Escape) { toggleMenu(); return; }

    // Save-state hotkeys (RetroArch-style: F2 save, F4 load) - reserved, not remappable.
    if (e->key() == Qt::Key_F2) { QString err; if (!saveState(&err)) emit statusMessage(err); return; }
    if (e->key() == Qt::Key_F4) { QString err; if (!loadState(&err)) emit statusMessage(err); return; }

    pressedKeys_.insert(e->key()); // resolved to a (port, button) per the keymap in inputState()
}

void RetroView::keyReleaseEvent(QKeyEvent* e)
{
    if (e->isAutoRepeat()) return;
    pressedKeys_.erase(e->key());
}

int16_t RetroView::inputState(unsigned port, unsigned device, unsigned index, unsigned id)
{
    // Threaded mode: this runs on the worker thread, so read the snapshot the GUI publishes (pollInput),
    // never the live pad/keyboard (owned by the GUI thread).
    if (threaded_)
    {
        if (port >= 4) return 0;
        QMutexLocker lk(&inputMutex_);
        if (device == RETRO_DEVICE_JOYPAD) return (snapBtn_[port] >> id) & 1;
        if (device == RETRO_DEVICE_ANALOG && index < 2 && id < 2) return snapAxis_[port][index][id];
        return 0;
    }
    return resolveInput(port, device, index, id);
}

int16_t RetroView::resolveInput(unsigned port, unsigned device, unsigned index, unsigned id)
{
    if (!inputActive_) return 0; // split screen: only the focused pane's game receives controller/keyboard
    if (port >= Gamepad::kMaxPlayers) return 0;
    if (device == RETRO_DEVICE_JOYPAD)
    {
        const int k = keymap_.key(port, id); // this player's keyboard binding for this button
        const bool kb = (k != Keymap::kUnbound) && pressedKeys_.count(k);
        const bool held = kb || pad_.button(port, id);
        if (!held) return 0;
        // Turbo buttons only register on the "on" half of the autofire cycle while held.
        if (turboMask_[port] & (1 << id)) return turboOn_ ? 1 : 0;
        return 1;
    }
    if (device == RETRO_DEVICE_ANALOG)
        return pad_.axis(port, index, id); // analog sticks (keyboard has none)
    return 0;
}

void RetroView::setVolume(qreal v) // 0.0..1.0; lets each split pane mix at its own level
{
    volume_ = qBound(0.0, v, 1.0);
    if (threaded_ && emuTimer_) // the sink lives on the worker thread - apply there
        QMetaObject::invokeMethod(emuTimer_, [this] { if (audioSink_) audioSink_->setVolume(volume_); }, Qt::QueuedConnection);
    else if (audioSink_)
        audioSink_->setVolume(volume_);
}

void RetroView::setInputActive(bool active)
{
    inputActive_ = active;
    if (!active) pressedKeys_.clear(); // drop any held keys so they don't stick when focus leaves
}

void RetroView::loadTurbo()
{
    turboHalfPeriod_ = Settings::turboHalfPeriod();
    turboCounter_ = 0;
    turboOn_ = true;
    for (int p = 0; p < Gamepad::kMaxPlayers; ++p)
    {
        int mask = 0;
        for (int id = 0; id < Gamepad::kRetroPadButtons; ++id)
            if (Settings::turboButton(p, id)) mask |= (1 << id);
        turboMask_[p] = mask;
    }
}

void RetroView::updateControllerPorts()
{
    int mask = 1; // player 1 (port 0) is always active - keyboard and/or the first controller
    for (unsigned p = 1; p < Gamepad::kMaxPlayers; ++p)
        if (pad_.portConnected(p)) mask |= (1 << p);
    if (mask == portsMask_) return;
    portsMask_ = mask;

    // Tell the core which ports have a player, so it enables 2-4 player modes where supported.
    for (unsigned p = 0; p < Gamepad::kMaxPlayers; ++p)
        core_.setControllerPortDevice(p, (mask & (1 << p)) ? RETRO_DEVICE_JOYPAD : RETRO_DEVICE_NONE);
}

void RetroView::startAudio(int sampleRate)
{
    stopAudio();
    if (sampleRate <= 0) return;

    const QAudioDevice dev = QMediaDevices::defaultAudioOutput();
    if (dev.isNull()) return; // no output device -> run silent

    // Output at a rate the device actually supports (its native rate), and resample the core to it. Handing
    // QAudioSink an unsupported rate - SNES's 32040 Hz especially - gives static/garbled audio on Windows.
    auto supports = [&dev](int rate) {
        QAudioFormat f; f.setSampleRate(rate); f.setChannelCount(2); f.setSampleFormat(QAudioFormat::Int16);
        return dev.isFormatSupported(f);
    };
    int outRate = dev.preferredFormat().sampleRate();
    if (outRate <= 0 || !supports(outRate)) outRate = supports(48000) ? 48000 : (supports(44100) ? 44100 : sampleRate);

    QAudioFormat fmt;
    fmt.setSampleRate(outRate);
    fmt.setChannelCount(2);
    fmt.setSampleFormat(QAudioFormat::Int16);

    audioSink_ = new QAudioSink(dev, fmt, this);
    audioSink_->setBufferSize(fmt.bytesForDuration(100000)); // ~100 ms
    audioSink_->setVolume(volume_);                          // per-pane mix level (1.0 = full)
    audioSrcRate_ = sampleRate;
    audioOutRate_ = outRate;
    audioBytesPerSec_ = outRate * 2 * 2;                     // stereo S16 at the OUTPUT rate
    rsStep_ = double(sampleRate) / double(outRate);
    rsPos_ = 0.0; rsPrev_[0] = rsPrev_[1] = 0;
    audioIo_ = audioSink_->start();                          // push mode: write samples to this
}

// Linear-resample interleaved S16 stereo from audioSrcRate_ to audioOutRate_, carrying the fractional read
// position and the previous frame across calls so there's no click at buffer boundaries.
void RetroView::resampleAppend(const int16_t* in, size_t frames)
{
    const size_t N = frames;
    double pos = rsPos_; // stream index: 0 = rsPrev_, k>=1 = in[k-1]
    while (pos < double(N))
    {
        const int i = int(pos);
        const double f = pos - i;
        const int16_t aL = (i == 0) ? rsPrev_[0] : in[(i - 1) * 2];
        const int16_t aR = (i == 0) ? rsPrev_[1] : in[(i - 1) * 2 + 1];
        const int16_t bL = in[i * 2];
        const int16_t bR = in[i * 2 + 1];
        const int16_t o[2] = { int16_t(aL + (bL - aL) * f), int16_t(aR + (bR - aR) * f) };
        pendingAudio_.append(reinterpret_cast<const char*>(o), 4);
        pos += rsStep_;
    }
    rsPrev_[0] = in[(N - 1) * 2]; rsPrev_[1] = in[(N - 1) * 2 + 1];
    rsPos_ = pos - double(N);                 // shift baseline: old index N (last frame) becomes new index 0
    if (rsPos_ < 0.0) rsPos_ = 0.0;
}

void RetroView::stopAudio()
{
    if (audioSink_)
    {
        audioSink_->stop();
        delete audioSink_;
        audioSink_ = nullptr;
    }
    audioIo_ = nullptr;
    pendingAudio_.clear();
    audioSrcRate_ = audioOutRate_ = 0;
    rsPos_ = 0.0; rsPrev_[0] = rsPrev_[1] = 0;
}

// Called from the core during runFrame() (GUI thread): interleaved S16 stereo, 'frames' stereo samples.
void RetroView::pushAudio(const int16_t* data, size_t frames)
{
    if (!audioIo_ || frames == 0) return;
    if (audioSrcRate_ == audioOutRate_)
        pendingAudio_.append(reinterpret_cast<const char*>(data), static_cast<qsizetype>(frames) * 4); // 4 bytes/frame
    else
        resampleAppend(data, frames); // core rate (e.g. 32040) -> device native rate (e.g. 48000)

    const qint64 freeBytes = audioSink_->bytesFree();
    if (freeBytes > 0 && !pendingAudio_.isEmpty())
    {
        const qint64 n = qMin<qint64>(freeBytes, pendingAudio_.size());
        const qint64 written = audioIo_->write(pendingAudio_.constData(), n);
        if (written > 0) pendingAudio_.remove(0, written);
    }
    // Bound the backlog (~120 ms) so audio can't drift behind the picture.
    const qsizetype maxBacklog = static_cast<qsizetype>(audioBytesPerSec_) * 120 / 1000;
    if (audioBytesPerSec_ > 0 && pendingAudio_.size() > maxBacklog)
        pendingAudio_.remove(0, pendingAudio_.size() - maxBacklog);
}
