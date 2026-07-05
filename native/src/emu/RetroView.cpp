#include "RetroView.h"
#include "NetplaySession.h"
#include "../core/AppPaths.h"
#include "../core/CoreManager.h"
#include "../core/Settings.h"
#include "../core/Achievements.h"
#include "../core/SystemCatalog.h"
#include "../core/PortMapper.h"
#include <QTimer>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QFontMetrics>
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
#include <QDateTime>
#include <QIcon>
#include <QPixmap>
#include <QVBoxLayout>
#include <QInputDialog>
#include <QRandomGenerator>
#include <QLineEdit>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QHostAddress>
#include <QAbstractSocket>
#include <QScrollArea>
#include <QThread>
#include <QOpenGLContext>
#include <QOffscreenSurface>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QSurfaceFormat>
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

    loadVideoFilter();
    buildMenu();
}

void RetroView::buildMenu()
{
    menu_ = new QFrame(this);
    menu_->setObjectName(QStringLiteral("emuMenu"));
    menu_->setStyleSheet(QStringLiteral(
        "#emuMenu { background: rgba(20,20,24,0.94); border: 1px solid rgba(255,255,255,0.15); border-radius: 12px; }"
        "#emuMenu QPushButton { padding: 9px 18px; font-size: 15px; color:#e8e8e8; background: transparent;"
        " border: 1px solid transparent; border-radius: 6px; }"
        // The selected entry (keyboard or controller) is highlighted so you can see where you are.
        "#emuMenu QPushButton:focus { background: rgba(90,140,255,0.85); border: 1px solid rgba(255,255,255,0.6); }"
        "#emuMenu QPushButton:hover { background: rgba(90,140,255,0.35); }"
        "#emuMenu QLabel { color: #e8e8e8; }"));
    auto* v = new QVBoxLayout(menu_);
    v->setContentsMargins(20, 18, 20, 18);
    v->setSpacing(8);

    menuTitle_ = new QLabel(tr("Paused"), menu_);
    menuTitle_->setAlignment(Qt::AlignCenter);
    menuTitle_->setStyleSheet(QStringLiteral("font-size:18px; font-weight:600;"));
    v->addWidget(menuTitle_);

    // Body holds the two pages: the main button list and (built on demand) the state-slot grid.
    menuBody_ = new QVBoxLayout();
    menuBody_->setSpacing(8);
    v->addLayout(menuBody_);

    mainPage_ = new QWidget(menu_);
    auto* mp = new QVBoxLayout(mainPage_);
    mp->setContentsMargins(0, 0, 0, 0);
    mp->setSpacing(8);
    auto* resume = new QPushButton(tr("Resume"), mainPage_);
    auto* save   = new QPushButton(tr("Save State"), mainPage_);
    auto* load   = new QPushButton(tr("Load State"), mainPage_);
    diskBtn_     = new QPushButton(tr("Disk"), mainPage_);
    optBtn_      = new QPushButton(tr("Core Options"), mainPage_);
    auto* cheats = new QPushButton(tr("Cheats"), mainPage_);
    filterBtn_   = new QPushButton(videoFilterLabel(), mainPage_);
    auto* shot   = new QPushButton(tr("Screenshot"), mainPage_);
    auto* netp   = new QPushButton(tr("Netplay"), mainPage_);
    auto* exit   = new QPushButton(tr("Exit Emulator"), mainPage_);
    for (QPushButton* b : { resume, save, load, diskBtn_, optBtn_, cheats, filterBtn_, shot, netp, exit }) mp->addWidget(b);
    menuBody_->addWidget(mainPage_);

    menuStatus_ = new QLabel(QString(), menu_);
    menuStatus_->setAlignment(Qt::AlignCenter);
    menuStatus_->setStyleSheet(QStringLiteral("color:#aaa; font-size:12px;"));
    v->addWidget(menuStatus_);

    connect(resume, &QPushButton::clicked, this, &RetroView::hideMenu);
    connect(exit,   &QPushButton::clicked, this, [this] { hideMenu(); emit exitRequested(); });
    connect(save,   &QPushButton::clicked, this, [this] { showStateSlots(true); });
    connect(load,   &QPushButton::clicked, this, [this] { showStateSlots(false); });
    connect(cheats, &QPushButton::clicked, this, [this] { showCheats(); });
    connect(filterBtn_, &QPushButton::clicked, this, [this] { cycleVideoFilter(); filterBtn_->setText(videoFilterLabel()); });
    connect(shot, &QPushButton::clicked, this, [this] {
        const QString p = captureScreenshot();
        menuStatus_->setText(p.isEmpty() ? tr("Couldn't save screenshot.")
                                         : tr("Saved: %1").arg(QFileInfo(p).fileName())); });
    connect(netp, &QPushButton::clicked, this, [this] { showNetplay(); });
    connect(diskBtn_, &QPushButton::clicked, this, [this] { showDisk(); });
    connect(optBtn_, &QPushButton::clicked, this, [this] { showCoreOptions(); });
    // Remember the main buttons so showMainMenu() can restore navigation to them.
    mainButtons_ = { resume, save, load, diskBtn_, optBtn_, cheats, filterBtn_, shot, netp, exit };
    menuButtons_ = mainButtons_;

    menu_->hide();
}

// Switch the pause menu back to its main page (Resume / Save / Load / Exit).
void RetroView::showMainMenu()
{
    slotsMode_ = false;
    if (slotsPage_) { slotsPage_->hide(); slotsPage_->deleteLater(); slotsPage_ = nullptr; }
    menuTitle_->setText(tr("Paused"));
    mainPage_->show();
    subScroll_ = nullptr;               // main page doesn't scroll
    // Disk / Core Options only apply to some systems/cores. Filter the nav list by the logical condition, NOT
    // isHidden() — showMenu() calls this before menu_->show(), so the buttons' show is still deferred and
    // isHidden() would (wrongly) report every button hidden, leaving the nav list empty.
    const bool showDisk = running_ && core_.hasDiskControl();
    const bool showOpt  = running_ && !core_.options().empty();
    if (diskBtn_) diskBtn_->setVisible(showDisk);
    if (optBtn_)  optBtn_->setVisible(showOpt);
    menuButtons_.clear();               // navigation over the visible main-page buttons
    for (QPushButton* b : mainButtons_)
    {
        if (!b || (b == diskBtn_ && !showDisk) || (b == optBtn_ && !showOpt)) continue;
        menuButtons_ << b;
    }
    menu_->adjustSize();
    menu_->move((width() - menu_->width()) / 2, (height() - menu_->height()) / 2);
    if (!menuButtons_.isEmpty()) menuButtons_.first()->setFocus(Qt::TabFocusReason);
}

// Show the per-game state-slot grid. Each slot shows its thumbnail + timestamp (or "Empty"). In save mode
// every slot is writable; in load mode empty slots are disabled. Rebuilt each time so thumbnails stay fresh.
void RetroView::showStateSlots(bool saveMode)
{
    slotsMode_ = true;
    menuStatus_->clear();
    menuTitle_->setText(saveMode ? tr("Save State") : tr("Load State"));
    mainPage_->hide();
    if (slotsPage_) { slotsPage_->hide(); slotsPage_->deleteLater(); slotsPage_ = nullptr; }

    slotsPage_ = new QWidget(menu_);
    auto* sv = new QVBoxLayout(slotsPage_);
    sv->setContentsMargins(0, 0, 0, 0);
    sv->setSpacing(6);
    menuButtons_.clear();

    for (int slot = 1; slot <= kStateSlots; ++slot)
    {
        const bool exists = QFile::exists(statePath(slot))
                            || (slot == 1 && QFile::exists(statePath())); // legacy single-slot fallback
        auto* b = new QPushButton(slotsPage_);
        b->setStyleSheet(QStringLiteral("QPushButton { text-align:left; padding:6px 10px; }"));
        b->setIconSize(QSize(80, 60));
        QString label = tr("Slot %1").arg(slot);
        if (exists)
        {
            const QString tp = thumbPath(slot);
            if (QFile::exists(tp)) b->setIcon(QIcon(QPixmap(tp)));
            const QDateTime when = QFileInfo(statePath(slot)).lastModified();
            label += when.isValid() ? QStringLiteral("   ·   %1").arg(when.toString(QStringLiteral("MMM d, h:mm ap")))
                                    : QStringLiteral("   ·   saved");
        }
        else
        {
            label += QStringLiteral("   ·   ") + tr("Empty");
            if (!saveMode) b->setEnabled(false); // nothing to load
        }
        b->setText(label);
        connect(b, &QPushButton::clicked, this, [this, slot, saveMode] {
            QString e;
            if (saveMode)
            {
                if (saveState(slot, &e)) { menuStatus_->setText(tr("Saved to slot %1").arg(slot)); showStateSlots(true); }
                else menuStatus_->setText(e);
            }
            else
            {
                if (loadState(slot, &e)) hideMenu();           // resume straight into the restored state
                else menuStatus_->setText(e);
            }
        });
        sv->addWidget(b);
        if (b->isEnabled()) menuButtons_ << b;
    }
    auto* back = new QPushButton(tr("‹ Back"), slotsPage_);
    connect(back, &QPushButton::clicked, this, [this] { showMainMenu(); });
    sv->addWidget(back);
    menuButtons_ << back;

    menuBody_->addWidget(slotsPage_);
    slotsPage_->show();
    menu_->adjustSize();
    menu_->move((width() - menu_->width()) / 2, (height() - menu_->height()) / 2);
    if (!menuButtons_.isEmpty()) menuButtons_.first()->setFocus(Qt::TabFocusReason);
}

QString RetroView::captureScreenshot()
{
    const QImage img = currentFrameImage(); // the clean source frame (no scaling, no CRT overlay)
    if (img.isNull()) return QString();
    const QString dir = AppPaths::dataDir() + QStringLiteral("/screenshots");
    QDir().mkpath(dir);
    const QString base = QFileInfo(romPath_).completeBaseName();
    const QString path = QStringLiteral("%1/%2-%3.png")
        .arg(dir, base.isEmpty() ? QStringLiteral("screenshot") : base,
             QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")));
    return img.save(path, "PNG") ? path : QString();
}

// Pause-menu sub-page: disk control. Eject/insert the disk and pick which disk/side is loaded (FDS side-flip,
// multi-disc PS1, ...). Switching a disk ejects, sets the index, and re-inserts, as the libretro API requires.
void RetroView::showDisk()
{
    slotsMode_ = true;
    menuStatus_->clear();
    menuTitle_->setText(tr("Disk"));
    mainPage_->hide();
    if (slotsPage_) { slotsPage_->hide(); slotsPage_->deleteLater(); slotsPage_ = nullptr; }
    slotsPage_ = new QWidget(menu_);
    auto* sv = new QVBoxLayout(slotsPage_);
    sv->setContentsMargins(0, 0, 0, 0);
    sv->setSpacing(6);
    menuButtons_.clear();
    auto flat = [](QPushButton* b) { b->setStyleSheet(QStringLiteral("QPushButton { text-align:left; padding:6px 12px; border-radius:6px; } QPushButton:focus { background: rgba(90,140,255,0.85); border:1px solid rgba(255,255,255,0.6); }")); return b; };

    const unsigned count = core_.diskCount();
    const unsigned cur = core_.diskIndex();

    auto* ejectBtn = flat(new QPushButton(core_.diskEjected() ? tr("Insert disk") : tr("Eject disk"), slotsPage_));
    connect(ejectBtn, &QPushButton::clicked, this, [this] { core_.setDiskEject(!core_.diskEjected()); showDisk(); });
    sv->addWidget(ejectBtn); menuButtons_ << ejectBtn;

    if (count > 1)
    {
        auto* lbl = new QLabel(tr("Insert disk / side:"), slotsPage_);
        lbl->setStyleSheet(QStringLiteral("color:#9aa0aa;font-size:13px;"));
        sv->addWidget(lbl);
        for (unsigned i = 0; i < count; ++i)
        {
            const std::string lab = core_.diskLabel(i);
            const QString name = lab.empty() ? tr("Disk %1").arg(i + 1) : QString::fromStdString(lab);
            auto* b = flat(new QPushButton((i == cur ? QStringLiteral("✓  ") : QStringLiteral("     ")) + name, slotsPage_));
            connect(b, &QPushButton::clicked, this, [this, i] {
                core_.setDiskEject(true); core_.setDiskIndex(i); core_.setDiskEject(false); // eject -> switch -> insert
                emit statusMessage(tr("Inserted disk %1").arg(i + 1));
                showDisk(); });
            sv->addWidget(b); menuButtons_ << b;
        }
    }

    auto* back = flat(new QPushButton(tr("‹ Back"), slotsPage_));
    connect(back, &QPushButton::clicked, this, [this] { showMainMenu(); });
    sv->addWidget(back); menuButtons_ << back;

    menuBody_->addWidget(slotsPage_);
    slotsPage_->show();
    menu_->adjustSize();
    menu_->move((width() - menu_->width()) / 2, (height() - menu_->height()) / 2);
    if (!menuButtons_.isEmpty()) menuButtons_.first()->setFocus(Qt::TabFocusReason);
}

// Pause-menu sub-page: the running core's libretro options. Each row cycles that option's value on click;
// the core re-reads it live (setOptionValue flags it), and the choice persists per-core for next launch.
void RetroView::showCoreOptions()
{
    slotsMode_ = true;
    menuStatus_->clear();
    menuTitle_->setText(tr("Core Options"));
    mainPage_->hide();
    if (slotsPage_) { slotsPage_->hide(); slotsPage_->deleteLater(); slotsPage_ = nullptr; }
    slotsPage_ = new QWidget(menu_);
    auto* sv = new QVBoxLayout(slotsPage_);
    sv->setContentsMargins(0, 0, 0, 0);
    sv->setSpacing(6);
    menuButtons_.clear();

    // Options can be many, so they live in a scroll area (capped to the window); focus-follow scrolls to the
    // selected row (see the menu key/pad nav).
    subScroll_ = new QScrollArea(slotsPage_);
    subScroll_->setWidgetResizable(true);
    subScroll_->setFrameShape(QFrame::NoFrame);
    subScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    subScroll_->setMaximumHeight(qMax(200, height() - 200));
    subScroll_->setStyleSheet(QStringLiteral("background:transparent;"));
    auto* host = new QWidget(subScroll_);
    auto* ov = new QVBoxLayout(host);
    ov->setContentsMargins(0, 0, 6, 0);
    ov->setSpacing(6);

    auto label = [](const CoreOption& o, const std::string& val) {
        QString vlabel = QString::fromStdString(val);
        for (const auto& vp : o.values) if (vp.first == val) { vlabel = QString::fromStdString(vp.second); break; }
        return QString::fromStdString(o.desc) + QStringLiteral(":   ") + vlabel;
    };

    for (const CoreOption& opt : core_.options())
    {
        if (opt.values.size() < 2) continue; // a fixed/1-choice option isn't worth a row
        const std::string key = opt.key;
        auto* b = new QPushButton(label(opt, core_.optionValue(key)), host);
        b->setStyleSheet(QStringLiteral("QPushButton { text-align:left; padding:6px 12px; border-radius:6px; } QPushButton:focus { background: rgba(90,140,255,0.85); border:1px solid rgba(255,255,255,0.6); }"));
        b->setToolTip(QString::fromStdString(opt.info));
        connect(b, &QPushButton::clicked, this, [this, key, opt, b, label] {
            // Advance to the next value in the option's list (wrapping), apply live, and persist per-core.
            const std::string cur = core_.optionValue(key);
            int idx = 0;
            for (int i = 0; i < int(opt.values.size()); ++i) if (opt.values[i].first == cur) { idx = i; break; }
            const std::string next = opt.values[(idx + 1) % opt.values.size()].first;
            core_.setOptionValue(key, next);
            Settings::setOptionValue(coreName_, QString::fromStdString(key), QString::fromStdString(next));
            b->setText(label(opt, next));
        });
        ov->addWidget(b);
        menuButtons_ << b;
    }
    ov->addStretch(1);
    subScroll_->setWidget(host);
    sv->addWidget(subScroll_);

    auto* note = new QLabel(tr("Most options apply immediately; a few (e.g. resolution) take effect on the next "
                              "game load."), slotsPage_);
    note->setStyleSheet(QStringLiteral("color:#9aa0aa;font-size:12px;")); note->setWordWrap(true);
    sv->addWidget(note);

    auto* back = new QPushButton(tr("‹ Back"), slotsPage_);
    back->setStyleSheet(QStringLiteral("QPushButton { text-align:left; padding:6px 12px; border-radius:6px; } QPushButton:focus { background: rgba(90,140,255,0.85); border:1px solid rgba(255,255,255,0.6); }"));
    connect(back, &QPushButton::clicked, this, [this] { showMainMenu(); });
    sv->addWidget(back); menuButtons_ << back;

    menuBody_->addWidget(slotsPage_);
    slotsPage_->show();
    menu_->adjustSize();
    menu_->move((width() - menu_->width()) / 2, (height() - menu_->height()) / 2);
    if (!menuButtons_.isEmpty()) menuButtons_.first()->setFocus(Qt::TabFocusReason);
}

// Pause-menu sub-page: host a game or join one. Both sides must have the same game + core already loaded.
void RetroView::showNetplay()
{
    slotsMode_ = true;
    menuStatus_->clear();
    menuTitle_->setText(tr("Netplay"));
    mainPage_->hide();
    if (slotsPage_) { slotsPage_->hide(); slotsPage_->deleteLater(); slotsPage_ = nullptr; }
    slotsPage_ = new QWidget(menu_);
    auto* sv = new QVBoxLayout(slotsPage_);
    sv->setContentsMargins(0, 0, 0, 0);
    sv->setSpacing(6);
    menuButtons_.clear();

    auto* note = new QLabel(tr("2-player. Both players must already have the SAME game and core loaded; the host's "
                               "state syncs to player 2. Play on your local network, or online via a relay server."), slotsPage_);
    note->setStyleSheet(QStringLiteral("color:#999;font-size:12px;")); note->setWordWrap(true);
    sv->addWidget(note);

    QString ip;
    for (const QHostAddress& a : QNetworkInterface::allAddresses())
        if (a.protocol() == QAbstractSocket::IPv4Protocol && !a.isLoopback()) { ip = a.toString(); break; }

    auto flat = [](QPushButton* b) { b->setStyleSheet(QStringLiteral("QPushButton { text-align:left; padding:6px 12px; border-radius:6px; } QPushButton:focus { background: rgba(90,140,255,0.85); border:1px solid rgba(255,255,255,0.6); }")); return b; };

    auto* hostBtn = flat(new QPushButton(tr("Host game  (you are Player 1)"), slotsPage_));
    connect(hostBtn, &QPushButton::clicked, this, [this, ip] {
        startNetplay(true);
        if (menuStatus_)
            menuStatus_->setText(tr("Waiting for player 2 — join %1 : 55420").arg(ip.isEmpty() ? tr("your IP") : ip)); });
    sv->addWidget(hostBtn); menuButtons_ << hostBtn;

    auto* joinBtn = flat(new QPushButton(tr("Join game…  (Player 2, same network)"), slotsPage_));
    connect(joinBtn, &QPushButton::clicked, this, [this] {
        bool ok = false;
        const QString host = QInputDialog::getText(this, tr("Join Netplay"), tr("Host's IP address:"),
                                                    QLineEdit::Normal, QString(), &ok).trimmed();
        if (ok && !host.isEmpty()) startNetplay(false, host); });
    sv->addWidget(joinBtn); menuButtons_ << joinBtn;

    // ---- online (via the relay; no port-forwarding needed on either end) ----
    auto* hostOnlineBtn = flat(new QPushButton(tr("Host online  (over the internet)"), slotsPage_));
    connect(hostOnlineBtn, &QPushButton::clicked, this, [this] {
        if (Settings::netplayRelay().trimmed().isEmpty()) {
            if (menuStatus_) menuStatus_->setText(tr("Set a relay server first (the “Relay server…” button below.)"));
            return;
        }
        static const QString cs = QStringLiteral("ABCDEFGHJKLMNPQRSTUVWXYZ23456789"); // no ambiguous 0/O/1/I
        QString code;
        for (int i = 0; i < 5; ++i) code += cs.at(int(QRandomGenerator::global()->bounded(cs.size())));
        startNetplayOnline(true, code); });  // status (incl. the shareable code) is set once UPnP resolves
    sv->addWidget(hostOnlineBtn); menuButtons_ << hostOnlineBtn;

    auto* joinOnlineBtn = flat(new QPushButton(tr("Join online…  (enter a friend's code)"), slotsPage_));
    connect(joinOnlineBtn, &QPushButton::clicked, this, [this] {
        bool ok = false;
        const QString code = QInputDialog::getText(this, tr("Join Online Netplay"), tr("Room code from the host:"),
                                                    QLineEdit::Normal, QString(), &ok).trimmed().toUpper();
        if (ok && !code.isEmpty()) startNetplayOnline(false, code); });
    sv->addWidget(joinOnlineBtn); menuButtons_ << joinOnlineBtn;

    auto* relayBtn = flat(new QPushButton(tr("Relay server…  (for online play)"), slotsPage_));
    connect(relayBtn, &QPushButton::clicked, this, [this] {
        bool ok = false;
        const QString v = QInputDialog::getText(this, tr("Netplay Relay Server"),
            tr("Relay address as host:port.\nRun tools/netplay-relay.py on a public server and enter it here."),
            QLineEdit::Normal, Settings::netplayRelay(), &ok).trimmed();
        if (ok) { Settings::setNetplayRelay(v);
                  if (menuStatus_) menuStatus_->setText(v.isEmpty() ? tr("Relay cleared.") : tr("Relay set: %1").arg(v)); } });
    sv->addWidget(relayBtn); menuButtons_ << relayBtn;

    if (netActive_ || (net_ && net_->active()))
    {
        auto* leave = flat(new QPushButton(tr("Leave netplay"), slotsPage_));
        connect(leave, &QPushButton::clicked, this, [this] { if (net_) net_->stop(); netActive_ = false; showMainMenu(); });
        sv->addWidget(leave); menuButtons_ << leave;
    }

    auto* back = flat(new QPushButton(tr("‹ Back"), slotsPage_));
    connect(back, &QPushButton::clicked, this, [this] { showMainMenu(); });
    sv->addWidget(back); menuButtons_ << back;

    menuBody_->addWidget(slotsPage_);
    slotsPage_->show();
    menu_->adjustSize();
    menu_->move((width() - menu_->width()) / 2, (height() - menu_->height()) / 2);
    if (!menuButtons_.isEmpty()) menuButtons_.first()->setFocus(Qt::TabFocusReason);
}

QString RetroView::cheatsPath() const
{
    const QString dir = AppPaths::dataDir() + QStringLiteral("/cheats");
    QDir().mkpath(dir);
    return dir + QStringLiteral("/") + QFileInfo(romPath_).completeBaseName() + QStringLiteral(".json");
}

void RetroView::loadCheats()
{
    cheats_.clear();
    QFile f(cheatsPath());
    if (!f.open(QIODevice::ReadOnly)) return;
    const QJsonArray arr = QJsonDocument::fromJson(f.readAll()).array();
    for (const QJsonValue& v : arr)
    {
        const QJsonObject o = v.toObject();
        Cheat c;
        c.desc = o.value(QStringLiteral("desc")).toString();
        c.code = o.value(QStringLiteral("code")).toString();
        c.enabled = o.value(QStringLiteral("enabled")).toBool(true);
        if (!c.code.isEmpty()) cheats_ << c;
    }
}

void RetroView::saveCheats()
{
    QJsonArray arr;
    for (const Cheat& c : cheats_)
        arr.append(QJsonObject{ { QStringLiteral("desc"), c.desc },
                                { QStringLiteral("code"), c.code },
                                { QStringLiteral("enabled"), c.enabled } });
    QFile f(cheatsPath());
    if (f.open(QIODevice::WriteOnly)) f.write(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

// Reset the core's cheat set and push the enabled ones, numbered sequentially.
void RetroView::applyCheats()
{
    if (!running_) return;
    core_.cheatReset();
    unsigned idx = 0;
    for (const Cheat& c : cheats_)
        if (c.enabled && !c.code.isEmpty()) core_.cheatSet(idx++, true, c.code.toStdString());
}

void RetroView::addCheatDialog()
{
    bool ok = false;
    const QString code = QInputDialog::getText(this, tr("Add Cheat"),
        tr("Cheat code (Game Genie / Action Replay / raw, as the core expects).\n"
           "Join multi-line codes with '+'."), QLineEdit::Normal, QString(), &ok).trimmed();
    if (!ok || code.isEmpty()) { showCheats(); return; }
    const QString desc = QInputDialog::getText(this, tr("Add Cheat"),
        tr("Description (optional):"), QLineEdit::Normal, QString(), &ok).trimmed();
    cheats_.push_back({ desc, code, true });
    saveCheats();
    applyCheats();
    showCheats();
}

// The per-game cheat list: each cheat toggles on click; plus add / remove-all / back.
void RetroView::showCheats()
{
    slotsMode_ = true;
    menuStatus_->clear();
    menuTitle_->setText(tr("Cheats"));
    mainPage_->hide();
    if (slotsPage_) { slotsPage_->hide(); slotsPage_->deleteLater(); slotsPage_ = nullptr; }

    slotsPage_ = new QWidget(menu_);
    auto* sv = new QVBoxLayout(slotsPage_);
    sv->setContentsMargins(0, 0, 0, 0);
    sv->setSpacing(6);
    menuButtons_.clear();

    if (!core_.supportsCheats())
    {
        auto* note = new QLabel(tr("This core doesn't support cheats."), slotsPage_);
        note->setStyleSheet(QStringLiteral("color:#999; font-size:13px;"));
        note->setWordWrap(true);
        sv->addWidget(note);
    }
    else
    {
        for (int i = 0; i < cheats_.size(); ++i)
        {
            const Cheat& c = cheats_[i];
            const QString name = c.desc.isEmpty() ? c.code : c.desc;
            auto* b = new QPushButton((c.enabled ? QStringLiteral("✓  ") : QStringLiteral("○  ")) + name, slotsPage_);
            b->setStyleSheet(QStringLiteral("QPushButton { text-align:left; padding:6px 12px; border-radius:6px; } QPushButton:focus { background: rgba(90,140,255,0.85); border:1px solid rgba(255,255,255,0.6); }"));
            connect(b, &QPushButton::clicked, this, [this, i] {
                if (i < cheats_.size()) { cheats_[i].enabled = !cheats_[i].enabled; saveCheats(); applyCheats(); showCheats(); }
            });
            sv->addWidget(b);
            menuButtons_ << b;
        }
        if (cheats_.isEmpty())
        {
            auto* none = new QLabel(tr("No cheats yet. Add one below."), slotsPage_);
            none->setStyleSheet(QStringLiteral("color:#999; font-size:13px;"));
            sv->addWidget(none);
        }
        auto* add = new QPushButton(tr("＋  Add cheat…"), slotsPage_);
        connect(add, &QPushButton::clicked, this, [this] { addCheatDialog(); });
        sv->addWidget(add);
        menuButtons_ << add;
        if (!cheats_.isEmpty())
        {
            auto* clear = new QPushButton(tr("🗑  Remove all"), slotsPage_);
            connect(clear, &QPushButton::clicked, this, [this] { cheats_.clear(); saveCheats(); applyCheats(); showCheats(); });
            sv->addWidget(clear);
            menuButtons_ << clear;
        }
    }

    auto* back = new QPushButton(tr("‹ Back"), slotsPage_);
    connect(back, &QPushButton::clicked, this, [this] { showMainMenu(); });
    sv->addWidget(back);
    menuButtons_ << back;

    menuBody_->addWidget(slotsPage_);
    slotsPage_->show();
    menu_->adjustSize();
    menu_->move((width() - menu_->width()) / 2, (height() - menu_->height()) / 2);
    if (!menuButtons_.isEmpty()) menuButtons_.first()->setFocus(Qt::TabFocusReason);
}

void RetroView::loadVideoFilter()
{
    const QString id = Settings::videoFilter();
    filter_ = id == QStringLiteral("scanlines") ? FilterScanlines
            : id == QStringLiteral("crt")       ? FilterCrt
            : id == QStringLiteral("lcd")       ? FilterLcd
                                                : FilterOff;
}

QString RetroView::videoFilterLabel() const
{
    const QString name = filter_ == FilterScanlines ? tr("Scanlines")
                       : filter_ == FilterCrt       ? tr("CRT")
                       : filter_ == FilterLcd       ? tr("LCD")
                                                    : tr("Off");
    return tr("Video Filter: %1").arg(name);
}

void RetroView::cycleVideoFilter()
{
    filter_ = static_cast<VideoFilter>((filter_ + 1) % 4);
    const char* id = filter_ == FilterScanlines ? "scanlines"
                   : filter_ == FilterCrt       ? "crt"
                   : filter_ == FilterLcd       ? "lcd" : "off";
    Settings::setVideoFilter(QString::fromLatin1(id));
    crtKey_.clear();  // force the overlay to rebuild for the new filter
    update();
}

// Composite the cached retro overlay over the drawn frame. The overlay is rebuilt only when the destination
// size, source geometry, or filter changes — each paint is then a single drawImage.
void RetroView::applyVideoFilter(QPainter& p, const QRect& dst, int srcW, int srcH)
{
    if (filter_ == FilterOff || dst.isEmpty() || srcW <= 0 || srcH <= 0) return;
    const QString key = QStringLiteral("%1x%2:%3x%4:%5")
                            .arg(dst.width()).arg(dst.height()).arg(srcW).arg(srcH).arg(int(filter_));
    if (key != crtKey_) { crtOverlay_ = buildFilterOverlay(dst.size(), srcW, srcH, filter_); crtKey_ = key; }
    if (!crtOverlay_.isNull()) p.drawImage(dst.topLeft(), crtOverlay_);
}

// Build the translucent black overlay for a filter, at display resolution. Scanlines darken the gap below
// each source line; CRT adds a dim aperture-grille (vertical triads); LCD draws a faint per-pixel grid.
QImage RetroView::buildFilterOverlay(QSize dst, int srcW, int srcH, VideoFilter f)
{
    QImage img(dst, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    if (f == FilterOff) return img;
    QPainter q(&img);
    q.setPen(Qt::NoPen);

    if (f == FilterScanlines || f == FilterCrt)
    {
        const double lineH = double(dst.height()) / srcH; // display pixels per source scanline
        const int alpha = (f == FilterCrt) ? 105 : 80;
        for (int i = 0; i < srcH; ++i)
            q.fillRect(QRectF(0, (i + 0.5) * lineH, dst.width(), lineH * 0.5), QColor(0, 0, 0, alpha));
    }
    if (f == FilterCrt)
    {
        // One dim vertical stripe per source column (aperture-grille look), only if columns are wide enough
        // on screen to be visible without turning the picture to mush.
        const double colW = double(dst.width()) / srcW;
        if (colW >= 3.0)
            for (int i = 0; i < srcW; ++i)
                q.fillRect(QRectF((i + 0.5) * colW, 0, qMax(1.0, colW * 0.22), dst.height()), QColor(0, 0, 0, 55));
    }
    if (f == FilterLcd)
    {
        const double lineH = double(dst.height()) / srcH, colW = double(dst.width()) / srcW;
        if (lineH >= 3.0)
            for (int i = 1; i < srcH; ++i) q.fillRect(QRectF(0, i * lineH - 0.5, dst.width(), 1.0), QColor(0, 0, 0, 45));
        if (colW >= 3.0)
            for (int i = 1; i < srcW; ++i) q.fillRect(QRectF(i * colW - 0.5, 0, 1.0, dst.height()), QColor(0, 0, 0, 45));
    }
    q.end();
    return img;
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
    coreName_ = coreName;
    // A GL/GLES core (N64 with GLideN64, Beetle PSX HW, Flycast, ...) asks for hardware rendering during
    // loadGame. Stand up the offscreen GL context + FBO now, before the first frame. HW rendering runs on the
    // GUI thread (one GL context), so a split-pane HW core drops out of threaded mode.
    if (core_.usesHwRender())
        setupHwRender();
    loadSram(); // restore battery-backed in-game saves before the game starts
    double fps = core_.avInfo().timing.fps;
    if (fps <= 0.0) fps = 60.0;
    portsMask_ = -1;            // force a fresh port setup for this game (done here while nothing else runs)
    // Per-system input profile: point the bindings at this console's scope so its custom layout (if any) is
    // used, then reload the keyboard + pad maps.
    {
        const GameSystem* sys = SystemCatalog::forExtension(QFileInfo(romPath).suffix().toLower());
        Settings::setInputScope(sys ? sys->id : QString());
        keymap_.reload();
        pad_.reloadMapping();
    }
    updateControllerPorts();
    loadTurbo();
    // Bezel / border art: <data>/bezels/<core>.png, else default.png (only when enabled).
    bezel_ = QImage();
    if (Settings::bezelEnabled())
    {
        const QString dir = AppPaths::dataDir() + QStringLiteral("/bezels/");
        for (const QString& cand : { dir + coreName + QStringLiteral(".png"), dir + QStringLiteral("default.png") })
            if (QFile::exists(cand)) { bezel_.load(cand); break; }
    }
    frameIntervalMs_ = qMax(1, qRound(1000.0 / fps)); // nearest ms (e.g. 17 for 59.7fps, not 16) — less audio drift
    paused_ = false;
    running_ = true;
    startEmu();                 // GUI timer, or a dedicated worker thread in threaded (split-pane) mode
    loadCheats(); applyCheats(); // this game's saved cheats, pushed into the core
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

// Held state of the menu-relevant pad buttons, OR-ed across every connected controller.
int RetroView::menuPadMask() const
{
    auto any = [this](unsigned id) {
        for (unsigned p = 0; p < Gamepad::kMaxPlayers; ++p) if (pad_.button(p, id)) return true;
        return false;
    };
    int m = 0;
    if (any(RETRO_DEVICE_ID_JOYPAD_UP))   m |= 1;
    if (any(RETRO_DEVICE_ID_JOYPAD_DOWN)) m |= 2;
    if (any(RETRO_DEVICE_ID_JOYPAD_A))    m |= 4; // A confirms
    if (any(RETRO_DEVICE_ID_JOYPAD_B))    m |= 8; // B confirms on the main page, backs out of the slot grid
    return m;
}

// Controller navigation for the open pause menu: d-pad Up/Down move (wrapping), A/B activate. Rising-edge
// only so a held direction doesn't race through the short list.
void RetroView::handleMenuPad()
{
    const int cur = menuPadMask();
    const int pressed = cur & ~menuPadPrev_;
    menuPadPrev_ = cur;
    if (menuButtons_.isEmpty()) return;
    int idx = menuButtons_.indexOf(qobject_cast<QPushButton*>(focusWidget()));
    if (idx < 0) idx = 0;
    if (slotsMode_ && (pressed & 8)) { showMainMenu(); return; } // B backs out of the slot grid
    if      (pressed & 1) menuButtons_[(idx + menuButtons_.size() - 1) % menuButtons_.size()]->setFocus(Qt::TabFocusReason);
    else if (pressed & 2) menuButtons_[(idx + 1) % menuButtons_.size()]->setFocus(Qt::TabFocusReason);
    if (subScroll_ && focusWidget()) subScroll_->ensureWidgetVisible(focusWidget()); // scroll to the focused row
    if ((pressed & 4) || (!slotsMode_ && (pressed & 8))) // A always confirms; B also confirms on the main page
    {
        if (auto* b = qobject_cast<QPushButton*>(focusWidget())) b->click();
        else menuButtons_.first()->click();
    }
}

bool RetroView::menuComboHeld()
{
    auto any = [this](unsigned id) {
        for (unsigned p = 0; p < Gamepad::kMaxPlayers; ++p) if (pad_.button(p, id)) return true;
        return false; };
    return any(RETRO_DEVICE_ID_JOYPAD_START) && any(RETRO_DEVICE_ID_JOYPAD_SELECT);
}

void RetroView::pollInput() // GUI: poll the pad + keyboard, resolve, and publish a snapshot for the worker
{
    pad_.poll();
    // Start+Select toggles the pause menu, so a controller-only player can open it (and close it).
    const bool combo = [this] {
        auto any = [this](unsigned id) { for (unsigned p = 0; p < Gamepad::kMaxPlayers; ++p) if (pad_.button(p, id)) return true; return false; };
        return any(RETRO_DEVICE_ID_JOYPAD_START) && any(RETRO_DEVICE_ID_JOYPAD_SELECT);
    }();
    if (combo && !menuComboPrev_) toggleMenu();
    menuComboPrev_ = combo;
    if (menu_ && menu_->isVisible()) { handleMenuPad(); return; } // menu up: the pad drives it, not the game

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
    const bool wasRunning = running_; // so we only announce (and time) a session that actually started
    if (core_.gameLoaded()) saveSram(); // persist battery RAM before tearing the core down
    if (ach_) ach_->unloadGame();
    running_ = false;
    stopEmu();          // stop the GUI timer or the worker thread (+ its audio); no core access afterward
    paused_ = false;
    achActive_ = false; achQueue_.clear(); if (achTimer_) achTimer_->stop(); // drop any pending unlock toast
    hideMenu();
    if (net_) net_->stop();
    netActive_ = false;
    netLocalInputs_.clear();
    Settings::setInputScope(QString()); // back to the global binding scope once no game is running
    pressedKeys_.clear();
    ffKey_ = rewindKey_ = fastForward_ = rewinding_ = false;
    rewindBuf_.clear();
    rewindBytes_ = 0;
    pad_.stopRumble();
    teardownHwRender(); // context_destroy + drop the GL objects while the core is still loaded
    core_.unload();
    update();
    if (wasRunning) emit gameStopped();
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
    ffKey_ = rewindKey_ = fastForward_ = rewinding_ = false; // don't stay stuck if the menu opens mid-hold
    setPaused(true);
    menuStatus_->clear();
    showMainMenu();               // always open on the main page (centres + focuses the first button)
    menu_->show();
    menu_->raise();
    if (!menuButtons_.isEmpty()) menuButtons_.first()->setFocus(Qt::TabFocusReason); // arrow keys work at once
    menuPadPrev_ = menuPadMask(); // seed edge state so buttons held while opening aren't read as a press
    menuComboPrev_ = menuComboHeld(); // don't read the still-held opening combo as an immediate close
    // setPaused(true) stopped the frame timer; in full-screen restart it so tick() keeps polling the pad and
    // driving the menu (it skips the game frame while the menu is visible). Split panes use pollInput().
    if (!threaded_ && timer_) timer_->start(frameIntervalMs_);
}

void RetroView::hideMenu()
{
    menuComboPrev_ = menuComboHeld(); // carry the still-held closing combo so tick() doesn't reopen at once
    menu_->hide();
    setPaused(false);                 // resumes the game frame loop
    setFocus(); // keep Esc / gameplay keys coming to the view
}

void RetroView::resizeEvent(QResizeEvent*)
{
    if (menu_ && menu_->isVisible())
        menu_->move((width() - menu_->width()) / 2, (height() - menu_->height()) / 2);
}

// Advance the core one frame (hardware or software path) and do the per-frame bookkeeping. Returns false if
// the core crashed and was stopped, so the caller bails out of the frame.
bool RetroView::runOneCoreFrame()
{
    if (hwMode_ && glCtx_)  // GL core: run inside our context so it renders into the FBO, then read it back
    {
        glCtx_->makeCurrent(glSurface_);
        glFbo_->bind();                 // the core queries get_current_framebuffer, but bind it as a default too
        core_.runFrame();
        if (core_.takeHwFramePending()) readbackHwFrame();
        glFbo_->release();
        glCtx_->doneCurrent();
    }
    else
        core_.runFrame();   // audio is pushed via core_.onAudio (muted while fast-forwarding / rewinding)
    if (core_.crashed()) // a hard fault inside the core was caught; stop instead of faulting every frame
    {
        stop();
        emit statusMessage(tr("The emulator core crashed and was stopped."));
        return false;
    }
    if (ach_ && !paused_) ach_->doFrame(); // evaluate RetroAchievements against this frame's memory
    if (++sramAutosaveCounter_ >= 600) { sramAutosaveCounter_ = 0; saveSram(); } // ~10s autosave (crash safety)
    return true;
}

// Snapshot the current core state into the rewind buffer, dropping the oldest states past the byte cap.
void RetroView::captureRewind()
{
    std::vector<uint8_t> s;
    if (!core_.saveState(s) || s.empty()) return; // core can't serialize -> rewind simply stays unavailable
    rewindBytes_ += s.size();
    rewindBuf_.push_back(std::move(s));
    while (rewindBytes_ > kRewindMaxBytes && rewindBuf_.size() > 1)
    {
        rewindBytes_ -= rewindBuf_.front().size();
        rewindBuf_.pop_front();
    }
}

// This peer's RetroPad button mask, read from its own port-0 controls (keyboard + pad, with turbo applied).
quint16 RetroView::captureLocalButtons()
{
    quint16 m = 0;
    for (unsigned id = 0; id < 16; ++id)
        if (resolveInput(0, RETRO_DEVICE_JOYPAD, 0, id)) m |= quint16(1u << id);
    return m;
}

// The netplay frame loop: generate + send local input a few frames ahead, then advance the core only for
// frames where BOTH peers' inputs have arrived (lockstep). Stalls (advances nothing) if the peer is behind.
void RetroView::netTick()
{
    if (!net_ || !net_->ready()) { update(); return; }
    while (netGenFrame_ <= netFrame_ + kNetDelay)
    {
        const quint16 b = captureLocalButtons();
        netLocalInputs_.insert(netGenFrame_, b);
        net_->sendLocalInput(netGenFrame_, b);
        netGenFrame_++;
    }
    int advanced = 0;
    quint16 rb = 0;
    while (advanced < 8 && netLocalInputs_.contains(netFrame_) && net_->remoteInput(netFrame_, rb))
    {
        netCurLocal_  = netLocalInputs_.value(netFrame_);
        netCurRemote_ = rb;
        if (!runOneCoreFrame()) return; // core crashed -> stop()
        netFrame_++;
        advanced++;
        if (netFrame_ > quint32(kNetDelay + 2)) // prune inputs we'll never need again
        {
            netLocalInputs_.remove(netFrame_ - kNetDelay - 2);
            net_->pruneBefore(netFrame_ - kNetDelay - 2);
        }
    }
    update();
}

void RetroView::ensureNetSession()
{
    if (net_) { net_->gameId = QFileInfo(romPath_).completeBaseName() + QStringLiteral("|") + QString::number(QFileInfo(romPath_).size());
                net_->coreName = coreName_; return; }
    net_ = new NetplaySession(this);
    net_->serializeState = [this] {
        std::vector<uint8_t> s; core_.saveState(s);
        return QByteArray(reinterpret_cast<const char*>(s.data()), int(s.size())); };
    net_->applyState = [this](const QByteArray& b) {
        core_.loadState(reinterpret_cast<const uint8_t*>(b.constData()), size_t(b.size())); };
    connect(net_, &NetplaySession::status, this, [this](const QString& m) {
        if (menuStatus_ && menu_ && menu_->isVisible()) menuStatus_->setText(m);
        emit statusMessage(m); });
    connect(net_, &NetplaySession::started, this, [this] {
        netActive_ = true; netFrame_ = netGenFrame_ = 0; netCurLocal_ = netCurRemote_ = 0;
        netLocalInputs_.clear();
        netLocalPort_  = net_->isHost() ? 0 : 1;
        netRemotePort_ = net_->isHost() ? 1 : 0;
        hideMenu(); // unpause into the lockstep loop
        emit statusMessage(tr("Netplay started — you are Player %1.").arg(netLocalPort_ + 1)); });
    connect(net_, &NetplaySession::ended, this, [this](const QString& reason) {
        netActive_ = false;
        if (menuStatus_ && menu_ && menu_->isVisible()) menuStatus_->setText(reason);
        emit statusMessage(tr("Netplay ended: %1").arg(reason)); });
    net_->gameId = QFileInfo(romPath_).completeBaseName() + QStringLiteral("|") + QString::number(QFileInfo(romPath_).size());
    net_->coreName = coreName_;
}

void RetroView::startNetplay(bool asHost, const QString& hostAddr)
{
    if (!running_ || threaded_) return;
    ensureNetSession();
    if (asHost) net_->host(55420);
    else        net_->join(hostAddr, 55420);
}

// Online netplay, "Both" strategy: the host tries UPnP to open a port for a low-latency DIRECT connection AND
// registers on the relay at the same time (whichever the joiner reaches first wins). The shareable code carries
// the public endpoint when UPnP worked (ROOM~ip:port) so the joiner can try direct, else it's just the room code.
void RetroView::startNetplayOnline(bool asHost, const QString& code)
{
    if (!running_ || threaded_) return;
    const QString relay = Settings::netplayRelay().trimmed();
    if (relay.isEmpty())
    {
        if (menuStatus_) menuStatus_->setText(tr("Set a relay server first (the “Relay server…” button)."));
        return;
    }
    const int colon = relay.lastIndexOf(QLatin1Char(':'));
    const QString rhost = colon > 0 ? relay.left(colon) : relay;
    const quint16 rp = colon > 0 ? quint16(relay.mid(colon + 1).toUInt()) : quint16(0);
    const quint16 relayPort = rp ? rp : quint16(55666);
    constexpr quint16 kDirectPort = 55420;
    ensureNetSession();

    if (asHost)
    {
        if (!portMapper_) portMapper_ = new PortMapper(this);
        portMapper_->disconnect(this);   // clear any connections from a prior host attempt
        connect(portMapper_, &PortMapper::mapped, this,
                [this, code, rhost, relayPort](const QString& pubIp, quint16 ext) {
            net_->hostOnline(kDirectPort, rhost, relayPort, code);
            const QString full = code + QStringLiteral("~") + pubIp + QStringLiteral(":") + QString::number(ext);
            if (menuStatus_) menuStatus_->setText(tr("Waiting for player 2 — give them this code:  %1").arg(full));
        }, Qt::SingleShotConnection);
        connect(portMapper_, &PortMapper::failed, this, [this, code, rhost, relayPort](const QString&) {
            net_->hostOnline(kDirectPort, rhost, relayPort, code);  // relay-only; direct server still up for a manual forward
            if (menuStatus_)
                menuStatus_->setText(tr("Waiting for player 2 — give them this code:  %1  (relay)").arg(code));
        }, Qt::SingleShotConnection);
        if (menuStatus_) menuStatus_->setText(tr("Opening a port on your router…"));
        portMapper_->map(kDirectPort, kDirectPort);
    }
    else
    {
        // Parse ROOM or ROOM~ip:port. If an endpoint is present, joinOnline tries it first, then the relay.
        QString room = code, dip; quint16 dport = 0;
        const int tilde = code.indexOf(QLatin1Char('~'));
        if (tilde > 0)
        {
            room = code.left(tilde);
            const QString ep = code.mid(tilde + 1);
            const int c2 = ep.lastIndexOf(QLatin1Char(':'));
            if (c2 > 0) { dip = ep.left(c2); dport = quint16(ep.mid(c2 + 1).toUInt()); }
        }
        net_->joinOnline(rhost, relayPort, room, dip, dport);
    }
}

void RetroView::tick()
{
    if (!running_) return;
    pad_.poll();        // refresh controller state + handle hot-plug before the core reads input
    updateControllerPorts(); // pick up controllers plugged in/out mid-game
    // Start+Select toggles the pause menu (controller-only players). Edge-detected; on the frame it opens,
    // skip the game so it doesn't also see the combo. The timer keeps running while the menu is up (see
    // showMenu), so this same tick drives the menu below without advancing the game.
    if (!netActive_)
    {
        const bool combo = menuComboHeld();
        if (combo && !menuComboPrev_) { menuComboPrev_ = true; toggleMenu(); return; }
        menuComboPrev_ = combo;
    }
    if (menu_ && menu_->isVisible()) { handleMenuPad(); update(); return; } // menu open: drive it, don't run a frame

    // Advance the autofire phase: on for turboHalfPeriod_ frames, then off for the same.
    if (++turboCounter_ >= 2 * turboHalfPeriod_) turboCounter_ = 0;
    turboOn_ = turboCounter_ < turboHalfPeriod_;

    if (netActive_) { netTick(); return; } // netplay drives frame pacing itself (lockstep); no ff/rewind

    // Resolve fast-forward / rewind from the keyboard (Tab / R) or a controller combo (Select+R2 / Select+L2).
    auto anyPad = [this](unsigned id) {
        for (unsigned p = 0; p < Gamepad::kMaxPlayers; ++p) if (pad_.button(p, id)) return true;
        return false; };
    const bool sel = anyPad(RETRO_DEVICE_ID_JOYPAD_SELECT);
    fastForward_ = ffKey_     || (sel && anyPad(RETRO_DEVICE_ID_JOYPAD_R2));
    rewinding_   = rewindKey_ || (sel && anyPad(RETRO_DEVICE_ID_JOYPAD_L2));

    // Rewind: step back through the captured states (audio stays muted via the rewinding_ flag). One buffered
    // state is consumed per tick; when the buffer runs dry we hold on the oldest frame.
    if (rewinding_ && !threaded_)
    {
        if (!rewindBuf_.empty()) { rewindBytes_ -= rewindBuf_.back().size(); rewindBuf_.pop_back(); }
        if (rewindBuf_.empty()) { update(); return; }
        const std::vector<uint8_t>& s = rewindBuf_.back();
        core_.loadState(s.data(), s.size());
        if (!runOneCoreFrame()) return; // renders the restored state
        update();
        return;
    }

    // Normal / fast-forward: run one or several core frames. Capture a rewind snapshot before each real frame
    // (skipped while fast-forwarding, to keep its cost down).
    const int frames = fastForward_ ? kFfSpeed : 1;
    for (int i = 0; i < frames; ++i)
    {
        if (!threaded_ && !fastForward_) captureRewind();
        if (!runOneCoreFrame()) return;
    }
    update();
}

// Stand up an offscreen OpenGL context + FBO for a hardware-rendered core. The core draws into the FBO; we
// read it back each frame and paint it through the normal (software) path, so no native GL surface is ever
// composited into the window — that's what caused the fullscreen black-screen bugs, so we deliberately avoid it.
void RetroView::setupHwRender()
{
    const retro_hw_render_callback& cb = core_.hwRenderCallback();
    threaded_ = false; // a single GL context, driven from the GUI thread

    QSurfaceFormat fmt;
    const bool gles = cb.context_type == RETRO_HW_CONTEXT_OPENGLES2
                   || cb.context_type == RETRO_HW_CONTEXT_OPENGLES3
                   || cb.context_type == RETRO_HW_CONTEXT_OPENGLES_VERSION;
    if (gles)
        fmt.setRenderableType(QSurfaceFormat::OpenGLES);
    else
    {
        fmt.setRenderableType(QSurfaceFormat::OpenGL);
        if (cb.context_type == RETRO_HW_CONTEXT_OPENGL_CORE)
        {
            fmt.setProfile(QSurfaceFormat::CoreProfile);
            fmt.setVersion(cb.version_major ? int(cb.version_major) : 3, int(cb.version_minor));
        }
        else
            fmt.setProfile(QSurfaceFormat::CompatibilityProfile);
    }
    if (cb.depth)   fmt.setDepthBufferSize(24);
    if (cb.stencil) fmt.setStencilBufferSize(8);

    glSurface_ = new QOffscreenSurface();
    glSurface_->setFormat(fmt);
    glSurface_->create();
    glCtx_ = new QOpenGLContext();
    glCtx_->setFormat(fmt);
    if (!glCtx_->create() || !glCtx_->makeCurrent(glSurface_))
    {
        teardownHwRender();
        emit statusMessage(tr("This core needs OpenGL, which couldn't be initialised here."));
        return;
    }

    // The core reads these each frame (installed via SET_HW_RENDER): our FBO to draw into, and GL entry points.
    core_.hwGetFramebuffer  = [this]() -> uintptr_t { return glFbo_ ? uintptr_t(glFbo_->handle()) : 0; };
    core_.hwGetProcAddress  = [this](const char* s) -> void* { return glCtx_ ? (void*)glCtx_->getProcAddress(s) : nullptr; };

    QOpenGLFramebufferObjectFormat ff;
    ff.setAttachment(cb.stencil ? QOpenGLFramebufferObject::CombinedDepthStencil
                   : cb.depth   ? QOpenGLFramebufferObject::Depth
                                : QOpenGLFramebufferObject::NoAttachment);
    const retro_game_geometry& g = core_.avInfo().geometry;
    glFbo_ = new QOpenGLFramebufferObject(int(qMax(g.max_width, 1u)), int(qMax(g.max_height, 1u)), ff);
    hwMode_ = true;
    if (cb.context_reset) cb.context_reset(); // the core creates its GL resources here
    glCtx_->doneCurrent();
}

// Read the used region of the FBO into hwImg_ (flipped to a top-down QImage). Called with the context current
// and the FBO bound, right after runFrame().
void RetroView::readbackHwFrame()
{
    const unsigned w = core_.frameWidth(), h = core_.frameHeight();
    if (!w || !h || !glCtx_ || !glFbo_) return;
    glFbo_->bind(); // the core may have left a different framebuffer bound; read from ours
    QImage img(int(w), int(h), QImage::Format_RGBA8888);
    glCtx_->functions()->glReadPixels(0, 0, int(w), int(h), GL_RGBA, GL_UNSIGNED_BYTE, img.bits());
    // libretro GL cores render bottom-left origin; flip vertically so row 0 is the top for painting.
    hwImg_ = core_.hwRenderCallback().bottom_left_origin ? img.mirrored(false, true) : img;
}

void RetroView::teardownHwRender()
{
    if (glCtx_ && glSurface_ && glCtx_->makeCurrent(glSurface_))
    {
        const retro_hw_render_callback& cb = core_.hwRenderCallback();
        if (hwMode_ && cb.context_destroy) cb.context_destroy(); // let the core free its GL resources first
        delete glFbo_; glFbo_ = nullptr;
        glCtx_->doneCurrent();
    }
    delete glFbo_;     glFbo_ = nullptr; // (in case makeCurrent failed above)
    delete glCtx_;     glCtx_ = nullptr;
    delete glSurface_; glSurface_ = nullptr;
    core_.hwGetFramebuffer = nullptr;
    core_.hwGetProcAddress = nullptr;
    hwImg_ = QImage();
    hwMode_ = false;
}

QString RetroView::statePath() const
{
    const QString dir = AppPaths::dataDir() + QStringLiteral("/states");
    QDir().mkpath(dir);
    return dir + QStringLiteral("/") + QFileInfo(romPath_).completeBaseName() + QStringLiteral(".state");
}

QString RetroView::statePath(int slot) const
{
    return statePath() + QString::number(slot); // <base>.state1 … .state6 (legacy .state = quick slot fallback)
}

QString RetroView::thumbPath(int slot) const { return statePath(slot) + QStringLiteral(".png"); }

// A copy of the frame currently on screen (software or hardware path), for a slot thumbnail. Save states are
// blocked in threaded/split mode, so the worker-frame path isn't needed here.
QImage RetroView::currentFrameImage()
{
    if (hwMode_) return hwImg_;
    if (!core_.hasFrame()) return QImage();
    const unsigned w = core_.frameWidth(), h = core_.frameHeight();
    return QImage(core_.frameBGRA(), static_cast<int>(w), static_cast<int>(h),
                  static_cast<int>(w * 4), QImage::Format_RGB32).copy();
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

// F2/F4 quick save/load act on the current slot (which follows the last slot used in the visual menu).
bool RetroView::saveState(QString* error) { return saveState(currentSlot_, error); }
bool RetroView::loadState(QString* error) { return loadState(currentSlot_, error); }

bool RetroView::saveState(int slot, QString* error)
{
    if (!running_) { if (error) *error = tr("No game is running."); return false; }
    if (threaded_) { if (error) *error = tr("Save states aren’t available in split screen."); return false; }
    std::vector<uint8_t> data;
    if (!core_.saveState(data))
    {
        if (error) *error = tr("This core doesn't support save states for this game.");
        return false;
    }
    QFile f(statePath(slot));
    if (!f.open(QIODevice::WriteOnly) ||
        f.write(reinterpret_cast<const char*>(data.data()), static_cast<qint64>(data.size())) != static_cast<qint64>(data.size()))
    {
        if (error) *error = tr("Couldn't write the save-state file.");
        return false;
    }
    f.close();
    // A thumbnail of the current frame, so the slot menu shows what's in each slot.
    const QImage img = currentFrameImage();
    if (!img.isNull()) img.scaledToWidth(240, Qt::SmoothTransformation).save(thumbPath(slot), "PNG");
    currentSlot_ = slot;
    emit statusMessage(tr("State saved to slot %1").arg(slot));
    return true;
}

bool RetroView::loadState(int slot, QString* error)
{
    if (!running_) { if (error) *error = tr("No game is running."); return false; }
    if (threaded_) { if (error) *error = tr("Save states aren’t available in split screen."); return false; }
    QString path = statePath(slot);
    if (!QFile::exists(path) && slot == 1 && QFile::exists(statePath())) path = statePath(); // legacy slot
    QFile f(path);
    if (!f.exists()) { if (error) *error = tr("No saved state in slot %1 yet.").arg(slot); return false; }
    if (!f.open(QIODevice::ReadOnly)) { if (error) *error = tr("Couldn't read the save-state file."); return false; }
    const QByteArray bytes = f.readAll();
    if (!core_.loadState(reinterpret_cast<const uint8_t*>(bytes.constData()), static_cast<size_t>(bytes.size())))
    {
        if (error) *error = tr("The saved state couldn't be restored (it may be from a different core).");
        return false;
    }
    currentSlot_ = slot;
    emit statusMessage(tr("State loaded from slot %1").arg(slot));
    return true;
}

void RetroView::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), Qt::black);
    // Bezel/border art fills the surround; the game is drawn on top (centered, aspect-fit) so it covers the
    // bezel's transparent screen area and the artwork shows in the letterbox/pillarbox around it.
    if (!bezel_.isNull()) p.drawImage(rect(), bezel_);

    if (threaded_) // paint the worker's last handed-off frame (never touch the core from the GUI thread)
    {
        QMutexLocker lk(&frameMutex_);
        if (!frameImg_.isNull())
        {
            const QSize t = frameImg_.size().scaled(size(), Qt::KeepAspectRatio);
            const QRect dst(QPoint((width() - t.width()) / 2, (height() - t.height()) / 2), t);
            p.setRenderHint(QPainter::SmoothPixmapTransform, false);
            p.drawImage(dst, frameImg_);
            applyVideoFilter(p, dst, frameImg_.width(), frameImg_.height());
        }
    }
    else if (hwMode_) // hardware core: paint the frame we read back from the GL FBO
    {
        if (!hwImg_.isNull())
        {
            const QSize t = hwImg_.size().scaled(size(), Qt::KeepAspectRatio);
            const QRect dst(QPoint((width() - t.width()) / 2, (height() - t.height()) / 2), t);
            p.setRenderHint(QPainter::SmoothPixmapTransform, false);
            p.drawImage(dst, hwImg_);
            applyVideoFilter(p, dst, hwImg_.width(), hwImg_.height());
        }
    }
    else if (core_.hasFrame())
    {
        const unsigned w = core_.frameWidth(), h = core_.frameHeight();
        // frameBGRA() is tightly packed BGRA == QImage::Format_RGB32 byte order on little-endian.
        QImage img(core_.frameBGRA(), static_cast<int>(w), static_cast<int>(h),
                   static_cast<int>(w * 4), QImage::Format_RGB32);

        const QSize target = img.size().scaled(size(), Qt::KeepAspectRatio);
        const QRect dst(QPoint((width() - target.width()) / 2, (height() - target.height()) / 2), target);
        p.setRenderHint(QPainter::SmoothPixmapTransform, false); // crisp, non-blurry pixels
        p.drawImage(dst, img);
        applyVideoFilter(p, dst, static_cast<int>(w), static_cast<int>(h));
    }

    paintAchievementToast(p); // RetroAchievements unlock popup, over both render paths
}

// ---- On-screen RetroAchievements unlock toast ------------------------------------------------------------
// Fade-in / hold / fade-out timings (ms). The status bar is hidden in full-screen emulation, so the unlock
// has to be drawn on the game surface here or the player never sees it.
static constexpr int kAchFadeIn = 300, kAchHold = 3600, kAchFadeOut = 600;
static constexpr int kAchTotal = kAchFadeIn + kAchHold + kAchFadeOut;

void RetroView::showAchievement(const QString& title, const QString& description, int points, const QString& badgeUrl)
{
    AchToast t;
    t.title = title;
    t.sub = points > 0 ? tr("Achievement unlocked · %1 pts").arg(points) : tr("Achievement unlocked");
    if (!description.isEmpty()) t.sub += QStringLiteral(" — ") + description;
    t.badgeUrl = badgeUrl;
    achQueue_.push_back(t);
    if (!achActive_) startNextToast(); // otherwise it plays after the current one finishes
}

void RetroView::startNextToast()
{
    if (achQueue_.empty()) { achActive_ = false; if (achTimer_) achTimer_->stop(); update(); return; }
    achCur_ = achQueue_.front();
    achQueue_.pop_front();
    achBadge_ = QImage();
    achActive_ = true;
    achClock_.restart();

    // Fetch the badge art (best-effort; the toast shows text + a trophy glyph until/if it arrives).
    if (!achCur_.badgeUrl.isEmpty())
    {
        if (!achNam_) achNam_ = new QNetworkAccessManager(this);
        QNetworkRequest rq{ QUrl(achCur_.badgeUrl) };
        rq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        const QString wantUrl = achCur_.badgeUrl;
        QNetworkReply* reply = achNam_->get(rq);
        connect(reply, &QNetworkReply::finished, this, [this, reply, wantUrl] {
            reply->deleteLater();
            if (reply->error() == QNetworkReply::NoError && achActive_ && achCur_.badgeUrl == wantUrl)
            {
                QImage img; if (img.loadFromData(reply->readAll())) { achBadge_ = img; update(); }
            }
        });
    }

    if (!achTimer_) // ~30fps repaint so the fade animates even while the game is paused
    {
        achTimer_ = new QTimer(this);
        achTimer_->setInterval(33);
        connect(achTimer_, &QTimer::timeout, this, [this] {
            if (!achActive_) { achTimer_->stop(); return; }
            if (achClock_.elapsed() >= kAchTotal) startNextToast(); // advance to the next queued unlock (or stop)
            else update();
        });
    }
    achTimer_->start();
    update();
}

void RetroView::paintAchievementToast(QPainter& p)
{
    if (!achActive_) return;
    const qint64 e = achClock_.elapsed();
    if (e >= kAchTotal) return;
    // Opacity envelope: ease in, hold at full, ease out.
    double op = 1.0;
    if (e < kAchFadeIn)                    op = double(e) / kAchFadeIn;
    else if (e > kAchFadeIn + kAchHold)    op = 1.0 - double(e - kAchFadeIn - kAchHold) / kAchFadeOut;
    op = qBound(0.0, op, 1.0);

    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);

    // Card sized to the widget; a small badge on the left, two lines of text on the right. Anchored bottom-centre,
    // rising slightly as it fades in (a subtle slide, like the RetroArch/RA popup).
    const int pad = qMax(8, height() / 90);
    const int badge = qMax(40, height() / 12);
    QFont titleFont = p.font(); titleFont.setPixelSize(qMax(14, height() / 30)); titleFont.setBold(true);
    QFont subFont = p.font();   subFont.setPixelSize(qMax(11, height() / 45));
    const QFontMetrics tfm(titleFont), sfm(subFont);
    const int textW = qMax(tfm.horizontalAdvance(achCur_.title), sfm.horizontalAdvance(achCur_.sub));
    const int cardW = qMin(width() - 2 * pad, badge + 3 * pad + qMin(textW, width() * 2 / 3));
    const int cardH = badge + 2 * pad;
    const int rise = int((1.0 - op) * 24);
    const QRect card((width() - cardW) / 2, height() - cardH - pad * 2 + rise, cardW, cardH);

    p.setOpacity(op);
    // Card background + gold accent border.
    p.setPen(QPen(QColor(255, 200, 60, 220), 2));
    p.setBrush(QColor(18, 22, 30, 235));
    p.drawRoundedRect(card, 12, 12);

    // Badge (fetched art, else a trophy glyph on a gold chip).
    const QRect bRect(card.left() + pad, card.top() + pad, badge, badge);
    if (!achBadge_.isNull())
    {
        p.setBrush(Qt::NoBrush); p.setPen(Qt::NoPen);
        p.drawImage(bRect, achBadge_.scaled(bRect.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }
    else
    {
        p.setPen(Qt::NoPen); p.setBrush(QColor(255, 200, 60, 200));
        p.drawRoundedRect(bRect, 8, 8);
        p.setPen(QColor(30, 25, 10)); QFont gf = p.font(); gf.setPixelSize(badge * 3 / 5); p.setFont(gf);
        p.drawText(bRect, Qt::AlignCenter, QStringLiteral("🏆"));
    }

    // Text block: title (bold) over the "Achievement unlocked · N pts — desc" subtitle, both elided to fit.
    const int tx = bRect.right() + pad + 4;
    const int tw = card.right() - pad - tx;
    p.setPen(QColor(245, 246, 250));
    p.setFont(titleFont);
    p.drawText(QRect(tx, card.top() + pad, tw, tfm.height()),
               Qt::AlignLeft | Qt::AlignVCenter, tfm.elidedText(achCur_.title, Qt::ElideRight, tw));
    p.setPen(QColor(255, 205, 90));
    p.setFont(subFont);
    p.drawText(QRect(tx, card.bottom() - pad - sfm.height(), tw, sfm.height()),
               Qt::AlignLeft | Qt::AlignVCenter, sfm.elidedText(achCur_.sub, Qt::ElideRight, tw));
    p.restore();
}

void RetroView::keyPressEvent(QKeyEvent* e)
{
    if (e->isAutoRepeat()) return;

    // Esc toggles the in-game pause menu; within the slot grid it steps back to the main page first.
    if (e->key() == Qt::Key_Escape)
    {
        if (menu_ && menu_->isVisible() && slotsMode_) { showMainMenu(); return; }
        toggleMenu(); return;
    }

    // While the pause menu is up, arrow keys move between its buttons and Enter activates one; every other
    // key is swallowed so it never reaches the (paused) game.
    if (menu_ && menu_->isVisible())
    {
        const int key = e->key();
        if ((key == Qt::Key_Up || key == Qt::Key_Down) && !menuButtons_.isEmpty())
        {
            int idx = menuButtons_.indexOf(qobject_cast<QPushButton*>(focusWidget()));
            if (idx < 0) idx = 0;
            else         idx = (idx + (key == Qt::Key_Down ? 1 : menuButtons_.size() - 1)) % menuButtons_.size();
            menuButtons_[idx]->setFocus(Qt::TabFocusReason); // wraps around the short list
            if (subScroll_ && focusWidget()) subScroll_->ensureWidgetVisible(focusWidget()); // follow focus in a long list
        }
        else if (key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Select)
        {
            if (auto* b = qobject_cast<QPushButton*>(focusWidget())) b->click();
            else if (!menuButtons_.isEmpty()) menuButtons_.first()->click();
        }
        return;
    }

    // Save-state hotkeys (RetroArch-style: F2 save, F4 load) - reserved, not remappable.
    if (e->key() == Qt::Key_F2) { QString err; if (!saveState(&err)) emit statusMessage(err); return; }
    if (e->key() == Qt::Key_F4) { QString err; if (!loadState(&err)) emit statusMessage(err); return; }
    // Fast-forward (hold Tab) and rewind (hold R) - reserved hotkeys, kept out of the gameplay keymap.
    if (e->key() == Qt::Key_Tab) { ffKey_ = true; return; }
    if (e->key() == Qt::Key_R)   { rewindKey_ = true; return; }
    // Screenshot (F12) - saves the clean frame to <app>/screenshots.
    if (e->key() == Qt::Key_F12)
    {
        const QString p = captureScreenshot();
        emit statusMessage(p.isEmpty() ? tr("Couldn't save screenshot.")
                                       : tr("Screenshot saved: %1").arg(QFileInfo(p).fileName()));
        return;
    }

    pressedKeys_.insert(e->key()); // resolved to a (port, button) per the keymap in inputState()
}

void RetroView::keyReleaseEvent(QKeyEvent* e)
{
    if (e->isAutoRepeat()) return;
    if (e->key() == Qt::Key_Tab) { ffKey_ = false; return; }
    if (e->key() == Qt::Key_R)   { rewindKey_ = false; return; }
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
    // Netplay: the core reads both players from the synced input buffers, not the live devices (digital only).
    if (netActive_ && net_ && net_->ready())
    {
        if (device == RETRO_DEVICE_JOYPAD)
        {
            if (port == netLocalPort_)  return (netCurLocal_  >> id) & 1;
            if (port == netRemotePort_) return (netCurRemote_ >> id) & 1;
        }
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
    audioSink_->setBufferSize(fmt.bytesForDuration(150000)); // ~150 ms of slack for timer jitter
    audioSink_->setVolume(volume_);                          // per-pane mix level (1.0 = full)
    audioSrcRate_ = sampleRate;
    audioOutRate_ = outRate;
    audioBytesPerSec_ = outRate * 2 * 2;                     // stereo S16 at the OUTPUT rate
    rsStepBase_ = double(sampleRate) / double(outRate);
    rsStep_ = rsStepBase_;
    rsIntegral_ = 0.0;
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
    if (fastForward_ || rewinding_) return; // muted: N× or reversed audio is just noise
    if (audioSrcRate_ == audioOutRate_)
        pendingAudio_.append(reinterpret_cast<const char*>(data), static_cast<qsizetype>(frames) * 4); // 4 bytes/frame
    else
    {
        // Dynamic rate control (PI): the frame timer's integer-ms interval never matches the core's true frame
        // rate (17ms vs GBA's 16.74ms => ~1.5% slow => the buffer drains and underruns, which clicks). Nudge the
        // resample ratio to hold the buffered audio near 50%. The integral term cancels the *steady* drift (a
        // proportional-only controller would sit near-empty to keep producing extra, still risking underrun).
        if (audioSink_)
        {
            const qint64 bufSize = audioSink_->bufferSize();
            if (bufSize > 0)
            {
                const qint64 queued = (bufSize - audioSink_->bytesFree()) + pendingAudio_.size();
                const double err = double(queued) / double(bufSize) - 0.5; // + = too full, - = draining
                rsIntegral_ = qBound(-0.03, rsIntegral_ + err * 0.0004, 0.03); // cancels the steady drift (±3%)
                const double corr = qBound(-0.05, 0.05 * err + rsIntegral_, 0.05);
                rsStep_ = rsStepBase_ * (1.0 + corr); // too full -> larger step -> fewer out samples -> drains
            }
        }
        resampleAppend(data, frames); // core rate (e.g. 32040) -> device native rate (e.g. 48000)
    }

    const qint64 freeBytes = audioSink_->bytesFree();
    if (freeBytes > 0 && !pendingAudio_.isEmpty())
    {
        const qint64 n = qMin<qint64>(freeBytes, pendingAudio_.size());
        const qint64 written = audioIo_->write(pendingAudio_.constData(), n);
        if (written > 0) pendingAudio_.remove(0, written);
    }
    // Safety net only: with DRC holding the buffer steady the pending backlog stays tiny, so this rarely fires
    // (raised well above the DRC target so it isn't a periodic click source). Trims the oldest queued audio.
    const qsizetype maxBacklog = static_cast<qsizetype>(audioBytesPerSec_) * 300 / 1000;
    if (audioBytesPerSec_ > 0 && pendingAudio_.size() > maxBacklog)
        pendingAudio_.remove(0, pendingAudio_.size() - maxBacklog);
}
