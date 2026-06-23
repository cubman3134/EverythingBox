#include "HomeView.h"
#include "../addons/AddonManager.h"
#include "../core/RecentStore.h"
#include "../core/ProfileStore.h"
#include "../core/FavoritesStore.h"
#include "../core/Theme.h"
#include "../core/SystemCatalog.h"
#include "CarouselView.h"
#include <QHash>

#include <QApplication>
#include <QPaintEvent>
#include <QSet>
#include <QListWidget>
#include <QAbstractItemView>
#include <QMenu>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFrame>
#include <QTextBrowser>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QScrollBar>
#include <QPixmap>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QIcon>
#include <QFont>
#include <QStringList>
#include <QFileInfo>
#include <QDir>
#include <QFile>
#include <QBoxLayout>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QEvent>
#include <QColor>
#include <QPalette>
#include <QUrl>

static const QSize kPoster(140, 200);
static const int kTopBtnHeight = 34; // all top-bar buttons (tabs + chrome) share this height

// Addon-defined media-type visuals, keyed by media type. Populated from every addon's manifest
// "mediaTypes"/"accent" in refresh(); consulted (with a fallback to the built-ins) by typeColor/defaultIcon.
struct TypeVisual
{
    QColor color;
    QString glyph;        // emoji placeholder
    QString iconPath;     // resolved bundled image (svg/png) placeholder, if any
    QString openKind;
    QString detailLayout; // "" / "poster" | "banner" | "text"
};
static QHash<QString, TypeVisual> g_typeVisuals;
static Theme g_theme; // the active colour theme (set in refresh()/applyTheme())

// A distinct colour per media type. Resolution order: theme override -> addon-declared -> built-in.
static QColor typeColor(const QString& type)
{
    QColor c;
    auto th = g_theme.tabColors.constFind(type);
    auto reg = g_typeVisuals.constFind(type);
    if (th != g_theme.tabColors.constEnd() && th->isValid()) c = *th;            // theme override
    else if (reg != g_typeVisuals.constEnd() && reg->color.isValid()) c = reg->color; // addon-declared
    else if (type == "movie")                                      c = QColor(0xD7, 0x4B, 0x4B); // red
    else if (type == "series")                                     c = QColor(0x3F, 0x7B, 0xD8); // blue
    else if (type == "game" || type == "platform")                 c = QColor(0x3F, 0xA9, 0x5E); // green
    else if (type == "album" || type == "track" || type == "audiobook") c = QColor(0x8A, 0x5C, 0xC8); // purple
    else if (type == "book")                                       c = QColor(0xC9, 0x97, 0x2E); // amber
    else if (type == "comic" || type == "comic_issue")             c = QColor(0xE0, 0x7A, 0x2E); // orange
    else if (type == "manga" || type == "manga_chapter")           c = QColor(0xCE, 0x57, 0x97); // pink
    else if (type == "home")                                       c = QColor(0x53, 0x82, 0xC4); // home blue
    else                                                           c = QColor(0x6A, 0x6E, 0x78); // default grey
    return c.lighter(125); // lighter buttons
}

// A light, desaturated version of a colour (mostly white) for the catalogue background.
static QColor lightTint(const QColor& c, qreal w = 0.14)
{
    return QColor(int(255 * (1 - w) + c.red() * w),
                  int(255 * (1 - w) + c.green() * w),
                  int(255 * (1 - w) + c.blue() * w));
}

// Chrome buttons (Back / profile / Settings) take the active accent colour with white text - no dark.
// Same padding/shape as the tabs so the whole top bar is one seamless strip of equally-sized buttons.
static QString chromeButtonStyle(const QColor& c)
{
    return QString(
        "QPushButton{background:%1;color:white;border:none;border-radius:0;padding:8px 16px;font-weight:bold;}"
        "QPushButton:hover{background:%2;}"
        "QPushButton:disabled{background:%3;color:#f4f4f4;}")
        .arg(c.name(), c.lighter(112).name(), c.lighter(135).name());
}

static QString chromeEditStyle(const QColor& c, int radius)
{
    // Light *tint* of the accent (not pure white) and no border, so no white edge shows next to the buttons.
    return QString("QLineEdit{background:%1;color:#1b1b1b;border:none;border-radius:%2px;padding:6px 10px;}")
        .arg(lightTint(c, 0.30).name()).arg(radius);
}

static QString tabStyle(const QColor& c, bool active)
{
    const QColor base = active ? c.lighter(120) : c;
    const QColor hover = base.lighter(112);
    return QString(
        "QPushButton{background:%1;color:white;border:none;border-radius:0;%3 padding:8px 16px;font-weight:bold;}"
        "QPushButton:hover{background:%2;}")
        .arg(base.name(), hover.name(),
             active ? QStringLiteral("border-bottom:3px solid #ffffff;")
                    : QStringLiteral("border-bottom:3px solid transparent;"));
}

// A "+" tile used as the thumbnail for the "open a file" item at the head of a catalog.
static QIcon plusIcon(const QSize& size)
{
    QPixmap pm(size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(58, 58, 66));
    p.drawRoundedRect(QRectF(2, 2, size.width() - 4, size.height() - 4), 14, 14);
    QPen border(QColor(255, 255, 255, 60));
    border.setWidth(2); border.setStyle(Qt::DashLine);
    p.setPen(border); p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(9, 9, size.width() - 18, size.height() - 18), 10, 10);
    QPen plus(QColor(235, 235, 235));
    plus.setWidthF(size.width() * 0.06); plus.setCapStyle(Qt::RoundCap);
    p.setPen(plus);
    const qreal cx = size.width() / 2.0, cy = size.height() / 2.0, r = size.width() * 0.18;
    p.drawLine(QPointF(cx - r, cy), QPointF(cx + r, cy));
    p.drawLine(QPointF(cx, cy - r), QPointF(cx, cy + r));
    p.end();
    return QIcon(pm);
}

// A placeholder tile drawn for items whose poster image is missing or fails to load, keyed by media type:
// a music note, a gamepad, a film strip, a book, or a generic play glyph - each on a type-colored card.
static QIcon defaultIcon(const QString& type, const QSize& size)
{
    QPixmap pm(size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const qreal W = size.width(), H = size.height(), cx = W / 2, cy = H / 2;

    // Addon-defined type: a coloured card with the addon's bundled image (svg/png) or emoji centred.
    auto reg = g_typeVisuals.constFind(type);
    if (reg != g_typeVisuals.constEnd() && (!reg->iconPath.isEmpty() || !reg->glyph.isEmpty()))
    {
        p.setPen(Qt::NoPen);
        p.setBrush(reg->color.isValid() ? reg->color : QColor(70, 72, 82));
        p.drawRoundedRect(QRectF(2, 2, W - 4, H - 4), 14, 14);
        if (!reg->iconPath.isEmpty())
        {
            const QPixmap img(reg->iconPath);
            if (!img.isNull())
            {
                const QPixmap sc = img.scaled(int(W * 0.62), int(H * 0.62),
                                              Qt::KeepAspectRatio, Qt::SmoothTransformation);
                p.drawPixmap(int((W - sc.width()) / 2), int((H - sc.height()) / 2), sc);
                p.end();
                return QIcon(pm);
            }
        }
        QFont f = p.font();
        f.setPointSizeF(H * 0.34);
        p.setFont(f);
        p.setPen(Qt::white);
        p.drawText(QRect(0, 0, int(W), int(H)), Qt::AlignCenter, reg->glyph);
        p.end();
        return QIcon(pm);
    }

    const bool music = (type == "album" || type == "track");
    const bool game  = (type == "game" || type == "platform");
    const bool film  = (type == "movie" || type == "series" || type == "season" || type == "episode");
    const bool book  = (type == "book" || type == "audiobook" || type == "manga" || type == "manga_chapter"
                        || type == "comic" || type == "comic_issue");

    QColor bg = music ? QColor(99, 67, 168) : game ? QColor(38, 122, 60)
              : film  ? QColor(168, 52, 60) : book ? QColor(176, 122, 40) : QColor(70, 72, 82);
    const QColor fg(238, 238, 238);

    p.setPen(Qt::NoPen);
    p.setBrush(bg);
    p.drawRoundedRect(QRectF(2, 2, W - 4, H - 4), 14, 14);

    if (music)
    {
        const qreal r = W * 0.11, sx = cx - W * 0.10 + r * 0.95;
        p.setBrush(fg);
        p.drawEllipse(QPointF(cx - W * 0.10, cy + H * 0.13), r, r * 0.78); // note head
        QPen stem(fg); stem.setWidthF(W * 0.045); stem.setCapStyle(Qt::RoundCap);
        p.setPen(stem);
        p.drawLine(QPointF(sx, cy + H * 0.13), QPointF(sx, cy - H * 0.18)); // stem
        QPen flag(fg); flag.setWidthF(W * 0.05); flag.setCapStyle(Qt::RoundCap);
        p.setPen(flag); p.setBrush(Qt::NoBrush);
        QPainterPath fl; fl.moveTo(sx, cy - H * 0.18);
        fl.quadTo(cx + W * 0.22, cy - H * 0.16, cx + W * 0.10, cy - H * 0.02);
        p.drawPath(fl);
    }
    else if (game)
    {
        p.setBrush(fg);
        p.drawRoundedRect(QRectF(W * 0.16, cy - H * 0.085, W * 0.68, H * 0.17), H * 0.085, H * 0.085); // body
        p.drawEllipse(QPointF(W * 0.20, cy + H * 0.02), W * 0.10, W * 0.10); // left grip
        p.drawEllipse(QPointF(W * 0.80, cy + H * 0.02), W * 0.10, W * 0.10); // right grip
        p.setBrush(bg);
        const qreal d = W * 0.028;
        p.drawRect(QRectF(W * 0.31 - d, cy - d * 2.4, d * 2, d * 4.8));   // d-pad vertical
        p.drawRect(QRectF(W * 0.31 - d * 2.4, cy - d, d * 4.8, d * 2));   // d-pad horizontal
        p.drawEllipse(QPointF(W * 0.66, cy - d * 0.6), d, d);            // buttons
        p.drawEllipse(QPointF(W * 0.73, cy + d * 0.6), d, d);
    }
    else if (film)
    {
        const QRectF strip(W * 0.27, H * 0.22, W * 0.46, H * 0.56);
        p.setBrush(fg);
        p.drawRoundedRect(strip, 6, 6);
        p.setBrush(bg);
        const qreal pw = W * 0.05, ph = H * 0.045, gap = H * 0.085;
        for (qreal y = strip.top() + ph * 0.6; y < strip.bottom() - ph; y += gap)
        {
            p.drawRoundedRect(QRectF(strip.left() + W * 0.015, y, pw, ph), 2, 2);
            p.drawRoundedRect(QRectF(strip.right() - W * 0.015 - pw, y, pw, ph), 2, 2);
        }
        p.drawRect(QRectF(strip.left() + W * 0.09, strip.top() + ph * 0.6,
                          strip.width() - W * 0.18, strip.height() - ph * 1.2)); // window
    }
    else if (book)
    {
        const QRectF cover(W * 0.30, H * 0.24, W * 0.40, H * 0.52);
        p.setBrush(fg);
        p.drawRoundedRect(cover, 5, 5);
        QPen lines(bg); lines.setWidthF(W * 0.022); lines.setCapStyle(Qt::RoundCap);
        p.setPen(lines);
        p.drawLine(QPointF(cover.left() + W * 0.07, cover.top() + H * 0.04),
                   QPointF(cover.left() + W * 0.07, cover.bottom() - H * 0.04)); // spine
        p.drawLine(QPointF(cover.left() + W * 0.14, cover.center().y() - H * 0.04),
                   QPointF(cover.right() - W * 0.05, cover.center().y() - H * 0.04));
        p.drawLine(QPointF(cover.left() + W * 0.14, cover.center().y() + H * 0.02),
                   QPointF(cover.right() - W * 0.05, cover.center().y() + H * 0.02));
    }
    else
    {
        const qreal rr = W * 0.22;
        QPen ring(fg); ring.setWidthF(W * 0.04);
        p.setPen(ring); p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(cx, cy), rr, rr);
        p.setPen(Qt::NoPen); p.setBrush(fg);
        QPainterPath tri;
        tri.moveTo(cx - rr * 0.35, cy - rr * 0.5);
        tri.lineTo(cx + rr * 0.55, cy);
        tri.lineTo(cx - rr * 0.35, cy + rr * 0.5);
        tri.closeSubpath();
        p.drawPath(tri);
    }
    p.end();
    return QIcon(pm);
}

// A round avatar for the profile button: the chosen emoji centered on a colored disc (the colour is
// derived from the name so each profile is recognisable even if the glyph font renders plainly).
static QIcon avatarIcon(const QString& glyph, const QString& seed, int size)
{
    static const QColor palette[] = {
        QColor(0xE2, 0x57, 0x6B), QColor(0x4F, 0x9D, 0xE0), QColor(0x4C, 0xAF, 0x73),
        QColor(0xB0, 0x7A, 0x28), QColor(0x8E, 0x6F, 0xD0), QColor(0xE0, 0x8A, 0x3C),
        QColor(0x36, 0xB0, 0xB0), QColor(0x9B, 0x59, 0xB6)
    };
    const int n = int(sizeof(palette) / sizeof(palette[0]));
    const QColor bg = palette[qHash(seed.isEmpty() ? QStringLiteral("?") : seed) % n];

    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(bg);
    p.drawEllipse(0, 0, size, size);
    QFont f = p.font();
    f.setPointSizeF(size * 0.52);
    p.setFont(f);
    p.setPen(Qt::white);
    p.drawText(QRect(0, 0, size, size), Qt::AlignCenter,
               glyph.isEmpty() ? QStringLiteral("🙂") : glyph);
    p.end();
    return QIcon(pm);
}

static QString openTitleFor(const QString& kind)
{
    if (kind == QStringLiteral("video"))    return QObject::tr("Open a video file…");
    if (kind == QStringLiteral("audio"))    return QObject::tr("Open an audio file…");
    if (kind == QStringLiteral("document")) return QObject::tr("Open a book (EPUB/PDF)…");
    if (kind == QStringLiteral("game"))     return QObject::tr("Open a game file…");
    return QObject::tr("Open a file…");
}

// Map a Recent entry's kind to a media type (for the placeholder icon) and a human label (subtitle).
static QString iconTypeForKind(const QString& kind)
{
    if (kind == QStringLiteral("video"))    return QStringLiteral("movie");
    if (kind == QStringLiteral("audio"))    return QStringLiteral("album");
    if (kind == QStringLiteral("document")) return QStringLiteral("book");
    if (kind == QStringLiteral("game"))     return QStringLiteral("game");
    return QString();
}

// Group key for a Recent entry: by media type, and per-console for games ("game:<console>").
static QString recentGroupKey(const RecentItem& r)
{
    if (r.kind == QStringLiteral("game"))
    {
        const QString ext = QFileInfo(r.path).suffix().toLower();
        const GameSystem* sys = SystemCatalog::forExtension(ext);
        return QStringLiteral("game:") + (sys ? sys->name : QObject::tr("Game"));
    }
    return r.kind; // video / audio / document
}

static QString recentGroupLabel(const QString& key)
{
    if (key == QStringLiteral("video"))    return QObject::tr("Videos");
    if (key == QStringLiteral("audio"))    return QObject::tr("Audio");
    if (key == QStringLiteral("document")) return QObject::tr("Books");
    if (key.startsWith(QStringLiteral("game:"))) return key.mid(5); // the console name
    return key;
}


HomeView::HomeView(AddonManager* mgr, QWidget* parent) : QWidget(parent), mgr_(mgr)
{
    nam_ = new QNetworkAccessManager(this);

    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0); // top bar flush with the window edges and the catalogue area
    v->setSpacing(0);

    // Media-type bar (built in refresh()) + back + search, on a themed backing so no seam shows through.
    topBar_ = new QWidget(this);
    topBar_->setObjectName(QStringLiteral("topBar"));
    topBar_->setAttribute(Qt::WA_StyledBackground, true); // ensure its themed background actually paints
    auto* topRow = new QHBoxLayout(topBar_);
    topRow->setContentsMargins(0, 0, 0, 0); // no margins around the top buttons
    topRow->setSpacing(0);
    back_ = new QPushButton(tr("‹ Back"), this);
    back_->setEnabled(false);
    back_->setFixedHeight(kTopBtnHeight);
    connect(back_, &QPushButton::clicked, this, &HomeView::goBack);
    topRow->addWidget(back_);

    typeHost_ = new QWidget(this);
    typeHost_->setObjectName(QStringLiteral("typeHost"));
    typeBar_ = new QHBoxLayout(typeHost_);
    typeBar_->setContentsMargins(0, 0, 0, 0);
    topRow->addWidget(typeHost_, 1);

    search_ = new QLineEdit(this);
    search_->setPlaceholderText(tr("Search…"));
    search_->setMaximumWidth(260);
    search_->setFixedHeight(kTopBtnHeight - 8); // slightly shorter -> small top/bottom margin (centred)
    search_->setFrame(false); // no native frame -> no white edge beside the profile button
    connect(search_, &QLineEdit::returnPressed, this, &HomeView::doSearch);
    topRow->addSpacing(6);    // small margin around the search box only (buttons stay flush)
    topRow->addWidget(search_);
    topRow->addSpacing(6);

    profileBtn_ = new QPushButton(this);
    profileBtn_->setToolTip(tr("Switch profile"));
    profileBtn_->setFixedHeight(kTopBtnHeight);
    connect(profileBtn_, &QPushButton::clicked, this, &HomeView::switchProfileRequested);
    topRow->addWidget(profileBtn_);

    settingsBtn_ = new QPushButton(tr("Settings"), this);
    settingsBtn_->setFixedHeight(kTopBtnHeight);
    connect(settingsBtn_, &QPushButton::clicked, this, &HomeView::settingsRequested);
    topRow->addWidget(settingsBtn_);
    v->addWidget(topBar_);

    // Detail-page metadata header: cover on the left, title / facts / synopsis on the right.
    // Hidden on top-level catalog views; revealed when an item is opened.
    meta_ = new QFrame(this);
    meta_->setObjectName(QStringLiteral("metaHeader"));
    meta_->setFrameShape(QFrame::StyledPanel);
    meta_->setVisible(false);
    metaLayout_ = new QBoxLayout(QBoxLayout::LeftToRight, meta_); // direction switched per detailLayout
    auto* mh = metaLayout_;
    mh->setContentsMargins(12, 12, 12, 12);
    mh->setSpacing(16);
    metaImage_ = new QLabel(meta_);
    metaImage_->setFixedSize(170, 240);
    metaImage_->setAlignment(Qt::AlignCenter);
    mh->addWidget(metaImage_, 0, Qt::AlignTop);
    metaTextCol_ = new QVBoxLayout();
    auto* mc = metaTextCol_;
    mc->setSpacing(8);
    favBtn_ = new QPushButton(tr("☆ Favorite"), meta_);
    favBtn_->setCursor(Qt::PointingHandCursor);
    favBtn_->setStyleSheet(QStringLiteral(
        "QPushButton{background:#FFF1CC;border:2px solid #E0A92E;border-radius:6px;"
        "padding:6px 14px;color:#7A4E00;font-weight:bold;}"
        "QPushButton:hover{background:#FFE49E;}"
        "QPushButton:focus{background:#FFD66B;border-color:#C98A12;}"));
    connect(favBtn_, &QPushButton::clicked, this, [this] {
        if (stack_.isEmpty() || !stack_.last().detail) return;
        const Level& top = stack_.last();
        if (FavoritesStore::isFavorite(top.item.id))
        {
            FavoritesStore::remove(top.item.id);
            favBtn_->setText(tr("☆ Favorite"));
        }
        else
        {
            FavoriteItem f;
            f.addonId = top.addon ? top.addon->manifest.id : QString();
            f.itemId = top.item.id; f.title = top.item.title; f.subtitle = top.item.subtitle;
            f.type = top.item.type; f.thumbnailUrl = top.item.thumbnailUrl; f.expandable = top.item.expandable;
            FavoritesStore::add(f);
            favBtn_->setText(tr("★ Favorited"));
        }
    });
    favBtn_->installEventFilter(this); // Backspace here = Back (the detail page focuses this button)
    mc->addWidget(favBtn_, 0, Qt::AlignLeft);
    metaTitle_ = new QLabel(meta_);
    metaTitle_->setWordWrap(true);
    metaTitle_->setTextFormat(Qt::RichText);
    metaTitle_->setStyleSheet(QStringLiteral("font-size:15pt;"));
    mc->addWidget(metaTitle_);
    metaFacts_ = new QLabel(meta_);
    metaFacts_->setWordWrap(true);
    metaFacts_->setTextFormat(Qt::RichText);
    metaFacts_->setVisible(false);
    mc->addWidget(metaFacts_);
    metaOverview_ = new QTextBrowser(meta_);
    metaOverview_->setFrameShape(QFrame::NoFrame);
    metaOverview_->setOpenExternalLinks(false);
    metaOverview_->setVisible(false);
    mc->addWidget(metaOverview_, 1);
    mh->addLayout(mc, 1);
    v->addWidget(meta_);

    grid_ = new QListWidget(this);
    grid_->setViewMode(QListView::IconMode);
    grid_->setIconSize(kPoster);
    grid_->setGridSize(QSize(kPoster.width() + 24, kPoster.height() + 56));
    grid_->setResizeMode(QListView::Adjust);
    grid_->setMovement(QListView::Static);
    grid_->setWordWrap(true);
    grid_->setSpacing(8);
    grid_->setUniformItemSizes(true); // uniform poster tiles -> no per-scroll relayout (see applyGridMode)
    // Smooth pixel scrolling at a comfortable, fixed speed (we drive the wheel ourselves in eventFilter).
    grid_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    grid_->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    grid_->viewport()->installEventFilter(this); // wheel speed
    grid_->installEventFilter(this);             // Up at the top row -> back to the tabs
    connect(grid_, &QListWidget::itemClicked, this, &HomeView::onItemActivated); // single click opens

    // Right-click a favourite on the Home list to remove it.
    grid_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(grid_, &QListWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        if (!recentView_) return;
        QListWidgetItem* w = grid_->itemAt(pos);
        if (!w) return;
        const int row = grid_->row(w);
        if (row < 0 || row >= items_.size()) return;
        if (!items_[row].mime.startsWith(QStringLiteral("fav:"))) return; // only favourites
        const QString favId = items_[row].id;
        QMenu menu(this);
        QAction* remove = menu.addAction(tr("Remove from Favorites"));
        if (menu.exec(grid_->viewport()->mapToGlobal(pos)) == remove)
        {
            FavoritesStore::remove(favId);
            renderRecents(); // refresh the Home list
        }
    });
    // Infinite scroll: when the user nears the bottom, pull the next page (if the addon has one).
    connect(grid_->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
        QScrollBar* sb = grid_->verticalScrollBar();
        if (sb->maximum() > 0 && value >= sb->maximum() - 8) loadMore();
    });
    v->addWidget(grid_, 1);

    // The media-type carousel (shown instead of the grid landing when the theme's layout is "carousel").
    carousel_ = new CarouselView(this);
    carousel_->hide();
    connect(carousel_, &CarouselView::activated, this, [this](const QString& key) {
        if (key.startsWith(QStringLiteral("item:"))) activateItem(key.mid(5).toInt()); // a catalog item
        else activateNav(key);                                                          // a media type / Home
    });
    connect(carousel_, &CarouselView::backRequested, this, &HomeView::goBack);
    v->addWidget(carousel_, 1);

    // The bottom status/description strip was removed; keep the label as a hidden no-op sink so the
    // existing status_->setText(...) calls remain harmless.
    status_ = new QLabel(this);
    status_->hide();

    connect(mgr_, &AddonManager::catalogReady, this, &HomeView::onCatalogReady);
    connect(mgr_, &AddonManager::metaReady, this, &HomeView::onMetaReady);

    refresh();
}

void HomeView::refresh()
{
    g_theme = ThemeStore::current(); // the active profile's colour theme
    applyThemeFont();

    // Reflect the active profile in the top-bar button: the avatar as an icon + the name as text.
    if (profileBtn_)
    {
        const Profile me = ProfileStore::current();
        profileBtn_->setIcon(avatarIcon(me.icon, me.name, 26));
        profileBtn_->setIconSize(QSize(26, 26));
        profileBtn_->setText(me.name.isEmpty() ? tr("Profile") : me.name);
    }

    // Rebuild the registry of addon-declared media-type visuals (colour/icon/open-kind/layout per type).
    g_typeVisuals.clear();
    for (LoadedAddon* s : mgr_->sources())
    {
        for (const AddonMediaType& mt : s->manifest.mediaTypes)
        {
            if (mt.type.isEmpty()) continue;
            TypeVisual tv;
            if (!mt.color.isEmpty()) tv.color = QColor(mt.color);
            tv.openKind = mt.openKind;
            tv.detailLayout = mt.detailLayout;
            // icon: a bundled image (svg/png/...) resolved against the addon folder, else an emoji glyph.
            const QString icon = mt.icon;
            const bool looksLikeFile = icon.contains(QLatin1Char('/')) || icon.contains(QLatin1Char('\\'))
                || icon.endsWith(QStringLiteral(".svg"), Qt::CaseInsensitive)
                || icon.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive)
                || icon.endsWith(QStringLiteral(".jpg"), Qt::CaseInsensitive)
                || icon.endsWith(QStringLiteral(".webp"), Qt::CaseInsensitive);
            if (looksLikeFile)
            {
                const QString p = QDir::cleanPath(s->dir + QStringLiteral("/") + icon);
                if (QFile::exists(p)) tv.iconPath = p; else tv.glyph = icon;
            }
            else tv.glyph = icon;
            g_typeVisuals.insert(mt.type, tv);
        }
        // Per-addon accent: give this addon's catalog types its accent colour (unless already set above).
        if (!s->manifest.accent.isEmpty())
        {
            const QColor accent(s->manifest.accent);
            for (const AddonCatalog& c : s->manifest.catalogs)
                if (!c.type.isEmpty() && !g_typeVisuals.contains(c.type))
                {
                    TypeVisual tv; tv.color = accent;
                    g_typeVisuals.insert(c.type, tv);
                }
        }
    }
    // Per-theme placeholder icons override the addon/built-in icon for the given media types.
    for (auto it = g_theme.icons.constBegin(); it != g_theme.icons.constEnd(); ++it)
        g_typeVisuals[it.key()].iconPath = it.value();

    // Rebuild the media-type buttons from every enabled source's declared catalogs.
    while (QLayoutItem* it = typeBar_->takeAt(0)) { delete it->widget(); delete it; }
    typeBar_->setSpacing(0); // tabs touch each other
    typeButtons_.clear();
    navTargets_.clear();
    activeTypeButton_ = nullptr;

    auto makeTab = [this](QPushButton* btn, const QString& navKey, const QString& mediaType) {
        btn->setProperty("navKey", navKey);
        btn->setProperty("mediaType", mediaType);
        btn->setFixedHeight(kTopBtnHeight);      // same size as the chrome buttons
        btn->setFocusPolicy(Qt::StrongFocus);    // reachable + arrow-navigable by keyboard
        btn->installEventFilter(this);           // left/right between tabs, down into the grid
        btn->setStyleSheet(tabStyle(typeColor(mediaType), false));
        typeBar_->addWidget(btn);
        typeButtons_.push_back(btn);
    };

    // "Home" tab first (left of Movies): the profile's recently-opened content, grouped under headers.
    auto* homeBtn = new QPushButton(tr("Home"), this);
    connect(homeBtn, &QPushButton::clicked, this, &HomeView::selectRecent);
    makeTab(homeBtn, QStringLiteral("home"), QStringLiteral("home"));
    navTargets_.push_back({ QStringLiteral("home"), true, nullptr, QString(), QStringLiteral("home"), tr("Home") });

    bool first = true;
    LoadedAddon* firstAddon = nullptr; QString firstCat, firstType, firstName;
    for (LoadedAddon* s : mgr_->sources())
    {
        if (!mgr_->isEnabled(s->manifest.id)) continue;
        const QVector<AddonCatalog> cats = mgr_->catalogs(s);
        for (const AddonCatalog& c : cats)
        {
            const QString name = c.name; // just the catalog name (e.g. "Movies"), no addon prefix
            auto* btn = new QPushButton(name, this);
            LoadedAddon* addon = s; const QString cid = c.id, ctype = c.type;
            connect(btn, &QPushButton::clicked, this, [this, addon, cid, ctype, name] { selectType(addon, cid, ctype, name); });
            makeTab(btn, cid, ctype);
            navTargets_.push_back({ cid, false, addon, cid, ctype, name });
            if (first) { firstAddon = addon; firstCat = cid; firstType = ctype; firstName = name; first = false; }
        }
    }
    typeBar_->addStretch(1);

    // Carousel layout (ES/RetroBat-style): the media types become a spinning carousel; the tab strip hides.
    carouselMode_ = (g_theme.layout == QStringLiteral("carousel"));
    if (typeHost_) typeHost_->setVisible(!carouselMode_);
    if (carouselMode_)
    {
        styleTypeButtons(QStringLiteral("home")); // theme the chrome behind the carousel
        showCarousel();                           // builds the media-type carousel from navTargets_
        return;
    }
    carousel_->hide();

    // Land on Home (this profile's recent content) when there's anything in it; otherwise the first catalog.
    if (!RecentStore::list().isEmpty())
        selectRecent();
    else if (firstAddon)
        selectType(firstAddon, firstCat, firstType, firstName);
    else
    {
        grid_->clear(); items_.clear(); stack_.clear();
        status_->setText(tr("No catalog addons installed. Open the Library to install one."));
        updateChrome();
    }

    // Put keyboard focus on the active tab so arrow-key navigation works immediately.
    if (activeTypeButton_) activeTypeButton_->setFocus(Qt::OtherFocusReason);
}

void HomeView::showCarousel()
{
    // The media-type carousel is the root. Rebuild its entries (we may be returning from a catalog carousel).
    QVector<CarouselEntry> entries;
    for (const NavTarget& t : navTargets_)
        entries.push_back({ t.navKey, t.name, typeColor(t.type) });
    carousel_->setEntries(entries, lastMediaKey_);
    carousel_->setWrap(true); // the media types are a finite list -> tile infinitely

    atCarouselLanding_ = true;
    grid_->hide();
    hideMeta();
    carousel_->show();
    carousel_->raise();
    carousel_->setFocus(Qt::OtherFocusReason);
    updateChrome();
}

void HomeView::activateNav(const QString& navKey)
{
    for (const NavTarget& t : navTargets_)
        if (t.navKey == navKey)
        {
            atCarouselLanding_ = false;
            lastMediaKey_ = navKey;          // remember it so Back highlights this type in the carousel
            if (t.isHome) selectRecent();    // Home -> the recents list (grid)
            else          selectType(t.addon, t.catalogId, t.type, t.name); // catalog -> item carousel
            return;
        }
}

void HomeView::applyTheme()
{
    refresh(); // re-reads the theme: font, type-icon registry, colours, background - and re-styles the view
}

void HomeView::focusContent()
{
    if (carouselMode_ && carousel_ && carousel_->isVisible())
        carousel_->setFocus(Qt::OtherFocusReason);
    else if (grid_->isVisible() && grid_->count() > 0)
        grid_->setFocus(Qt::OtherFocusReason);
    else if (activeTypeButton_)
        activeTypeButton_->setFocus(Qt::OtherFocusReason);
}

void HomeView::styleTypeButtons(const QString& activeKey)
{
    activeTypeButton_ = nullptr;
    const QColor neutral = g_theme.neutralTab; // unselected media tabs (theme-controlled)
    QColor tabActive = typeColor(QStringLiteral("home"));
    for (QPushButton* b : typeButtons_)
    {
        const bool active = (b->property("navKey").toString() == activeKey);
        if (active)
        {
            tabActive = typeColor(b->property("mediaType").toString()); // the selected tab's special colour
            b->setStyleSheet(tabStyle(tabActive, true));
            activeTypeButton_ = b;
        }
        else
        {
            b->setStyleSheet(tabStyle(neutral, false)); // everything else is neutral
        }
    }
    // The accent drives the chrome + background: it either follows the selected tab or is a fixed theme colour.
    const QColor activeColor = g_theme.accentFollowsTab ? tabActive : g_theme.accent.lighter(125);
    // The catalogue background is a light tint of the active tab's colour; keep item text readable on it.
    // When the theme has a background image, the grid goes semi-transparent so the image shows behind it.
    const QColor tint = lightTint(activeColor);
    const bool hasBg = !g_theme.background.isEmpty();
    const QString gridBg = hasBg
        ? QString("rgba(%1,%2,%3,170)").arg(tint.red()).arg(tint.green()).arg(tint.blue())
        : tint.name();
    grid_->setStyleSheet(QString(
        "QListWidget{background:%1;color:#1b1b1b;border:none;}"
        "QListWidget::item:selected{background:%2;color:white;}")
        .arg(gridBg, activeColor.name()));

    // The chrome buttons (Back, profile, Settings) take the accent colour; the search box stays light.
    themeColor_ = activeColor;
    const QString cb = chromeButtonStyle(activeColor);
    if (back_)        back_->setStyleSheet(cb);
    if (profileBtn_)  profileBtn_->setStyleSheet(cb);
    if (settingsBtn_) settingsBtn_->setStyleSheet(cb);
    if (search_)      search_->setStyleSheet(chromeEditStyle(activeColor, g_theme.cornerRadius));
    if (status_)      status_->setStyleSheet(QStringLiteral("color:#2a2c30;"));
    // Back the whole top row + the tabs' empty stretch with the accent so no seam shows the light tint.
    if (topBar_)      topBar_->setStyleSheet(QString("#topBar{background:%1;}").arg(activeColor.name()));
    if (typeHost_)    typeHost_->setStyleSheet(QString("#typeHost{background:%1;}").arg(activeColor.name()));

    // The whole home surface goes light (no dark anywhere): set the palette so child text is dark too.
    QPalette pal = palette();
    pal.setColor(QPalette::Window, tint);
    pal.setColor(QPalette::Base, QColor(0xfb, 0xfb, 0xfd));
    pal.setColor(QPalette::WindowText, QColor(0x22, 0x24, 0x28));
    pal.setColor(QPalette::Text, QColor(0x1b, 0x1b, 0x1b));
    pal.setColor(QPalette::Highlight, activeColor);
    pal.setColor(QPalette::HighlightedText, Qt::white);
    setPalette(pal);
    setAutoFillBackground(!hasBg); // when a bg image is set, paintEvent draws it instead
    update();

    emit themeChanged(tint, activeColor); // let the main window theme its window + status bar to match
}

void HomeView::paintEvent(QPaintEvent* event)
{
    if (!g_theme.background.isEmpty())
    {
        static QPixmap cached;
        static QString cachedPath;
        if (cachedPath != g_theme.background) { cached = QPixmap(g_theme.background); cachedPath = g_theme.background; }
        if (!cached.isNull())
        {
            QPainter p(this);
            const QPixmap sc = cached.scaled(size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
            p.drawPixmap((width() - sc.width()) / 2, (height() - sc.height()) / 2, sc);
            // A light overlay keeps the (dark) text readable over the image.
            p.fillRect(rect(), QColor(255, 255, 255, int(255 * qBound(0.0, g_theme.backgroundDim, 1.0))));
            return;
        }
    }
    QWidget::paintEvent(event);
}

void HomeView::applyThemeFont()
{
    static int s_basePt = -1;
    static QString s_baseFamily;
    if (s_basePt < 0)
    {
        const QFont base = QApplication::font();
        s_basePt = base.pointSize() > 0 ? base.pointSize() : 9;
        s_baseFamily = base.family();
    }
    QFont f = QApplication::font();
    f.setFamily(g_theme.fontFamily.isEmpty() ? s_baseFamily : g_theme.fontFamily);
    f.setPointSize(qMax(6, int(s_basePt * g_theme.fontScale)));
    qApp->setFont(f);
}

// Arrange the detail-page text sections in the theme's declared order (missing ones appended, so nothing
// silently disappears). Driven by ThemeDetail.order; the per-type detailLayout still controls image placement.
void HomeView::layoutMetaSections(const QString& itemType)
{
    Q_UNUSED(itemType);
    QStringList order = g_theme.detail.order;
    if (order.isEmpty()) order = { "favorite", "title", "facts", "overview" };

    metaTextCol_->removeWidget(favBtn_);
    metaTextCol_->removeWidget(metaTitle_);
    metaTextCol_->removeWidget(metaFacts_);
    metaTextCol_->removeWidget(metaOverview_);

    QSet<QString> added;
    auto place = [&](const QString& key) {
        if (added.contains(key)) return;
        added.insert(key);
        if (key == "favorite")      metaTextCol_->addWidget(favBtn_, 0, Qt::AlignLeft);
        else if (key == "title")    metaTextCol_->addWidget(metaTitle_);
        else if (key == "facts")    metaTextCol_->addWidget(metaFacts_);
        else if (key == "overview") metaTextCol_->addWidget(metaOverview_, 1);
    };
    for (const QString& k : order) place(k);
    for (const QString& k : { QStringLiteral("favorite"), QStringLiteral("title"),
                              QStringLiteral("facts"), QStringLiteral("overview") }) place(k);
}

void HomeView::focusTypeButton(int idx)
{
    if (typeButtons_.isEmpty()) return;
    idx = qBound(0, idx, typeButtons_.size() - 1);
    QPushButton* b = typeButtons_[idx];
    b->setFocus(Qt::OtherFocusReason);
    b->click(); // activate -> load that catalog (also recolours the tab + background)
}

void HomeView::focusGridTop()
{
    grid_->setFocus(Qt::OtherFocusReason);
    if (grid_->count() == 0) return;
    int r = 0;
    while (r < items_.size() && items_[r].type == QStringLiteral("rechdr")) ++r; // skip recent headers
    if (r >= grid_->count()) r = 0;
    grid_->setCurrentRow(r);
}

void HomeView::selectType(LoadedAddon* addon, const QString& catalogId, const QString& type, const QString& name)
{
    recentView_ = false;
    applyGridMode(/*recentList*/ false);
    styleTypeButtons(catalogId);
    search_->clear();
    stack_.clear();
    Level lvl;
    lvl.addon = addon; lvl.detail = false; lvl.catalogId = catalogId; lvl.catalogType = type; lvl.title = name;
    stack_.push_back(lvl);
    loadTop();
}

void HomeView::selectRecent()
{
    recentView_ = true;
    styleTypeButtons(QStringLiteral("home"));
    search_->clear();
    stack_.clear();
    hideMeta();
    pendingReqId_ = -1; // ignore any in-flight addon result
    loading_ = false;
    hasMore_ = false;
    renderRecents();
}

void HomeView::renderRecents()
{
    ++generation_;             // invalidate stale thumbnail loads
    atCarouselLanding_ = false;
    if (carousel_) carousel_->hide(); // Home is always the recents list, even in carousel layout
    applyGridMode(/*recentList*/ true);
    grid_->clear();
    items_.clear();
    thumbQueue_.clear();
    grid_->show();

    const QSize iconSz(44, 44);

    // A non-selectable header row that spans the list width.
    auto addHeader = [this](const QString& label) {
        MediaItem hdr;
        hdr.type = QStringLiteral("rechdr");
        items_.push_back(hdr);
        auto* h = new QListWidgetItem(label, grid_);
        QFont hf = h->font();
        hf.setBold(true);
        h->setFont(hf);
        h->setFlags(Qt::ItemIsEnabled);                 // visible but not selectable/clickable
        h->setBackground(lightTint(themeColor_, 0.34)); // a medium tint divider bar (not dark)
        h->setForeground(QColor(0x22, 0x24, 0x28));      // dark label text
        h->setSizeHint(QSize(0, 30));
    };

    // Favourites first (a per-profile, starred-media section).
    const QVector<FavoriteItem> favs = FavoritesStore::list();
    if (!favs.isEmpty())
    {
        addHeader(tr("★ Favorites"));
        for (const FavoriteItem& f : favs)
        {
            MediaItem it;
            it.id = f.itemId;
            it.type = f.type;
            it.title = f.title;
            it.subtitle = f.subtitle;
            it.thumbnailUrl = f.thumbnailUrl;
            it.expandable = f.expandable;
            it.mime = QStringLiteral("fav:") + f.addonId; // marks a favourite + carries its source addon
            items_.push_back(it);

            auto* w = new QListWidgetItem(QStringLiteral("  ") + it.title, grid_);
            w->setSizeHint(QSize(0, 52));
            w->setIcon(defaultIcon(it.type, iconSz));
        }
    }

    const QVector<RecentItem> recents = RecentStore::list();

    // Bucket recents into groups (media type, per-console for games), keeping newest-first group order.
    QStringList order;
    QHash<QString, QVector<RecentItem>> groups;
    for (const RecentItem& r : recents)
    {
        const QString key = recentGroupKey(r);
        if (!groups.contains(key)) order << key;
        groups[key].push_back(r);
    }

    for (const QString& key : order)
    {
        addHeader(recentGroupLabel(key));
        for (const RecentItem& r : groups[key])
        {
            MediaItem it;
            it.url = r.path;                         // re-open target
            it.mime = r.kind;                        // routing kind (video/audio/document/game)
            it.type = iconTypeForKind(r.kind);       // drives the placeholder icon
            it.title = r.title.isEmpty() ? QFileInfo(r.path).completeBaseName() : r.title;
            items_.push_back(it);

            auto* w = new QListWidgetItem(QStringLiteral("  ") + it.title, grid_);
            w->setSizeHint(QSize(0, 52));
            w->setIcon(defaultIcon(it.type, iconSz));
        }
    }

    loadThumbnails(0); // favourites have posters; recents don't (harmlessly skipped)
    updateChrome();
    updateStatus();

    // Keep keyboard focus on the content. Without this, activating Home from the carousel hides the
    // (focused) carousel and Qt hands focus to the next widget in the chain - the search box.
    if (grid_->isVisible()) grid_->setFocus(Qt::OtherFocusReason);
}

void HomeView::applyGridMode(bool recentList)
{
    // Pixel-based scrolling in both modes; the wheel step is controlled in eventFilter().
    grid_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    grid_->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);

    if (recentList)
    {
        // Recent: a vertical list of full-width rows, so group headers span the width cleanly.
        grid_->setViewMode(QListView::ListMode);
        grid_->setFlow(QListView::TopToBottom);
        grid_->setWrapping(false);
        grid_->setGridSize(QSize());
        grid_->setIconSize(QSize(44, 44));
        grid_->setSpacing(1);
        grid_->setWordWrap(false);
        grid_->setUniformItemSizes(false); // header rows and item rows differ in height
    }
    else
    {
        // Catalogs: the poster grid. uniformItemSizes(true) is essential here - with ResizeMode::Adjust,
        // IconMode otherwise re-lays out every tile on each scroll, which gets very slow as pages pile up.
        grid_->setViewMode(QListView::IconMode);
        grid_->setFlow(QListView::LeftToRight);
        grid_->setWrapping(true);
        grid_->setGridSize(QSize(kPoster.width() + 24, kPoster.height() + 56));
        grid_->setIconSize(kPoster);
        grid_->setSpacing(8);
        grid_->setWordWrap(true);
        grid_->setMovement(QListView::Static);
        grid_->setUniformItemSizes(true);
    }
}

QString HomeView::openKindForView() const
{
    if (stack_.isEmpty()) return QString();
    const Level& top = stack_.last();
    if (top.detail)
        return (top.item.type == QStringLiteral("platform")) ? QStringLiteral("game") : QString(); // games per-console
    const QString& t = top.catalogType;
    auto reg = g_typeVisuals.constFind(t); // addon-declared file-open kind for a custom type
    if (reg != g_typeVisuals.constEnd() && !reg->openKind.isEmpty()) return reg->openKind;
    if (t == QStringLiteral("movie") || t == QStringLiteral("series")) return QStringLiteral("video");
    if (t == QStringLiteral("album") || t == QStringLiteral("audiobook")) return QStringLiteral("audio");
    if (t == QStringLiteral("book") || t == QStringLiteral("comic") || t == QStringLiteral("manga"))
        return QStringLiteral("document");
    return QString(); // "game" (console list) shows nothing; the open item appears inside each console
}

bool HomeView::eventFilter(QObject* obj, QEvent* event)
{
    // Keyboard navigation: left/right move between the top tabs, down drops into the grid, and up from the
    // grid's top row returns to the active tab.
    if (event->type() == QEvent::KeyPress)
    {
        auto* ke = static_cast<QKeyEvent*>(event);

        // Backspace acts as the Back button when focus is on a tab or the grid (the search box isn't
        // filtered, so backspacing while typing a query still deletes characters).
        if (ke->key() == Qt::Key_Backspace) { goBack(); return true; }

        const int idx = typeButtons_.indexOf(qobject_cast<QPushButton*>(obj));
        if (idx >= 0)
        {
            if (ke->key() == Qt::Key_Right) { focusTypeButton(idx + 1); return true; }
            if (ke->key() == Qt::Key_Left)  { focusTypeButton(idx - 1); return true; }
            if (ke->key() == Qt::Key_Down)  { focusGridTop();           return true; }
        }
        else if (obj == grid_)
        {
            // Enter opens the focused item (same as a click).
            if (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) { onItemActivated(); return true; }

            // Up from anywhere in the top row returns to the media-type tabs. "Top row" = items sharing the
            // first selectable item's vertical position (works for the multi-column grid and the recent list).
            if (ke->key() == Qt::Key_Up && activeTypeButton_)
            {
                int firstSel = 0;
                while (firstSel < items_.size() && items_[firstSel].type == QStringLiteral("rechdr")) ++firstSel;
                QListWidgetItem* firstItem = (firstSel < grid_->count()) ? grid_->item(firstSel) : nullptr;
                QListWidgetItem* cur = grid_->currentItem();
                if (firstItem && cur &&
                    grid_->visualItemRect(cur).top() <= grid_->visualItemRect(firstItem).top())
                {
                    if (carouselMode_) showCarousel();                  // back up to the carousel
                    else activeTypeButton_->setFocus(Qt::OtherFocusReason); // back up to the tabs
                    return true;
                }
            }
        }
    }

    // Drive the grid's wheel scrolling at a fixed, comfortable pixels-per-notch (ScrollPerPixel's default
    // step is too large; ScrollPerItem moves only a fraction of a row per notch in a multi-column grid).
    if (obj == grid_->viewport() && event->type() == QEvent::Wheel)
    {
        auto* we = static_cast<QWheelEvent*>(event);
        const int dy = we->angleDelta().y();
        if (dy != 0)
        {
            const int kPixelsPerNotch = 120; // a notch is 120 angle units
            QScrollBar* sb = grid_->verticalScrollBar();
            sb->setValue(sb->value() - dy * kPixelsPerNotch / 120);
            return true; // handled
        }
    }
    return QWidget::eventFilter(obj, event);
}

void HomeView::onItemActivated()
{
    activateItem(grid_->currentRow());
}

void HomeView::activateItem(int row)
{
    if (row < 0 || row >= items_.size()) return;
    const MediaItem& it = items_[row];
    if (it.type == QStringLiteral("info")) return; // guidance rows aren't actionable

    if (recentView_)
    {
        if (it.type == QStringLiteral("rechdr")) return;                 // a group header, not actionable
        if (it.mime.startsWith(QStringLiteral("fav:"))) { openFavorite(it); return; } // a favourite -> detail
        if (!it.url.isEmpty()) emit openRecent(it.url, it.mime);         // a recent -> re-open the file
        return;
    }

    if (it.type == QStringLiteral("_open"))
    {
        emit requestOpenFile(it.url); // url carries the kind: video/audio/document/game
        return;
    }
    if (!it.url.isEmpty())
    {
        emit openItem(it); // a file is associated with this item -> the main window plays it
        return;
    }

    // No file yet: open a detail page. Its metadata header describes the item; for a container
    // (TV show / season / album / console) the page also drills into its children below the header.
    stack_.last().childRow = row; // remember where we drilled in, so Back restores this position
    const Level& top = stack_.last();
    Level lvl;
    lvl.addon = top.addon; lvl.detail = true; lvl.item = it; lvl.title = it.title;
    stack_.push_back(lvl);
    loadTop();
}

void HomeView::openFavorite(const MediaItem& favItem)
{
    // Resolve the favourite's source addon and open its detail page (rooted at Home so Back returns here).
    const QString addonId = favItem.mime.mid(4); // strip "fav:"
    LoadedAddon* addon = nullptr;
    for (LoadedAddon* s : mgr_->sources())
        if (s->manifest.id == addonId) { addon = s; break; }
    if (!addon)
    {
        status_->setText(tr("That favourite's source addon isn't available."));
        return;
    }

    recentView_ = false;
    applyGridMode(/*recentList*/ false);
    styleTypeButtons(QStringLiteral("home")); // keep Home highlighted/themed - favourites live there
    stack_.clear();
    MediaItem mi = favItem;
    mi.mime.clear();                          // drop the "fav:" marker before drilling in
    Level lvl;
    lvl.addon = addon; lvl.detail = true; lvl.item = mi; lvl.title = mi.title;
    stack_.push_back(lvl);
    loadTop();
}

void HomeView::goBack()
{
    if (stack_.size() > 1) { stack_.pop_back(); loadTop(); return; }
    // A favourite opened from Home is a lone detail level -> Back returns to Home.
    if (stack_.size() == 1 && stack_.last().detail) { selectRecent(); return; }
    // In carousel layout, Back from a catalog (or Home) returns to the media-type carousel.
    if (carouselMode_ && !atCarouselLanding_) { showCarousel(); return; }
}

void HomeView::doSearch()
{
    if (stack_.isEmpty()) return;
    // Search re-runs the base media-type catalog with the query (drops any drill-down).
    Level base = stack_.first();
    base.detail = false;
    base.childRow = -1; // a fresh result set -> land on the first item, not the old drill position
    base.query = search_->text().trimmed();
    stack_.clear();
    stack_.push_back(base);
    loadTop();
}

void HomeView::loadTop()
{
    if (stack_.isEmpty()) return;
    const Level& top = stack_.last();

    const bool container = top.detail && top.item.expandable;       // has children to drill into
    const bool wantMeta  = top.detail && top.item.type != QStringLiteral("platform"); // console is not "media"

    if (wantMeta) requestMeta(top.item);
    else          hideMeta();

    if (top.detail && !container)
    {
        // Leaf (episode / song / movie / game / book): a metadata-only page, no child grid/carousel.
        ++generation_;
        grid_->clear();
        items_.clear();
        grid_->hide();
        if (carousel_) carousel_->hide();
        loading_ = false; hasMore_ = false; currentPage_ = 1; pendingReqId_ = -1;
        updateChrome();
        updateStatus();
        // The grid/carousel that held focus is now hidden; park focus on the Favorite button so the
        // detail page still has a keyboard target (and Backspace routes to Back via its event filter).
        if (favBtn_ && meta_->isVisible()) favBtn_->setFocus(Qt::OtherFocusReason);
        return;
    }

    if (carouselMode_) grid_->hide(); // the carousel shows catalog items; populate() fills it
    else               grid_->show();
    issueRequest(/*append*/ false);
}

void HomeView::requestMeta(const MediaItem& item)
{
    // Show the header straight away with a placeholder cover + the item's own title;
    // onMetaReady() fills in the facts, synopsis and real cover when they arrive.
    metaTitle_->setText(item.title.toHtmlEscaped());
    metaFacts_->clear();    metaFacts_->setVisible(false);
    metaOverview_->clear(); metaOverview_->setVisible(false);
    // Cover size from the theme.
    const int iw = qBound(60, g_theme.detail.imageWidth, 360);
    metaImage_->setFixedSize(iw, int(iw * 240.0 / 170.0));
    metaImage_->setPixmap(defaultIcon(item.type, metaImage_->size()).pixmap(metaImage_->size()));
    if (favBtn_) favBtn_->setText(FavoritesStore::isFavorite(item.id) ? tr("★ Favorited") : tr("☆ Favorite"));

    // Cover placement: a per-type detailLayout (poster/banner/text) wins; otherwise the theme's detail.image.
    QString imgMode = g_theme.detail.image.isEmpty() ? QStringLiteral("left") : g_theme.detail.image;
    auto reg = g_typeVisuals.constFind(item.type);
    if (reg != g_typeVisuals.constEnd() && !reg->detailLayout.isEmpty())
        imgMode = (reg->detailLayout == QStringLiteral("banner")) ? QStringLiteral("top")
                : (reg->detailLayout == QStringLiteral("text"))   ? QStringLiteral("hidden")
                                                                  : QStringLiteral("left");
    if (imgMode == QStringLiteral("hidden"))
    {
        metaImage_->hide();
        metaLayout_->setDirection(QBoxLayout::LeftToRight);
    }
    else if (imgMode == QStringLiteral("top"))
    {
        metaImage_->show();
        metaLayout_->setDirection(QBoxLayout::TopToBottom);
        metaLayout_->setAlignment(metaImage_, Qt::AlignHCenter);
    }
    else
    {
        metaImage_->show();
        metaLayout_->setDirection(QBoxLayout::LeftToRight);
        metaLayout_->setAlignment(metaImage_, Qt::AlignTop);
    }

    layoutMetaSections(item.type); // order the text rows per the theme
    meta_->setVisible(true);
    pendingMetaReqId_ = mgr_->requestMeta(stack_.last().addon, item);
}

void HomeView::onMetaReady(int requestId, const MediaDetail& detail)
{
    if (requestId != pendingMetaReqId_) return; // stale (navigated away / newer item)
    if (!detail.valid) return;                  // nothing usable; keep the placeholder header
    showMeta(detail);
}

void HomeView::showMeta(const MediaDetail& d)
{
    QString titleHtml = QStringLiteral("<b>%1</b>").arg(d.title.toHtmlEscaped());
    if (!d.subtitle.isEmpty())
        titleHtml += QStringLiteral("<br><span style='font-size:11pt;color:#9aa3ad;'>%1</span>")
                         .arg(d.subtitle.toHtmlEscaped());
    metaTitle_->setText(titleHtml);

    QStringList rows;
    for (const MediaFact& f : d.facts)
        rows << QStringLiteral("<b>%1:</b> %2").arg(f.label.toHtmlEscaped(), f.value.toHtmlEscaped());
    metaFacts_->setText(rows.join(QStringLiteral("<br>")));
    metaFacts_->setVisible(!rows.isEmpty());

    metaOverview_->setPlainText(d.overview);
    metaOverview_->setVisible(!d.overview.isEmpty());

    meta_->setVisible(true);

    if (d.imageUrl.isEmpty()) return; // keep the type placeholder set in requestMeta()
    const int myMeta = pendingMetaReqId_;
    if (!d.imageUrl.startsWith(QStringLiteral("http")))
    {
        const QPixmap pm(d.imageUrl); // bundled/local cover
        if (!pm.isNull())
            metaImage_->setPixmap(pm.scaled(metaImage_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
        return;
    }
    QNetworkRequest req((QUrl(d.imageUrl)));
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MyMediaVault"));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = nam_->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, myMeta] {
        reply->deleteLater();
        if (myMeta != pendingMetaReqId_) return;            // navigated to another item meanwhile
        if (reply->error() != QNetworkReply::NoError) return;
        QPixmap pm;
        if (pm.loadFromData(reply->readAll()))
            metaImage_->setPixmap(pm.scaled(metaImage_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    });
}

void HomeView::hideMeta()
{
    pendingMetaReqId_ = -1; // invalidate any in-flight metadata / cover load
    meta_->setVisible(false);
}

void HomeView::loadMore()
{
    if (loading_ || !hasMore_ || stack_.isEmpty()) return;
    issueRequest(/*append*/ true);
}

void HomeView::issueRequest(bool append)
{
    if (stack_.isEmpty()) return;
    const Level& top = stack_.last();
    const int page = append ? currentPage_ + 1 : 1;
    pendingAppend_ = append;
    pendingPage_ = page;
    loading_ = true;
    status_->setText(append ? tr("Loading more…") : tr("Loading…"));

    pendingReqId_ = top.detail ? mgr_->requestDetail(top.addon, top.item, page)
                               : mgr_->requestCatalog(top.addon, top.catalogId, top.query, page);
}

void HomeView::onCatalogReady(int requestId, const MediaCatalog& cat)
{
    if (requestId != pendingReqId_) return; // a superseded request (navigated away / newer page)
    loading_ = false;
    currentPage_ = pendingPage_;
    hasMore_ = cat.hasMore;
    populate(cat, pendingAppend_);
}

void HomeView::populate(const MediaCatalog& cat, bool append)
{
    int from;
    if (!append)
    {
        ++generation_; // invalidate in-flight thumbnail loads from the previous view
        applyGridMode(/*recentList*/ false); // ensure the poster grid (recents may have left it in list mode)
        grid_->clear();
        items_.clear();
        // Lead with an "open a file of this type" item (with a + icon) instead of toolbar buttons.
        const QString kind = openKindForView();
        if (!kind.isEmpty())
        {
            MediaItem open;
            open.id = QStringLiteral("_open");
            open.type = QStringLiteral("_open");
            open.title = openTitleFor(kind);
            open.url = kind; // carries the kind for onItemActivated
            items_.push_back(open);
        }
        from = 0;
    }
    else
    {
        from = items_.size();
    }
    items_ += cat.items;

    for (int i = from; i < items_.size(); ++i)
    {
        const MediaItem& it = items_[i];
        QString label = it.title;
        if (!it.subtitle.isEmpty()) label += QStringLiteral("\n") + it.subtitle;
        auto* w = new QListWidgetItem(label, grid_);
        w->setSizeHint(QSize(kPoster.width() + 16, kPoster.height() + 48));
        w->setTextAlignment(Qt::AlignHCenter | Qt::AlignTop);
        if (it.type == QStringLiteral("_open"))
            w->setIcon(plusIcon(kPoster));
        else
        {
            // Type-based placeholder; a real poster (if any) overwrites it in loadThumbnails().
            if (it.type != QStringLiteral("info")) w->setIcon(defaultIcon(it.type, kPoster));
            if (it.expandable) w->setToolTip(tr("Open for episodes/tracks"));
        }
    }

    updateChrome();
    updateStatus();
    loadThumbnails(from);

    // In carousel layout, catalog items are shown as a (wrapping) carousel instead of the grid.
    if (carouselMode_)
    {
        atCarouselLanding_ = false;
        fillCarouselFromItems(from);
    }
    else if (!append)
    {
        // Grid layout: returning via Back restores the row we'd drilled into (otherwise no selection).
        const int restoreRow = stack_.isEmpty() ? -1 : stack_.last().childRow;
        if (restoreRow >= 0 && restoreRow < grid_->count())
        {
            grid_->setCurrentRow(restoreRow);
            grid_->scrollToItem(grid_->item(restoreRow), QAbstractItemView::PositionAtCenter);
        }
    }
}

// Build (from==0) or extend (append) the carousel from items_[from..], skipping guidance rows. Box art comes
// from each item's thumbnailUrl. If a fresh build has no usable items (a leaf detail), the carousel hides.
void HomeView::fillCarouselFromItems(int from)
{
    QVector<CarouselEntry> entries;
    for (int i = qMax(0, from); i < items_.size(); ++i)
    {
        const MediaItem& it = items_[i];
        if (it.type == QStringLiteral("info") || it.type == QStringLiteral("rechdr")) continue;
        const QColor c = (it.type == QStringLiteral("_open")) ? QColor(0x6A, 0x6E, 0x78) : typeColor(it.type);
        entries.push_back({ QStringLiteral("item:") + QString::number(i), it.title, c, it.thumbnailUrl });
    }

    if (from > 0) { carousel_->addEntries(entries); return; } // paged append -> extend in place

    if (entries.isEmpty()) { carousel_->hide(); return; }
    grid_->hide();
    // Returning to this level via Back? Restore the item we'd drilled into; otherwise land on the first.
    const int restoreRow = stack_.isEmpty() ? -1 : stack_.last().childRow;
    const QString restoreKey = (restoreRow >= 0) ? (QStringLiteral("item:") + QString::number(restoreRow))
                                                 : QString();
    carousel_->setEntries(entries, restoreKey);
    // Multi-page catalogs are a partial window, so don't wrap (no scrolling left past the start). Finite
    // lists (consoles, seasons, ...) still tile infinitely.
    carousel_->setWrap(!hasMore_);
    carousel_->show();
    carousel_->raise();
    carousel_->setFocus(Qt::OtherFocusReason);
}

void HomeView::updateStatus()
{
    if (recentView_)
    {
        // Count real entries (skip header rows).
        int n = 0;
        for (const MediaItem& it : items_) if (it.type != QStringLiteral("rechdr")) ++n;
        status_->setText(n == 0
            ? tr("Home   —   Nothing opened yet. Open a video, audio file, book or game and it shows up here.")
            : tr("Home   —   %1 recently opened").arg(n));
        return;
    }

    QStringList crumbs;
    for (const Level& l : stack_) crumbs << l.title;
    // Don't count the leading "open a file" item as a catalog result.
    const int count = items_.size() - ((!items_.isEmpty() && items_.first().type == QStringLiteral("_open")) ? 1 : 0);
    const bool leafDetail = !stack_.isEmpty() && stack_.last().detail && !stack_.last().item.expandable
                            && stack_.last().item.type != QStringLiteral("platform");
    QString tail;
    if (leafDetail)      tail = tr("Details");
    else if (loading_)   tail = tr("Loading more…");
    else if (hasMore_)   tail = tr("%1 items · scroll down for more").arg(count);
    else if (count == 0) tail = tr("No results");
    else                 tail = tr("%1 items · End of results").arg(count);
    status_->setText(crumbs.join(QStringLiteral("  ›  ")) + QStringLiteral("   —   ") + tail);
}

void HomeView::loadThumbnails(int fromIndex)
{
    if (fromIndex <= 0) thumbQueue_.clear(); // fresh view: drop any stale queued loads from the last one
    for (int i = qMax(0, fromIndex); i < items_.size(); ++i)
    {
        const QString url = items_[i].thumbnailUrl;
        if (url.isEmpty()) continue;
        QListWidgetItem* w = grid_->item(i);
        if (!w) continue;

        // Local file (a bundled tile, e.g. console art) - load directly via Qt's image plugins (incl. SVG).
        if (!url.startsWith(QStringLiteral("http")))
        {
            const QPixmap pm(url);
            if (!pm.isNull())
                w->setIcon(QIcon(pm.scaled(kPoster, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
            continue;
        }
        thumbQueue_.push_back(i); // remote: fetched by pumpThumbnails(), capped so we don't flood the host
    }
    pumpThumbnails();
}

// Start queued remote poster loads up to a small concurrency cap. Loading every poster at once opens a
// burst of parallel requests to one host; some servers (e.g. MangaDex over HTTP/2) refuse the excess
// streams. A handful in flight at a time loads everything reliably without tripping those limits.
void HomeView::pumpThumbnails()
{
    const int kMaxConcurrent = 6;
    while (thumbActive_ < kMaxConcurrent && !thumbQueue_.isEmpty())
    {
        const int i = thumbQueue_.takeFirst();
        if (i < 0 || i >= items_.size() || i >= grid_->count()) continue;
        const QString url = items_[i].thumbnailUrl;
        if (url.isEmpty() || !url.startsWith(QStringLiteral("http"))) continue;
        QListWidgetItem* w = grid_->item(i);
        const int gen = generation_;

        QNetworkRequest req((QUrl(url)));
        req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MyMediaVault"));
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        QNetworkReply* reply = nam_->get(req);
        ++thumbActive_;
        connect(reply, &QNetworkReply::finished, this, [this, reply, w, gen] {
            reply->deleteLater();
            --thumbActive_;
            if (gen == generation_ && reply->error() == QNetworkReply::NoError) // else navigated away / failed
            {
                QPixmap pm;
                if (pm.loadFromData(reply->readAll()))
                    w->setIcon(QIcon(pm.scaled(kPoster, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
            }
            pumpThumbnails(); // a slot freed up - start the next queued poster
        });
    }
}

void HomeView::updateChrome()
{
    // Enabled while drilled in - including a lone favourite detail (Back returns to Home), and in carousel
    // layout whenever we've left the media-type landing (Back returns to the carousel).
    back_->setEnabled(stack_.size() > 1 || (stack_.size() == 1 && stack_.last().detail)
                      || (carouselMode_ && !atCarouselLanding_));
}
