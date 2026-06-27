#include "HomeView.h"
#include "../addons/AddonManager.h"
#include "../core/RecentStore.h"
#include "../core/ProfileStore.h"
#include "../core/FavoritesStore.h"
#include "../core/Theme.h"
#include "../core/SystemCatalog.h"
#include "../core/SteamLibrary.h"
#include "CarouselView.h"
#include "XmbView.h"
#include <QHash>

#include <QApplication>
#include <QPaintEvent>
#include <QSet>
#include <QListWidget>
#include <QAbstractItemView>
#include <QMenu>
#include <QLineEdit>
#include <QLabel>
#include <QTimer>
#include <QResizeEvent>
#include <QRegularExpression>
#include <QPushButton>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFrame>
#include <QTextBrowser>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUrlQuery>
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
#include <QSettings>
#include <QCryptographicHash>
#include <QCoreApplication>

static const QSize kPoster(140, 200);

// A specific chapter/issue leaf that we can resolve to readable page images. Its detail page gets a
// "Read" button. (Manga chapters resolve via MangaDex; comic issues are metadata-only for now.)
static bool isReadableChapter(const QString& t)
{
    return t == QStringLiteral("manga_chapter");
}

// Per-profile settings store (shared ini); used here to read media resume progress.
static QSettings& settingsStore()
{
    static QSettings s(QCoreApplication::applicationDirPath() + QStringLiteral("/mymediavault.ini"),
                       QSettings::IniFormat);
    return s;
}

// Resume progress (0..1) for a played media path/url, or -1 if none. Mirrors MainWindow's key scheme
// (resume/<md5-of-path>/{pos,dur}); a bar shows only once both a position and a duration are known.
static double resumeFraction(const QString& url)
{
    if (url.isEmpty()) return -1.0;
    const QByteArray h = QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Md5).toHex().left(10);
    const QString k = QStringLiteral("resume/") + QString::fromLatin1(h) + QStringLiteral("/");
    const double pos = settingsStore().value(k + QStringLiteral("pos"), 0.0).toDouble();
    const double dur = settingsStore().value(k + QStringLiteral("dur"), 0.0).toDouble();
    if (pos <= 1.0 || dur <= 1.0) return -1.0;
    return qBound(0.0, pos / dur, 1.0);
}

// The key a played item's resume position is stored under: its stable addon id when it has one (a streamed
// URL changes every resolution), else its url/path. Matches MainWindow::openLibraryItem's resume keying, so a
// movie shows "continue watching" progress whether it's a catalog poster or a Recent row.
static QString resumeKeyFor(const MediaItem& it)
{
    return it.id.isEmpty() ? it.url : it.id;
}

// Forget the saved resume position (pos/dur/title) for a media key, so it starts from the beginning next
// time. Mirrors MainWindow's resume keying (resume/<md5-of-key>/…).
static void clearResume(const QString& key)
{
    if (key.isEmpty()) return;
    const QByteArray h = QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Md5).toHex().left(10);
    settingsStore().remove(QStringLiteral("resume/") + QString::fromLatin1(h)); // removes the whole group
    settingsStore().sync();
}

// Build a metadata-lookup item from a source item that embeds an IMDB id (e.g. Allarr "mv:tt123" or an
// episode "ep:tt123:1:2"). Returns an item a provider addon (AIO Catalog) can map IMDB->TMDB for; the id is
// empty when no IMDB id is present.
static MediaItem imdbMetaItem(const MediaItem& src)
{
    static const QRegularExpression re(QStringLiteral("(tt\\d+)(?::(\\d+):(\\d+))?"));
    const QRegularExpressionMatch m = re.match(src.id);
    MediaItem mi;
    if (!m.hasMatch()) return mi;
    const QString imdb = m.captured(1);
    if (!m.captured(2).isEmpty()) // tt…:S:E -> an episode
    { mi.type = QStringLiteral("series"); mi.id = QStringLiteral("imdb:episode:") + imdb + QStringLiteral(":") + m.captured(2) + QStringLiteral(":") + m.captured(3); }
    else if (src.type == QStringLiteral("movie"))
    { mi.type = QStringLiteral("movie");  mi.id = QStringLiteral("imdb:movie:") + imdb; }
    else                          // a show (series / tv)
    { mi.type = QStringLiteral("series"); mi.id = QStringLiteral("imdb:series:") + imdb; }
    return mi;
}

// Overlay a "continue watching" progress bar along the bottom of a poster pixmap (in place).
static QIcon iconWithProgress(QPixmap pm, const QString& url)
{
    const double frac = resumeFraction(url);
    if (frac >= 0.0 && !pm.isNull())
    {
        QPainter p(&pm);
        const int barH = qMax(4, pm.height() / 36);
        const int y = pm.height() - barH;
        p.fillRect(QRect(0, y, pm.width(), barH), QColor(0, 0, 0, 140));                  // track
        p.fillRect(QRect(0, y, int(pm.width() * frac), barH), QColor(0xE5, 0x3E, 0x3E));  // watched portion
        p.end();
    }
    return QIcon(pm);
}
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
    // :focus draws a white inset border (with reduced padding so the box size doesn't shift) so keyboard /
    // controller users can see which chrome control is selected.
    return QString(
        "QPushButton{background:%1;color:white;border:none;border-radius:0;padding:8px 16px;font-weight:bold;}"
        "QPushButton:hover{background:%2;}"
        "QPushButton:focus{background:%2;border:2px solid white;padding:6px 14px;}"
        "QPushButton:disabled{background:%3;color:#f4f4f4;}")
        .arg(c.name(), c.lighter(112).name(), c.lighter(135).name());
}

// The Back button always blends into the top bar: its fill stays the background colour in every state
// (no hover/focus lightening). Focus still draws a white inset border for keyboard/controller users.
static QString backButtonStyle(const QColor& c)
{
    return QString(
        "QPushButton{background:%1;color:white;border:none;border-radius:0;padding:8px 16px;font-weight:bold;}"
        "QPushButton:hover{background:%1;}"
        "QPushButton:focus{background:%1;border:2px solid white;padding:6px 14px;}"
        "QPushButton:disabled{background:%1;color:rgba(255,255,255,0.55);}")
        .arg(c.name());
}

static QString chromeEditStyle(const QColor& c, int radius)
{
    // Light *tint* of the accent (not pure white) and no border, so no white edge shows next to the buttons.
    // :focus shows a white border (reduced padding keeps the size stable) so it's clearly selected.
    return QString("QLineEdit{background:%1;color:#1b1b1b;border:none;border-radius:%2px;padding:6px 10px;}"
                   "QLineEdit:focus{border:2px solid white;padding:4px 8px;}")
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
    // Live search: re-run the query a short beat after the user stops typing (debounced so we don't fire a
    // request per keystroke). Enter still searches immediately via returnPressed.
    searchTimer_ = new QTimer(this);
    searchTimer_->setSingleShot(true);
    connect(searchTimer_, &QTimer::timeout, this, &HomeView::doSearch);
    // Only when the user is actually typing (has focus) - not when code clears the box on a tab switch.
    connect(search_, &QLineEdit::textChanged, this, [this] { if (searchTimer_ && search_->hasFocus()) searchTimer_->start(300); });
    topRow->addSpacing(6);    // small margin around the search box only (buttons stay flush)
    topRow->addWidget(search_, 0, Qt::AlignVCenter); // centre it in the bar so it isn't clipped at the bottom
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

    // Make the top chrome keyboard/controller navigable: arrows move between Back / Search / Profile /
    // Settings, Down drops into the content, Enter activates (Enter on Search begins typing).
    for (QWidget* w : { static_cast<QWidget*>(back_), static_cast<QWidget*>(search_),
                        static_cast<QWidget*>(profileBtn_), static_cast<QWidget*>(settingsBtn_) })
    {
        w->setFocusPolicy(Qt::StrongFocus);
        w->installEventFilter(this);
    }

    // Detail-page metadata header: cover on the left, title / facts / synopsis on the right.
    // Hidden on top-level catalog views; revealed when an item is opened.
    meta_ = new QFrame(this);
    meta_->setObjectName(QStringLiteral("metaHeader"));
    meta_->setFrameShape(QFrame::StyledPanel);
    // A translucent light card so the detail page stays readable over any theme background (esp. dark ones).
    meta_->setStyleSheet(QStringLiteral(
        "QFrame#metaHeader{background:rgba(255,255,255,0.94);border:1px solid rgba(0,0,0,0.12);border-radius:12px;}"));
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
    // Action row on the detail header: Play (Steam games) and/or Favorite, side by side. Only the relevant
    // buttons are visible; Left/Right move between them. The row sits in the theme's "favorite" slot.
    actionRow_ = new QWidget(meta_);
    auto* arl = new QHBoxLayout(actionRow_);
    arl->setContentsMargins(0, 0, 0, 0);
    arl->setSpacing(8);

    playBtn_ = new QPushButton(tr("▶  Play"), actionRow_);
    playBtn_->setCursor(Qt::PointingHandCursor);
    playBtn_->setVisible(false); // shown only for Steam games
    playBtn_->setStyleSheet(QStringLiteral(
        "QPushButton{background:#3FA95E;border:2px solid #2E7D45;border-radius:6px;"
        "padding:6px 18px;color:#fff;font-weight:bold;}"
        "QPushButton:hover{background:#48BE6B;}"
        "QPushButton:focus{background:#54CE78;border-color:#1E5E32;}"));
    connect(playBtn_, &QPushButton::clicked, this, [this] {
        if (stack_.isEmpty() || !stack_.last().detail) return;
        const Level& top = stack_.last();
        const MediaItem it = top.item;
        if (it.mime == QStringLiteral("steamgame"))
        {
            MediaItem m = it;
            m.url = SteamLibrary::launchUrl(it.id.mid(QStringLiteral("steam:").size()));
            emit openItem(m); // MainWindow launches the steam:// URL
            return;
        }
        if (isReadableChapter(it.type)) // a manga chapter -> resolve its page images, then open the reader
        {
            showToast(tr("Loading “%1”…").arg(it.title), 20000);
            playBtn_->setEnabled(false);
            const QString key = it.id, title = it.title;
            mgr_->resolveMangaChapterPages(it.id, [this, key, title](const QStringList& pages) {
                playBtn_->setEnabled(true);
                if (pages.isEmpty())
                    showToast(tr("No readable pages for “%1”. Licensed/official English chapters "
                                 "aren't hosted here — try another chapter or title.").arg(title), 7000);
                else { if (toast_) toast_->hide(); emit openImagePages(title, key, pages); }
            });
            return;
        }
        // A metadata-only item browsed from a LOCAL catalog (AIO Catalog) - comic issue / book / audiobook /
        // retro game - whose actual file the file provider (Allarr) supplies. Bridge it by searching the
        // provider's catalog of that type for a query built from the title (+ its context), then open the
        // first match. A game's console (the platform we drilled in from) tags the search and picks the core.
        const bool localBridge = top.addon && top.addon->transport != LoadedAddon::RemoteHttp
            && (it.type == QStringLiteral("comic_issue") || it.type == QStringLiteral("book")
                || it.type == QStringLiteral("audiobook") || it.type == QStringLiteral("game"));
        if (localBridge)
        {
            const QString catType = (it.type == QStringLiteral("comic_issue")) ? QStringLiteral("comic") : it.type;
            QString query;
            if (it.type == QStringLiteral("comic_issue"))
            {
                // "<volume name> <issue #>" - the volume is the level we drilled in from.
                QString volume;
                if (stack_.size() >= 2) volume = stack_.at(stack_.size() - 2).item.title.trimmed();
                const QRegularExpression re(QStringLiteral("#\\s*([0-9]+(?:\\.[0-9]+)?)"));
                const auto m = re.match(it.title);
                query = (volume + QLatin1Char(' ') + (m.hasMatch() ? m.captured(1) : QString())).trimmed();
            }
            else if (it.type == QStringLiteral("game"))
            {
                // "<game> <console>": the console is the platform we drilled in from; Allarr parses the
                // trailing console name to tag its ROM search and choose the file extensions to look for.
                QString console;
                if (stack_.size() >= 2 && stack_.at(stack_.size() - 2).item.type == QStringLiteral("platform"))
                    console = stack_.at(stack_.size() - 2).item.title.trimmed();
                query = (it.title + QLatin1Char(' ') + console).trimmed();
            }
            else
            {
                // Book / audiobook: "<title> <author>" (the subtitle is "Author · Year").
                const QString author = it.subtitle.section(QStringLiteral(" · "), 0, 0).trimmed();
                query = (it.title + QLatin1Char(' ') + author).trimmed();
            }
            if (query.isEmpty()) query = it.title;
            const bool read = (it.type == QStringLiteral("comic_issue") || it.type == QStringLiteral("book"));
            showToast(read ? tr("Finding “%1” to read…").arg(it.title) : tr("Finding “%1” to play…").arg(it.title), 30000);
            playBtn_->setEnabled(false);
            const QString title = it.title;
            mgr_->resolveDocumentByQuery(query, catType, [this, it, title](const QString& url, const QString& mime, const QString& err) {
                playBtn_->setEnabled(true);
                if (!url.isEmpty()) { if (toast_) toast_->hide(); MediaItem m = it; m.url = url; m.mime = mime; emit openItem(m); }
                else if (!err.isEmpty())
                    showToast(tr("Couldn't reach the file provider (Allarr) — is it running? (%1)").arg(err), 8000);
                else
                    showToast(tr("The file provider (Allarr) has no copy of “%1”.").arg(title), 7000);
            });
            return;
        }
        if (top.addon && top.addon->transport == LoadedAddon::RemoteHttp) // resolve via the addon's /stream
        {
            LoadedAddon* addon = top.addon;
            const bool fileProvider = !addon->stremio; // Allarr-style provider: supports alternate sources (?n=)
            lastPlay_ = { addon, it, false, {}, {}, 0 };
            // A type-aware "looking…" line: the host addon may search indexers, open a
            // ROM pack, or extract a file before it can hand back a URL, so this can sit
            // for a few seconds. (Download progress is shown separately once it resolves.)
            const QString lookingMsg =
                it.type == QStringLiteral("game")  ? tr("Looking for the ROM for “%1”…").arg(it.title)
              : (it.type == QStringLiteral("movie") || it.type == QStringLiteral("series"))
                                                   ? tr("Finding a stream for “%1”…").arg(it.title)
                                                   : tr("Looking for “%1”…").arg(it.title);
            showToast(lookingMsg, 30000);
            playBtn_->setEnabled(false);
            mgr_->resolveStream(addon, it, [this, it, fileProvider](const QString& url, const QString& mime) {
                playBtn_->setEnabled(true);
                if (!url.isEmpty()) { if (toast_) toast_->hide(); MediaItem m = it; m.url = url; m.mime = mime; m.nextSourceCapable = fileProvider; emit openItem(m); }
                else showToast(tr("No playable source for “%1”. The addon returned no usable link.").arg(it.title), 7000);
            });
            return;
        }
        if (!playImdbId_.isEmpty()) // a non-Stremio catalog item bridged to IMDB -> resolve via stream addons
        {
            lastPlay_ = { nullptr, it, true, playStremioType_, playImdbId_, 0 };
            const bool fileProvider = mgr_->hasFileProvider(); // an alternate source is only offerable via Allarr
            showToast(tr("Finding a stream for “%1”…").arg(it.title), 30000);
            playBtn_->setEnabled(false);
            mgr_->resolveStreamByImdb(playStremioType_, playImdbId_, [this, it, fileProvider](const QString& url, const QString& mime) {
                playBtn_->setEnabled(true);
                if (!url.isEmpty()) { if (toast_) toast_->hide(); MediaItem m = it; m.url = url; m.mime = mime; m.nextSourceCapable = fileProvider; emit openItem(m); }
                else showToast(tr("No sources found for “%1”. No stream addon returned a playable link "
                                  "(check that Allarr is configured and returning results).").arg(it.title), 7000);
            });
            return;
        }
    });
    playBtn_->installEventFilter(this); // Backspace here = Back
    arl->addWidget(playBtn_);

    favBtn_ = new QPushButton(tr("☆ Favorite"), actionRow_);
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
    arl->addWidget(favBtn_);

    downloadBtn_ = new QPushButton(tr("⬇ Download"), actionRow_);
    downloadBtn_->setCursor(Qt::PointingHandCursor);
    downloadBtn_->setStyleSheet(QStringLiteral(
        "QPushButton{background:#DDEBFF;border:2px solid #5A8CFF;border-radius:6px;"
        "padding:6px 14px;color:#1A3A7A;font-weight:bold;}"
        "QPushButton:hover{background:#C6DBFF;}"
        "QPushButton:focus{background:#A9C8FF;border-color:#2E5BC9;}"));
    downloadBtn_->setVisible(false); // shown in requestMeta() for downloadable items
    connect(downloadBtn_, &QPushButton::clicked, this, [this] { startDownload(); });
    downloadBtn_->installEventFilter(this);
    arl->addWidget(downloadBtn_);

    arl->addStretch(1);
    mc->addWidget(actionRow_);

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
    metaOverview_->viewport()->setAutoFillBackground(false); // let the (themed) card show through
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
    // Single click: move the cursor to the item that was actually clicked (not whatever was "current"),
    // scroll it fully into view, then open it.
    connect(grid_, &QListWidget::itemClicked, this, [this](QListWidgetItem* it) {
        if (!it) return;
        grid_->setCurrentItem(it);
        grid_->scrollToItem(it, QAbstractItemView::EnsureVisible);
        activateItem(grid_->row(it));
    });

    // Right-click a favourite on the Home list to remove it.
    grid_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(grid_, &QListWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QListWidgetItem* w = grid_->itemAt(pos);
        if (w) showItemContextMenu(grid_->row(w), grid_->viewport()->mapToGlobal(pos));
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
    connect(carousel_, &CarouselView::navUp, this, [this] { focusUpFromColumn(); });
    v->addWidget(carousel_, 1);

    // The PS3 XMB view (shown instead of the grid/carousel when the theme's layout is "xmb").
    xmb_ = new XmbView(this);
    xmb_->hide();
    connect(xmb_, &XmbView::activated, this, [this](const QString& key) {
        if (key.startsWith(QStringLiteral("item:"))) activateItem(key.mid(5).toInt());
    });
    connect(xmb_, &XmbView::categoryChanged, this, &HomeView::activateNav); // moved to another category
    connect(xmb_, &XmbView::backRequested, this, &HomeView::goBack);
    connect(xmb_, &XmbView::navUpOffTop, this, [this] { focusUpFromColumn(); });
    connect(xmb_, &XmbView::currentChanged, this, [this](int idx, int total) {
        if (total > 0 && idx >= total - 2) loadMore(); // near the end -> pull the next page
    });
    connect(xmb_, &XmbView::itemContextMenu, this, [this](const QString& key, const QPoint& gp) {
        if (key.startsWith(QStringLiteral("item:"))) showItemContextMenu(key.mid(5).toInt(), gp);
    });
    v->addWidget(xmb_, 1);

    // The bottom status/description strip was removed; keep the label as a hidden no-op sink so the
    // existing status_->setText(...) calls remain harmless.
    status_ = new QLabel(this);
    status_->hide();

    // A floating toast for Play/Read progress + errors (the old bottom status strip was removed, so plain
    // status_->setText messages were invisible). Centred near the bottom; auto-hides after a few seconds.
    toast_ = new QLabel(this);
    toast_->setVisible(false);
    toast_->setWordWrap(true);
    toast_->setAlignment(Qt::AlignCenter);
    toast_->setTextInteractionFlags(Qt::NoTextInteraction);
    toast_->setStyleSheet(QStringLiteral(
        "QLabel{background:rgba(18,20,26,0.95);color:#f4f6f8;border:1px solid rgba(255,255,255,0.18);"
        "border-radius:10px;padding:12px 22px;font-size:12pt;font-weight:600;}"));
    toastTimer_ = new QTimer(this);
    toastTimer_->setSingleShot(true);
    connect(toastTimer_, &QTimer::timeout, this, [this] { if (toast_) toast_->hide(); });

    connect(mgr_, &AddonManager::catalogReady, this, &HomeView::onCatalogReady);
    connect(mgr_, &AddonManager::metaReady, this, &HomeView::onMetaReady);
    connect(mgr_, &AddonManager::sourcesChanged, this, &HomeView::refresh); // a remote addon was added/removed

    refresh();
}

void HomeView::refresh()
{
    g_theme = ThemeStore::current(); // the active profile's colour theme
    applyThemeFont();
    // A theme with a dark background image (low dim) wants a dark, light-text detail card so it stays
    // readable AND fits the theme; light themes get a light, dark-text card.
    styleMetaPanel(!g_theme.background.isEmpty() && g_theme.backgroundDim < 0.30);

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

    auto isSeriesType = [](const QString& t) { return t == QStringLiteral("series") || t == QStringLiteral("tv"); };

    // Gather every enabled catalog, then show ONE tab per media type. The browsable local catalog (AIO
    // Catalog), and any Stremio catalog, own the tabs; a non-Stremio file provider (Allarr) supplies files
    // (movies/TV resolve through it by IMDB id; comics are read by bridging the browsed title to its search)
    // and doesn't add its own tab. So comics keep AIO Catalog's browsable list and read via the provider.
    auto sourceScore = [](LoadedAddon* a) {
        const bool fileProvider = (a->transport == LoadedAddon::RemoteHttp && !a->stremio);
        return fileProvider ? 0 : 1; // a browsable catalog (local/Stremio) wins a tab over a file provider
    };
    struct CatRef { LoadedAddon* addon; AddonCatalog cat; };
    QVector<CatRef> all;
    for (LoadedAddon* s : mgr_->sources())
    {
        if (!mgr_->isEnabled(s->manifest.id)) continue;
        for (const AddonCatalog& c : mgr_->catalogs(s)) all.push_back({ s, c });
    }
    QHash<QString, int> bestScore; // best source score available per media type
    for (const CatRef& c : all)
        bestScore[c.cat.type] = qMax(bestScore.value(c.cat.type, -1), sourceScore(c.addon));
    auto wins = [&](const CatRef& c) { return sourceScore(c.addon) >= bestScore.value(c.cat.type, 0); };

    auto addCat = [&](LoadedAddon* addon, const AddonCatalog& c, const QString& display) {
        auto* btn = new QPushButton(display, this);
        const QString cid = c.id, ctype = c.type;
        connect(btn, &QPushButton::clicked, this, [this, addon, cid, ctype, display] { selectType(addon, cid, ctype, display); });
        makeTab(btn, cid, ctype);
        navTargets_.push_back({ cid, false, addon, cid, ctype, display });
        if (first) { firstAddon = addon; firstCat = cid; firstType = ctype; firstName = display; first = false; }
    };

    // Lead with a single Movies tab, then a single TV tab, then every other winning catalog (one per type).
    bool didMovie = false, didSeries = false;
    for (const CatRef& c : all)
        if (wins(c) && c.cat.type == QStringLiteral("movie") && !didMovie) { addCat(c.addon, c.cat, tr("Movies")); didMovie = true; }
    for (const CatRef& c : all)
        if (wins(c) && isSeriesType(c.cat.type) && !didSeries) { addCat(c.addon, c.cat, tr("TV")); didSeries = true; }
    for (const CatRef& c : all)
    {
        if (!wins(c)) continue;
        if (c.cat.type == QStringLiteral("movie") || isSeriesType(c.cat.type)) continue; // already led with Movies/TV
        addCat(c.addon, c.cat, c.cat.name);
    }

    typeBar_->addStretch(1);

    // Carousel layout (ES/RetroBat-style): the media types become a spinning carousel; the tab strip hides.
    carouselMode_ = (g_theme.layout == QStringLiteral("carousel"));
    xmbMode_      = (g_theme.layout == QStringLiteral("xmb"));
    if (typeHost_) typeHost_->setVisible(!carouselMode_ && !xmbMode_);
    if (carouselMode_)
    {
        xmb_->hide();
        styleTypeButtons(QStringLiteral("home")); // theme the chrome behind the carousel
        showCarousel();                           // builds the media-type carousel from navTargets_
        return;
    }
    if (xmbMode_)
    {
        carousel_->hide();
        styleTypeButtons(QStringLiteral("home")); // theme the chrome behind the XMB
        showXmb();                                // builds the XMB categories from navTargets_
        return;
    }
    carousel_->hide();
    xmb_->hide();

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

void HomeView::showXmb()
{
    // The category bar is built from the nav targets (Home + each catalog) and stays visible. The active
    // category's items fill the vertical column; activating a category loads it (via activateNav).
    QVector<XmbEntry> cats;
    for (const NavTarget& t : navTargets_)
        cats.push_back({ t.navKey, t.name, typeColor(t.type), QString() });

    // Land on the last-used category if known, else Home when it has content, else the first catalog.
    QString activeKey = lastMediaKey_;
    bool valid = false;
    for (const NavTarget& t : navTargets_) if (t.navKey == activeKey) { valid = true; break; }
    if (!valid)
    {
        activeKey.clear();
        for (const NavTarget& t : navTargets_)
        {
            if (t.isHome && !RecentStore::list().isEmpty()) { activeKey = t.navKey; break; }
            if (!t.isHome && activeKey.isEmpty()) activeKey = t.navKey; // first catalog as fallback
        }
    }

    xmb_->setCategories(cats, activeKey);
    grid_->hide();
    hideMeta();
    carousel_->hide();
    xmb_->show();
    xmb_->raise();
    xmb_->setFocus(Qt::OtherFocusReason);
    updateChrome();
    if (!activeKey.isEmpty()) activateNav(activeKey); // load the active category's column
}

void HomeView::activateNav(const QString& navKey)
{
    for (const NavTarget& t : navTargets_)
        if (t.navKey == navKey)
        {
            atCarouselLanding_ = false;
            atXmbRoot_ = true;               // activating a category lands at its top level
            lastMediaKey_ = navKey;          // remember it so Back highlights this type in the carousel/XMB
            if (xmbMode_)
            {
                xmb_->setActiveCategory(navKey); // sync the bar (no-op if already there)
                xmb_->setAtRoot(true);
                xmb_->clearItems();              // clear the old column while the new one loads
            }
            if (t.isHome) selectRecent();    // Home -> the recents list / XMB column
            else          selectType(t.addon, t.catalogId, t.type, t.name); // catalog -> item view
            return;
        }
}

void HomeView::fillXmbFromItems(int from)
{
    QVector<XmbEntry> entries;
    for (int i = qMax(0, from); i < items_.size(); ++i)
    {
        const MediaItem& it = items_[i];
        if (it.type == QStringLiteral("info") || it.type == QStringLiteral("rechdr")) continue;
        const QColor c = (it.type == QStringLiteral("_open")) ? QColor(0x6A, 0x6E, 0x78) : typeColor(it.type);
        QString label = it.title;
        const double frac = resumeFraction(resumeKeyFor(it)); // "how far in" for a partly-played movie/episode
        if (frac >= 0.0) label += QStringLiteral("    ·  %1%").arg(int(frac * 100.0));
        entries.push_back({ QStringLiteral("item:") + QString::number(i), label, c, it.thumbnailUrl });
    }

    if (from > 0) { xmb_->addItems(entries); return; } // paged append

    // Root = Home (recents) or a category's top-level catalog; drilled-in containers are not root.
    atXmbRoot_ = recentView_ || (stack_.size() == 1 && !stack_.last().detail);
    xmb_->setAtRoot(atXmbRoot_);
    const int restoreRow = stack_.isEmpty() ? -1 : stack_.last().childRow;
    const QString restoreKey = (restoreRow >= 0) ? (QStringLiteral("item:") + QString::number(restoreRow)) : QString();
    xmb_->setItems(entries, restoreKey);
    grid_->hide();
    xmb_->show();
    xmb_->raise();
    xmb_->setFocus(Qt::OtherFocusReason);
}

void HomeView::applyTheme()
{
    refresh(); // re-reads the theme: font, type-icon registry, colours, background - and re-styles the view
}

void HomeView::focusContent()
{
    searchEditing_ = false; // leaving the chrome row -> the search box is no longer in edit mode
    if (xmbMode_ && xmb_ && xmb_->isVisible())
        xmb_->setFocus(Qt::OtherFocusReason);
    else if (carouselMode_ && carousel_ && carousel_->isVisible())
        carousel_->setFocus(Qt::OtherFocusReason);
    else if (grid_->isVisible() && grid_->count() > 0)
        grid_->setFocus(Qt::OtherFocusReason);
    else if (meta_ && meta_->isVisible() && detailActionButton())
        detailActionButton()->setFocus(Qt::OtherFocusReason); // a leaf detail page -> its action button
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
    if (back_)        back_->setStyleSheet(backButtonStyle(activeColor)); // always matches the bar background
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

    metaTextCol_->removeWidget(actionRow_);
    metaTextCol_->removeWidget(metaTitle_);
    metaTextCol_->removeWidget(metaFacts_);
    metaTextCol_->removeWidget(metaOverview_);

    QSet<QString> added;
    auto place = [&](const QString& key) {
        if (added.contains(key)) return;
        added.insert(key);
        if (key == "favorite")      metaTextCol_->addWidget(actionRow_); // Play + Favorite row
        else if (key == "title")    metaTextCol_->addWidget(metaTitle_);
        else if (key == "facts")    metaTextCol_->addWidget(metaFacts_);
        else if (key == "overview") metaTextCol_->addWidget(metaOverview_, 1);
    };
    for (const QString& k : order) place(k);
    for (const QString& k : { QStringLiteral("favorite"), QStringLiteral("title"),
                              QStringLiteral("facts"), QStringLiteral("overview") }) place(k);
}

// The focusable action on the current detail page: Play for a Steam game, otherwise Favorite.
QWidget* HomeView::detailActionButton() const
{
    if (playBtn_ && playBtn_->isVisible()) return playBtn_;
    if (favBtn_  && favBtn_->isVisible())  return favBtn_;
    return nullptr;
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

// The focusable top-bar controls, left to right. Back is skipped when disabled (at the root view).
QVector<QWidget*> HomeView::chromeRow() const
{
    QVector<QWidget*> r;
    if (back_ && back_->isEnabled()) r << back_;
    if (search_)      r << search_;
    if (profileBtn_)  r << profileBtn_;
    if (settingsBtn_) r << settingsBtn_;
    return r;
}

// Jump keyboard/controller focus up into the chrome row (default: the first control, i.e. Back if it's
// available, otherwise Search).
void HomeView::focusChromeRow(QWidget* preferred)
{
    searchEditing_ = false;
    const QVector<QWidget*> row = chromeRow();
    if (row.isEmpty()) return;
    QWidget* target = (preferred && row.contains(preferred)) ? preferred : row.first();
    target->setFocus(Qt::OtherFocusReason);
}

// Move Left/Right within the chrome row, clamped at the ends.
void HomeView::focusChrome(QWidget* from, int dir)
{
    searchEditing_ = false;
    const QVector<QWidget*> row = chromeRow();
    const int i = row.indexOf(from);
    if (i < 0) { focusChromeRow(); return; }
    const int j = i + (dir > 0 ? 1 : -1);
    if (j < 0 || j >= row.size()) return; // stop at the ends
    row[j]->setFocus(Qt::OtherFocusReason);
}

// Up from the top of a content column: on a container detail page (meta header + child column) land on the
// Favorite button first; otherwise go straight to the top chrome.
void HomeView::focusUpFromColumn()
{
    if (meta_ && meta_->isVisible() && detailActionButton())
        detailActionButton()->setFocus(Qt::OtherFocusReason);
    else
        focusChromeRow();
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

// Drill into the synthetic "Steam" console (a child of the Games catalog): list the local Steam library as
// games. Pushed as a detail level so Back returns to the Games console list.
void HomeView::openSteamConsole(const MediaItem& consoleItem)
{
    if (xmbMode_) { atXmbRoot_ = false; if (xmb_) xmb_->setAtRoot(false); }
    Level lvl;
    lvl.addon = nullptr; lvl.detail = true; lvl.item = consoleItem; lvl.title = tr("Steam");
    stack_.push_back(lvl);
    populateSteamGames(); // also re-run by loadTop() when Back returns to this level
}

// (Re)build the Steam games grid/column natively from the local library (no addon request).
void HomeView::populateSteamGames()
{
    MediaCatalog cat;
    cat.title = tr("Steam");
    for (const SteamGame& g : SteamLibrary::installedGames())
    {
        MediaItem it;
        it.id = QStringLiteral("steam:") + g.appid;
        it.type = QStringLiteral("game");
        it.title = g.name;
        it.mime = QStringLiteral("steamgame"); // no url -> clicking opens the info page; Play launches it
        it.thumbnailUrl = SteamLibrary::posterUrl(g.appid);
        cat.items.push_back(it);
    }
    cat.hasMore = false;

    pendingReqId_ = -1; loading_ = false; hasMore_ = false; currentPage_ = 1;
    hideMeta();
    if (carouselMode_ || xmbMode_) grid_->hide(); else grid_->show();
    populate(cat, /*append*/ false);
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
    settingsStore().sync(); // pick up resume positions written by the player since the last render

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
            it.id = r.key;                           // stable resume key (streamed items); also read by XMB/carousel
            it.mime = r.kind;                        // routing kind (video/audio/document/game)
            it.type = iconTypeForKind(r.kind);       // drives the placeholder icon
            it.thumbnailUrl = r.thumb;               // the real poster (streamed media records it), else a placeholder
            it.title = r.title.isEmpty() ? QFileInfo(r.path).completeBaseName() : r.title;
            items_.push_back(it);

            // "Continue watching": show a percentage in the row text and a resume bar on the (small) icon.
            const double frac = resumeFraction(resumeKeyFor(it));
            QString label = QStringLiteral("  ") + it.title;
            if (frac >= 0.0) label += QStringLiteral("    ·  %1%").arg(int(frac * 100.0));
            auto* w = new QListWidgetItem(label, grid_);
            w->setSizeHint(QSize(0, 52));
            w->setIcon(iconWithProgress(defaultIcon(it.type, iconSz).pixmap(iconSz), resumeKeyFor(it)));
        }
    }

    loadThumbnails(0); // load posters for recents/favourites that have one (else the placeholder stays)
    updateChrome();
    updateStatus();

    // In XMB layout, Home is the active category's column (recents/favourites), not the grid list.
    if (xmbMode_)
    {
        grid_->hide();
        fillXmbFromItems(0); // shows + focuses the XMB; skips the group-header rows
        return;
    }

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
    {
        if (top.item.mime == QStringLiteral("steam:console")) return QString(); // Steam games aren't ROM files
        return (top.item.type == QStringLiteral("platform")) ? QStringLiteral("game") : QString(); // games per-console
    }
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
        const int k = ke->key();

        // --- Top chrome row: the search box (highlighted vs. typing) ---
        if (obj == search_)
        {
            if (searchEditing_)
            {
                // Typing: the line edit handles letters / Backspace / Enter; Esc or Down exits edit mode.
                if (k == Qt::Key_Escape) { searchEditing_ = false; return true; }
                if (k == Qt::Key_Down)   { searchEditing_ = false; focusContent(); return true; }
                return false;
            }
            if (k == Qt::Key_Left)  { focusChrome(search_, -1); return true; }
            if (k == Qt::Key_Right) { focusChrome(search_, +1); return true; }
            if (k == Qt::Key_Down)  { focusContent();           return true; }
            if (k == Qt::Key_Up)    { return true; }
            if (k == Qt::Key_Return || k == Qt::Key_Enter || k == Qt::Key_Space)
            { searchEditing_ = true; return true; } // select/Enter -> cursor in the field, start typing
            if (k == Qt::Key_Backspace) { goBack(); return true; }
            if (!ke->text().isEmpty() && ke->text().at(0).isPrint()) { searchEditing_ = true; return false; }
            return true; // swallow other keys while highlighted (not yet typing)
        }
        // --- Top chrome row: the buttons (Back / Profile / Settings) ---
        if (obj == back_ || obj == profileBtn_ || obj == settingsBtn_)
        {
            if (k == Qt::Key_Left)  { focusChrome(static_cast<QWidget*>(obj), -1); return true; }
            if (k == Qt::Key_Right) { focusChrome(static_cast<QWidget*>(obj), +1); return true; }
            if (k == Qt::Key_Down)  { focusContent(); return true; }
            if (k == Qt::Key_Up)    { return true; }
            if (k == Qt::Key_Return || k == Qt::Key_Enter || k == Qt::Key_Space)
            { if (auto* b = qobject_cast<QPushButton*>(obj)) b->click(); return true; }
            if (k == Qt::Key_Backspace) { goBack(); return true; }
            return false;
        }
        // --- Detail page: the action button (Favorite, or Play for a Steam game) ---
        if (obj == favBtn_ || (playBtn_ && obj == playBtn_))
        {
            if (k == Qt::Key_Up)   { focusChromeRow(); return true; }
            if (k == Qt::Key_Down) // drop into the child column (container detail), if any is shown
            {
                const bool col = (xmb_ && xmb_->isVisible()) || (carousel_ && carousel_->isVisible())
                                 || (grid_->isVisible() && grid_->count() > 0);
                if (col) focusContent();
                return true;
            }
            if (k == Qt::Key_Left || k == Qt::Key_Right) // move between Play and Favorite when both are shown
            {
                QWidget* other = (obj == playBtn_) ? static_cast<QWidget*>(favBtn_) : static_cast<QWidget*>(playBtn_);
                if (other && other->isVisible()) other->setFocus(Qt::OtherFocusReason);
                return true;
            }
            if (k == Qt::Key_Return || k == Qt::Key_Enter || k == Qt::Key_Space)
            { if (auto* b = qobject_cast<QPushButton*>(obj)) b->click(); return true; }
            if (k == Qt::Key_Backspace) { goBack(); return true; }
            return false;
        }

        // Backspace acts as the Back button when focus is on a tab or the grid.
        if (k == Qt::Key_Backspace) { goBack(); return true; }

        const int idx = typeButtons_.indexOf(qobject_cast<QPushButton*>(obj));
        if (idx >= 0)
        {
            // Left/Right move between tabs, then off the ends into the chrome row; Up reaches the chrome too.
            if (k == Qt::Key_Right) { if (idx + 1 < typeButtons_.size()) focusTypeButton(idx + 1); else focusChromeRow(search_); return true; }
            if (k == Qt::Key_Left)  { if (idx > 0) focusTypeButton(idx - 1); else focusChromeRow(back_); return true; }
            if (k == Qt::Key_Down)  { focusGridTop();    return true; }
            if (k == Qt::Key_Up)    { focusChromeRow();  return true; }
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
                    if (meta_ && meta_->isVisible() && detailActionButton())
                        detailActionButton()->setFocus(Qt::OtherFocusReason); // container detail -> action button
                    else if (carouselMode_)     showCarousel();        // back up to the carousel
                    else if (activeTypeButton_) activeTypeButton_->setFocus(Qt::OtherFocusReason); // to the tabs
                    else                        focusChromeRow();     // no tabs -> up to the chrome
                    return true;
                }
            }
        }
    }

    // Clicking the search box (mouse) means "edit", so arrows move the text cursor, not the chrome focus.
    if (obj == search_ && event->type() == QEvent::MouseButtonPress) searchEditing_ = true;
    if (obj == search_ && event->type() == QEvent::FocusOut)         searchEditing_ = false;

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
        if (!it.url.isEmpty()) emit openRecent(it.url, it.mime, resumeKeyFor(it), it.title, it.thumbnailUrl); // a recent -> re-open
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

    stack_.last().childRow = row; // remember where we drilled in, so Back restores this position

    // The synthetic Steam console drills into the local library natively (not via the addon).
    if (it.mime == QStringLiteral("steam:console")) { openSteamConsole(it); return; }

    LoadedAddon* addon = stack_.last().addon;

    // A remote leaf (a track, etc.) carries no url in the catalog - its source comes from the /stream
    // endpoint, fetched on open: resolve and open it directly. Movies/episodes (Play) and comics/manga/books
    // (Read) instead open an info page with a button that resolves on demand, like Stremio items - skip those.
    const bool infoPageType = it.type == QStringLiteral("movie")  || it.type == QStringLiteral("series")
                           || it.type == QStringLiteral("tv")     || it.type == QStringLiteral("episode")
                           || it.type == QStringLiteral("comic")  || it.type == QStringLiteral("manga")
                           || it.type == QStringLiteral("book")   || it.type == QStringLiteral("audiobook");
    if (!it.expandable && addon && addon->transport == LoadedAddon::RemoteHttp && !addon->stremio
        && it.type != QStringLiteral("platform") && !infoPageType)
    {
        const MediaItem item = it; // copy for the async callback
        const bool fileProvider = !addon->stremio; // Allarr-style provider: supports alternate sources (?n=)
        lastPlay_ = { addon, item, false, {}, {}, 0 };
        showToast(tr("Finding a source for “%1”…").arg(it.title), 30000);
        mgr_->resolveStream(addon, item, [this, addon, item, fileProvider](const QString& url, const QString& mime) {
            if (!url.isEmpty()) { if (toast_) toast_->hide(); MediaItem m = item; m.url = url; m.mime = mime; m.nextSourceCapable = fileProvider; emit openItem(m); }
            else { if (toast_) toast_->hide(); openDetailLevel(addon, item); } // no stream -> show its metadata instead
        });
        return;
    }

    // No file yet: open a detail page. Its metadata header describes the item; for a container
    // (TV show / season / album / console) the page also drills into its children below the header.
    openDetailLevel(addon, it);
}

void HomeView::requestNextSource()
{
    // Nothing opened from a file provider yet (or it came from a Stremio source, which has no ?n=).
    if (!lastPlay_.viaImdb && !lastPlay_.addon) { emit nextSourceResult(false, tr("No alternate source to try.")); return; }

    const int attempt = lastPlay_.attempt + 1; // advance only on success, so a failed try can be repeated
    const MediaItem item = lastPlay_.item;

    auto onResolved = [this, item, attempt](const QString& url, const QString& mime) {
        if (url.isEmpty()) { emit nextSourceResult(false, tr("No other source available for “%1”.").arg(item.title)); return; }
        lastPlay_.attempt = attempt;
        MediaItem m = item; m.url = url; m.mime = mime; m.nextSourceCapable = true;
        emit nextSourceResult(true, QString());
        emit openItem(m); // re-opens in the right view (player/reader); resume keys on the stable id
    };

    if (lastPlay_.viaImdb) mgr_->resolveStreamByImdb(lastPlay_.imdbType, lastPlay_.imdbId, onResolved, attempt);
    else                   mgr_->resolveStream(lastPlay_.addon, item, onResolved, attempt);
}

// ---- download crawl: resolve one item (or a whole series/season) to files, queued to MainWindow ----------

void HomeView::startDownload()
{
    if (stack_.isEmpty() || !stack_.last().detail) return;
    if (dlBusy_) { showToast(tr("A download is already being prepared…"), 4000); return; }
    const Level& top = stack_.last();
    DlNode root;
    root.addon = top.addon;
    root.item = top.item;
    if (stack_.size() >= 2) { root.parentTitle = stack_.at(stack_.size() - 2).item.title;
                              root.parentType  = stack_.at(stack_.size() - 2).item.type; }
    dlQueue_.clear();
    dlQueue_.append(root);
    dlQueued_ = 0;
    dlBusy_ = true;
    showToast(top.item.expandable ? tr("Preparing downloads for “%1”…").arg(top.item.title)
                                  : tr("Preparing download for “%1”…").arg(top.item.title), 30000);
    dlNext();
}

void HomeView::dlNext()
{
    if (dlQueue_.isEmpty())
    {
        dlBusy_ = false;
        showToast(dlQueued_ > 0 ? tr("Queued %1 item(s) to download — they’ll appear in Recent.").arg(dlQueued_)
                                : tr("Nothing here could be downloaded."), 6000);
        return;
    }
    const DlNode node = dlQueue_.takeFirst();
    if (node.item.expandable)
    {
        dlDetailNode_ = node;
        dlDetailReq_ = mgr_->requestDetail(node.addon, node.item, 1); // children -> onCatalogReady crawl branch
    }
    else
    {
        dlResolveLeaf(node);
    }
}

void HomeView::dlResolveLeaf(const DlNode& node)
{
    const MediaItem it = node.item;
    // Can't pull as a single file: a Steam launch, or a page-based manga chapter.
    if (it.mime == QStringLiteral("steamgame") || isReadableChapter(it.type)) { dlNext(); return; }

    const bool localBridge = node.addon && node.addon->transport != LoadedAddon::RemoteHttp
        && (it.type == QStringLiteral("comic_issue") || it.type == QStringLiteral("book")
            || it.type == QStringLiteral("audiobook") || it.type == QStringLiteral("game"));
    if (localBridge)
    {
        const QString catType = (it.type == QStringLiteral("comic_issue")) ? QStringLiteral("comic") : it.type;
        QString query;
        if (it.type == QStringLiteral("comic_issue"))
        {
            const QRegularExpression re(QStringLiteral("#\\s*([0-9]+(?:\\.[0-9]+)?)"));
            const auto m = re.match(it.title);
            query = (node.parentTitle + QLatin1Char(' ') + (m.hasMatch() ? m.captured(1) : QString())).trimmed();
        }
        else if (it.type == QStringLiteral("game"))
        {
            const QString console = (node.parentType == QStringLiteral("platform")) ? node.parentTitle : QString();
            query = (it.title + QLatin1Char(' ') + console).trimmed();
        }
        else
        {
            const QString author = it.subtitle.section(QStringLiteral(" · "), 0, 0).trimmed();
            query = (it.title + QLatin1Char(' ') + author).trimmed();
        }
        if (query.isEmpty()) query = it.title;
        mgr_->resolveDocumentByQuery(query, catType, [this, it](const QString& url, const QString& mime, const QString&) {
            if (!url.isEmpty()) dlEmit(it, url, mime);
            dlNext();
        });
        return;
    }
    if (node.addon && node.addon->transport == LoadedAddon::RemoteHttp) // file provider OR Stremio: its /stream
    {
        mgr_->resolveStream(node.addon, it, [this, it](const QString& url, const QString& mime) {
            if (!url.isEmpty()) dlEmit(it, url, mime);
            dlNext();
        });
        return;
    }
    // A movie/episode browsed from a local catalog (AIO): fetch its /meta to learn the IMDB id, then bridge.
    if (it.type == QStringLiteral("movie") || it.type == QStringLiteral("episode")
        || it.type == QStringLiteral("series") || it.type == QStringLiteral("tv"))
    {
        dlMetaNode_ = node;
        dlMetaReq_ = mgr_->requestMeta(node.addon, it); // -> onMetaReady crawl branch
        return;
    }
    dlNext(); // unknown / non-downloadable leaf
}

void HomeView::dlEmit(const MediaItem& it, const QString& url, const QString& mime)
{
    MediaItem m = it; m.url = url; m.mime = mime;
    emit downloadItem(m);
    ++dlQueued_;
}

void HomeView::openDetailLevel(LoadedAddon* addon, const MediaItem& it)
{
    if (xmbMode_) { atXmbRoot_ = false; if (xmb_) xmb_->setAtRoot(false); } // drilled below the category root
    Level lvl;
    lvl.addon = addon; lvl.detail = true; lvl.item = it; lvl.title = it.title;
    stack_.push_back(lvl);
    loadTop();
}

// Right-click on the Home list: offer to remove the Recent or Favorite under the cursor.
void HomeView::showItemContextMenu(int row, const QPoint& globalPos)
{
    if (!recentView_ || row < 0 || row >= items_.size()) return;
    const MediaItem& it = items_[row];
    if (it.type == QStringLiteral("rechdr") || it.type == QStringLiteral("info")) return; // a header, not actionable

    QMenu menu(this);
    const bool fav = it.mime.startsWith(QStringLiteral("fav:"));
    QAction* remove = menu.addAction(fav ? tr("Remove from Favorites") : tr("Remove from Recent"));
    if (menu.exec(globalPos) != remove) return;
    if (fav)
        FavoritesStore::remove(it.id);
    else
    {
        RecentStore::remove(it.url.isEmpty() ? resumeKeyFor(it) : it.url);
        clearResume(resumeKeyFor(it)); // also forget where you left off, so it starts fresh next time
    }
    renderRecents(); // refresh the Home list
}

void HomeView::openFavorite(const MediaItem& favItem)
{
    // A favourited Steam game has no source addon - reopen its native info page (rooted at Home).
    if (favItem.id.startsWith(QStringLiteral("steam:")))
    {
        recentView_ = false;
        applyGridMode(/*recentList*/ false);
        styleTypeButtons(QStringLiteral("home"));
        stack_.clear();
        MediaItem mi = favItem;
        mi.mime = QStringLiteral("steamgame"); // restore the marker (drops the "fav:" tag)
        mi.url.clear();
        Level lvl;
        lvl.addon = nullptr; lvl.detail = true; lvl.item = mi; lvl.title = mi.title;
        stack_.push_back(lvl);
        loadTop();
        return;
    }
    // Resolve the favourite's source addon and open its detail page (rooted at Home so Back returns here).
    const QString addonId = favItem.mime.mid(4); // strip "fav:"
    LoadedAddon* addon = nullptr;
    for (LoadedAddon* s : mgr_->sources())
        if (s->manifest.id == addonId) { addon = s; break; }
    if (!addon)
    {
        showToast(tr("That favourite's source addon isn't available."), 6000);
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
    pendingRestoreRow_ = -1; // fresh view: any in-progress "page toward the drilled item" restore is moot
    const Level& top = stack_.last();

    // Returning to the Steam console (e.g. Back from a game's info page): repopulate natively, not via addon.
    if (top.detail && top.item.mime == QStringLiteral("steam:console")) { populateSteamGames(); return; }

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
        if (xmb_) xmb_->hide();
        loading_ = false; hasMore_ = false; currentPage_ = 1; pendingReqId_ = -1;
        updateChrome();
        updateStatus();
        // The grid/carousel that held focus is now hidden; park focus on the Favorite button so the
        // detail page still has a keyboard target (and Backspace routes to Back via its event filter).
        if (meta_->isVisible()) { if (QWidget* a = detailActionButton()) a->setFocus(Qt::OtherFocusReason); }
        return;
    }

    if (carouselMode_ || xmbMode_) grid_->hide(); // the carousel/XMB shows catalog items; populate() fills them
    else                           grid_->show();
    issueRequest(/*append*/ false);
}

void HomeView::requestMeta(const MediaItem& item)
{
    metaItem_ = item;             // remembered for the meta fallback in onMetaReady
    metaFallbackTried_ = false;
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

    // Show an action button for launchable leaves from a remote addon (Stremio, or a library like Allarr):
    // movie/episode -> "▶ Play", comic/manga/book document -> "📖 Read". Both resolve via the addon's /stream
    // on click. A specific MangaDex chapter also gets "Read". Steam games get "▶ Play". Containers get none.
    const bool isSteam = (item.mime == QStringLiteral("steamgame"));
    const bool remoteLeaf = !stack_.isEmpty() && stack_.last().addon && !item.expandable
        && stack_.last().addon->transport == LoadedAddon::RemoteHttp;
    const bool isRemotePlayable = remoteLeaf
        && (stack_.last().addon->stremio
            || item.type == QStringLiteral("movie") || item.type == QStringLiteral("series")
            || item.type == QStringLiteral("tv")    || item.type == QStringLiteral("episode")
            || item.type == QStringLiteral("audiobook"));
    const bool isRemoteReadable = remoteLeaf && !stack_.last().addon->stremio
        && (item.type == QStringLiteral("comic") || item.type == QStringLiteral("manga")
            || item.type == QStringLiteral("book"));
    // A comic issue browsed from AIO Catalog (Comic Vine, metadata-only): readable if a file provider
    // (Allarr) is available to supply the actual CBZ, found by bridging the title to its search.
    // A metadata-only leaf browsed from a LOCAL catalog (AIO Catalog) whose actual file the file provider
    // (Allarr) can supply by title: a comic issue or book is read; an audiobook is played. (Comic Vine /
    // Google Books carry no file themselves, so without a provider these would offer nothing.)
    const bool localLeaf = !item.expandable && !stack_.isEmpty() && stack_.last().addon
        && stack_.last().addon->transport != LoadedAddon::RemoteHttp;
    const bool canBridge = localLeaf && mgr_->hasFileProvider();
    const bool isBridgedReadable = canBridge
        && (item.type == QStringLiteral("comic_issue") || item.type == QStringLiteral("book"));
    const bool isBridgedAudio = canBridge && item.type == QStringLiteral("audiobook");
    // A game browsed from AIO Catalog (IGDB, metadata-only): playable if the provider (Allarr) can supply the
    // ROM, found by bridging "<game> <console>" to its retro-games search. The console is the parent platform.
    const bool isBridgedGame = canBridge && item.type == QStringLiteral("game");
    const bool isReadable = isReadableChapter(item.type) || isRemoteReadable || isBridgedReadable;
    playImdbId_.clear(); playStremioType_.clear(); // a bridged Play (if any) is established in showMeta()
    if (playBtn_)
    {
        playBtn_->setText(isReadable ? tr("📖  Read") : tr("▶  Play"));
        playBtn_->setVisible(isSteam || isRemotePlayable || isReadable || isBridgedAudio || isBridgedGame);
    }
    if (downloadBtn_)
    {
        // Downloadable: a resolvable leaf (anything but a Steam launch or a page-based manga chapter), or a
        // container we can crawl (a series/season -> episodes, a comic volume -> issues).
        const bool dlLeaf = !item.expandable
            && (isRemotePlayable || isRemoteReadable || isBridgedReadable || isBridgedAudio || isBridgedGame);
        const bool dlContainer = item.expandable
            && (item.type == QStringLiteral("series") || item.type == QStringLiteral("tv")
                || item.type == QStringLiteral("season") || item.type == QStringLiteral("comic"));
        downloadBtn_->setVisible(dlLeaf || dlContainer);
    }
    if (favBtn_)  favBtn_->setVisible(true); // favourite-able like normal media (text set above)

    layoutMetaSections(item.type); // order the text rows per the theme
    meta_->setVisible(true);
    if (isSteam)
    {
        pendingMetaReqId_ = (steamMetaSeq_ -= 1); // a unique (negative) id for the async store fetch guard
        requestSteamMeta(item, pendingMetaReqId_);
        return;
    }
    pendingMetaReqId_ = mgr_->requestMeta(stack_.last().addon, item);

    // Show the catalog poster + title right away (guarded by the request id we just set), so the info page
    // has a cover immediately - and still shows one if the addon returns no /meta at all (e.g. Allarr). A
    // valid /meta result later overrides this with the addon's own cover + facts + synopsis.
    if (!item.thumbnailUrl.isEmpty())
    {
        MediaDetail d0; d0.title = item.title; d0.imageUrl = item.thumbnailUrl; d0.valid = true;
        showMeta(d0);
    }
}

// Build a Steam game's detail page: cover from the library art immediately, then enrich (synopsis, genres,
// developer, release date, Metacritic) from Steam's public store appdetails API. Best-effort; no key needed.
void HomeView::requestSteamMeta(const MediaItem& item, int reqId)
{
    MediaDetail d0;
    d0.title = item.title;
    d0.imageUrl = item.thumbnailUrl; // the vertical capsule
    d0.valid = true;
    showMeta(d0); // show the cover + title straight away

    const QString appid = item.id.mid(QStringLiteral("steam:").size());
    QUrl u(QStringLiteral("https://store.steampowered.com/api/appdetails"));
    QUrlQuery q; q.addQueryItem(QStringLiteral("appids"), appid); q.addQueryItem(QStringLiteral("l"), QStringLiteral("english"));
    u.setQuery(q);
    QNetworkRequest req(u);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MyMediaVault"));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = nam_->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, appid, item, reqId] {
        reply->deleteLater();
        if (reqId != pendingMetaReqId_ || reply->error() != QNetworkReply::NoError) return; // stale / failed
        const QJsonObject entry = QJsonDocument::fromJson(reply->readAll()).object().value(appid).toObject();
        if (!entry.value(QStringLiteral("success")).toBool()) return; // keep the minimal cover+title
        const QJsonObject data = entry.value(QStringLiteral("data")).toObject();
        MediaDetail d;
        d.title = data.value(QStringLiteral("name")).toString(item.title);
        d.overview = data.value(QStringLiteral("short_description")).toString();
        d.imageUrl = item.thumbnailUrl;
        QStringList genres;
        for (const QJsonValue& v : data.value(QStringLiteral("genres")).toArray())
            genres << v.toObject().value(QStringLiteral("description")).toString();
        if (!genres.isEmpty()) d.facts.push_back({ tr("Genres"), genres.join(QStringLiteral(", ")) });
        QStringList devs;
        for (const QJsonValue& v : data.value(QStringLiteral("developers")).toArray()) devs << v.toString();
        if (!devs.isEmpty()) d.facts.push_back({ tr("Developer"), devs.join(QStringLiteral(", ")) });
        const QString rel = data.value(QStringLiteral("release_date")).toObject().value(QStringLiteral("date")).toString();
        if (!rel.isEmpty()) d.facts.push_back({ tr("Released"), rel });
        const int mc = data.value(QStringLiteral("metacritic")).toObject().value(QStringLiteral("score")).toInt();
        if (mc > 0) d.facts.push_back({ tr("Metacritic"), QString::number(mc) });
        d.valid = true;
        if (reqId == pendingMetaReqId_) showMeta(d);
    });
}

void HomeView::onMetaReady(int requestId, const MediaDetail& detail)
{
    if (requestId == dlMetaReq_) // a download crawl's item meta arrived: bridge it by IMDB id, then continue
    {
        dlMetaReq_ = -1;
        const MediaItem it = dlMetaNode_.item;
        const QString imdb = detail.imdbStreamId;
        if (!imdb.isEmpty())
        {
            const QString stremioType = (it.type == QStringLiteral("movie")) ? QStringLiteral("movie")
                                                                              : QStringLiteral("series");
            mgr_->resolveStreamByImdb(stremioType, imdb, [this, it](const QString& url, const QString& mime) {
                if (!url.isEmpty()) dlEmit(it, url, mime);
                dlNext();
            });
        }
        else dlNext(); // no IMDB id -> can't resolve this one
        return;
    }
    if (requestId != pendingMetaReqId_) return; // stale (navigated away / newer item)
    if (detail.valid) { showMeta(detail); return; }

    // The source addon returned no metadata. If this is a movie/episode with an embedded IMDB id and another
    // installed addon (e.g. AIO Catalog) can supply metadata, enrich from it - once - so the info page isn't bare.
    if (metaFallbackTried_) return; // keep the placeholder cover+title
    metaFallbackTried_ = true;
    MediaItem mi = imdbMetaItem(metaItem_);
    if (mi.id.isEmpty() || stack_.isEmpty()) return;
    LoadedAddon* prov = mgr_->metaProviderFor(stack_.last().addon, mi.type);
    if (!prov) return;
    pendingMetaReqId_ = mgr_->requestMeta(prov, mi); // its onMetaReady (now valid) will showMeta()
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

    // TMDB->IMDB bridge: a non-Stremio catalog item (AIO Catalog movie/episode) that supplied an IMDB stream
    // id can be played through the installed Stremio stream addons (Allarr/Torrentio) - reveal a Play button.
    if (!d.imdbStreamId.isEmpty() && !stack_.isEmpty() && stack_.last().detail
        && !(stack_.last().addon && stack_.last().addon->stremio)) // Stremio items already get one in requestMeta
    {
        const QString t = stack_.last().item.type;
        const QString stremioType = (t == QStringLiteral("episode")) ? QStringLiteral("series")
                                  : (t == QStringLiteral("movie"))   ? QStringLiteral("movie") : QString();
        if (!stremioType.isEmpty() && mgr_->hasStreamProvider(stremioType))
        {
            playImdbId_ = d.imdbStreamId;
            playStremioType_ = stremioType;
            if (playBtn_) { playBtn_->setText(tr("▶  Play")); playBtn_->setVisible(true); }
        }
    }

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

void HomeView::showToast(const QString& text, int ms)
{
    if (!toast_) return;
    toast_->setText(text);
    toast_->setMaximumWidth(qMax(240, int(width() * 0.7)));
    toast_->adjustSize();
    repositionToast();
    toast_->show();
    toast_->raise();
    if (toastTimer_) toastTimer_->start(ms);
}

void HomeView::repositionToast()
{
    if (!toast_ || toast_->isHidden()) return;
    toast_->adjustSize();
    const int x = (width() - toast_->width()) / 2;
    const int y = height() - toast_->height() - 48; // floats just above the bottom edge
    toast_->move(qMax(8, x), qMax(8, y));
}

void HomeView::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    repositionToast();
}

// Theme the detail card. Colours are set EXPLICITLY (not via palette) because a stylesheet on the panel
// breaks Qt's palette propagation to the child labels - which on a dark-mode OS would otherwise render
// them in the default light text, i.e. light-on-light. Dark themes get a dark card with light text.
void HomeView::styleMetaPanel(bool dark)
{
    if (!meta_) return;
    if (dark)
    {
        meta_->setStyleSheet(QStringLiteral(
            "QFrame#metaHeader{background:rgba(18,26,42,0.84);border:1px solid rgba(255,255,255,0.16);border-radius:12px;}"));
        if (metaTitle_)    metaTitle_->setStyleSheet(QStringLiteral("font-size:15pt;color:#eef2f7;"));
        if (metaFacts_)    metaFacts_->setStyleSheet(QStringLiteral("color:#c7cfdb;"));
        if (metaOverview_) metaOverview_->setStyleSheet(QStringLiteral(
            "QTextBrowser{background:transparent;color:#dfe5ee;border:none;}"));
    }
    else
    {
        meta_->setStyleSheet(QStringLiteral(
            "QFrame#metaHeader{background:rgba(255,255,255,0.96);border:1px solid rgba(0,0,0,0.12);border-radius:12px;}"));
        if (metaTitle_)    metaTitle_->setStyleSheet(QStringLiteral("font-size:15pt;color:#1b1b1b;"));
        if (metaFacts_)    metaFacts_->setStyleSheet(QStringLiteral("color:#2a2d33;"));
        if (metaOverview_) metaOverview_->setStyleSheet(QStringLiteral(
            "QTextBrowser{background:transparent;color:#26282c;border:none;}"));
    }
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
    if (requestId == dlDetailReq_) // a download crawl's children arrived: queue them depth-first, then continue
    {
        dlDetailReq_ = -1;
        const DlNode parent = dlDetailNode_;
        QList<DlNode> kids;
        for (const MediaItem& child : cat.items)
        {
            if (child.type == QStringLiteral("info") || child.type == QStringLiteral("rechdr")) continue;
            DlNode n; n.addon = parent.addon; n.item = child;
            n.parentTitle = parent.item.title; n.parentType = parent.item.type;
            kids.append(n);
        }
        for (int i = kids.size() - 1; i >= 0; --i) dlQueue_.prepend(kids[i]); // process this container's content first
        dlNext();
        return;
    }
    if (requestId != pendingReqId_) return; // a superseded request (navigated away / newer page)
    loading_ = false;
    currentPage_ = pendingPage_;
    hasMore_ = cat.hasMore;
    populate(cat, pendingAppend_);
}

void HomeView::populate(const MediaCatalog& cat, bool append)
{
    // If the user is typing in the search box (live search), rebuilding the view below can steal focus
    // (the carousel/grid grab it). Remember, and hand focus back at the end so typing isn't interrupted.
    const bool keepSearchFocus = search_ && search_->hasFocus();

    int from;
    if (!append)
    {
        ++generation_; // invalidate in-flight thumbnail loads from the previous view
        applyGridMode(/*recentList*/ false); // ensure the poster grid (recents may have left it in list mode)
        grid_->clear();
        items_.clear();
        settingsStore().sync(); // fresh resume positions for the progress bars
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
        // For the timed-media views, also offer streaming a direct link (routes to the inline URL form).
        if (kind == QStringLiteral("video") || kind == QStringLiteral("audio"))
        {
            MediaItem stream;
            stream.id = QStringLiteral("_stream");
            stream.type = QStringLiteral("_open"); // reuse the +-tile + requestOpenFile routing
            stream.title = tr("Stream from a link…");
            stream.url = QStringLiteral("stream");  // the kind handled by onRequestOpenFile
            items_.push_back(stream);
        }
        // On the Games console list, surface the local Steam library as a native "Steam" console.
        if (!stack_.isEmpty() && !stack_.last().detail && stack_.last().query.isEmpty()
            && stack_.last().catalogType == QStringLiteral("game")
            && SteamLibrary::isAvailable() && !SteamLibrary::installedGames().isEmpty())
        {
            MediaItem steam;
            steam.id = QStringLiteral("steam:console");
            steam.type = QStringLiteral("platform"); // a console (drills into its games)
            steam.title = tr("Steam");
            steam.expandable = true;
            steam.mime = QStringLiteral("steam:console"); // marker -> drilled natively, not via the addon
            items_.push_back(steam);
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
            // Type-based placeholder (+ resume bar if started); a real poster overwrites it in loadThumbnails().
            if (it.type != QStringLiteral("info"))
                w->setIcon(iconWithProgress(defaultIcon(it.type, kPoster).pixmap(kPoster), resumeKeyFor(it)));
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
    else if (xmbMode_)
    {
        fillXmbFromItems(from); // the active category's vertical column
    }
    // Returning via Back: select + scroll to the item we'd drilled into (the carousel/xmb already restore the
    // page-1 case via their fill funcs; this also pages further in when the item was loaded by infinite-scroll).
    maybeRestoreSelection();

    // Live search: give focus back to the search box if rebuilding the view took it (keep the cursor there).
    if (keepSearchFocus && search_ && !search_->hasFocus())
    {
        search_->setFocus(Qt::OtherFocusReason);
        search_->deselect();   // keep the caret at the end rather than selecting all the typed text
        searchEditing_ = true; // stay in type mode (FocusOut had flipped it off)
    }
}

// Scroll to / select the row we last drilled into (stack childRow). If it hasn't been loaded yet (it was on a
// later page), keep fetching pages until it is, then land on it. Bounded so a bad hasMore_ can't loop forever.
void HomeView::maybeRestoreSelection()
{
    if (stack_.isEmpty()) return;
    const int row = stack_.last().childRow;
    if (row < 0) { pendingRestoreRow_ = -1; return; }

    if (row < items_.size())
    {
        if (carouselMode_) { if (pendingRestoreRow_ >= 0) fillCarouselFromItems(0); } // rebuild to select the key
        else if (xmbMode_) { /* the XMB column restores its own position on rebuild */ }
        else if (grid_ && row < grid_->count())
        {
            grid_->setCurrentRow(row);
            grid_->scrollToItem(grid_->item(row), QAbstractItemView::PositionAtCenter);
        }
        pendingRestoreRow_ = -1;
        return;
    }
    if (hasMore_ && !loading_ && currentPage_ < 25) { pendingRestoreRow_ = row; loadMore(); } // page toward it
    else pendingRestoreRow_ = -1;                                                             // give up
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
        QString label = it.title;
        const double frac = resumeFraction(resumeKeyFor(it)); // "how far in" for a partly-played movie/episode
        if (frac >= 0.0) label += QStringLiteral("    ·  %1%").arg(int(frac * 100.0));
        entries.push_back({ QStringLiteral("item:") + QString::number(i), label, c, it.thumbnailUrl });
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
    if (!(search_ && search_->hasFocus())) carousel_->setFocus(Qt::OtherFocusReason); // keep typing during live search
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
    // Don't count the leading "open a file" / "stream a link" items as catalog results.
    int count = 0;
    for (const MediaItem& it : items_) if (it.type != QStringLiteral("_open")) ++count;
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
                w->setIcon(iconWithProgress(pm.scaled(kPoster, Qt::KeepAspectRatio, Qt::SmoothTransformation),
                                            resumeKeyFor(items_[i])));
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
        const QString itemUrl = resumeKeyFor(items_[i]); // stable key for the resume-progress overlay

        QNetworkRequest req((QUrl(url)));
        req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MyMediaVault"));
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        QNetworkReply* reply = nam_->get(req);
        ++thumbActive_;
        connect(reply, &QNetworkReply::finished, this, [this, reply, w, gen, itemUrl] {
            reply->deleteLater();
            --thumbActive_;
            if (gen == generation_ && reply->error() == QNetworkReply::NoError) // else navigated away / failed
            {
                QPixmap pm;
                if (pm.loadFromData(reply->readAll()))
                    w->setIcon(iconWithProgress(pm.scaled(kPoster, Qt::KeepAspectRatio, Qt::SmoothTransformation),
                                                itemUrl));
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
