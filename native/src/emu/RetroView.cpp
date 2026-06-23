#include "RetroView.h"
#include "../core/Settings.h"
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
    core_.onRumble = [this](unsigned port, unsigned effect, uint16_t strength) { pad_.setRumble(port, effect, strength); };
    if (!core_.loadGame(romPath.toStdString(), &err))
    {
        if (error) *error = QString::fromStdString(err);
        core_.unload();
        return false;
    }
    romPath_ = romPath;
    double fps = core_.avInfo().timing.fps;
    if (fps <= 0.0) fps = 60.0;
    startAudio(static_cast<int>(core_.avInfo().timing.sample_rate));
    portsMask_ = -1;            // force a fresh port setup for this game
    updateControllerPorts();
    loadTurbo();
    frameIntervalMs_ = qMax(1, static_cast<int>(1000.0 / fps));
    paused_ = false;
    timer_->start(frameIntervalMs_);
    running_ = true;
    setFocus();
    return true;
}

void RetroView::stop()
{
    if (timer_) timer_->stop();
    running_ = false;
    paused_ = false;
    hideMenu();
    pressedKeys_.clear();
    pad_.stopRumble();
    stopAudio();
    core_.unload();
    update();
}

void RetroView::setPaused(bool paused)
{
    paused_ = paused;
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
    update();
}

QString RetroView::statePath() const
{
    const QString dir = QCoreApplication::applicationDirPath() + QStringLiteral("/states");
    QDir().mkpath(dir);
    return dir + QStringLiteral("/") + QFileInfo(romPath_).completeBaseName() + QStringLiteral(".state");
}

bool RetroView::saveState(QString* error)
{
    if (!running_) { if (error) *error = tr("No game is running."); return false; }
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

    QAudioFormat fmt;
    fmt.setSampleRate(sampleRate);
    fmt.setChannelCount(2);
    fmt.setSampleFormat(QAudioFormat::Int16);

    const QAudioDevice dev = QMediaDevices::defaultAudioOutput();
    if (dev.isNull()) return; // no output device -> run silent

    audioSink_ = new QAudioSink(dev, fmt, this);
    audioSink_->setBufferSize(fmt.bytesForDuration(100000)); // ~100 ms
    audioBytesPerSec_ = sampleRate * 2 * 2;                  // stereo S16
    audioIo_ = audioSink_->start();                          // push mode: write samples to this
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
}

// Called from the core during runFrame() (GUI thread): interleaved S16 stereo, 'frames' stereo samples.
void RetroView::pushAudio(const int16_t* data, size_t frames)
{
    if (!audioIo_ || frames == 0) return;
    pendingAudio_.append(reinterpret_cast<const char*>(data), static_cast<qsizetype>(frames) * 4); // 4 bytes/frame

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
