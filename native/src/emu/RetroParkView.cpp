#include "RetroParkView.h"

#include <QTimer>
#include <QPainter>
#include <QFrame>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QKeyEvent>

// The RetroPark runtime is a desktop/Windows static lib linked into the app under this define (see the RetroPark
// block in native/CMakeLists.txt). Everything that touches rp_runtime_* lives behind it, so the widget still
// compiles on a build without RetroPark — openGame() just fails gracefully there.
#ifdef EB_HAVE_RETROPARK
#include <retropark/retropark.h>
#include "loader/StaticCoreRegistry.h"
// The driven reference core's getter, renamed at compile time by native/CMakeLists.txt so its RefCoreDriven.cpp
// links straight into the app without colliding with the ABI's canonical rp_get_core_abi symbol name — the same
// DLL-free static-core path probe_retropark / probe_retropark_loop use.
extern "C" const rp_core_abi* refcore_driven_static_get_core_abi(void);
#endif

namespace {
// The runtime's internal render resolution for the driven surface. The driven core paints a 64×64 field that the
// compositor upscales into this target; paintEvent then aspect-fits the read-back image into the widget. Fixed
// (not re-sized with the widget) so the per-frame read-back buffer never reallocates inside the present loop.
constexpr uint32_t kRpW = 512, kRpH = 448;
// ~60 fps — the rate the driven core reports (RefCoreDriven get_av_info fps = 60). A later content core would
// pace to its own get_av_info; 2a's driven pattern is happy at 60.
constexpr int kFrameIntervalMs = 16;
}

RetroParkView::RetroParkView(QWidget* parent) : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    // Painted black in paintEvent; opaque so no compositor cost from a transparent surface.
    setAttribute(Qt::WA_OpaquePaintEvent, true);

    timer_ = new QTimer(this);
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

    auto* title = new QLabel(tr("Paused"), menu_);
    title->setAlignment(Qt::AlignCenter);
    title->setStyleSheet(QStringLiteral("font-size:18px; font-weight:600;"));
    v->addWidget(title);

    resumeBtn_ = new QPushButton(tr("Resume"), menu_);
    exitBtn_   = new QPushButton(tr("Exit"), menu_);
    v->addWidget(resumeBtn_);
    v->addWidget(exitBtn_);

    connect(resumeBtn_, &QPushButton::clicked, this, &RetroParkView::hideMenu);
    connect(exitBtn_,   &QPushButton::clicked, this, [this] {
        menu_->hide();
        stop();                 // tear down the runtime (emits gameStopped)
        emit exitRequested();   // …then the host returns Home, mirroring RetroView's Exit
    });

    menu_->hide();
}

void RetroParkView::openGame(const QString& coreOrId, const QString& romPath, const QString& title,
                             const QString& systemId, const QString& gameKey, QString* error)
{
    Q_UNUSED(coreOrId);
    Q_UNUSED(romPath);   // 2a: the ROM is ignored — the driven reference core is what loads.
    stop();              // tear down anything already running (openGame restarts cleanly)

    title_ = title; systemId_ = systemId; gameKey_ = gameKey;

#ifdef EB_HAVE_RETROPARK
    // Register the statically-compiled-in driven core once, under the id the runtime resolves with no DLL.
    static bool registered = false;
    if (!registered) {
        rp::StaticCoreRegistry::register_core("refcore_driven", &refcore_driven_static_get_core_abi);
        registered = true;
    }

    rt_ = rp_runtime_create(RP_GFX_D3D11, nullptr);
    if (!rt_) {
        if (error) *error = tr("RetroPark could not create a graphics device.");
        return;
    }
    rpW_ = kRpW; rpH_ = kRpH;
    if (rp_runtime_resize(rt_, rpW_, rpH_) != RP_OK) {
        if (error) *error = tr("RetroPark could not size its output.");
        rp_runtime_destroy(rt_); rt_ = nullptr; rpW_ = rpH_ = 0;
        return;
    }
    if (rp_runtime_load_static_core(rt_, "refcore_driven") != RP_OK) {
        if (error) *error = tr("RetroPark could not load its reference core.");
        rp_runtime_destroy(rt_); rt_ = nullptr; rpW_ = rpH_ = 0;
        return;
    }
    buf_.assign((size_t)rpW_ * rpH_ * 4, 0);
    running_ = true;
    setFocus();
    timer_->start(kFrameIntervalMs);
    update();
#else
    Q_UNUSED(title); Q_UNUSED(systemId); Q_UNUSED(gameKey);
    if (error) *error = tr("RetroPark is not available in this build.");
#endif
}

void RetroParkView::tick()
{
#ifdef EB_HAVE_RETROPARK
    if (!rt_ || buf_.empty()) return;
    // Composite the driven core's advanced frame into our RGBA8 read-back buffer, then repaint. present() advances
    // the core one frame unless it is paused (in which case the timer is stopped and tick() does not run anyway).
    if (rp_runtime_present(rt_, buf_.data()) != RP_OK) return;
    update();
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
    if (e->isAutoRepeat()) { QWidget::keyPressEvent(e); return; }

    // Esc / Back opens and closes the pause menu (Qt::Key_Back is the Android/TV-remote Back).
    if (e->key() == Qt::Key_Escape || e->key() == Qt::Key_Back) { toggleMenu(); return; }

    // While the menu is up, Up/Down move between its two buttons and Enter activates one; nothing reaches the
    // (paused) game.
    if (menu_ && menu_->isVisible())
    {
        const int k = e->key();
        if ((k == Qt::Key_Up || k == Qt::Key_Down) && resumeBtn_ && exitBtn_)
        {
            (focusWidget() == resumeBtn_ ? exitBtn_ : resumeBtn_)->setFocus(Qt::TabFocusReason);
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

    QWidget::keyPressEvent(e);
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
    if (running_ && timer_) timer_->start(kFrameIntervalMs);
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
    update();
    if (was) emit gameStopped();  // only when a game was actually running (stop() is safe to call when idle)
}
