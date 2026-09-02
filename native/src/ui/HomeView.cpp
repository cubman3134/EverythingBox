#include "HomeView.h"
#include "../core/AppBrand.h"
#include "../theme2/FormFactor.h"
#ifdef EB_HAVE_QML
#include "../theme2/ThemeEngine.h" // hasInstalledTheme(): the grid's poster pipeline is skipped under a themed UI
#endif
#include <QScrollArea>
#include <QScroller>
#include "FeedbackPolicy.h"   // kFeedbackShort/Long — feedback duration policy (J06/J07)
#include "../core/AppPaths.h"
#include "../core/MediaCategories.h"
#include "../core/ReadingForm.h"
#include "../core/Miximage.h"   // issue #90: composite the miximage card from cached art on the display path
#include "../core/HashVerify.h" // issue #97: DAT dump-verification badge on the game detail view
#include "../core/ArchiveRom.h" // #97: hash a zipped ROM's extracted stream, not the archive bytes
#include "../addons/AddonManager.h"
#include "../addons/GameMetaAggregator.h"
#include "../core/CatalogMatch.h"
#include "../core/RecentStore.h"
#include "../core/ResumeStore.h"   // issue #150: the resume key scheme + the tombstoned clear
#include "../core/DownloadsStore.h"
#include "../core/LocalLibrary.h"
#include "../core/PhotoLibrary.h"
#include "../core/BingeStore.h"
#include "../core/RaBrowse.h"
#include "../core/Achievements.h"
#include "../core/SteamAchievements.h"
#include "../core/PcGameStore.h"
#include "../core/PcGameRemap.h"   // the PC-game id migration, re-run on every PC Games refresh
#include "../addons/StremioTranslate.h" // kMaxDescribeChars — the picker's one-row budget
#include "../core/PlayStats.h"
#include "../core/ProfileStore.h"
#include "../core/ExternalPlayer.h"
#include "../core/FavoritesStore.h"
#include "../core/PlaylistStore.h"
#include "../core/Theme.h"
#include "../core/SystemCatalog.h"
#include "../core/NativePorts.h" // issue #233: the native-port catalog + the game binding
#include "../core/RecompRows.h"  // issue #248: the Recomps section's pure row/state model
#include "../core/EmulatorManager.h" // issue #248: is this port installed, and where (the install-state input)
#include "../core/RomLibrary.h"
#include "../core/SteamLibrary.h"
#include "../core/EpicLibrary.h"
#include "../core/GogLibrary.h"
#include "../core/BattleNetLibrary.h"
#include "../core/PcScanCache.h"   // #62: persist the last good installed-scan per launcher
#include "../core/Settings.h"
#include "../core/ItemMarks.h"
#include "../core/GameFilter.h"        // pure filter model/evaluator for saved-filter shelves (#63)
#include "../core/FilterPresetStore.h" // per-profile saved filter presets (#63)
#include "../core/TraktClient.h"   // calendarAvailable()/cachedCalendar() — the Trakt shelf's only gate (#23)
#include "../core/HomeRows.h"   // issue #161: the per-profile home row list + the pure planner
#include "CarouselView.h"
#include "XmbView.h"
#include <QHash>

#include <QApplication>
#include <QPaintEvent>
#include <QSet>
#include <QListWidget>
#include <QRandomGenerator>
#include <QSharedPointer>
#include "nav/NavOverlay.h"
#include "nav/Osk.h"
#include "../core/GamelistStore.h"
#include "../core/MetaCache.h"
#include "../core/MetaOverrides.h"
#include "../core/MissedDismiss.h"   // "You missed" (#25): the per-show dismissal watermarks the rule reads
#include "../core/PerfTrace.h"
#include "../browse/SyntheticCatalogs.h"
#include "../browse/MusicCatalogs.h"   // issue #74: the Artists/Albums/Tracks browse over the music index
#include "../browse/LeafRoute.h"       // the ONE table both this file's two Enter paths route a local leaf by
#include "../browse/RomhackTarget.h"   // the console a base-ROM crawl carries, from the system the verb was offered on
#include "../browse/RemoteLeafResolve.h" // a remote source's leaf: resolve by id, else by title+console
#include "../core/MusicLibrary.h"      // ...and the index those three builders render
#include "../browse/AudiobookCatalogs.h" // issue #139: the Authors/Narrators/Series browse over the books
#include "../core/AudiobookLibrary.h"    // ...and the index those builders render
#include "../core/MusicArt.h"            // keyedCover: the ONE picture rule, shared by albums and books
#include "../browse/BookCatalogs.h"     // issue #134: the Authors/Series browse over the reading library
#include "../core/BookLibrary.h"        // ...and the index those builders render
#include "../core/IptvSourceStore.h"   // Live TV sources (#75 inc 2)
#include "../core/LiveTvMigrate.h"     // #203: re-identify legacy livetv: rows when a channel list arrives
#include "../core/OpdsCatalogStore.h"  // OPDS book catalogs (#146)
#include "../core/SubsonicServerStore.h" // Subsonic music servers (#193)
#include "../core/Jellyfin.h"            // #160: the server-qualified id and the transport verdicts
#include "../core/JellyfinClient.h"      // #160: /System/Info/Public + the sign-in
#include "../core/JellyfinServerStore.h" // #160: the connected servers (tokens device-local)
#include "../core/SubsonicClient.h"      // ...their fetches, cache and MusicSupply (#193)
#include "../core/MusicId.h"             // issue #194: cross-source identity + the manual override
#include "../core/MusicMerge.h"          // ...and the merged view every music level renders
#include "../core/MusicRemap.h"          // ...and the one-way move that keeps what was banked (#194 inc 2)
#include "../core/JellyfinMusicClient.h"  // #194 inc 3: a Jellyfin server's music as a supplier
#include "../core/ServerMusic.h"          // #194 inc 3: the EverythingBox server's music shelf (ids, readers)
#include "../core/ServerMusicClient.h"    // ...and its fetches
#include "../ebook/OpdsFeed.h"         // parseOpds + opdsBasicAuth (#146)
#include "../core/NetHeaderApply.h"    // OPDS feed fetch: auth header + cross-origin drop on redirect (#146)
#include "../media/StreamResolver.h"   // parseM3u — turn a fetched playlist into channels (#75 inc 2)
#include "../core/XmltvGuide.h"        // XMLTV EPG parse + gunzip (#75 inc 3)
#include "../browse/LiveTvGuide.h"     // now/next-by-tvg-id + the guide-grid builder (#75 inc 3)
#include "../browse/SearchAggregator.h"
#include <QAbstractItemView>
#include <QMenu>
#include <QLineEdit>
#include <QLabel>
#include <QComboBox>
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
#include <QDateTime>
#include <QDir>
#include <QThreadPool>   // #97: background ROM hashing off the UI thread
#include <QFile>
#include <QStandardPaths>
#include <QDialog>
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
#include <memory>
#include <functional>

static QString retroSystemFor(const MediaItem& it, const QString& consoleName = QString());   // defined with the other item predicates below


static const QSize kPoster(140, 200);

// A chapter leaf of a serial work — one entry of something read in installments. Its detail page gets a
// "Read" button, and opening it asks the owning addon for the chapter's pages (#188).
//
// A TYPE SHAPE, not a list of providers and not a list of media types. "{family}_chapter" is the leaf type
// of a family that answers the `chapters` resource, which is how AddonManager::familyType reads it back;
// a source serving light novels as "novel_chapter" is readable here with no change to this file. Whether
// the addon can actually supply the pages is a separate question, asked of its manifest at open time —
// this predicate only says what KIND of thing was pressed. (Comic issues are metadata-only; they reach a
// file provider by a different route below.)
static bool isReadableChapter(const QString& t)
{
    static const QString kSuffix = QStringLiteral("_chapter");
    return t.size() > kSuffix.size() && t.endsWith(kSuffix);   // a bare "_chapter" names no family
}

// Per-profile settings store (shared ini); used here to read media resume progress.
static QSettings& settingsStore()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

// The GLOBAL "show hidden items" override (Settings ▸ General): when on, hidden marks are ignored so hidden
// items render everywhere they normally would. Off by default. Read live on every populate/render so a toggle
// takes effect on the next refresh (no cached copy to go stale).
static bool showHiddenItems()
{
    return settingsStore().value(QStringLiteral("library/showHidden"), false).toBool();
}

// A catalog item is dropped from the rows/search when it carries the per-profile `hidden` mark and the global
// Show-hidden override is off. Synthetic rows (folders/headers, type starting '_' or "rechdr") have no marks
// key and are never filtered — callers only pass real media items here.
static bool isHiddenItem(const MediaItem& it)
{
    return !showHiddenItems() && ItemMarks::get(MetaCache::keyFor(it)).hidden;
}

// Resume progress (0..1) for a played media path/url, or -1 if none. Mirrors MainWindow's key scheme
// (resume/<md5-of-path>/{pos,dur}); a bar shows only once both a position and a duration are known.
static double resumeFraction(const QString& url)
{
    if (url.isEmpty()) return -1.0;
    // Live TV / HLS streams have no fixed length, so "how far in" is meaningless - never show a progress
    // percentage or bar for them (the duration mpv reports for a live stream would be misleading).
    if (url.contains(QStringLiteral(".m3u8"), Qt::CaseInsensitive)) return -1.0;
    const QString k = ResumeStore::groupFor(url) + QStringLiteral("/"); // one spelling of resume/<hash>/
    const double pos = settingsStore().value(k + QStringLiteral("pos"), 0.0).toDouble();
    const double dur = settingsStore().value(k + QStringLiteral("dur"), 0.0).toDouble();
    if (pos <= 1.0 || dur <= 1.0) return -1.0;
    return qBound(0.0, pos / dur, 1.0);
}

// "45% watched  ·  1h 12m left" for a partly-played movie/episode, or "" when there is no resume position
// (never played, finished, or a live stream with no meaningful length). The percentage alone answered "how
// far in" on the row label; the panel is where "how much is left" belongs, so it reads the same pos/dur the
// resume seek uses rather than a second source of truth.
static QString resumeSummary(const QString& url)
{
    if (url.isEmpty() || url.contains(QStringLiteral(".m3u8"), Qt::CaseInsensitive)) return {};
    const QString k = ResumeStore::groupFor(url) + QStringLiteral("/");
    const double pos = settingsStore().value(k + QStringLiteral("pos"), 0.0).toDouble();
    const double dur = settingsStore().value(k + QStringLiteral("dur"), 0.0).toDouble();
    if (pos <= 1.0 || dur <= 1.0) return {};
    const int pct = qBound(0, int(qRound(pos / dur * 100.0)), 100);
    const qint64 left = qint64(qMax(0.0, dur - pos));
    return left >= 60
        ? QCoreApplication::translate("HomeView", "%1% watched  ·  %2 left")
              .arg(pct).arg(PlayStats::formatDuration(left))
        : QCoreApplication::translate("HomeView", "%1% watched").arg(pct);
}

// The key a played item's resume position is stored under: its stable addon id when it has one (a streamed
// URL changes every resolution), else its url/path. Matches MainWindow::openLibraryItem's resume keying, so a
// movie shows "continue watching" progress whether it's a catalog poster or a Recent row.
static QString resumeKeyFor(const MediaItem& it)
{
    return it.id.isEmpty() ? it.url : it.id;
}

// Forget the saved resume position (pos/dur/title) for a media key, so it starts from the beginning next time.
// Through ResumeStore, the same funnel PlaybackSession::finishResume uses: removing the group also records a
// dated TOMBSTONE (issue #150), without which a peer still holding the position — or this device's own copy in
// the cloud document — resurrects it at the next merge, because a removed group is indistinguishable from
// "never played". ResumeStore owns the resume/<md5-of-key> spelling this used to repeat.
static void clearResume(const QString& key)
{
    ResumeStore::clear(settingsStore(), key);
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

// HOW FAR THROUGH A ROW IS, whichever way it knows (issue #139 increment 2).
//
// Almost every row is answered by the resume store under its own stable key, which is what resumeFraction
// does and has always done. A row that carries its OWN fraction (MediaItem::progress — a local audiobook,
// whose position is a sum over its parts' marks and is filed under none of them) is believed instead, because
// it is the only one that can know. The default is -1, so this is the old lookup for every other row.
static double rowFraction(const MediaItem& it)
{
    return it.progress >= 0.0 ? qBound(0.0, it.progress, 1.0) : resumeFraction(resumeKeyFor(it));
}

// Overlay a "continue watching" progress bar along the bottom of a poster pixmap (in place). Takes the
// FRACTION rather than a key, so the one caller whose row knows its own (rowFraction above) paints through
// exactly the same code as every caller whose row does not.
static QIcon iconWithProgress(QPixmap pm, double frac)
{
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

// Types that open a full info/detail page (a metadata header + a Play/Read button) rather than playing
// straight away. The themed home surfaces this page (it renders on the classic HomeView).
static bool isInfoPageType(const QString& t)
{
    return t == QStringLiteral("movie")  || t == QStringLiteral("series") || t == QStringLiteral("tv")
        || t == QStringLiteral("episode")|| t == QStringLiteral("comic")  || t == QStringLiteral("manga")
        || t == QStringLiteral("book")   || t == QStringLiteral("audiobook");
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

// Group key for a Recent entry: by media type, and per-console for games ("game:<console>").
static QString recentGroupKey(const RecentItem& r)
{
    // One "Games" group at the top level — don't split by console here; the per-console split only happens
    // when you drill into a specific console.
    if (r.kind == QStringLiteral("game") || r.kind == QStringLiteral("pcgame")) return QStringLiteral("game");
    return r.kind; // video / audio / document
}

static QString recentGroupLabel(const QString& key)
{
    if (key == QStringLiteral("video"))    return QObject::tr("Videos");
    if (key == QStringLiteral("audio"))    return QObject::tr("Audio");
    if (key == QStringLiteral("document")) return QObject::tr("Books");
    if (key == QStringLiteral("game"))     return QObject::tr("Games");
    if (key.startsWith(QStringLiteral("game:"))) return key.mid(5); // the console name (per-console views)
    return key;
}


HomeView::HomeView(AddonManager* mgr, QWidget* parent) : QWidget(parent), mgr_(mgr)
{
    nam_ = new QNetworkAccessManager(this);

    // The last calendar TraktClient cached, read ONCE here so an offline launch already has something to
    // draw before (or instead of) any fetch. Skipped entirely when Trakt is off, so an install that never
    // linked an account does not so much as open the cache key. MainWindow refreshes it later.
    if (TraktClient::calendarAvailable()) traktCal_ = TraktClient::cachedCalendar();
    if (TraktClient::calendarAvailable())
    {
        traktWatchlist_  = TraktClient::cachedWatchlist();
        traktCollection_ = TraktClient::cachedCollection();
    }

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
    if (FormFactor::instance().mode() == FormFactor::Mode::Mobile)
    {
        // A phone width can't seat every media-type tab beside the rest of the chrome — QHBoxLayout
        // would clip them mid-text. Pan the tabs in a horizontal scroller instead (touch-draggable;
        // widgetResizable never sizes the host below its minimum, so nothing clips).
        auto* scroll = new QScrollArea(this);
        scroll->setObjectName(QStringLiteral("typeScroll"));
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidgetResizable(true);
        scroll->setWidget(typeHost_);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        // No fixed height: stretch with the bar exactly like the bare typeHost_ does, so the tabs sit
        // at the same vertical position as the rest of the chrome (a fixed 34px viewport clipped them).
        scroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        QScroller::grabGesture(scroll->viewport(), QScroller::LeftMouseButtonGesture);
        topRow->addWidget(scroll, 1);
    }
    else
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
        // The console/platform we drilled in from (if any). Stamped onto a resolved game so the launcher can
        // pick the right emulator even when the file extension is shared (e.g. a PSP .iso vs a GameCube .iso).
        QString console;
        if (stack_.size() >= 2 && stack_.at(stack_.size() - 2).item.type == QStringLiteral("platform"))
            console = stack_.at(stack_.size() - 2).item.title.trimmed();
        // The volume/series we drilled in from (used to build a comic issue's bridge query).
        const QString parentTitle = (stack_.size() >= 2) ? stack_.at(stack_.size() - 2).item.title.trimmed()
                                                         : QString();
        resolvePlay(top.addon, it, parentTitle, console, playImdbId_, playStremioType_);
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
        else if (top.item.id.startsWith(QStringLiteral("pcgame:")))
        {
            // A merged PC game stars through the SAME builder the game action menu uses, so it is stamped
            // system = "pc" and lands in the PC console's ★ Favorites folder. Starring the identical game from
            // its info page and from its row must not produce two differently-shaped records.
            FavoritesStore::add(browse::localGameFavorite(top.item, QStringLiteral("pc")));
            favBtn_->setText(tr("★ Favorited"));
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

    // "Choose source…": the manual release picker, offered beside Play on any leaf that resolves through the
    // Stremio stream add-ons. Shown from requestMeta (a Stremio leaf) and showMeta (a bridged one), the same
    // two places Play itself is revealed from, so the two can't disagree about what is resolvable.
    sourceBtn_ = new QPushButton(tr("🔀  Choose source…"), actionRow_);
    sourceBtn_->setCursor(Qt::PointingHandCursor);
    sourceBtn_->setStyleSheet(QStringLiteral(
        "QPushButton{background:#EDE4FF;border:2px solid #7C5CFF;border-radius:6px;"
        "padding:6px 14px;color:#3A2A7A;font-weight:bold;}"
        "QPushButton:hover{background:#DCCEFF;}"
        "QPushButton:focus{background:#C9B4FF;border-color:#5A3ED6;}"));
    sourceBtn_->setVisible(false);
    connect(sourceBtn_, &QPushButton::clicked, this, [this] {
        if (stack_.isEmpty() || !stack_.last().detail) return;
        MediaItem it = stack_.last().item;
        // A bridged leaf got its stream id from /meta (showMeta armed playImdbId_) — carry it, exactly as
        // resolvePlay's own callback stamps it onto the item it opens.
        if (it.imdbStreamId.isEmpty() && !playImdbId_.isEmpty()) it.imdbStreamId = playImdbId_;
        emit chooseSourceRequested(it);
    });
    sourceBtn_->installEventFilter(this);
    arl->addWidget(sourceBtn_);

    // "Romhacks…": what hacks exist for this game. Only on a retro game leaf — a hack is a patch for a
    // specific ROM, so it means nothing on a film, and nothing on a PC game either.
    romhackBtn_ = new QPushButton(tr("🧩  Romhacks…"), actionRow_);
    romhackBtn_->setCursor(Qt::PointingHandCursor);
    romhackBtn_->setStyleSheet(QStringLiteral(
        "QPushButton{background:#E4F5E8;border:2px solid #3FA35C;border-radius:6px;"
        "padding:6px 14px;color:#1E5B33;font-weight:bold;}"
        "QPushButton:hover{background:#CFEBD8;}"
        "QPushButton:focus{background:#B4E0C3;border-color:#2E7F46;}"));
    romhackBtn_->setVisible(false);
    connect(romhackBtn_, &QPushButton::clicked, this, [this] {
        if (stack_.isEmpty() || !stack_.last().detail) return;
        const MediaItem it = stack_.last().item;
        const QString sys = retroSystemFor(it, browseConsoleName());
        if (sys.isEmpty()) return;            // the gate below should have hidden the button already
        noteRomhackTarget(it, stack_.last().addon, sys);
        emit romhacksRequested(it, sys);
    });
    romhackBtn_->installEventFilter(this);
    arl->addWidget(romhackBtn_);

    // "Fix this entry…": the PC-game merge override (issue #44), on the page of the entry it is about. A
    // wrongly merged tile is only identifiable while you are looking at it — one "Prey" with two Steam
    // copies on it — so the correction belongs beside Play, not in a settings screen the user would have to
    // remember the problem to go and find.
    pcFixBtn_ = new QPushButton(tr("⚙  Fix this entry…"), actionRow_);
    pcFixBtn_->setCursor(Qt::PointingHandCursor);
    pcFixBtn_->setStyleSheet(QStringLiteral(
        "QPushButton{background:#E7EBF2;border:2px solid #8A97AD;border-radius:6px;"
        "padding:6px 14px;color:#33405A;font-weight:bold;}"
        "QPushButton:hover{background:#D6DCE7;}"
        "QPushButton:focus{background:#C3CCDC;border-color:#5D6B85;}"));
    pcFixBtn_->setVisible(false);   // revealed in requestMeta() for a merged PC game
    connect(pcFixBtn_, &QPushButton::clicked, this, [this] {
        if (stack_.isEmpty() || !stack_.last().detail) return;
        const MediaItem it = stack_.last().item;   // copy: refreshAfterPcMergeFix pops this level
        if (fixPcGameEntry(it)) refreshAfterPcMergeFix();
    });
    pcFixBtn_->installEventFilter(this);   // Backspace here = Back, like every other action button
    arl->addWidget(pcFixBtn_);
    // "Fix info…": the per-item metadata editor (issue #24), the classic twin of the themed detail's editmeta
    // pill. It belongs where the wrong data is visible, so it sits on the detail card rather than in settings.
    // Always shown on a real detail (unlike Play/Download it needs no resolvable source — a mis-scrape is
    // exactly as wrong on an item you cannot play), which also makes it the one action that is always here for
    // the arrow ring to land on.
    editMetaBtn_ = new QPushButton(tr("✎  Fix info…"), actionRow_);
    editMetaBtn_->setCursor(Qt::PointingHandCursor);
    editMetaBtn_->setStyleSheet(QStringLiteral(
        "QPushButton{background:#E7EBF2;border:2px solid #8C9AB4;border-radius:6px;"
        "padding:6px 14px;color:#33405A;font-weight:bold;}"
        "QPushButton:hover{background:#D6DDE8;}"
        "QPushButton:focus{background:#C3CCDC;border-color:#5A6B8C;}"));
    editMetaBtn_->setVisible(false);
    connect(editMetaBtn_, &QPushButton::clicked, this, [this] {
        if (stack_.isEmpty() || !stack_.last().detail) return;
        const QString key = MetaCache::keyFor(stack_.last().item);
        if (!key.isEmpty()) emit editMetadataRequested(key, detailScrapedValues());
    });
    editMetaBtn_->installEventFilter(this);
    arl->addWidget(editMetaBtn_);

    // "📖 Manual": the scraped game manual (issue #89). Shown only for a game whose bundle carries a manual
    // role (or that already has one on disk). The PDF is megabytes, so it is NOT prefetched — clicking fetches
    // it on demand (progress shown in the label) and then opens it in the reader we already ship (PdfView, via
    // the shared openItem path, so per-file page resume works exactly as mid-playthrough).
    manualBtn_ = new QPushButton(tr("📖  Manual"), actionRow_);
    manualBtn_->setCursor(Qt::PointingHandCursor);
    manualBtn_->setStyleSheet(QStringLiteral(
        "QPushButton{background:#E4F0FF;border:2px solid #4C8BFF;border-radius:6px;"
        "padding:6px 14px;color:#1A3A7A;font-weight:bold;}"
        "QPushButton:hover{background:#CFE2FF;}"
        "QPushButton:focus{background:#B4D2FF;border-color:#2E5BC9;}"));
    manualBtn_->setVisible(false); // revealed by refreshManualButton() for a game with a manual
    connect(manualBtn_, &QPushButton::clicked, this, [this] {
        if (stack_.isEmpty() || !stack_.last().detail) return;
        const MediaItem it = stack_.last().item;
        const QString key = MetaCache::keyFor(it);
        if (!key.isEmpty()) openManualFor(key, it.title);
    });
    manualBtn_->installEventFilter(this); // Backspace here = Back, like every other action button
    arl->addWidget(manualBtn_);

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
    // Filter bar: a row of per-catalog dropdowns (Genre/Year/Rating/Sort) above the grid. Built dynamically
    // from each catalog's advertised filters; hidden when a catalog has none (or in carousel/XMB layouts).
    filterBar_ = new QWidget(this);
    filterLayout_ = new QHBoxLayout(filterBar_);
    filterLayout_->setContentsMargins(12, 2, 12, 6);
    filterLayout_->setSpacing(8);
    filterBar_->setVisible(false);
    v->addWidget(filterBar_);

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

    // Play/Read progress + errors are shown as a window-level overlay (see MainWindow::notify), not a child
    // of this view: a themed home is a native QQuickView our own widgets can't paint over. showToast/hideToast
    // just relay to MainWindow via toastRequested/toastHideRequested so the notice floats over any theme.

    connect(mgr_, &AddonManager::catalogReady, this, &HomeView::onCatalogReady);
    connect(mgr_, &AddonManager::metaReady, this, &HomeView::onMetaReady);
    connect(mgr_, &AddonManager::sourcesChanged, this, &HomeView::refresh); // a remote addon was added/removed
    // ...and the music-shelf list rides the SAME signal (#194 increment 3). Kept off the browse path on
    // purpose: the Settings row that lists the sources to prefer, and MusicSupply::playUrl resolving a shelf
    // track for a Continue-listening row, both have to work before anybody has opened the Music tab.
    connect(mgr_, &AddonManager::sourcesChanged, this, &HomeView::refreshMusicShelves);
    refreshMusicShelves();

    // Cross-addon "search everything" fan-out. The aggregator owns the request lifecycle + merge rules; HomeView
    // paints each streamed batch into the "_search" grid and mirrors the loading/no-results UI (byte-for-byte
    // the old in-handler search branch, just split across two signals).
    agg_ = new SearchAggregator(mgr_, this);
    connect(agg_, &SearchAggregator::resultsAppended, this, [this](const MediaCatalog& add, bool /*firstBatch*/) {
        if (!perfSearchFirstSeen_)
        { perfSearchFirstSeen_ = true; PerfTrace::end(QStringLiteral("search.first"), QStringLiteral("n=%1").arg(add.items.size())); }
        if (!stack_.isEmpty() && stack_.last().item.type == QStringLiteral("_search") && !add.items.isEmpty())
            populate(add, /*append*/ !items_.isEmpty());
        updateStatus();
    });
    connect(agg_, &SearchAggregator::finished, this, [this](int totalResults) {
        PerfTrace::end(QStringLiteral("search.drain"), QStringLiteral("total=%1").arg(totalResults));
        loading_ = false;
        if (totalResults == 0) showToast(tr("No results for “%1”.").arg(agg_->query()), kFeedbackLong);
        updateStatus();
    });

    refresh();
}

void HomeView::refresh()
{
    themedArtCache_.clear(); // a full rebuild -> drop the per-session page cache of resolved panel art
    g_theme = ThemeStore::current(); // the active profile's colour theme
    applyThemeFont();
    // A theme with a dark background image (low dim) wants a dark, light-text detail card so it stays
    // readable AND fits the theme; light themes get a light, dark-text card.
    styleMetaPanel(g_theme.dark || (!g_theme.background.isEmpty() && g_theme.backgroundDim < 0.30));

    // Reflect the active profile in the top-bar button: the avatar as an icon + the name as text.
    if (profileBtn_)
    {
        const Profile me = ProfileStore::current();
        profileBtn_->setIcon(avatarIcon(me.icon, me.name, 26));
        profileBtn_->setIconSize(QSize(26, 26));
        // The top bar shares one row with Back/search/Settings, and QPushButton's minimum size hint
        // enforces its full icon+text width — a long name overflows the bar and gets clipped. Mobile:
        // avatar only (the standard phone pattern). Desktop/TV: elide the name to a sane cap. The full
        // name always survives in the tooltip.
        const QString name = me.name.isEmpty() ? tr("Profile") : me.name;
        if (FormFactor::instance().mode() == FormFactor::Mode::Mobile)
            profileBtn_->setText(QString());
        else
            profileBtn_->setText(QFontMetrics(profileBtn_->font()).elidedText(name, Qt::ElideRight, 160));
        profileBtn_->setToolTip(name);
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
    // A catalog that can only ever answer with an explanation instead of items. TWO causes, and both must be
    // treated alike everywhere below or the "usable-first" rule only holds for one of them:
    //   * the catalog REQUIRES an extra we have no value for (a Stremio Unsatisfiable catalog -> skipReason);
    //   * the whole ADD-ON says it must be configured first, so every catalog it declares answers with
    //     "needs to be configured" (LoadedAddon::stremioManifest.configurationRequired — nothing writes a
    //     skipReason for this case, so testing skipReason alone silently missed it).
    // dispatchRemoteCatalog answers both locally with the same synthetic info row; this is the browse side.
    auto isSelfExplaining = [](LoadedAddon* a, const AddonCatalog& c) {
        return !c.skipReason.isEmpty() || (a->stremio && a->stremioManifest.configurationRequired);
    };
    struct CatRef { LoadedAddon* addon; AddonCatalog cat; bool selfExplaining; };
    QVector<CatRef> all;
    for (LoadedAddon* s : mgr_->sources())
    {
        if (!mgr_->isEnabled(s->manifest.id)) continue;
        // searchOnly catalogs can't answer a bare landing request (they REQUIRE a search term), so they get
        // no tab — they stay listed for the search fan-out, which is the only surface that can use them.
        // A `bios:` catalog (the file provider's BIOS index — Kind "game") is machinery for the BIOS fetcher,
        // never a browsable shelf: keep it off the home screen.
        for (const AddonCatalog& c : mgr_->catalogs(s))
            if (!c.searchOnly && !c.id.startsWith(QStringLiteral("bios:")))
                all.push_back({ s, c, isSelfExplaining(s, c) });
    }
    // Best source score available per media type. A self-explaining catalog is EXCLUDED from this (exactly as
    // searchOnly ones are excluded from `all`) while still competing in the election below. sourceScore is
    // per-ADDON, not per-catalog, so counting one here would raise the bar for its whole media type and knock
    // every file-provider catalog of that type out of wins() — a catalog that can never be fetched would then
    // eliminate a working shelf before either election pass ran (and, for a type outside movie/series, the
    // working shelf would be dropped outright rather than merely out-ranked).
    QHash<QString, int> bestScore;
    for (const CatRef& c : all)
        if (!c.selfExplaining)
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
    //
    // A self-explaining catalog can never be FETCHED (see isSelfExplaining above); opening it shows the reason
    // instead of items. It is still offered — that is the only way its reason reaches anyone — but it must
    // never DISPLACE a working shelf. Movies and TV get exactly one tab each, so each is elected in two
    // passes: a usable catalog first, and the self-explaining one only when the type has nothing usable at all
    // (an explained tab beats a type that silently isn't there). Every other type adds a tab per winning
    // catalog below, so there it is an addition, never a swap.
    bool didMovie = false, didSeries = false;
    for (const CatRef& c : all)
        if (wins(c) && c.cat.type == QStringLiteral("movie") && !c.selfExplaining && !didMovie)
        { addCat(c.addon, c.cat, tr("Movies")); didMovie = true; }
    for (const CatRef& c : all)
        if (wins(c) && c.cat.type == QStringLiteral("movie") && !didMovie)
        { addCat(c.addon, c.cat, tr("Movies")); didMovie = true; }
    for (const CatRef& c : all)
        if (wins(c) && isSeriesType(c.cat.type) && !c.selfExplaining && !didSeries)
        { addCat(c.addon, c.cat, tr("TV")); didSeries = true; }
    for (const CatRef& c : all)
        if (wins(c) && isSeriesType(c.cat.type) && !didSeries)
        { addCat(c.addon, c.cat, tr("TV")); didSeries = true; }
    QSet<QString> explainedTypes; // one "here is why this can't work" tab per type, not one per catalog
    for (const CatRef& c : all)
    {
        if (!wins(c)) continue;
        if (c.cat.type == QStringLiteral("movie") || isSeriesType(c.cat.type)) continue; // already led with Movies/TV
        // Several Unsatisfiable catalogs of the same type say the same thing to the same effect; a strip of
        // identical "can't work" entries is noise. The FIRST one carries the reason for that type.
        if (c.selfExplaining)
        {
            if (explainedTypes.contains(c.cat.type)) continue;
            explainedTypes.insert(c.cat.type);
        }
        addCat(c.addon, c.cat, c.cat.name);
    }

    // The Photos category (#102) — the browse half of the photo feature. Offered ONLY when the configured
    // photo library actually has images, exactly as the reading/game categories only appear with content: an
    // install with no photosFolder images gets no Photos tab (hasImages stops at the first image, so this is
    // cheap even on a large library). Activating it shows browse::photosCatalog via selectPhotos.
    if (PhotoLibrary::hasImages(PhotoLibrary::root()))
    {
        auto* photosBtn = new QPushButton(tr("Photos"), this);
        connect(photosBtn, &QPushButton::clicked, this, &HomeView::selectPhotos);
        makeTab(photosBtn, QStringLiteral("photos"), QStringLiteral("photo"));
        navTargets_.push_back({ QStringLiteral("photos"), false, nullptr, QString(),
                                QStringLiteral("photo"), tr("Photos"), true });
    }

    // The Music category (#74) — the browse half of the local MUSIC library, and the increment that makes
    // pointing the app at a music folder produce artists and albums rather than a file list. Offered on the
    // same rule the Photos tab uses, with one deliberate difference: it appears as soon as the configured
    // root EXISTS (MusicLibrary::hasLibrary), not only once tracks have been found. A scan is asynchronous
    // and a folder can turn out to hold nothing, and both of those need somewhere to SAY so — a tab that
    // silently fails to appear is the worst of the three outcomes. selectMusic renders the explanation.
    // ...OR a Subsonic music server is configured (#193, increment 5). A user whose whole library is a
    // Navidrome box has no local music folder and never will, so gating the category on the LOCAL supplier
    // alone would make them point the app at a folder they do not have in order to reach a server they have
    // already set up - which is the feature not shipping. It also contradicts what the issue asks for:
    // "the same UI with different suppliers" is a statement about suppliers being interchangeable, and a
    // gate only one of them can open is not that.
    //
    // hasServers() is one settings read and a small JSON parse - see SubsonicServerStore.h. It must stay
    // that cheap: this runs on every home refresh, and a gate that reached a server to decide whether to
    // draw a tab would make the home screen wait on a box that may be switched off.
    if (MusicLibrary::hasLibrary() || SubsonicServerStore::hasServers())
    {
        auto* musicBtn = new QPushButton(tr("Music"), this);
        connect(musicBtn, &QPushButton::clicked, this, &HomeView::selectMusic);
        // Type "album": core::mediaCategory already files album/track under the "audio" category, and it is
        // what gives the tab and its tiles the music colour and the note glyph.
        makeTab(musicBtn, QStringLiteral("music"), QStringLiteral("album"));
        navTargets_.push_back({ QStringLiteral("music"), false, nullptr, QString(),
                                QStringLiteral("album"), tr("Music"), false, true });
    }

    // The Audiobooks category (#139) — the browse half of the local audiobook library. Same rule as Music
    // and for the same reasons: it appears as soon as the configured root EXISTS, not only once books have
    // been found, because the scan is asynchronous and "that folder has nothing in it" needs somewhere to
    // SAY so. A music-only install has no audiobook root — the default <data>/audiobooks is never created by
    // anything — so it gets no tab at all, which is #139's compatibility requirement made structural.
    //
    // Type "audiobook": core::mediaCategory already files it under "audio", so on the themed layouts this
    // lands in the Audio bucket beside Music rather than inventing a category.
    if (AudiobookLibrary::hasLibrary())
    {
        auto* booksBtn = new QPushButton(tr("Audiobooks"), this);
        connect(booksBtn, &QPushButton::clicked, this, &HomeView::selectAudiobooks);
        makeTab(booksBtn, QStringLiteral("audiobooks"), QStringLiteral("audiobook"));
        navTargets_.push_back({ QStringLiteral("audiobooks"), false, nullptr, QString(),
                                QStringLiteral("audiobook"), tr("Audiobooks"), false, false, true });
    }

    // The Books category (#134) — the browse half of the local reading library. Same rule as Music and
    // Audiobooks, for the same reasons: it appears as soon as the configured root EXISTS, not only once
    // books have been found, because the scan is asynchronous and "that folder has nothing in it" needs
    // somewhere to SAY so. An install that has never pointed this anywhere has no reading root — the default
    // <data>/books is never created by anything — so it gets no tab at all, which is #134's compatibility
    // requirement made structural.
    //
    // Type "book": core::mediaCategory already files it under "reading", so on the themed layouts this lands
    // in the Reading bucket beside whatever book catalog an addon supplies rather than inventing a category.
    // The LABEL says "My Books" rather than "Books" because an OPDS or store addon's own catalog is very
    // often called exactly that, and two tabs with one name is a worse problem than a slightly longer one.
    if (BookLibrary::hasLibrary())
    {
        auto* readBtn = new QPushButton(tr("My Books"), this);
        connect(readBtn, &QPushButton::clicked, this, &HomeView::selectBooks);
        makeTab(readBtn, QStringLiteral("books"), QStringLiteral("book"));
        navTargets_.push_back({ QStringLiteral("books"), false, nullptr, QString(),
                                QStringLiteral("book"), tr("My Books"), false, false, false, true });
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
        preCorrection_.clear(); // the pre-correction stash is per rendered page, like items_ itself
        status_->setText(tr("No catalog addons installed. Open the Library to install one."));
        updateChrome();
    }

    // Put keyboard focus on the active tab so arrow-key navigation works immediately.
    if (activeTypeButton_) takeFocus(activeTypeButton_);
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
    takeFocus(carousel_);
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
    takeFocus(xmb_);
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
            if (t.isHome)       selectRecent();  // Home -> the recents list / XMB column
            else if (t.photos)  selectPhotos();  // Photos (#102) -> the synthetic photo browser
            else if (t.music)   selectMusic();   // Music  (#74)  -> the synthetic Artists/Albums browser
            else if (t.audiobooks) selectAudiobooks();   // Audiobooks (#139) -> the synthetic book browser
            else if (t.books)   selectBooks();   // My Books (#134) -> the synthetic reading browser
            else                selectType(t.addon, t.catalogId, t.type, t.name); // catalog -> item view
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
        const double frac = rowFraction(it); // "how far in" for a partly-played movie/episode/audiobook
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
    takeFocus(xmb_);
}

void HomeView::applyTheme()
{
    refresh(); // re-reads the theme: font, type-icon registry, colours, background - and re-styles the view
}

// Give `w` the keyboard — but ONLY while this view is the page on screen.
//
// In themed mode the classic HomeView is never shown, yet it stays fully alive: it is the data engine the
// themed home/browse run on (activateNav / browseActivate / systemItems all land here), so its own focus
// calls keep firing from refresh() and from the async populate/fill paths while a QQuickWidget owns the
// screen. Those calls used to reach across and take the keyboard off the visible themed page: Qt only skips
// the app-wide focus assignment when the whole WINDOW is inactive — a hidden widget inside an active window
// becomes QApplication::focusWidget() perfectly happily. The themed QQuickWidget then gets a focus-out, its
// QML scene loses its active-focus item, and every key routed to it afterwards is silently dropped: arrows,
// Enter and Back all stop working with nothing on screen to explain why (issue: themed home inert to keys).
// Hidden ⇒ no focus. isHidden() (not isVisible()) is the gate on purpose: a stacked page that is NOT current
// is explicitly hidden, while the current page reads as not-hidden even before the window is first shown —
// so the classic home still takes focus on a classic-mode startup, exactly as it always did.
void HomeView::takeFocus(QWidget* w)
{
    if (!w || isHidden()) return;
    w->setFocus(Qt::OtherFocusReason);
}

void HomeView::focusContent()
{
    searchEditing_ = false; // leaving the chrome row -> the search box is no longer in edit mode
    if (xmbMode_ && xmb_ && xmb_->isVisible())
        takeFocus(xmb_);
    else if (carouselMode_ && carousel_ && carousel_->isVisible())
        takeFocus(carousel_);
    else if (grid_->isVisible() && grid_->count() > 0)
        takeFocus(grid_);
    else if (meta_ && meta_->isVisible() && detailActionButton())
        takeFocus(detailActionButton()); // a leaf detail page -> its action button
    else if (activeTypeButton_)
        takeFocus(activeTypeButton_);
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
    const bool dark = g_theme.dark;
    const QColor tint = dark ? QColor(0x10, 0x13, 0x1A) : lightTint(activeColor); // dark surface, or a light tint
    const bool hasBg = !g_theme.background.isEmpty();
    const QString gridBg = hasBg
        ? QString("rgba(%1,%2,%3,170)").arg(tint.red()).arg(tint.green()).arg(tint.blue())
        : tint.name();
    grid_->setStyleSheet(QString(
        "QListWidget{background:%1;color:%2;border:none;}"
        "QListWidget::item:selected{background:%3;color:white;}")
        .arg(gridBg, dark ? QStringLiteral("#e6e9ef") : QStringLiteral("#1b1b1b"),
             dark ? QColor(0x32, 0x3A, 0x48).name() : activeColor.name()));

    // The chrome buttons (Back, profile, Settings) take the accent colour; the search box stays light.
    themeColor_ = activeColor;
    const QString cb = chromeButtonStyle(activeColor);
    if (back_)        back_->setStyleSheet(backButtonStyle(activeColor)); // always matches the bar background
    if (profileBtn_)  profileBtn_->setStyleSheet(cb);
    if (settingsBtn_) settingsBtn_->setStyleSheet(cb);
    if (search_)      search_->setStyleSheet(chromeEditStyle(activeColor, g_theme.cornerRadius));
    if (status_)      status_->setStyleSheet(dark ? QStringLiteral("color:#aeb4c2;") : QStringLiteral("color:#2a2c30;"));
    // Back the whole top row + the tabs' empty stretch with the accent so no seam shows the light tint.
    if (topBar_)      topBar_->setStyleSheet(QString("#topBar{background:%1;}").arg(activeColor.name()));
    if (typeHost_)    typeHost_->setStyleSheet(QString("#typeHost{background:%1;}").arg(activeColor.name()));

    // Set the palette so child text matches the surface (dark text on a light theme, light text on the dark one).
    QPalette pal = palette();
    pal.setColor(QPalette::Window, tint);
    pal.setColor(QPalette::Base, dark ? QColor(0x16, 0x1A, 0x22) : QColor(0xfb, 0xfb, 0xfd));
    pal.setColor(QPalette::WindowText, dark ? QColor(0xe6, 0xe9, 0xef) : QColor(0x22, 0x24, 0x28));
    pal.setColor(QPalette::Text, dark ? QColor(0xe6, 0xe9, 0xef) : QColor(0x1b, 0x1b, 0x1b));
    pal.setColor(QPalette::Highlight, dark ? QColor(0x32, 0x3A, 0x48) : activeColor);
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
    // Last resort: "Fix info…" is shown on every real detail card, so on a page where neither Play nor
    // Favorite is offered it is the only action there — and without this the D-pad would land on nothing
    // (the #40/#47 shape of bug).
    if (editMetaBtn_ && editMetaBtn_->isVisible()) return editMetaBtn_;
    return nullptr;
}

void HomeView::focusTypeButton(int idx)
{
    if (typeButtons_.isEmpty()) return;
    idx = qBound(0, idx, typeButtons_.size() - 1);
    QPushButton* b = typeButtons_[idx];
    takeFocus(b);
    b->click(); // activate -> load that catalog (also recolours the tab + background)
}

void HomeView::focusGridTop()
{
    takeFocus(grid_);
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
    takeFocus(target);
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
    takeFocus(row[j]);
}

// Up from the top of a content column: on a container detail page (meta header + child column) land on the
// Favorite button first; otherwise go straight to the top chrome.
void HomeView::focusUpFromColumn()
{
    if (meta_ && meta_->isVisible() && detailActionButton())
        takeFocus(detailActionButton());
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
    if (agg_) agg_->cancel(); // J17: switching to a catalog abandons any in-flight cross-addon search
    Level lvl;
    lvl.addon = addon; lvl.detail = false; lvl.catalogId = catalogId; lvl.catalogType = type; lvl.title = name;
    stack_.push_back(lvl);
    loadTop();
}

// The media-type catalogs (one tab each) as data for the themed "system view". Each row's navKey opens that
// catalog via activateNav(). Colours match the tabs (typeColor).
// Classify a catalog/media type into one of the four inherent top-level categories. Unknown types fall back
// to Video (the most common media kind), so a new addon type still lands somewhere sensible.
// Delegates to the core oracle (core/MediaCategories.h) so PlaylistStore's category migration + probe_playlists
// pin the exact same type->bucket mapping this UI uses. Keep this a thin forwarder — never fork the rules here.
QString HomeView::mediaCategory(const QString& type)
{
    return core::mediaCategory(type);
}

// Static metadata for a bucket key: display name, accent colour, and the glyph the XMB draws.
static QVariantMap categoryMeta(const QString& key)
{
    struct M { const char* name; const char* color; };
    static const QHash<QString, M> meta = {
        { QStringLiteral("video"),   { "Video",   "#C0392B" } },
        { QStringLiteral("audio"),   { "Audio",   "#8A5CC8" } },
        { QStringLiteral("game"),    { "Games",   "#3FA95E" } },
        { QStringLiteral("reading"), { "Reading", "#C9972E" } },
        // Photos (#102). core::mediaCategory has filed type "photo" under its own "photos" bucket since that
        // issue, but this table and the order below never learned the key — so the Photos tab existed in the
        // classic grid and had NO themed category at all, on the layout this app is used through. Missing
        // here it would also have drawn the fallback GEAR, because Xmb.qml's glyph painter ends in the
        // settings arm; the twin arm added there is what makes this a picture.
        { QStringLiteral("photos"),  { "Photos",  "#2E86AB" } },
    };
    const M m = meta.value(key, { "Other", "#6A6E78" });
    return QVariantMap{ { QStringLiteral("title"), QString::fromLatin1(m.name) },
                        { QStringLiteral("key"), key },
                        { QStringLiteral("glyph"), key },
                        { QStringLiteral("accent"), QString::fromLatin1(m.color) } };
}

// The buckets that actually have a catalog, in a fixed friendly order (skips Home).
// Every row this device could put on a home, in the app's default order (issue #161). See HomeView.h for why
// it spans both layouts' families at once.
//
// Built from navTargets_ and the stores DIRECTLY, never from categoryItems()/systemItems(): those two now run
// the row list themselves, so asking them would hand the editor a catalogue with the hidden rows already
// missing — and "Add row…" would then be unable to offer back the one row the user just hid.
QVector<HomeView::HomeRowChoice> HomeView::homeRowCatalogue()
{
    if (navTargets_.isEmpty()) refresh();
    QVector<HomeRowChoice> out;

    // The classic home's built-in shelves, in the order it produces them.
    out.push_back({ QStringLiteral("continue"), tr("Continue watching"), true });
    out.push_back({ QStringLiteral("trakt:missed"), tr("You Missed"), true });
    out.push_back({ QStringLiteral("trakt:calendar"), tr("Airing Soon"), true });
    out.push_back({ QStringLiteral("favorites"), tr("★ Favorites"), true });
    // ...and the opt-in ones, offered whether or not they currently hold anything: this is the ADD list, and a
    // producer that is empty today is exactly the row a user wants to place before it fills up.
    out.push_back({ QStringLiteral("downloads"), tr("⬇ Downloaded"), true });
    for (const QString& cat : { QStringLiteral("video"), QStringLiteral("audio"),
                                QStringLiteral("game"), QStringLiteral("reading") })
        for (const Playlist& p : PlaylistStore::forCategory(cat))
            out.push_back({ QStringLiteral("playlist:") + p.id, tr("Playlist: %1").arg(p.name), true });
    for (const FilterPreset& p : FilterPresetStore::list())
        out.push_back({ QStringLiteral("preset:") + p.name, tr("Saved filter: %1").arg(p.name), true });

    // The themed home's rows: the media-type buckets, then the catalogue tiles. Not cappable — each is a
    // single tile, so a cap has nothing to truncate.
    QSet<QString> buckets;
    for (const NavTarget& t : navTargets_)
        if (!t.isHome) buckets.insert(mediaCategory(t.type));
    for (const QString& key : { QStringLiteral("video"), QStringLiteral("game"), QStringLiteral("audio"),
                                QStringLiteral("reading"), QStringLiteral("photos") })
        if (buckets.contains(key))
            out.push_back({ QStringLiteral("category:") + key,
                            tr("Category: %1").arg(categoryMeta(key).value(QStringLiteral("title")).toString()),
                            false });
    for (const NavTarget& t : navTargets_)
        if (!t.isHome && !t.navKey.isEmpty())
            out.push_back({ QStringLiteral("source:") + t.navKey, tr("Catalogue: %1").arg(t.name), false });
    return out;
}

// ---- Custom home rows on the THEMED home (issue #161) ------------------------------------------------------
// The themed home's rows are the media-type BUCKETS and the CATALOGUE tiles, not the classic home's shelves,
// so this is the surface the `category:<key>` / `source:<navKey>` half of the row vocabulary orders and hides.
// One helper serves all three producers below.
//
// `rowIdOf` names a row's producer. A row that has no id is NOT addressable and passes through untouched at
// the end — that is the trailing "Playlists" folder, which is a door rather than a shelf and must never be
// arranged away. With no stored list this returns `rows` VERBATIM, so an untouched profile's themed home is
// byte-for-byte the home it had before #161 — the guarantee probe_homerows pins on the planner, and that this
// early return keeps at the call site.
//
// The list ORDERS and HIDES here; it does not cap. A catalogue tile is one row, so a cap has nothing to
// truncate, which is why the editor only offers the cap action where a cap can change something.
static QVariantList applyHomeRowList(const QVariantList& rows,
                                     const std::function<QString(const QVariantMap&)>& rowIdOf)
{
    const QVector<homerows::Row> list = HomeRowStore::list();
    if (list.isEmpty()) return rows;      // the default: today's home, untouched
    QVector<homerows::Available> available;
    QHash<QString, QVariantMap> byId;
    QVariantList unaddressable;
    for (const QVariant& v : rows)
    {
        const QVariantMap m = v.toMap();
        const QString id = rowIdOf(m);
        if (id.isEmpty()) { unaddressable << v; continue; }
        if (byId.contains(id)) continue;
        byId.insert(id, m);
        available.push_back({ id, 1 });
    }
    QVariantList out;
    for (const homerows::Planned& p : homerows::plan(available, list))
        if (byId.contains(p.rowId)) out << byId.value(p.rowId);
    out += unaddressable;
    return out;
}

QVariantList HomeView::categoryItems()
{
    if (navTargets_.isEmpty()) refresh();
    QSet<QString> present;
    for (const NavTarget& t : navTargets_)
        if (!t.isHome) present.insert(mediaCategory(t.type));
    QVariantList out;
    for (const QString& key : { QStringLiteral("video"), QStringLiteral("game"),
                                QStringLiteral("audio"), QStringLiteral("reading"),
                                QStringLiteral("photos") })   // #102 — see categoryMeta
        if (present.contains(key)) out << categoryMeta(key);
    // #161: the profile's row list orders/hides the buckets ("category:<key>").
    return applyHomeRowList(out, [](const QVariantMap& m) {
        const QString k = m.value(QStringLiteral("key")).toString();
        return k.isEmpty() ? QString() : QStringLiteral("category:") + k;
    });
}

// The catalogs inside one bucket, as a column the themed XMB can show and drill into via activateNav(navKey).
QVariantList HomeView::categoryCatalogs(const QString& categoryKey)
{
    if (navTargets_.isEmpty()) refresh();
    QVariantList out;
    for (const NavTarget& t : navTargets_)
    {
        if (t.isHome || mediaCategory(t.type) != categoryKey) continue;
        out << QVariantMap{ { QStringLiteral("title"), t.name }, { QStringLiteral("navKey"), t.navKey },
                            { QStringLiteral("type"), t.type }, { QStringLiteral("catalog"), true },
                            { QStringLiteral("accent"), typeColor(t.type).name() } };
    }
    // The category-level Playlists folder: opens this bucket's saved lists (mixed across its catalogues). Carries
    // no navKey — the themed home routes it to openPlaylistsLevel via "playlistsCategory". Kept out of the
    // single-catalog auto-open count (that keys off navKey), so a lone-catalog bucket still dives straight in.
    if (!out.isEmpty())
        out << QVariantMap{ { QStringLiteral("title"), tr("Playlists") }, { QStringLiteral("type"), QStringLiteral("_playlists") },
                            { QStringLiteral("playlistsCategory"), categoryKey }, { QStringLiteral("accent"), QStringLiteral("#6A6E78") } };
    // #161: the profile's row list orders/hides the catalogues ("source:<navKey>"). The Playlists folder
    // carries no navKey, so it is unaddressable and stays where it is — see applyHomeRowList.
    return applyHomeRowList(out, [](const QVariantMap& m) {
        const QString nk = m.value(QStringLiteral("navKey")).toString();
        return nk.isEmpty() ? QString() : QStringLiteral("source:") + nk;
    });
}

QVariantList HomeView::systemItems()
{
    if (navTargets_.isEmpty()) refresh(); // make sure the catalog list has been built
    QVariantList out;
    for (const NavTarget& t : navTargets_)
    {
        if (t.isHome) continue;
        out << QVariantMap{ { QStringLiteral("title"), t.name }, { QStringLiteral("type"), t.type },
                            { QStringLiteral("navKey"), t.navKey },
                            { QStringLiteral("subtitle"), tr("Open to browse") },
                            { QStringLiteral("overview"), tr("Browse the %1 catalog. Press Enter to open it.").arg(t.name) },
                            { QStringLiteral("accent"), typeColor(t.type).name() } };
    }
    // #161: the profile's row list orders/hides the catalogue tiles ("source:<navKey>").
    return applyHomeRowList(out, [](const QVariantMap& m) {
        const QString nk = m.value(QStringLiteral("navKey")).toString();
        return nk.isEmpty() ? QString() : QStringLiteral("source:") + nk;
    });
}

// The current level's items as data for the themed browse view. Skips synthetic rows (the "open a file"
// lead, info/header rows) and records the map back to the real items_ row for browseActivate().
QVariantList HomeView::browseItems()
{
    browseRowMap_.clear();
    QVariantList out;
    // Section headers ("rechdr") are emitted LAZILY: a divider is remembered and only flushed once a real item
    // beneath it survives filtering. So a mid-session Hide (or the transient browse filter) that empties a group
    // — dropping the last item between two dividers — takes its now-orphaned header with it instead of leaving a
    // bare label lingering above the next group.
    int pendingHeaderRow = -1;
    // A guidance row ("info") is HELD BACK rather than dropped, and flushed at the bottom only if
    // nothing else survived. Beside real items it is chrome the themed column does not want; ALONE it is
    // the only thing on screen that says why the column is empty, and skipping it unconditionally is what
    // turned every such case into a blank panel with no text at all in themed mode — an addon error, a
    // "Loading channels…", and (issue #74) the Music category's "no music folder yet" explanation, which
    // exists precisely so an empty shelf is never unexplained. The classic grid has always shown them.
    int loneInfoRow = -1;
    for (int r = 0; r < items_.size(); ++r)
    {
        const MediaItem& it = items_[r];
        if (it.type == QStringLiteral("_open")) continue;
        if (it.type == QStringLiteral("info"))
        {
            if (loneInfoRow < 0) loneInfoRow = r;   // the FIRST one; see the flush below
            continue;
        }
        if (it.type == QStringLiteral("rechdr")) // a section divider — defer; flush when its first item survives
        {
            pendingHeaderRow = r;
            continue;
        }
        const bool realMedia = !it.type.startsWith(QLatin1Char('_'));
        // An item hidden AFTER this level was populated is still sitting in items_ (populate only filters on a
        // fresh load). Skip it here too so a mid-session Hide vanishes from the themed model on the next rebuild
        // (browseItemsChanged) with no re-fetch. Synthetic folder rows (type starting '_') carry no marks key.
        if (realMedia && isHiddenItem(it))
            continue;
        // The transient, level-scoped browse filter (All/Favorites/status/tag) narrows the presentation only.
        if (realMedia && !passesBrowseFilter(it))
            continue;
        // A surviving real row (or a synthetic shelf/folder row) flushes any deferred header first.
        if (pendingHeaderRow >= 0)
        {
            browseRowMap_ << pendingHeaderRow;
            out << QVariantMap{ { QStringLiteral("title"), items_[pendingHeaderRow].title },
                                { QStringLiteral("header"), true } };
            pendingHeaderRow = -1;
        }
        browseRowMap_ << r;
        QVariantMap m{ { QStringLiteral("title"), it.title }, { QStringLiteral("subtitle"), it.subtitle },
                       // Offline-first: serve the locally cached copy of the tile art when we have one, so
                       // rows whose remote thumbnail is dead/unreachable (console SVGs especially) still
                       // show the art we have instead of falling back to the accent rectangle.
                       { QStringLiteral("image"), MetaCache::displayImage(MetaCache::keyFor(it), it.thumbnailUrl) },
                       { QStringLiteral("type"), it.type },
                       { QStringLiteral("accent"), typeColor(it.type).name() },
                       { QStringLiteral("expandable"), it.expandable } };
        // "Continue watching/listening", as the themed delegate's bottom bar (issue #139 increment 2). The
        // classic grid has painted this on the poster itself since the beginning; the themed grid had no
        // binding for it at all, so a part-way film and a part-way book both rendered as untouched tiles on
        // the layout this app is most used through. ONE fraction, from the same rowFraction the classic
        // paint uses — which is what makes a book's own carried progress and a film's resume lookup arrive
        // here as the same number rather than as two features.
        //
        // Absent for a row with nothing to show, rather than present-and-negative: a theme binds it with a
        // plain `modelData.progress > 0` and an absent key is undefined, which reads false.
        const double frac = rowFraction(it);
        if (frac >= 0.0) m[QStringLiteral("progress")] = frac;
        // Local library: if we own this catalog item on disk, flag it so the delegate shows an "On disk"
        // badge (and the count for a series). Purely additive — un-owned tiles are untouched.
        if (!it.id.isEmpty() && LocalLibrary::index().ownsId(it.id))
        {
            m[QStringLiteral("onDisk")] = true;
            const int eps = LocalLibrary::index().ownedEpisodes(it.id);
            if (eps > 0) m[QStringLiteral("onDiskCount")] = eps;
        }
        // Any richer artwork/videos/audio/meta the catalog already carries -> selected.logo, selected.box,
        // selected.images.screenshot, selected.videos, ... (the aggregator enriches this further on hover).
        it.art.writeInto(m);
        // The user's correction to a wrong scrape composites LAST, over everything the providers wrote
        // (issue #24) — the tile is where a mis-scrape is usually noticed, so it must be where it stops
        // showing. Cheap: one cache-backed lookup per row, and a no-op for the items with no correction.
        MetaOverrides::applyTo(MetaOverrides::get(MetaCache::keyFor(it)), m);
        out << m;
    }
    // Nothing survived: the held-back guidance row IS the level. Emitted as an ordinary row (mapped back
    // into items_, so the themed column can sit on it) and inert on Enter, because activateItem already
    // refuses type "info".
    if (out.isEmpty() && loneInfoRow >= 0)
    {
        const MediaItem& info = items_[loneInfoRow];
        browseRowMap_ << loneInfoRow;
        out << QVariantMap{ { QStringLiteral("title"), info.title },
                            { QStringLiteral("subtitle"), info.subtitle },
                            { QStringLiteral("type"), info.type },
                            { QStringLiteral("accent"), typeColor(info.type).name() } };
    }
    return out;
}

// Membership test for the transient browse filter. All -> everything; Favorites -> the profile's starred items
// (FavoritesStore is keyed by the same id keyFor() yields); Status -> the item's completion mark; Tag -> the
// item carries that tag. Marks are cache-backed (O(1)/item), so this is cheap to run over the whole level.
bool HomeView::passesBrowseFilter(const MediaItem& it) const
{
    switch (browseFilterMode_)
    {
        case 1: return FavoritesStore::isFavorite(MetaCache::keyFor(it));
        case 2: return static_cast<int>(ItemMarks::get(MetaCache::keyFor(it)).completion) == browseFilterComp_;
        case 3: return ItemMarks::get(MetaCache::keyFor(it)).tags.contains(browseFilterTag_);
        default: return true; // 0 = All
    }
}

void HomeView::setBrowseFilter(int mode, int comp, const QString& tag)
{
    browseFilterMode_ = mode;
    browseFilterComp_ = comp;
    browseFilterTag_  = tag;
}

void HomeView::clearBrowseFilter()
{
    browseFilterMode_ = 0;
    browseFilterComp_ = 0;
    browseFilterTag_.clear();
}

QString HomeView::browseTitle() const
{
    return stack_.isEmpty() ? QString() : stack_.last().title;
}

void HomeView::browseActivate(int index)
{
    if (index >= 0 && index < browseRowMap_.size()) activateItem(browseRowMap_[index]);
}

bool HomeView::browseBack()
{
    if (stack_.size() > 1) { stack_.pop_back(); emit browseLevelPopped(); loadTop(); return true; }
    return false; // at the catalog root -> the host returns to the themed home
}

bool HomeView::browseHasMore() const { return hasMore_ && !loading_; }

// After a Back, the current level's childRow is the item we drilled into. Map it to the (filtered) browse
// index so the themed column re-selects it instead of jumping to the top. browseItems() must be called first
// (it rebuilds browseRowMap_). Returns 0 for a fresh level (childRow < 0).
int HomeView::browseRestoreIndex() const
{
    // A one-shot request to keep the selection on a specific item after a re-sync (e.g. the game just
    // favourited/unfavourited), so it doesn't snap to the top of the list.
    if (!browseSelectKey_.isEmpty())
        for (int i = 0; i < browseRowMap_.size(); ++i)
        {
            const MediaItem& it = items_[browseRowMap_[i]];
            if (it.url == browseSelectKey_ || (!it.id.isEmpty() && it.id == browseSelectKey_)) return i;
        }
    // The first browse row can be a section header now; return the first SELECTABLE row from the restore point.
    auto firstSelectableFrom = [this](int start) {
        for (int i = qMax(0, start); i < browseRowMap_.size(); ++i)
            if (items_[browseRowMap_[i]].type != QStringLiteral("rechdr")) return i;
        return 0;
    };
    if (stack_.isEmpty()) return firstSelectableFrom(0);
    const int cr = stack_.last().childRow;
    if (cr < 0) return firstSelectableFrom(0);
    for (int i = 0; i < browseRowMap_.size(); ++i)
        if (browseRowMap_[i] == cr) return items_[cr].type == QStringLiteral("rechdr") ? firstSelectableFrom(i) : i;
    return firstSelectableFrom(0);
}

void HomeView::browseLoadMore() { loadMore(); } // pull the next page; onCatalogReady fires browseItemsChanged

void HomeView::searchInBrowse(const QString& query)
{
    if (search_) search_->setText(query); // doSearch() reads the query from the box
    doSearch();                            // scoped to the current console, else re-runs the base catalog
}

// Cross-addon search: replace the browse stack with a synthetic "_search" level and fan the query out to every
// enabled source's catalogs; results stream into one merged grid as each responds. Back returns here (loadTop
// re-runs it). Each result is tagged with its origin so activateItem re-opens it through the right addon.
void HomeView::searchEverything(const QString& query)
{
    const QString q = query.trimmed();
    if (q.isEmpty()) return;
    recentView_ = false;
    applyGridMode(/*recentList*/ false);
    styleTypeButtons(QStringLiteral("home"));
    hideMeta();
    Level lvl;
    lvl.addon = nullptr; lvl.detail = true;
    lvl.item.type = QStringLiteral("_search");
    lvl.item.mime = QStringLiteral("search:") + q;
    lvl.item.title = tr("Search: %1").arg(q);
    lvl.title = lvl.item.title;
    lvl.query = q;
    stack_.clear();
    stack_.push_back(lvl);
    startSearch(q);
}

// HomeView side of a cross-addon search: reset the grid state, hand the fan-out to the aggregator, and mirror
// its in-flight state. The aggregator streams results back via resultsAppended/finished (wired in the ctor).
void HomeView::startSearch(const QString& query)
{
    ++generation_;                        // fresh grid -> ignore stale async thumbnails
    pendingReqId_ = -1;                    // not driven by the single-request path
    hasMore_ = false;
    agg_->start(query);                    // clears prior state + fans the query out to every enabled source
    perfSearchFirstSeen_ = false;
    PerfTrace::begin(QStringLiteral("search.first"));
    PerfTrace::begin(QStringLiteral("search.drain"));
    loading_ = agg_->active();
    populate(agg_->accumulated(), /*append*/ false); // clears the grid, shows the (empty) searching state
    if (!agg_->active()) showToast(tr("No add-ons to search."), kFeedbackLong);
    updateStatus();
}

void HomeView::selectRecent()
{
    recentView_ = true;
    styleTypeButtons(QStringLiteral("home"));
    search_->clear();
    stack_.clear();
    hideMeta();
    pendingReqId_ = -1; // ignore any in-flight addon result
    if (agg_) agg_->cancel(); // J17: abandon any in-flight cross-addon search so stale results don't stream into Home
    loading_ = false;
    hasMore_ = false;
    renderRecents();
}

// The Photos category (#102): a synthetic top-level browser over the configured photo library, built by
// browse::photosCatalog. Set up exactly like the synthetic folder levels (a detail root with an expandable
// container item), so loadTop() repopulates it natively on Back and never falls through to the addon path.
void HomeView::selectPhotos()
{
    recentView_ = false;
    applyGridMode(/*recentList*/ false);
    styleTypeButtons(QStringLiteral("photos"));
    search_->clear();
    stack_.clear();
    if (agg_) agg_->cancel();
    Level lvl;
    lvl.addon = nullptr; lvl.detail = true; lvl.title = tr("Photos");
    lvl.item.id = QStringLiteral("_photos");
    lvl.item.type = QStringLiteral("_photosroot");
    lvl.item.expandable = true;
    lvl.item.mime = QStringLiteral("photos"); // so loadTop() repopulates on Back
    stack_.push_back(lvl);
    populatePhotos();
}

void HomeView::populatePhotos()
{ showSyntheticCatalog(browse::photosCatalog(PhotoLibrary::scanFolder(PhotoLibrary::root()))); }

void HomeView::openPhotoFolderLevel(const QString& folder)
{
    if (xmbMode_) { atXmbRoot_ = false; if (xmb_) xmb_->setAtRoot(false); }
    Level lvl;
    lvl.addon = nullptr; lvl.detail = true; lvl.title = QFileInfo(folder).fileName();
    lvl.item.id = QStringLiteral("_photofolder");
    lvl.item.type = QStringLiteral("_photofolder");
    lvl.item.expandable = true;
    lvl.item.mime = QStringLiteral("photofolder:") + folder; // so loadTop() repopulates on Back
    stack_.push_back(lvl);
    populatePhotoFolder(folder);
}

void HomeView::populatePhotoFolder(const QString& folder)
{ showSyntheticCatalog(browse::photosFolderCatalog(PhotoLibrary::scanFolder(PhotoLibrary::root()), folder)); }

// ---- The Music category (#74) ---------------------------------------------------------------------------
//
// Three synthetic levels over MusicLibrary's index — Artists, one artist's Albums, one album's Tracks — set
// up exactly like the Photos levels above (a detail root carrying an expandable container item), so loadTop()
// repopulates each of them natively on Back and none of them ever falls through to the addon path.
//
// Unlike Photos, NOTHING here rescans on entry. The music scan is a tag parse per changed file and can be
// tens of thousands of files; it runs once at startup and on demand from Settings (MainWindow::
// rescanMusicLibrary), and this surface reads the installed index. onMusicLibraryChanged() below is how a
// finished scan reaches a level the user is already standing in.

// ---- The synthetic AUDIOBOOKS category (issue #139) -----------------------------------------------------
//
// Six levels, all built the same way the Music ones are and for the same reasons: each is a detail root
// carrying an expandable container item whose `mime` is the level's own MARKER, so loadTop() repopulates it
// natively on Back and none of them ever falls through to the addon path. Nothing here rescans — MainWindow
// owns the scan (rescanAudiobookLibrary) and this surface reads the installed index; onAudiobookLibraryChanged
// below is how a finished scan reaches a level the user is already standing in.

// Why the Audiobooks category is empty, in the user's terms. Only this layer can tell the three cases apart —
// the pure builder is handed the sentence precisely so the reasons can live next to the Settings state they
// are about.
browse::AudiobookEmptyNote HomeView::audiobookEmptyNote() const
{
    if (!AudiobookLibrary::index().isEmpty()) return {};
    const QString root  = AudiobookLibrary::root();
    const QString shown = QDir::toNativeSeparators(root);
    if (root.isEmpty() || !QFileInfo::exists(root))
        return { tr("No audiobook folder yet. Choose one under Settings → Audiobooks and your books show up "
                    "here by author, narrator and series."), QString() };
    // The scan is asynchronous, so the category is reachable before the first one has landed. "Nothing here"
    // and "not looked yet" want opposite sentences, and only indexReady() can tell them apart.
    if (!AudiobookLibrary::indexReady())
        return { tr("Scanning your audiobook folder…"), shown };
    return { tr("No audiobooks found. Put audio files in this folder, or choose another under "
                "Settings → Audiobooks."), shown };
}

// The ONE cover supplier for every audiobook level: the extracted embedded art if the scan wrote one, else a
// cover.*/folder.* beside the book. MusicArt::keyedCover is that rule — the SAME rule an album tile uses,
// through the same cache — rather than a second copy of it (MusicArt.h says why).
static browse::AudiobookCoverFn audiobookCover()
{
    return [](const AudiobookLibrary::Book& b) {
        static const QString dir = MusicArt::cacheDir();   // one AppPaths read per process, not one per tile
        return MusicArt::keyedCover(b.key, b.folder, dir);
    };
}

// HOW FAR INTO A PART SOMEBODY IS — the ONE reader of the resume store this feature has (#139 increment 2).
//
// The same ini group, the same spelling and the same file the player writes, reached through ResumeStore so
// there is no second opinion about where a position lives. It answers per PART because that is the only
// granularity the marks have; turning a book's worth of them into one number is AudiobookLibrary's job and
// is stated there.
static AudiobookLibrary::PartPositionFn audiobookPartPosition()
{
    return [](const QString& path) {
        const QString g = ResumeStore::groupFor(path) + QStringLiteral("/");
        return settingsStore().value(g + QStringLiteral("pos"), 0.0).toDouble();
    };
}

// The book-level progress every audiobook surface shows: the marks above, plus the book's own completion
// MARK, which is the only evidence a FINISHED book leaves (AudiobookLibrary.h says why). The mark is keyed
// exactly as the book row is — its `id`, i.e. the book prefix and key — so a status set from the browse
// filter's own Mark-as menu is the one this reads.
static browse::AudiobookProgressFn audiobookProgress()
{
    return [](const AudiobookLibrary::Book& b) {
        const QString markKey = QString::fromLatin1(browse::kAudiobookBookPrefix) + b.key;
        const bool done = ItemMarks::get(markKey).completion == ItemMarks::Completion::Finished;
        return AudiobookLibrary::progressFor(b, audiobookPartPosition(), done);
    };
}

void HomeView::selectAudiobooks()
{
    recentView_ = false;
    applyGridMode(/*recentList*/ false);
    styleTypeButtons(QStringLiteral("audiobooks"));
    search_->clear();
    stack_.clear();
    if (agg_) agg_->cancel();
    Level lvl;
    lvl.addon = nullptr; lvl.detail = true; lvl.title = tr("Audiobooks");
    lvl.item.id = QStringLiteral("_audiobooks");
    lvl.item.type = QStringLiteral("_abroot");
    lvl.item.expandable = true;
    lvl.item.mime = QStringLiteral("audiobooks"); // so loadTop() repopulates on Back
    stack_.push_back(lvl);
    populateAudiobooks();
}

void HomeView::populateAudiobooks()
{
    showSyntheticCatalog(browse::audiobookRootCatalog(AudiobookLibrary::index(), audiobookEmptyNote(),
                                                      audiobookCover()));
}

// One push site per level, all the same shape. `type` is what loadTop dispatches on and `mime` is the
// marker it rebuilds FROM — a level that stored neither would open fine and repopulate empty on the way back
// out, which is the failure the `synthetic level Back survival` gate exists to catch.
void HomeView::openAudiobookAuthorLevel(const QString& authorKey)
{
    if (xmbMode_) { atXmbRoot_ = false; if (xmb_) xmb_->setAtRoot(false); }
    const AudiobookLibrary::Author* a = AudiobookLibrary::index().author(authorKey);
    Level lvl;
    lvl.addon = nullptr; lvl.detail = true;
    lvl.title = a ? AudiobookLibrary::displayAuthor(*a) : tr("Audiobooks");
    lvl.item.id = QStringLiteral("_abauthor");
    lvl.item.type = QStringLiteral("_abauthor");
    lvl.item.expandable = true;
    lvl.item.mime = QString::fromLatin1(browse::kAudiobookAuthorPrefix) + authorKey;
    stack_.push_back(lvl);
    populateAudiobookAuthor(authorKey);
}

void HomeView::populateAudiobookAuthor(const QString& authorKey)
{
    showSyntheticCatalog(browse::audiobookAuthorCatalog(AudiobookLibrary::index(), authorKey,
                                                        audiobookCover(), audiobookProgress()));
}

void HomeView::openAudiobookNarratorsLevel()
{
    if (xmbMode_) { atXmbRoot_ = false; if (xmb_) xmb_->setAtRoot(false); }
    Level lvl;
    lvl.addon = nullptr; lvl.detail = true; lvl.title = tr("Narrators");
    lvl.item.id = QStringLiteral("_abnarrators");
    lvl.item.type = QStringLiteral("_abnarrators");
    lvl.item.expandable = true;
    lvl.item.mime = QString::fromLatin1(browse::kAudiobookNarratorsPrefix);
    stack_.push_back(lvl);
    populateAudiobookNarrators();
}

void HomeView::populateAudiobookNarrators()
{
    showSyntheticCatalog(browse::audiobookNarratorsCatalog(AudiobookLibrary::index(), audiobookCover()));
}

void HomeView::openAudiobookNarratorLevel(const QString& narratorKey)
{
    if (xmbMode_) { atXmbRoot_ = false; if (xmb_) xmb_->setAtRoot(false); }
    const AudiobookLibrary::Narrator* n = AudiobookLibrary::index().narrator(narratorKey);
    Level lvl;
    lvl.addon = nullptr; lvl.detail = true;
    lvl.title = n && !n->name.trimmed().isEmpty() ? n->name.trimmed() : tr("Narrators");
    lvl.item.id = QStringLiteral("_abnarrator");
    lvl.item.type = QStringLiteral("_abnarrator");
    lvl.item.expandable = true;
    lvl.item.mime = QString::fromLatin1(browse::kAudiobookNarratorPrefix) + narratorKey;
    stack_.push_back(lvl);
    populateAudiobookNarrator(narratorKey);
}

void HomeView::populateAudiobookNarrator(const QString& narratorKey)
{
    showSyntheticCatalog(browse::audiobookNarratorCatalog(AudiobookLibrary::index(), narratorKey,
                                                          audiobookCover(), audiobookProgress()));
}

void HomeView::openAudiobookSeriesListLevel()
{
    if (xmbMode_) { atXmbRoot_ = false; if (xmb_) xmb_->setAtRoot(false); }
    Level lvl;
    lvl.addon = nullptr; lvl.detail = true; lvl.title = tr("Series");
    lvl.item.id = QStringLiteral("_abserieslist");
    lvl.item.type = QStringLiteral("_abserieslist");
    lvl.item.expandable = true;
    lvl.item.mime = QString::fromLatin1(browse::kAudiobookSeriesListPrefix);
    stack_.push_back(lvl);
    populateAudiobookSeriesList();
}

void HomeView::populateAudiobookSeriesList()
{
    showSyntheticCatalog(browse::audiobookSeriesListCatalog(AudiobookLibrary::index(), audiobookCover()));
}

void HomeView::openAudiobookSeriesLevel(const QString& seriesKey)
{
    if (xmbMode_) { atXmbRoot_ = false; if (xmb_) xmb_->setAtRoot(false); }
    const AudiobookLibrary::Series* s = AudiobookLibrary::index().seriesFor(seriesKey);
    Level lvl;
    lvl.addon = nullptr; lvl.detail = true;
    lvl.title = s && !s->name.trimmed().isEmpty() ? s->name.trimmed() : tr("Series");
    lvl.item.id = QStringLiteral("_abseries");
    lvl.item.type = QStringLiteral("_abseries");
    lvl.item.expandable = true;
    lvl.item.mime = QString::fromLatin1(browse::kAudiobookSeriesPrefix) + seriesKey;
    stack_.push_back(lvl);
    populateAudiobookSeries(seriesKey);
}

void HomeView::populateAudiobookSeries(const QString& seriesKey)
{
    showSyntheticCatalog(browse::audiobookSeriesCatalog(AudiobookLibrary::index(), seriesKey,
                                                        audiobookCover(), audiobookProgress()));
}

void HomeView::openAudiobookBookLevel(const QString& bookKey)
{
    if (xmbMode_) { atXmbRoot_ = false; if (xmb_) xmb_->setAtRoot(false); }
    const AudiobookLibrary::Book* b = AudiobookLibrary::index().book(bookKey);
    Level lvl;
    lvl.addon = nullptr; lvl.detail = true;
    lvl.title = b ? AudiobookLibrary::displayBook(*b) : tr("Audiobooks");
    lvl.item.id = QStringLiteral("_abbook");
    lvl.item.type = QStringLiteral("_abbook");
    lvl.item.expandable = true;
    lvl.item.mime = QString::fromLatin1(browse::kAudiobookBookPrefix) + bookKey;
    stack_.push_back(lvl);
    populateAudiobookBook(bookKey);
}

void HomeView::populateAudiobookBook(const QString& bookKey)
{
    showSyntheticCatalog(browse::audiobookBookCatalog(AudiobookLibrary::index(), bookKey, audiobookCover(),
                                                      audiobookProgress()));
}

// THE CHAPTER LIST (#139 increment 2) — an .m4b's chapter atoms or a folder's parts, whichever the book is,
// as ONE overlay list.
//
// A NavMenu rather than a browse level, and that is the whole of the difference from every other row in this
// feature: the levels are places you ARE, this is a jump you make and leave. It is also the nav kit's own
// answer to "show a list on top of what is there" — no QDialog, reachable by pad and by keyboard on all four
// layouts, and its onChosen runs AFTER the overlay closes, which is what keeps a play (which tears this very
// browse level down and rebuilds the screen) out of a live delegate emission (issue #28 / #211).
//
// The rows come from the INDEX, never by opening a file: an .m4b's atoms were read once at scan time and a
// part's title and length are what the tags said. Activating one plays the book through the SAME
// openAudiobook every other route uses, handed the row's file and its offset inside that file — so the queue
// is the book's queue, the whole-book timeline is seeded exactly as it always is, and resume keeps working
// because nothing about the play differs except where it starts.
void HomeView::openAudiobookChapters(const QString& bookKey)
{
    const AudiobookLibrary::Book* b = AudiobookLibrary::index().book(bookKey);
    if (!b) { showToast(tr("That audiobook is no longer in your library.")); return; }

    const QVector<AudiobookLibrary::ChapterRow> rows =
        AudiobookLibrary::chapterRows(*b, audiobookPartPosition());
    if (rows.size() < 2) return;   // the door is not offered for a book of one row; a stale press does nothing

    // Open ON the row the listener is standing in, so a fifty-chapter book does not start the pick at the top
    // every time. NavMenu::pick has no initial-row argument, so the selection is placed by the same means the
    // rest of this file uses for a list: build the menu, then move to the row.
    int current = 0;
    for (int i = 0; i < rows.size(); ++i) if (rows.at(i).current) current = i;

    const int pick = NavMenu::pick(AudiobookLibrary::displayBook(*b),
                                   browse::audiobookChapterMenuRows(rows), window(), current);
    if (pick < 0 || pick >= rows.size()) return;
    emit playAudiobookRequested(bookKey, rows.at(pick).path, rows.at(pick).startSec);
}

// A finished scan installed a new index (MainWindow::rescanAudiobookLibrary). Refresh whichever Audiobooks
// level the user is standing in, and nothing else — the same rule onMusicLibraryChanged follows, including
// the deliberate absence of a loadTop() for every other level: a book scan cannot add anything to a level
// that is not one of these six, and whether the Audiobooks TAB is offered at all is decided by the root
// EXISTING, which was already true before any scan ran.
void HomeView::onAudiobookLibraryChanged()
{
    if (stack_.isEmpty()) return;
    const auto& top = stack_.last();
    if (top.item.type == QStringLiteral("_abroot")) { populateAudiobooks(); return; }
    if (top.item.type == QStringLiteral("_abauthor"))
        { populateAudiobookAuthor(browse::audiobookKeyOf(top.item.mime, browse::kAudiobookAuthorPrefix)); return; }
    if (top.item.type == QStringLiteral("_abnarrators")) { populateAudiobookNarrators(); return; }
    if (top.item.type == QStringLiteral("_abnarrator"))
        { populateAudiobookNarrator(browse::audiobookKeyOf(top.item.mime, browse::kAudiobookNarratorPrefix)); return; }
    if (top.item.type == QStringLiteral("_abserieslist")) { populateAudiobookSeriesList(); return; }
    if (top.item.type == QStringLiteral("_abseries"))
        { populateAudiobookSeries(browse::audiobookKeyOf(top.item.mime, browse::kAudiobookSeriesPrefix)); return; }
    if (top.item.type == QStringLiteral("_abbook"))
        { populateAudiobookBook(browse::audiobookKeyOf(top.item.mime, browse::kAudiobookBookPrefix)); return; }
}


// ---- The synthetic BOOKS category (issue #134) ----------------------------------------------------------
//
// Four levels - the root, an author, the series list, a series - all built the same way the Audiobooks ones
// are and for the same reasons: each is a detail root carrying an expandable container item whose `mime` is
// the level's own MARKER, so loadTop() repopulates it natively on Back and none of them ever falls through
// to the addon path. Nothing here rescans - MainWindow owns the scan (rescanBookLibrary) and this surface
// reads the installed index; onBookLibraryChanged below is how a finished scan reaches a level the user is
// already standing in.
//
// THERE IS NO FIFTH LEVEL, and that absence is the shape of the feature rather than an omission: one file is
// one book, so a book row is a LEAF and pressing it opens the reader. See BookCatalogs.h.

// Why the Books category is empty, in the user's terms. Only this layer can tell the three cases apart - the
// pure builder is handed the sentence precisely so the reasons can live next to the Settings state they are
// about.
browse::BookEmptyNote HomeView::bookEmptyNote() const
{
    if (!BookLibrary::index().isEmpty()) return {};
    const QString root  = BookLibrary::root();
    const QString shown = QDir::toNativeSeparators(root);
    if (root.isEmpty() || !QFileInfo::exists(root))
        return { tr("No books folder yet. Choose one under Settings → Books and your books and comics "
                    "show up here by author and series."), QString() };
    // The scan is asynchronous, so the category is reachable before the first one has landed. "Nothing here"
    // and "not looked yet" want opposite sentences, and only indexReady() can tell them apart.
    if (!BookLibrary::indexReady())
        return { tr("Scanning your books folder…"), shown };
    return { tr("No books found. Put .epub, .fb2, .azw3, .txt, .md, .pdf, .cbz or .cbr files in this "
                "folder, or choose another under Settings → Books."), shown };
}

// The ONE cover supplier for every book level: the cover the scan extracted out of the file, else a
// cover.*/folder.* beside it. MusicArt::keyedCover is that rule - the SAME rule an album tile and an
// audiobook tile use, through the same cache - rather than a third copy of it (MusicArt.h says why).
static browse::BookCoverFn bookCover()
{
    return [](const BookLibrary::Book& b) {
        static const QString dir = MusicArt::cacheDir();   // one AppPaths read per process, not one per tile
        return MusicArt::keyedCover(b.key, b.folder, dir);
    };
}

void HomeView::selectBooks()
{
    recentView_ = false;
    applyGridMode(/*recentList*/ false);
    styleTypeButtons(QStringLiteral("books"));
    search_->clear();
    stack_.clear();
    if (agg_) agg_->cancel();
    Level lvl;
    lvl.addon = nullptr; lvl.detail = true; lvl.title = tr("My Books");
    lvl.item.id = QStringLiteral("_books");
    lvl.item.type = QStringLiteral("_bkroot");
    lvl.item.expandable = true;
    lvl.item.mime = QStringLiteral("books"); // so loadTop() repopulates on Back
    stack_.push_back(lvl);
    populateBooks();
}

void HomeView::populateBooks()
{
    showSyntheticCatalog(browse::bookRootCatalog(BookLibrary::index(), bookEmptyNote(), bookCover()));
}

// One push site per level, all the same shape. `type` is what loadTop dispatches on and `mime` is the marker
// it rebuilds FROM - a level that stored neither would open fine and repopulate empty on the way back out,
// which is the failure the `synthetic level Back survival` gate exists to catch.
void HomeView::openBookAuthorLevel(const QString& authorKey)
{
    if (xmbMode_) { atXmbRoot_ = false; if (xmb_) xmb_->setAtRoot(false); }
    const BookLibrary::Author* a = BookLibrary::index().author(authorKey);
    Level lvl;
    lvl.addon = nullptr; lvl.detail = true;
    lvl.title = a ? BookLibrary::displayAuthor(*a) : tr("My Books");
    lvl.item.id = QStringLiteral("_bkauthor");
    lvl.item.type = QStringLiteral("_bkauthor");
    lvl.item.expandable = true;
    lvl.item.mime = QString::fromLatin1(browse::kBookAuthorPrefix) + authorKey;
    stack_.push_back(lvl);
    populateBookAuthor(authorKey);
}

void HomeView::populateBookAuthor(const QString& authorKey)
{
    showSyntheticCatalog(browse::bookAuthorCatalog(BookLibrary::index(), authorKey, bookCover()));
}

void HomeView::openBookSeriesListLevel()
{
    if (xmbMode_) { atXmbRoot_ = false; if (xmb_) xmb_->setAtRoot(false); }
    Level lvl;
    lvl.addon = nullptr; lvl.detail = true; lvl.title = tr("Series");
    lvl.item.id = QStringLiteral("_bkserieslist");
    lvl.item.type = QStringLiteral("_bkserieslist");
    lvl.item.expandable = true;
    lvl.item.mime = QString::fromLatin1(browse::kBookSeriesListPrefix);
    stack_.push_back(lvl);
    populateBookSeriesList();
}

void HomeView::populateBookSeriesList()
{
    showSyntheticCatalog(browse::bookSeriesListCatalog(BookLibrary::index(), bookCover()));
}

void HomeView::openBookSeriesLevel(const QString& seriesKey)
{
    if (xmbMode_) { atXmbRoot_ = false; if (xmb_) xmb_->setAtRoot(false); }
    const BookLibrary::Series* s = BookLibrary::index().seriesFor(seriesKey);
    Level lvl;
    lvl.addon = nullptr; lvl.detail = true;
    lvl.title = s && !s->name.trimmed().isEmpty() ? s->name.trimmed() : tr("Series");
    lvl.item.id = QStringLiteral("_bkseries");
    lvl.item.type = QStringLiteral("_bkseries");
    lvl.item.expandable = true;
    lvl.item.mime = QString::fromLatin1(browse::kBookSeriesPrefix) + seriesKey;
    stack_.push_back(lvl);
    populateBookSeries(seriesKey);
}

void HomeView::populateBookSeries(const QString& seriesKey)
{
    showSyntheticCatalog(browse::bookSeriesCatalog(BookLibrary::index(), seriesKey, bookCover()));
}

// A finished scan installed a new index (MainWindow::rescanBookLibrary). Refresh whichever Books level the
// user is standing in, and nothing else - the same rule onMusicLibraryChanged and onAudiobookLibraryChanged
// follow, including the deliberate absence of a loadTop() for every other level: a book scan cannot add
// anything to a level that is not one of these four, and whether the Books TAB is offered at all is decided
// by the root EXISTING, which was already true before any scan ran.
void HomeView::onBookLibraryChanged()
{
    if (stack_.isEmpty()) return;
    const auto& top = stack_.last();
    if (top.item.type == QStringLiteral("_bkroot")) { populateBooks(); return; }
    if (top.item.type == QStringLiteral("_bkauthor"))
        { populateBookAuthor(browse::bookKeyOf(top.item.mime, browse::kBookAuthorPrefix)); return; }
    if (top.item.type == QStringLiteral("_bkserieslist")) { populateBookSeriesList(); return; }
    if (top.item.type == QStringLiteral("_bkseries"))
        { populateBookSeries(browse::bookKeyOf(top.item.mime, browse::kBookSeriesPrefix)); return; }
}

// Why the Music category is empty, in the user's terms. Only this layer can tell the three cases apart — the
// pure builder is handed the sentence (see musicArtistsCatalog's `emptyReason`) precisely so the reasons can
// live next to the Settings state they are about.
browse::MusicEmptyNote HomeView::musicEmptyNote() const
{
    if (!MusicLibrary::index().isEmpty()) return {};
    // A configured music server IS the answer to "there is nothing here", and it is drawn one row above this
    // sentence. Telling somebody to go and choose a folder when the thing they actually set up is sitting
    // right there would be actively wrong, so say nothing - which is already this struct's contract for an
    // empty `text` (see MusicEmptyNote).
    if (SubsonicServerStore::hasServers()) return {};
    const QString root = MusicLibrary::root();
    const QString shown = QDir::toNativeSeparators(root);
    if (root.isEmpty() || !QFileInfo::exists(root))
        return { tr("No music folder yet. Choose one under Settings → Music and your own music shows up "
                    "here by artist and album."), QString() };
    // The scan is asynchronous, so the category is reachable before the first one has landed. "Nothing here"
    // and "not looked yet" want opposite sentences, and only indexReady() can tell them apart.
    if (!MusicLibrary::indexReady())
        return { tr("Scanning your music folder…"), shown };
    return { tr("No music found. Put audio files in this folder, or choose another under Settings → Music."),
             shown };
}

void HomeView::selectMusic()
{
    recentView_ = false;
    applyGridMode(/*recentList*/ false);
    styleTypeButtons(QStringLiteral("music"));
    search_->clear();
    stack_.clear();
    if (agg_) agg_->cancel();
    Level lvl;
    lvl.addon = nullptr; lvl.detail = true; lvl.title = tr("Music");
    lvl.item.id = QStringLiteral("_music");
    lvl.item.type = QStringLiteral("_musicroot");
    lvl.item.expandable = true;
    lvl.item.mime = QStringLiteral("music"); // so loadTop() repopulates on Back
    stack_.push_back(lvl);
    populateMusicArtists();
}

// The ONE cover supplier for every music level. For a LOCAL album MusicSupply::albumArt answers exactly what
// browse's own default answers (MusicArt::albumCover over MusicArt::cacheDir), so passing it everywhere
// leaves #74 rendering precisely what it rendered; for a REMOTE album it answers the sleeve MetaCache has
// fetched, and an empty string until it has.
static browse::MusicCoverFn musicCover()
{
    return [](const MusicLibrary::Album& b) { return MusicSupply::albumArt(b); };
}

// ---- ONE QUESTION PER LEVEL, WHATEVER THE SUPPLIER (issue #194, increment 3) ---------------------------
//
// There are now three remote suppliers, and every browse level has to ask each of them the same two
// questions: does this key still owe a fetch, and how do I ask for it? Written out at each call site that
// would be four branches in eleven places, and the failure mode of that is specific and certain — somebody
// adds a supplier and misses one of them, and that supplier's albums are silently empty on exactly one
// level. So the routing is here, once, expressed the way MusicSupply expresses it: each family's own parser
// answers "is this mine", and the families are mutually unreadable by construction.
//
// `done` collapses the three clients' Result types to (ok, message) because every caller uses exactly those
// two fields, and the third — Subsonic's `auth` — is not acted on differently anywhere in this file.
static bool musicNeedsArtistFetch(const QString& artistKey)
{
    if (Subsonic::isQualified(artistKey))   return !SubsonicClient::instance().artistLoaded(artistKey);
    if (Jellyfin::isQualified(artistKey))   return !JellyfinMusicClient::instance().artistLoaded(artistKey);
    if (ServerMusic::isQualified(artistKey)) return !ServerMusicClient::instance().artistLoaded(artistKey);
    return false;   // a local key: its albums are in the scanned index already
}

static void musicFetchArtistAlbums(const QString& artistKey,
                                   std::function<void(bool ok, const QString& message)> done)
{
    if (Subsonic::isQualified(artistKey))
        SubsonicClient::instance().fetchArtistAlbums(artistKey,
            [done](const SubsonicClient::Result& r) { done(r.ok, r.message); });
    else if (Jellyfin::isQualified(artistKey))
        JellyfinMusicClient::instance().fetchArtistAlbums(artistKey,
            [done](const JellyfinMusicClient::Result& r) { done(r.ok, r.message); });
    else if (ServerMusic::isQualified(artistKey))
        ServerMusicClient::instance().fetchArtistAlbums(artistKey,
            [done](const ServerMusicClient::Result& r) { done(r.ok, r.message); });
    else
        done(true, QString());
}

static bool musicNeedsAlbumFetch(const QString& albumKey)
{
    if (Subsonic::isQualified(albumKey))   return !SubsonicClient::instance().albumTracksLoaded(albumKey);
    if (Jellyfin::isQualified(albumKey))   return !JellyfinMusicClient::instance().albumTracksLoaded(albumKey);
    if (ServerMusic::isQualified(albumKey)) return !ServerMusicClient::instance().albumTracksLoaded(albumKey);
    return false;
}

static void musicFetchAlbumTracks(const QString& albumKey,
                                  std::function<void(bool ok, const QString& message)> done)
{
    if (Subsonic::isQualified(albumKey))
        SubsonicClient::instance().fetchAlbumTracks(albumKey,
            [done](const SubsonicClient::Result& r) { done(r.ok, r.message); });
    else if (Jellyfin::isQualified(albumKey))
        JellyfinMusicClient::instance().fetchAlbumTracks(albumKey,
            [done](const JellyfinMusicClient::Result& r) { done(r.ok, r.message); });
    else if (ServerMusic::isQualified(albumKey))
        ServerMusicClient::instance().fetchAlbumTracks(albumKey,
            [done](const ServerMusicClient::Result& r) { done(r.ok, r.message); });
    else
        done(true, QString());
}

// The sleeve, likewise. A local album's art is already on disk; each remote supplier fetches it into
// MetaCache under that copy's own key.
static void musicPrefetchCover(const QString& albumKey, std::function<void()> then)
{
    if (Subsonic::isQualified(albumKey))        SubsonicClient::instance().prefetchAlbumCover(albumKey, then);
    else if (Jellyfin::isQualified(albumKey))   JellyfinMusicClient::instance().prefetchAlbumCover(albumKey, then);
    else if (ServerMusic::isQualified(albumKey)) ServerMusicClient::instance().prefetchAlbumCover(albumKey, then);
}

void HomeView::populateMusicArtists()
{
    // Which connected servers serve music is read fresh here, before the supplier COUNT is taken: a shelf
    // that arrived since the last look is the difference between the merged path and the single-source one.
    refreshMusicShelves();
    const int serverCount = int(SubsonicServerStore::list().size());
    // ONE SUPPLIER: exactly the call this function has always made. Not "the merge happens to be a no-op" -
    // the merged path is not entered at all, so there is no way for it to change a row, a count or an order
    // in a library it has no business touching. See MusicMerge.h.
    if (!musicMergeActive())
    {
        showSyntheticCatalog(browse::musicArtistsCatalog(MusicLibrary::index(), musicEmptyNote(), musicCover(),
                                                         serverCount));
        return;
    }
    // Several suppliers. Ask each server for its artist list once, render what has arrived, and repopulate as
    // the rest land - the local library is on screen immediately either way, so nothing waits on a box that
    // may be switched off.
    fetchMergeSources();
    rebuildMergedMusic();
    showSyntheticCatalog(browse::musicArtistsCatalog(mergedMusic_.idx, musicEmptyNote(), musicCover(),
                                                     serverCount));
}

// ---- ONE LIBRARY ACROSS SOURCES (issue #194, increment 1) ----------------------------------------------

bool HomeView::musicMergePossible() const
{
    // A COUNT OF SUPPLIERS, not of content: the local library gates on hasLibrary() (a configured root that
    // exists) exactly as the Music tab itself does, because the scan is asynchronous and "no tracks yet" and
    // "no library" want opposite answers.
    //
    // (#194 increment 3) Jellyfin counts its ENABLED servers rather than all of them, because `enabled` is
    // the switch that means "get this library out of the way for the evening" — a server switched off is a
    // supplier that is not supplying, and counting it would put a two-supplier install into the merged path
    // with nothing to merge. Subsonic has no such switch, so its list is counted whole, unchanged.
    return (MusicLibrary::hasLibrary() ? 1 : 0)
           + int(SubsonicServerStore::list().size())
           + int(JellyfinServerStore::enabled().size())
           + int(ServerMusicClient::instance().shelves().size()) >= 2;
}

// The connected servers that serve a music shelf. See the header for why the answer is pushed down.
//
// THE GATE IS STRUCTURAL, NOT A NAME CHECK, and it is the load-bearing line in this function: a shelf
// qualifies only when it is served by a REMOTE server the user connected over our own addon protocol. A
// bundled metadata add-on has a catalogue of type `music` too (the AIO catalog's MusicBrainz shelf), and
// merging THAT into somebody's library would fold a database of every record ever pressed into the twelve
// albums they own. A metadata shelf answers "what exists"; a server shelf answers "what you have".
void HomeView::refreshMusicShelves()
{
    QVector<ServerMusicClient::Shelf> shelves;
    if (mgr_)
    {
        for (LoadedAddon* src : mgr_->sources())
        {
            if (!src || src->transport != LoadedAddon::RemoteHttp) continue;
            if (src->stremio) continue;                        // a third-party Stremio addon is not our server
            if (!mgr_->isEnabled(src->manifest.id)) continue;
            for (const AddonCatalog& c : mgr_->catalogs(src))
            {
                if (c.type != QStringLiteral("music")) continue;
                if (c.searchOnly || !c.skipReason.isEmpty()) continue;   // it can never be browsed
                ServerMusicClient::Shelf s;
                s.id           = src->manifest.id;
                s.name         = src->manifest.name.isEmpty() ? src->manifest.id : src->manifest.name;
                s.baseUrl      = src->baseUrl;
                s.catalogId    = c.id;
                s.configHeader = mgr_->serverConfigHeader(src);
                shelves.push_back(s);
                break;   // one music shelf per server: a second would be the same library twice
            }
        }
    }
    ServerMusicClient::instance().setShelves(shelves);
}

bool HomeView::insideMusicServerLevel() const
{
    for (const Level& l : stack_)
        if (l.item.type == QString::fromLatin1(browse::kMusicServerType)) return true;
    return false;
}

bool HomeView::musicMergeActive() const { return musicMergePossible() && !insideMusicServerLevel(); }

void HomeView::rebuildMergedMusic()
{
    if (mergedMusicValid_) return;
    QVector<MusicMerge::Source> srcs;
    srcs.push_back({ QString(), &MusicLibrary::index() });          // "" == local, always first
    for (const SubsonicServer& s : SubsonicServerStore::list())
        srcs.push_back({ s.id, &SubsonicClient::instance().index(s.id) });
    // THE ORDER IS THE PREFERENCE'S FALLBACK ORDER, so it has to be stable run to run: the servers as they
    // were added, then the shelves as their sources load. MusicId::pickAutoSource is a total function of the
    // preference and THIS order (MusicId.h), which is what stops a merged row's identity flapping between
    // two refreshes.
    for (const JellyfinServer& s : JellyfinServerStore::enabled())
        srcs.push_back({ s.id, &JellyfinMusicClient::instance().index(s.id) });
    for (const ServerMusicClient::Shelf& s : ServerMusicClient::instance().shelves())
        srcs.push_back({ s.id, &ServerMusicClient::instance().index(s.id) });
    // merge() copies everything it keeps, so none of those references outlive this call.
    mergedMusic_      = MusicMerge::merge(srcs, Settings::musicPreferredSource());
    mergedMusicValid_ = true;
    applyMusicRemap();
}

// KEEP WHAT THE USER BANKED WHEN THE PICK MOVES (issue #194, increment 2).
//
// A merged album is played under the key of the copy the preference picked, so a resume position and the
// listening seconds accrue against THAT copy's tracks. Change "Play music from" and the row on screen is the
// other copy, whose tracks have never been played — everything banked is stranded, invisible rather than
// deleted. MusicRemap moves it, and this is the one place it is driven from.
//
// ON EVERY REBUILD, NOT ONCE. rebuildMergedMusic() returns early while the merge is still valid, so this
// runs only when the merge has actually been recomputed — which is exactly when a preference changed, a
// server's artists landed, or an album's track list arrived. That last one is why a one-shot stamped
// migration would be wrong here and a repeatable idempotent pass is right: a remote copy has no track ids
// until it is fetched, so an album's records become movable long after the first merge. MusicRemap.h has the
// full argument, and PcGameRemap's header makes the same one for the same reason.
//
// COST WHEN THERE IS NOTHING TO DO: the groups loop below runs over merged albums only (a single-supplier
// install has none, because merge() short-circuits before it makes any), the table comes out empty, and
// applyRemap returns without opening the ini.
void HomeView::applyMusicRemap()
{
    if (!mergedMusic_.active || mergedMusic_.albumGroup.isEmpty()) return;

    QVector<MusicRemap::AlbumGroup> groups;
    groups.reserve(mergedMusic_.albumGroup.size());
    for (auto it = mergedMusic_.albumGroup.constBegin(); it != mergedMusic_.albumGroup.constEnd(); ++it)
    {
        // albumGroup stores the PRIMARY first and MusicRemap requires exactly that, so the order is carried
        // rather than re-derived — a second opinion about which copy is primary is the one thing that could
        // send every record to the copy the user did not choose.
        MusicRemap::AlbumGroup g;
        for (const QString& k : it.value())
        {
            MusicRemap::Instance in;
            in.key = k;
            if (const MusicLibrary::Album* b = MusicSupply::indexFor(k).album(k))
                for (const MusicLibrary::IndexTrack& t : b->tracks)
                {
                    MusicRemap::TrackId id;
                    id.number  = t.track;   // NOT disc*n+track: the two copies may not agree on discs at all
                    id.title   = t.title;
                    // THE ONE NAME A TRACK ANSWERS TO (#204). This used to fill `playId` as well — what the
                    // player was handed, which for a server track is a signed stream url — because the
                    // resume and consumption stores keyed on that. They key on the index path now, for every
                    // route, so there is one identity and one table and nothing here mints a url at all.
                    id.indexId = t.path;
                    in.tracks.push_back(id);
                }
            g.instances.push_back(in);
        }
        groups.push_back(g);
    }
    MusicRemap::applyRemap(MusicRemap::tableFor(groups));
}

// OFF THE SIGNED URL AND ONTO THE TRACK'S OWN NAME (issue #204), for an album that is about to be LOOKED AT
// rather than played.
//
// MainWindow::adoptMusicQueueIdentities does this for the album that is about to PLAY, which is where the
// resume position is read back. It is not the only reader: a track row's progress bar has always looked up
// `resume/md5(IndexTrack::path)` (HomeView's resumeFraction, over resumeKeyFor -> the row's id, which for a
// music track IS its index path). So a Subsonic track that had been listened to half way through showed no
// bar at all — the position was banked under the stream url and nothing ever asked for it there. That is the
// clearest evidence about which of the two names was meant to be the identity, and it is fixed here, before
// the catalog is built, so the first render of the album is already right.
//
// Costs a LOCAL album exactly one `isQualified` test. Costs an album whose rows are already migrated one
// empty table (every track self-maps once the row is where it belongs, so `offer` refuses all of them and
// applyRemap returns without opening the ini) — which is also what makes calling it on every render fine.
void HomeView::applyMusicStreamRekey(const QString& albumKey)
{
    if (!Subsonic::isQualified(albumKey)) return;   // a local album's tracks were never keyed on a url
    const MusicLibrary::Album* b = MusicSupply::indexFor(albumKey).album(albumKey);
    if (!b) return;                                 // not fetched yet: nothing to name — rule 1, wait
    QVector<MusicRemap::TrackId> ids;
    ids.reserve(b->tracks.size());
    for (const MusicLibrary::IndexTrack& t : b->tracks)
        ids.push_back(MusicRemap::TrackId{ 0, QString(), MusicSupply::playUrl(t.path), t.path });
    MusicRemap::applyRemap(MusicRemap::streamKeyTable(ids));
}

void HomeView::fetchMergeSources()
{
    // The callback every supplier's artist fetch shares: whatever the outcome, repopulate the music level
    // the user is standing in. A supplier that answered adds its artists; one that refused adds nothing and
    // must not replace the rows already on screen with an error — the local library is still perfectly
    // browsable, which is the whole point of merging rather than switching. This is also the whole of "a
    // slow supplier does not stall the library": nothing waits on any of these.
    auto landed = [this] {
        if (stack_.isEmpty()) return;
        const QString t = stack_.last().item.type;
        if (t == QStringLiteral("_musicroot") || t == QString::fromLatin1(browse::kMusicArtistType)
            || t == QString::fromLatin1(browse::kMusicAlbumType))
        { mergedMusicValid_ = false; loadTop(); }
    };

    // (#194 increment 3) The two new suppliers, asked exactly as the Subsonic one is — and MARKED BEFORE
    // THE REQUEST for the same reason spelled out below.
    JellyfinMusicClient& jf = JellyfinMusicClient::instance();
    for (const JellyfinServer& srv : JellyfinServerStore::enabled())
    {
        const QString tag = QStringLiteral("jf:") + srv.id;
        if (jf.artistsLoaded(srv.id) || musicMergeFetched_.contains(tag)) continue;
        musicMergeFetched_.insert(tag);
        jf.fetchArtists(srv.id, [landed](const JellyfinMusicClient::Result&) { landed(); });
    }
    ServerMusicClient& sh = ServerMusicClient::instance();
    for (const ServerMusicClient::Shelf& s : sh.shelves())
    {
        const QString tag = QStringLiteral("ebs:") + s.id;
        if (sh.artistsLoaded(s.id) || musicMergeFetched_.contains(tag)) continue;
        musicMergeFetched_.insert(tag);
        sh.fetchArtists(s.id, [landed](const ServerMusicClient::Result&) { landed(); });
    }

    SubsonicClient& c = SubsonicClient::instance();
    for (const SubsonicServer& srv : SubsonicServerStore::list())
    {
        if (c.artistsLoaded(srv.id) || musicMergeFetched_.contains(srv.id)) continue;
        // MARKED BEFORE THE REQUEST, not after it lands. A REFUSED server leaves artistsLoaded() false for
        // ever, and this callback repopulates the level, which calls back into here - so a gate on
        // artistsLoaded alone is an unbounded request loop against a box that is already saying no.
        musicMergeFetched_.insert(srv.id);
        c.fetchArtists(srv.id, [this](const SubsonicClient::Result&) {
            // Whatever the outcome. A server that answered adds its artists; one that refused adds nothing
            // and must not replace the rows already on screen with an error - the local library is still
            // perfectly browsable, which is the whole point of merging rather than switching.
            if (stack_.isEmpty()) return;
            const QString t = stack_.last().item.type;
            if (t == QStringLiteral("_musicroot") || t == QString::fromLatin1(browse::kMusicArtistType)
                || t == QString::fromLatin1(browse::kMusicAlbumType))
            { mergedMusicValid_ = false; loadTop(); }
        });
    }
}

QString HomeView::mergedArtistPrimary(const QString& key) const
{
    if (key.isEmpty() || mergedMusic_.artistGroup.contains(key)) return key;
    for (auto it = mergedMusic_.artistGroup.constBegin(); it != mergedMusic_.artistGroup.constEnd(); ++it)
        if (it->contains(key)) return it.key();
    return key;
}

QString HomeView::mergedAlbumPrimary(const QString& key) const
{
    if (key.isEmpty() || mergedMusic_.albumGroup.contains(key)) return key;
    for (auto it = mergedMusic_.albumGroup.constBegin(); it != mergedMusic_.albumGroup.constEnd(); ++it)
        if (it->contains(key)) return it.key();
    return key;
}

QString HomeView::musicSourceLabel(const QString& sourceId) const
{
    if (sourceId.isEmpty()) return tr("This device");
    SubsonicServer srv;
    if (SubsonicServerStore::get(sourceId, srv) && !srv.name.isEmpty()) return srv.name;
    // (#194 increment 3) A Jellyfin server names itself: `ServerName` from /System/Info/Public, which is
    // what #160 stores and what its rows are already tagged with elsewhere.
    JellyfinServer jf;
    if (JellyfinServerStore::get(sourceId, jf) && !jf.name.isEmpty()) return jf.name;
    const QString shelf = ServerMusicClient::instance().nameOf(sourceId);
    if (!shelf.isEmpty()) return shelf;
    return tr("Music server");
}

browse::MusicAlbumSources HomeView::musicAlbumSourcesFor(const QString& albumKey) const
{
    browse::MusicAlbumSources out;
    if (!mergedMusic_.active || albumKey.isEmpty()) return out;

    const QStringList inst = mergedMusic_.albumInstances(albumKey);
    if (inst.size() < 2)
    {
        // Nothing merged onto this record, but there IS somewhere else it could live - so offer the join.
        out.offerManualMerge = true;
        return out;
    }
    for (const QString& k : inst)
    {
        browse::MusicAlbumSource s;
        s.albumKey = k;
        s.label    = musicSourceLabel(mergedMusic_.sourceOf.value(k));
        s.chosen   = (k == albumKey);
        // QUALITY AWARENESS WHERE IT IS FREE, and only there (#194 increment 3). The track count is in hand
        // here because it is prose that needs translating; everything else a copy can honestly claim comes
        // from MusicMerge::qualityBits, which is ONE rule over every supplier — the extension for a local
        // copy, the container and bitrate a Jellyfin server or the EverythingBox server's shelf reported,
        // and NOTHING for a Subsonic copy, whose API does not tell us. See MusicMerge.h for why a guess
        // here would defeat the whole point of the line.
        QStringList bits;
        if (const MusicLibrary::Album* b = MusicSupply::indexFor(k).album(k))
        {
            bits << tr("%n track(s)", "", b->trackCount);
            bits += MusicMerge::qualityBits(*b);
        }
        s.detail = bits.join(QString::fromUtf8(" \xc2\xb7 "));
        out.instances.push_back(s);
    }
    return out;
}

void HomeView::playMusicAlbumFromSource(const QString& albumKey)
{
    if (albumKey.isEmpty()) return;
    // A remote copy the user has never opened has no track list yet, and a queue built from it would be
    // empty - the row would look like it did nothing at all, which this codebase treats as worse than an
    // error. So fetch the one request's worth first, then play.
    if (musicNeedsAlbumFetch(albumKey))
    {
        musicFetchAlbumTracks(albumKey, [this, albumKey](bool ok, const QString& message) {
            if (!ok) { showMusicServerError(tr("Music"), message); return; }
            mergedMusicValid_ = false;
            emit playMusicAlbumRequested(albumKey, QString());
        });
        return;
    }
    emit playMusicAlbumRequested(albumKey, QString());
}

// The index a multi-album music queue is built from. See the header: merged while the merge is active, the
// owning supplier's otherwise — and the owning supplier's for the KEYLESS whole-library shuffle, which walks
// the local library and is deliberately unchanged (a shuffle spanning every server's catalogue would have to
// fetch every album on every server before it could name a single track).
const MusicLibrary::Index& HomeView::musicIndexForArtist(const QString& artistKey)
{
    if (artistKey.isEmpty() || !musicMergeActive()) return MusicSupply::indexFor(artistKey);
    rebuildMergedMusic();
    if (!mergedMusic_.active) return MusicSupply::indexFor(artistKey);
    if (mergedMusic_.idx.artist(mergedArtistPrimary(artistKey))) return mergedMusic_.idx;
    return MusicSupply::indexFor(artistKey);   // a stale route: let the owning supplier answer, or not
}

// "Play all" / "Shuffle all" on an artist whose records may live on a server.
//
// The rows are offered on a count the server gave us (browse::musicArtistCatalog says why), but a COUNT is
// not a track list: a queue built from an album nobody has opened would be empty, and the row would look
// like it did nothing at all — which this codebase treats as worse than an error. So the missing track
// lists are fetched first, exactly as playMusicAlbumFromSource does for one record, and only then does the
// queue get built. One request per unfetched album, fired together rather than chained, because the whole
// point of the verb is an hour of music and a serial chain would make the user wait album by album.
//
// A SECOND PRESS SUPERSEDES THE FIRST through its own generation counter — deliberately not musicFetchGen_,
// which guards LEVEL navigation: pressing a verb must not cancel the page's own pending populate, and
// walking away from the page must not cancel a queue the user explicitly asked for (music plays behind the
// browse surfaces, which is the whole of #193's third increment).
void HomeView::playMusicArtistQueue(const QString& artistKey, bool shuffle)
{
    const QString shown = musicMergeActive() ? mergedArtistPrimary(artistKey) : artistKey;
    const MusicLibrary::Artist* a = musicIndexForArtist(shown).artist(shown);
    if (!a) { emit playMusicQueueRequested(shown, shuffle); return; }   // stale: let the opener say so

    QStringList todo;
    for (const MusicLibrary::Album& b : a->albums)
        if (musicNeedsAlbumFetch(b.key)) todo << b.key;
    if (todo.isEmpty()) { emit playMusicQueueRequested(shown, shuffle); return; }

    const int gen = ++musicQueueFetchGen_;
    showToast(tr("Loading %n record(s)…", "", int(todo.size())), 0);   // sticky; the last reply hides it

    auto remaining = QSharedPointer<int>::create(int(todo.size()));
    auto failed    = QSharedPointer<int>::create(0);
    for (const QString& k : todo)
    {
        musicFetchAlbumTracks(k,
            [this, shown, shuffle, gen, remaining, failed](bool ok, const QString&) {
                if (gen != musicQueueFetchGen_) return;     // superseded by a later press
                if (!ok) ++(*failed);
                if (--(*remaining) > 0) return;             // still waiting on a sibling record
                hideToast();
                // A record that would not load is NOT a reason to refuse the rest: the queue is built from
                // whatever arrived, and the count is said out loud rather than silently short.
                if (*failed > 0)
                    showToast(tr("%n record(s) could not be loaded from the server.", "", *failed));
                mergedMusicValid_ = false;                  // the fetched tracks change the merged index
                emit playMusicQueueRequested(shown, shuffle);
            });
    }
}

// "These are NOT the same album" - the important half of the escape hatch, because a wrong merge is the one
// that hides a record the user owns with nothing on screen to say why.
//
// The verdict is recorded between EVERY pair in the group, not only against the copy on screen: a three-way
// merge separated only from its primary would re-fuse the other two on the next refresh, which would look
// like the row did nothing.
void HomeView::unmergeAlbumInteractive(const QString& albumKey)
{
    rebuildMergedMusic();
    const QStringList inst = mergedMusic_.albumInstances(mergedAlbumPrimary(albumKey));
    if (inst.size() < 2) return;

    QVector<QPair<QString, QString>> named;   // (album artist, title) as each supplier spells it
    for (const QString& k : inst)
        if (const MusicLibrary::Album* b = MusicSupply::indexFor(k).album(k))
            named.push_back(qMakePair(b->albumArtist, b->title));

    for (int i = 0; i < named.size(); ++i)
        for (int j = i + 1; j < named.size(); ++j)
            MusicId::setAlbumOverride(named.at(i).first, named.at(i).second,
                                      named.at(j).first, named.at(j).second, /*same*/ false);

    mergedMusicValid_ = false;
    rebuildMergedMusic();
    if (!stack_.isEmpty()) loadTop();
}

// "This IS the same album as..." - the other direction, for a match the conservative rules refused. The
// candidate list is every copy by the same ARTIST held by a DIFFERENT supplier, which is short, is exactly
// the population the user is looking at, and is never a wall of the whole library.
void HomeView::mergeAlbumInteractive(const QString& albumKey)
{
    rebuildMergedMusic();
    const QString key = mergedAlbumPrimary(albumKey);
    const MusicLibrary::Album* mine = MusicSupply::indexFor(key).album(key);
    if (!mine) return;
    const QString mySource   = mergedMusic_.sourceOf.value(key);
    const QString myArtist   = MusicId::normalizeArtist(mine->albumArtist);
    const QString myTitle    = mine->title;
    const QString myArtistIn = mine->albumArtist;

    QStringList                          rows;
    QVector<QPair<QString, QString>>     cands;   // (album artist, title) of each candidate
    for (const MusicLibrary::Artist& a : mergedMusic_.idx.artists)
        for (const MusicLibrary::Album& b : a.albums)
        {
            if (b.key == key) continue;
            if (mergedMusic_.sourceOf.value(b.key) == mySource) continue;   // never join a supplier to itself
            if (MusicId::normalizeArtist(b.albumArtist) != myArtist) continue;
            cands.push_back(qMakePair(b.albumArtist, b.title));
            rows << QStringLiteral("%1 - %2").arg(MusicLibrary::displayAlbum(b),
                                                  musicSourceLabel(mergedMusic_.sourceOf.value(b.key)));
        }
    if (cands.isEmpty()) return;

    const int pick = NavMenu::pick(MusicLibrary::displayAlbum(*mine), rows, window());
    if (pick < 0 || pick >= cands.size()) return;

    MusicId::setAlbumOverride(myArtistIn, myTitle, cands.at(pick).first, cands.at(pick).second, /*same*/ true);
    mergedMusicValid_ = false;
    rebuildMergedMusic();
    if (!stack_.isEmpty()) loadTop();
}

// ---- Subsonic music servers (issue #193, increment 5) -------------------------------------------------
//
// The levels below are the shape openOpdsCatalogsLevel/fetchOpdsFeed already established for a self-hosted
// BOOK server, over the SAME three builders #74 renders the local library with. There is deliberately no
// second artist list, album row, track row or player anywhere in this feature: a server supplies a
// MusicLibrary::Index (Subsonic.h), and every level below a server is one of musicArtistsCatalog,
// musicArtistCatalog and musicAlbumCatalog - the very functions the local library uses.
//
// Each level fetches exactly one request's worth of data and renders what it has. `musicFetchGen_`
// supersedes an in-flight fetch the way opdsFetchGen_ does, so a reply that lands after the user has
// navigated away is dropped instead of overwriting the level they are now standing in.

// A readable one-row failure instead of a blank shelf. The message is the SERVER's own words or one of
// SubsonicClient's transport sentences - never a request, because a Subsonic request url carries the user's
// token and salt (SubsonicClient.h says why at length).
void HomeView::showMusicServerError(const QString& title, const QString& why)
{
    MediaCatalog c;
    c.title = title.isEmpty() ? tr("Music") : title;
    MediaItem info;
    info.type  = QStringLiteral("info");           // non-actionable guidance row
    info.title = why.isEmpty() ? tr("Couldn't reach that music server.") : why;
    c.items.push_back(info);
    showSyntheticCatalog(c);
}

void HomeView::showMusicLoading(const QString& title)
{
    MediaCatalog c;
    c.title = title.isEmpty() ? tr("Music") : title;
    MediaItem info;
    info.type  = QStringLiteral("info");
    info.title = tr("Loading...");
    c.items.push_back(info);
    showSyntheticCatalog(c);
}

// Artwork arrives after the rows do - a cover is a second request per album. Rather than re-render once per
// cover that lands (which would rebuild the model under the user's selection dozens of times), each landing
// arms ONE debounced repopulate. prefetchAlbumCover fires its callback only when new bytes were actually
// stored, so a level whose covers are all cached schedules nothing and this cannot loop.
void HomeView::scheduleMusicArtRefresh()
{
    if (musicArtRefreshPending_) return;
    musicArtRefreshPending_ = true;
    QTimer::singleShot(400, this, [this] {
        musicArtRefreshPending_ = false;
        if (stack_.isEmpty()) return;
        const QString t = stack_.last().item.type;
        if (t == QStringLiteral("_musicserver") || t == QStringLiteral("_musicartist")
            || t == QStringLiteral("_musicalbum") || t == QStringLiteral("_musicroot"))
            loadTop();
    });
}

void HomeView::prefetchAlbumCovers(const QVector<MusicLibrary::Album>& albums)
{
    for (const MusicLibrary::Album& b : albums)
        musicPrefetchCover(b.key, [this] { scheduleMusicArtRefresh(); });
}

void HomeView::openMusicServersLevel()
{
    if (xmbMode_) { atXmbRoot_ = false; if (xmb_) xmb_->setAtRoot(false); }
    Level lvl;
    lvl.addon = nullptr; lvl.detail = true; lvl.title = tr("Music Servers");
    lvl.item.id = QStringLiteral("_musicservers");
    lvl.item.type = QStringLiteral("_musicservers");
    lvl.item.expandable = true;
    lvl.item.mime = QString::fromLatin1(browse::kMusicServersPrefix);   // keyless: the one door
    stack_.push_back(lvl);
    populateMusicServers();
}

void HomeView::populateMusicServers()
{
    QStringList ids, names, urls;
    for (const SubsonicServer& s : SubsonicServerStore::list())
        { ids << s.id; names << s.name; urls << s.url; }
    showSyntheticCatalog(browse::musicServersCatalog(ids, names, urls));
}

void HomeView::openMusicServerLevel(const QString& serverId)
{
    SubsonicServer srv;
    if (!SubsonicServerStore::get(serverId, srv)) return;   // removed out from under the row
    if (xmbMode_) { atXmbRoot_ = false; if (xmb_) xmb_->setAtRoot(false); }
    Level lvl;
    lvl.addon = nullptr; lvl.detail = true;
    lvl.title = srv.name.isEmpty() ? tr("Music server") : srv.name;
    lvl.item.id = QStringLiteral("_musicserver");
    lvl.item.type = QStringLiteral("_musicserver");
    lvl.item.expandable = true;
    lvl.item.mime = QString::fromLatin1(browse::kMusicServerPrefix) + serverId;  // Back repopulates
    stack_.push_back(lvl);
    populateMusicServer(serverId);
}

void HomeView::populateMusicServer(const QString& serverId)
{
    SubsonicClient& c = SubsonicClient::instance();
    const QString title = stack_.isEmpty() ? tr("Music") : stack_.last().title;
    if (!c.artistsLoaded(serverId))
    {
        const int gen = ++musicFetchGen_;
        showMusicLoading(title);
        c.fetchArtists(serverId, [this, serverId, title, gen](const SubsonicClient::Result& r) {
            if (gen != musicFetchGen_) return;                 // superseded by a newer navigation
            if (!r.ok) { showMusicServerError(title, r.message); return; }
            renderMusicServer(serverId);
        });
        return;
    }
    renderMusicServer(serverId);
}

void HomeView::renderMusicServer(const QString& serverId)
{
    const MusicLibrary::Index& idx = SubsonicClient::instance().index(serverId);
    // The SAME builder the local library's artist list uses. `note` explains an EMPTY server rather than
    // leaving a blank shelf - the reason that parameter exists at all - and no servers door is offered here
    // because we are already inside one.
    browse::MusicEmptyNote note;
    if (idx.artists.isEmpty())
        note.text = tr("This music server has no artists in it yet.");
    MediaCatalog cat = browse::musicArtistsCatalog(idx, note, musicCover(), /*musicServerCount*/ 0);
    if (!stack_.isEmpty()) cat.title = stack_.last().title;
    showSyntheticCatalog(cat);
}


void HomeView::openMusicArtistLevel(const QString& artistKey)
{
    if (xmbMode_) { atXmbRoot_ = false; if (xmb_) xmb_->setAtRoot(false); }
    const MusicLibrary::Artist* a = MusicSupply::indexFor(artistKey).artist(artistKey);
    Level lvl;
    lvl.addon = nullptr; lvl.detail = true;
    lvl.title = a ? MusicLibrary::displayArtist(*a) : tr("Music");
    lvl.item.id = QStringLiteral("_musicartist");
    lvl.item.type = QStringLiteral("_musicartist");
    lvl.item.expandable = true;
    lvl.item.mime = QString::fromLatin1(browse::kMusicArtistPrefix) + artistKey; // loadTop() repopulates on Back
    stack_.push_back(lvl);
    populateMusicArtist(artistKey);
}

void HomeView::populateMusicArtist(const QString& artistKey)
{
    // SEVERAL SUPPLIERS: the artist row on screen may stand for a local bucket AND one or more remote ones,
    // and each remote one owes a getArtist before its albums exist. Fire them all, render when the last
    // lands, and never blank a discography we already have while waiting.
    if (musicMergeActive())
    {
        rebuildMergedMusic();
        const QString shown = mergedArtistPrimary(artistKey);
        QStringList todo;
        for (const QString& k : mergedMusic_.artistInstances(shown))
            if (musicNeedsArtistFetch(k) && !musicMergeArtistFetched_.contains(k))
                todo << k;
        if (todo.isEmpty()) { renderMusicArtist(shown); return; }

        const int     gen   = ++musicFetchGen_;
        const QString title = stack_.isEmpty() ? tr("Music") : stack_.last().title;
        const MusicLibrary::Artist* have = mergedMusic_.idx.artist(shown);
        if (have && !have->albums.isEmpty()) renderMusicArtist(shown);   // something to look at already
        else                                 showMusicLoading(title);

        auto remaining = QSharedPointer<int>::create(int(todo.size()));
        for (const QString& k : todo)
        {
            musicMergeArtistFetched_.insert(k);       // before the request; see fetchMergeSources
            musicFetchArtistAlbums(k,
                [this, shown, gen, remaining](bool, const QString&) {
                    if (gen != musicFetchGen_) return;             // superseded by a newer navigation
                    if (--(*remaining) > 0) return;                // still waiting on a sibling supplier
                    mergedMusicValid_ = false;
                    rebuildMergedMusic();
                    renderMusicArtist(mergedArtistPrimary(shown));
                });
        }
        return;
    }
    // Which supplier owns this key is decided in ONE place, structurally - see MusicSupply / Subsonic::parse.
    // A local key can never parse as a qualified one, so this is a routing question rather than a guess.
    if (musicNeedsArtistFetch(artistKey))
    {
        const int gen = ++musicFetchGen_;
        const QString title = stack_.isEmpty() ? tr("Music") : stack_.last().title;
        showMusicLoading(title);
        musicFetchArtistAlbums(artistKey,
            [this, artistKey, title, gen](bool ok, const QString& message) {
                if (gen != musicFetchGen_) return;
                if (!ok) { showMusicServerError(title, message); return; }
                renderMusicArtist(artistKey);
            });
        return;
    }
    renderMusicArtist(artistKey);
}

void HomeView::renderMusicArtist(const QString& artistKey)
{
    if (musicMergeActive())
    {
        rebuildMergedMusic();
        const QString shown = mergedArtistPrimary(artistKey);
        showSyntheticCatalog(browse::musicArtistCatalog(mergedMusic_.idx, shown, musicCover()));
        if (const MusicLibrary::Artist* a = mergedMusic_.idx.artist(shown)) prefetchAlbumCovers(a->albums);
        return;
    }
    const MusicLibrary::Index& idx = MusicSupply::indexFor(artistKey);
    showSyntheticCatalog(browse::musicArtistCatalog(idx, artistKey, musicCover()));
    if (const MusicLibrary::Artist* a = idx.artist(artistKey)) prefetchAlbumCovers(a->albums);
}

void HomeView::openMusicAlbumLevel(const QString& albumKey)
{
    if (xmbMode_) { atXmbRoot_ = false; if (xmb_) xmb_->setAtRoot(false); }
    const MusicLibrary::Album* b = MusicSupply::indexFor(albumKey).album(albumKey);
    Level lvl;
    lvl.addon = nullptr; lvl.detail = true;
    lvl.title = b ? MusicLibrary::displayAlbum(*b) : tr("Music");
    lvl.item.id = QStringLiteral("_musicalbum");
    lvl.item.type = QStringLiteral("_musicalbum");
    lvl.item.expandable = true;
    lvl.item.mime = QString::fromLatin1(browse::kMusicAlbumPrefix) + albumKey;
    stack_.push_back(lvl);
    populateMusicAlbum(albumKey);
}

void HomeView::populateMusicAlbum(const QString& albumKey)
{
    // The merged row is rendered under ONE of its copies' keys (MusicMerge.h says why nothing new is minted),
    // so resolve to that copy first: a route saved before the preference changed, or a "Play from ..." row
    // followed by a Back, can perfectly well name a sibling.
    QString key = albumKey;
    if (musicMergeActive()) { rebuildMergedMusic(); key = mergedAlbumPrimary(albumKey); }
    const QString albumKeyResolved = key;
    if (musicNeedsAlbumFetch(albumKeyResolved))
    {
        const int gen = ++musicFetchGen_;
        const QString title = stack_.isEmpty() ? tr("Music") : stack_.last().title;
        showMusicLoading(title);
        musicFetchAlbumTracks(albumKeyResolved,
            [this, albumKeyResolved, title, gen](bool ok, const QString& message) {
                if (gen != musicFetchGen_) return;
                if (!ok) { showMusicServerError(title, message); return; }
                mergedMusicValid_ = false;
                renderMusicAlbum(albumKeyResolved);
            });
        return;
    }
    renderMusicAlbum(albumKeyResolved);
}

void HomeView::renderMusicAlbum(const QString& albumKey)
{
    if (musicMergeActive())
    {
        rebuildMergedMusic();
        const QString shown = mergedAlbumPrimary(albumKey);
        // #204: EVERY copy, not just the one on screen. rebuildMergedMusic's own pass then moves each copy's
        // records onto the primary's — but it can only move what is filed under a copy's INDEX identity, so a
        // sibling still banked under its stream url has to be brought onto its own name FIRST. Ordered
        // deliberately: this call before the tail of rebuildMergedMusic would be one rebuild too late, so it
        // happens here, where the album that is about to be rendered is known.
        for (const QString& k : mergedMusic_.albumInstances(shown)) applyMusicStreamRekey(k);
        applyMusicRemap();
        showSyntheticCatalog(browse::musicAlbumCatalog(mergedMusic_.idx, shown, musicCover(),
                                                       musicAlbumSourcesFor(shown)));
        if (const MusicLibrary::Album* b = mergedMusic_.idx.album(shown))
            musicPrefetchCover(b->key, [this] { scheduleMusicArtRefresh(); });
        return;
    }
    applyMusicStreamRekey(albumKey);
    const MusicLibrary::Index& idx = MusicSupply::indexFor(albumKey);
    showSyntheticCatalog(browse::musicAlbumCatalog(idx, albumKey, musicCover()));
    if (const MusicLibrary::Album* b = idx.album(albumKey))
        musicPrefetchCover(b->key, [this] { scheduleMusicArtRefresh(); });
}

// The CLASSICAL VIEW (#196, part 2) - Composers -> that composer's Works -> that work's Tracks. Three more
// synthetic levels set up exactly like the three above, deliberately: the same detail-root-with-an-
// expandable-container shape is what makes loadTop() rebuild each of them on Back, and what keeps a finished
// rescan able to refresh whichever one the user is standing in. Nothing here plays anything - a work's rows
// are the same track rows an album shows and route through the same album queue.
void HomeView::openMusicComposersLevel()
{
    if (xmbMode_) { atXmbRoot_ = false; if (xmb_) xmb_->setAtRoot(false); }
    Level lvl;
    lvl.addon = nullptr; lvl.detail = true;
    lvl.title = tr("Composers");
    lvl.item.id = QStringLiteral("_musiccomposers");
    lvl.item.type = QStringLiteral("_musiccomposers");
    lvl.item.expandable = true;
    lvl.item.mime = QString::fromLatin1(browse::kMusicComposersPrefix);   // keyless: the one door
    stack_.push_back(lvl);
    populateMusicComposers();
}

void HomeView::populateMusicComposers()
{ showSyntheticCatalog(browse::musicComposersCatalog(MusicLibrary::index())); }

void HomeView::openMusicComposerLevel(const QString& composerKey)
{
    if (xmbMode_) { atXmbRoot_ = false; if (xmb_) xmb_->setAtRoot(false); }
    const MusicLibrary::Composer* c = MusicLibrary::index().composer(composerKey);
    Level lvl;
    lvl.addon = nullptr; lvl.detail = true;
    lvl.title = c ? c->name : tr("Composers");
    lvl.item.id = QStringLiteral("_musiccomposer");
    lvl.item.type = QStringLiteral("_musiccomposer");
    lvl.item.expandable = true;
    lvl.item.mime = QString::fromLatin1(browse::kMusicComposerPrefix) + composerKey;
    stack_.push_back(lvl);
    populateMusicComposer(composerKey);
}

void HomeView::populateMusicComposer(const QString& composerKey)
{ showSyntheticCatalog(browse::musicComposerCatalog(MusicLibrary::index(), composerKey)); }

void HomeView::openMusicWorkLevel(const QString& workKey)
{
    if (xmbMode_) { atXmbRoot_ = false; if (xmb_) xmb_->setAtRoot(false); }
    const MusicLibrary::ComposerWork* w = MusicLibrary::index().work(workKey);
    Level lvl;
    lvl.addon = nullptr; lvl.detail = true;
    lvl.title = (w && !w->title.isEmpty()) ? w->title : tr("Composers");
    lvl.item.id = QStringLiteral("_musicwork");
    lvl.item.type = QStringLiteral("_musicwork");
    lvl.item.expandable = true;
    lvl.item.mime = QString::fromLatin1(browse::kMusicWorkPrefix) + workKey;
    stack_.push_back(lvl);
    populateMusicWork(workKey);
}

void HomeView::populateMusicWork(const QString& workKey)
{ showSyntheticCatalog(browse::musicWorkCatalog(MusicLibrary::index(), workKey)); }

// A finished scan installed a new index (MainWindow::rescanMusicLibrary). Refresh whichever music level the
// user is standing in — the same rule as onLocalLibraryChanged: never reload a level that is not showing
// this data, and refresh a catalogue ROOT so the category can appear for the first time.
void HomeView::refreshMusicLevels()
{
    mergedMusicValid_ = false;
    refreshMusicShelves();   // an addon may have been connected or switched off since the last look (#194 inc 3)
    onMusicLibraryChanged();
}

void HomeView::onMusicLibraryChanged()
{
    mergedMusicValid_ = false;   // a fresh local index changes what the merge is OVER (#194)
    if (stack_.isEmpty()) return;
    const auto& top = stack_.last();
    if (top.item.type == QStringLiteral("_musicroot"))   { populateMusicArtists(); return; }
    if (top.item.type == QStringLiteral("_musicartist"))
        { populateMusicArtist(browse::musicKeyOf(top.item.mime, browse::kMusicArtistPrefix)); return; }
    if (top.item.type == QStringLiteral("_musicalbum"))
        { populateMusicAlbum(browse::musicKeyOf(top.item.mime, browse::kMusicAlbumPrefix)); return; }
    if (top.item.type == QStringLiteral("_musicservers")) { populateMusicServers(); return; }
    if (top.item.type == QStringLiteral("_musicserver"))
        { populateMusicServer(browse::musicKeyOf(top.item.mime, browse::kMusicServerPrefix)); return; }
    if (top.item.type == QStringLiteral("_musiccomposers")) { populateMusicComposers(); return; }
    if (top.item.type == QStringLiteral("_musiccomposer"))
        { populateMusicComposer(browse::musicKeyOf(top.item.mime, browse::kMusicComposerPrefix)); return; }
    if (top.item.type == QStringLiteral("_musicwork"))
        { populateMusicWork(browse::musicKeyOf(top.item.mime, browse::kMusicWorkPrefix)); return; }
    // Anywhere else: nothing to do, and deliberately NOT a loadTop() the way onLocalLibraryChanged does one.
    // The video library's arrival can add a FOLDER to a catalogue root, which a reload surfaces; music's
    // arrival cannot add anything to a level that is not one of the three above. Whether the Music TAB is
    // offered at all is decided by the root EXISTING (MusicLibrary::hasLibrary), which is already true before
    // any scan runs — and a folder chosen mid-session reaches the tab strip through refresh() on the way back
    // out of Settings. So reloading someone's current level here would be pure churn.
}

// ---- The ONE PC Games folder --------------------------------------------------------------------------
//
// This replaced four folders (Steam / Epic Games / GOG / Battle.net), each built by its own catalog builder.
// The same game appeared in as many of them as owned it, under four unrelated ids, so the user's star, marks
// and play time attached to whichever copy they happened to launch it from and the other copies looked
// untouched. One folder, one item per game, every copy a SOURCE on that item.

// ONE sweep of every launcher. It is a struct rather than four calls at each use site because there are two
// consumers per refresh — the folder itself and the remap's candidate ids — and they were each doing their
// own full scan: two enumerations of Steam's steamapps, Epic's manifests, GOG's registry and Battle.net's
// install records for one keypress. The scans are disk/registry work proportional to the library, so the
// duplicate is exactly the cost that grows with the user who has the most to lose.
struct PcLibScan
{
    QList<SteamGame>       steam;
    QList<EpicGame>        epic;
    QList<GogGame>         gog;
    QList<BattleNetGame>   bnet;
    QVector<DownloadedItem> downloads;
    // Owned-but-not-installed on Steam. ownedGamesCached is network-free (populatePcGames arms the
    // background refresh separately), so gathering it here never blocks the GUI thread.
    QList<SteamGame>       steamOwned;
};

// Route one launcher's fresh scan through the persisted-scan cache (issue #62). On a readable scan the fresh
// list is authoritative and becomes the new last-good cache; on an UNREADABLE one the games do not vanish —
// they come back from the cache marked unavailable (rebuilt as id+name-only structs, which are not launchable,
// which is correct: an unreadable store cannot be launched from). The full fresh struct (exe / install dir) is
// preserved on the readable path by looking each returned id back up in the fresh list.
//
// `readable` is the caller's classification, and it is the whole crux of the feature — see the header of
// PcScanCache.h. Steam has a truly INDEPENDENT readability signal (a Steam CLIENT install was found), so its
// empty-but-installed case is correctly "genuinely empty" and clears the cache. GOG and Battle.net expose no
// signal separate from "found games" (their isAvailable() IS a games scan), so an empty result is treated as
// unreadable there — the honest consequence is that a genuinely-emptied GOG/Battle.net library keeps showing
// from cache until a non-empty scan replaces it. Epic's isAvailable() (manifests dir with items) is
// effectively the same as non-empty, so it is grouped with those two.
template <class G>
static QList<G> reconcileLauncherScan(const QString& source, bool readable, const QList<G>& fresh,
                                      const std::function<QString(const G&)>& idOf)
{
    pcscan::ScanResult r;
    r.status = readable ? pcscan::ScanStatus::Ok : pcscan::ScanStatus::Unreadable;
    QHash<QString, G> byId;
    for (const G& g : fresh) { r.entries.push_back({ idOf(g), g.name, true }); byId.insert(idOf(g), g); }

    QList<G> out;
    for (const pcscan::ScanEntry& e : pcscan::reconcile(source, r))
    {
        G g = byId.value(e.id);   // full fields on the readable path; a blank struct on the cache-fallback path
        g.name      = e.name;     // authoritative from the entry either way (cache carries the last-good name)
        g.available = e.available;
        out << g;
    }
    return out;
}

static PcLibScan scanPcLibrary()
{
    PcLibScan s;
    s.steam = reconcileLauncherScan<SteamGame>(QStringLiteral("steam"), SteamLibrary::isAvailable(),
        SteamLibrary::installedGames(), [](const SteamGame& g) { return g.appid; });
    s.epic = reconcileLauncherScan<EpicGame>(QStringLiteral("epic"), EpicLibrary::isAvailable(),
        EpicLibrary::installedGames(), [](const EpicGame& g) { return g.appName; });
    {
        const QList<GogGame> fresh = GogLibrary::installedGames();
        s.gog = reconcileLauncherScan<GogGame>(QStringLiteral("gog"), !fresh.isEmpty(), fresh,
            [](const GogGame& g) { return g.id; });
    }
    {
        const QList<BattleNetGame> fresh = BattleNetLibrary::installedGames();
        // Battle.net's cache id must be stable and distinct per game — a code-less title has no code, so it
        // keys on its name, matching how legacyLaunchId builds a code-less id.
        s.bnet = reconcileLauncherScan<BattleNetGame>(QStringLiteral("battlenet"), !fresh.isEmpty(), fresh,
            [](const BattleNetGame& g) { return g.code.isEmpty() ? g.name : g.code; });
    }
    s.downloads  = DownloadsStore::list();
    s.steamOwned = SteamLibrary::ownedGamesCached(Settings::steamWebApiKey(), Settings::steamId());
    return s;
}

// Every ingredient the folder is built from, gathered HERE and nowhere else. populatePcGames and the
// re-derivation path (pcSourcesForId) both call this, so a tile's sources and a re-derived game's sources are
// the same function of the same library — the property the whole feature rests on, since the re-derived list
// is what Play uses for a favourite that outlived the session that built the tile.
//
// `pre` is a scan the caller already holds (see the header); without one this does its own, which is what
// every re-derivation call does.
MediaCatalog HomeView::pcLibraryCatalog(const QString& query, const QString& launcherFilter,
                                        const PcLibScan* pre) const
{
    PcLibScan own;
    if (!pre) { own = scanPcLibrary(); pre = &own; }
    // Downloaded copies (PcGameStore's record of what we fetched and where it landed). The Downloads store is
    // the enumerable half — PcGameStore is keyed by id with no listing — so it names the games and the store
    // supplies the CURRENT exe, which survives the game being reinstalled somewhere else. `label` doubles as
    // the copy's title (pcGamesCatalog groups on it and shows it as the picker row), so the release name is
    // kept verbatim.
    QVector<pcgame::PcGameSource> downloaded;
    for (const DownloadedItem& d : pre->downloads)
    {
        if (d.kind != QStringLiteral("pcgame")) continue;
        pcgame::PcGameSource s;
        s.kind        = pcgame::PcGameSource::Downloaded;
        s.addonItemId = d.key.isEmpty() ? d.path : d.key;
        const PcGameStore::Entry e = PcGameStore::get(s.addonItemId);
        s.exePath = !e.exe.isEmpty() ? e.exe : d.path;
        // pcgame::downloadedTitle and NOT an inline ternary: this string is what pcGamesCatalog GROUPS
        // this copy under (its tile id is pcgame::itemId of it), and populatePcGames feeds the SAME call
        // to remapTable as this copy's destination. Written out twice they drifted for exactly the
        // titleless records — see the header note on downloadedTitle.
        s.label   = pcgame::downloadedTitle(d.title, d.path);
        // Ready means "launches NOW, with no download". A recorded exe that no longer exists does not.
        s.ready   = !s.exePath.isEmpty() && QFileInfo::exists(s.exePath);
        downloaded.push_back(s);
    }
    // The owned-but-not-installed Steam list rides the scan (creds-gated, TTL-cached, network-free);
    // populatePcGames arms the background refresh that fills it.
    return browse::pcGamesCatalog(pre->steam, pre->epic, pre->gog, pre->bnet,
                                  downloaded, query, launcherFilter, {}, pre->steamOwned);
}

// Drill into the synthetic "PC Games" console (a child of the Games catalog). Pushed as a detail level so
// Back returns to the Games console list.
void HomeView::openPcGamesConsole(const MediaItem& consoleItem)
{
    if (xmbMode_) { atXmbRoot_ = false; if (xmb_) xmb_->setAtRoot(false); }
    Level lvl;
    lvl.addon = nullptr; lvl.detail = true; lvl.item = consoleItem; lvl.title = tr("PC Games");
    stack_.push_back(lvl);
    populatePcGames(); // also re-run by loadTop() when Back returns to this level
}

// The top level is still the synthetic PC Games console — the async owned-games re-present only fires while it
// is (the browse-side analogue of MainWindow::themedPanelIsTop: a late reply must never rebuild an unrelated
// view).
bool HomeView::atPcGamesConsole() const
{
    return !stack_.isEmpty() && stack_.last().detail
        && stack_.last().item.mime == QStringLiteral("pcgames:console");
}

// (Re)build the PC Games grid/column natively from the local library (no addon request).
void HomeView::populatePcGames(bool runRemap)
{
    const QString query = stack_.isEmpty() ? QString() : stack_.last().query;

    // ONE scan of every launcher, shared by the remap's candidate ids and the folder itself. Both used to do
    // their own, which meant enumerating Steam, Epic, GOG and Battle.net TWICE per refresh.
    const PcLibScan scan = scanPcLibrary();

    // THE REMAP, on every REFRESH that populates this folder — not once behind a schema stamp. Records are
    // stored under a HASH of the id, so the only way to find a game's old records is to derive its old ids
    // from the library it is in RIGHT NOW; a game that is not installed today contributes no candidate, and a
    // one-shot pass would mark itself done and strand those records forever. Running it every refresh costs a
    // few hundred hash lookups and migrates a reinstalled game the moment it reappears. applyRemap is
    // idempotent by construction, so running it always is safe (see PcGameRemap.h).
    //
    // NOT on a query change (runRemap == false; see the header). Typing in the in-folder search box
    // repopulates every 300 ms, and a keystroke cannot alter the library the table is derived from — that
    // pass would rebuild and re-apply an identical table, walking the ini once per letter.
    //
    // It is deliberately fed the UNFILTERED library: the table must not depend on what the user typed into the
    // search box, or a filtered refresh would migrate only the games matching that query.
    if (runRemap)
    {
        QVector<QPair<QString, QString>> lib;
        for (const SteamGame& g : scan.steam)
            lib << qMakePair(QStringLiteral("steam:") + g.appid, g.name);
        for (const EpicGame& g : scan.epic)
            lib << qMakePair(QStringLiteral("epic:") + g.appName, g.name);
        for (const GogGame& g : scan.gog)
            lib << qMakePair(QStringLiteral("gog:") + g.id, g.name);
        // The code-less form is the one pcgame::legacyLaunchId reproduces for a Battle.net source, so the id
        // a launch banks its records under is a key of this table by construction (probe_browse pins it).
        for (const BattleNetGame& g : scan.bnet)
            lib << qMakePair(QStringLiteral("bnet:") + (g.code.isEmpty() ? g.name : g.code), g.name);
        // OWNED-but-not-installed on Steam. It is in the same scan and it keys exactly like an installed
        // Steam entry ("steam:<appid>"), so leaving it out meant a pre-branch favourite on a game the user
        // owns and has not installed stayed under its per-launcher id — a second, stale star sitting beside
        // the merged tile until the day they install it. An appid that is BOTH owned and installed
        // contributes the same pair twice, which a QHash collapses; the destination is the same either way.
        for (const SteamGame& g : scan.steamOwned)
            lib << qMakePair(QStringLiteral("steam:") + g.appid, g.name);
        for (const DownloadedItem& d : scan.downloads)
        {
            if (d.kind != QStringLiteral("pcgame")) continue;
            // THE ID THIS COPY'S RECORDS ACTUALLY BANK UNDER. A downloaded copy launches through
            // openRecent with `addonItemId`, which pcLibraryCatalog builds as `key` when there is one and
            // the PATH otherwise (DownloadsStore documents `key` as "empty -> use path"). Requiring a
            // non-empty key here therefore excluded a whole class of real records — keyless downloads —
            // from the table while the folder happily built tiles for them. The same fallback the launch
            // uses is the only candidate that can migrate them, so it is the one used. With NEITHER a key
            // nor a path there is no id at all, and rule 1 says such an entry is absent from the table,
            // never mapped from an empty key.
            const QString oldId = d.key.isEmpty() ? d.path : d.key;
            if (oldId.isEmpty()) continue;
            // The title half is downloadedTitle, the SAME call pcLibraryCatalog groups the tile on.
            lib << qMakePair(oldId, pcgame::downloadedTitle(d.title, d.path));
        }
        pcgame::applyRemap(pcgame::remapTable(lib));
    }

    // The launcher filter (issue #44). `launcherFilter` has always been a parameter of pcGamesCatalog and
    // has always worked; nothing offered it, so the "filter inside the folder" that justified deleting the
    // four per-launcher folders never appeared. It is FOLDER STATE, not a setting: it belongs to this level
    // the way a search query does, it is set and cleared from the row that displays it, and a persisted
    // copy would silently hide most of the library on the next launch with no visible cause.
    //
    // Which launchers to OFFER is decided from the same scan the folder is built from, so the menu can
    // never list a launcher this machine has nothing in.
    pcLaunchersAvailable_ = browse::pcLaunchersPresent(scan.steam, scan.epic, scan.gog, scan.bnet,
                                                       scan.steamOwned);
    MediaCatalog cat = pcLibraryCatalog(query, pcLauncherFilter_, &scan);
    // Pinned at the TOP, and shown unconditionally — including when the filter has emptied the folder,
    // which is exactly when it must still be reachable to clear. It is inserted HERE and not inside
    // pcLibraryCatalog because that builder is also the re-derivation path (pcSourcesForId), which looks a
    // game up by id and has no business seeing a control row.
    cat.items.insert(0, browse::pcLauncherFilterRow(pcLauncherFilter_));
    showSyntheticCatalog(cat);

    // BACKGROUND: with a key+SteamID configured and the cache stale, fetch the owned library off the GUI
    // thread (async, 8s reply timeout). On completion — ONLY if this console is STILL the top level — re-present
    // so the owned entries appear. A fresh cache / no key -> ownedGamesFetch no-ops (also what stops the
    // re-present loop). Failures stay silent + TTL-cached (the folder never surfaces an error).
    // In-flight dedup: a generation stamp bumped on every populatePcGames. A late reply from a superseded
    // populate (rapid Back/re-enter or filter typing fires several) is dropped — only the latest fetch
    // re-presents, so stale replies can't stack rebuilds or fight over the cursor.
    const int gen = ++ownedFetchGen_;
    SteamLibrary::ownedGamesFetch(Settings::steamWebApiKey(), Settings::steamId(), this,
                                  [this, gen](const QVector<SteamGame>&) {
        if (gen != ownedFetchGen_ || !atPcGamesConsole()) return; // superseded / navigated away
        // Cursor preserve: an owned game can MERGE INTO an existing tile (it is a source, not a row of its
        // own), so unlike the old Steam console the re-present is not a pure append and row indices can move.
        // Keep the selection on the same GAME by id, falling back to the row when the id has gone.
        const int keepRow = grid_ ? grid_->currentRow() : -1;
        const QString keepId = (keepRow >= 0 && keepRow < items_.size()) ? items_[keepRow].id : QString();
        populatePcGames();
        int row = -1;
        if (!keepId.isEmpty())
            for (int i = 0; i < items_.size(); ++i) if (items_[i].id == keepId) { row = i; break; }
        if (row < 0 && keepRow > 0 && grid_ && keepRow < grid_->count()) row = keepRow;
        if (row > 0 && grid_ && row < grid_->count())
        {
            grid_->setCurrentRow(row);
            grid_->scrollToItem(grid_->item(row), QAbstractItemView::PositionAtCenter);
        }
    });
}

// The launcher filter's menu (issue #44). A NavMenu from the nav kit — an in-window child overlay that a
// D-pad reaches, never a QDialog or a QComboBox: three of this app's four layouts render no widget chrome at
// all, so a dropdown above the grid would be a control most users never see and only a mouse could reach.
//
// The FOLDER ROW is what opens this, so the control lives inside the thing it controls and says, on the row
// itself, which launcher is currently showing.
void HomeView::showPcLauncherFilterMenu()
{
    const QVector<QPair<QString, QString>> choices =
        browse::pcLauncherFilterChoices(pcLaunchersAvailable_, pcLauncherFilter_);
    QStringList rows;
    rows.reserve(choices.size());
    for (const QPair<QString, QString>& c : choices) rows << c.second;

    new NavMenu(tr("Show games from:"), rows, [this, choices](int row) {
        if (row < 0 || row >= choices.size()) return;             // Back: leave the folder as it was
        const QString picked = choices.at(row).first;
        if (picked == pcLauncherFilter_) return;                  // the ticked row: nothing to rebuild
        pcLauncherFilter_ = picked;
        // Keep the cursor on the control row: it is row 0 of the rebuilt folder and it is the thing that
        // just changed, so the user can see the new state and press again without hunting for it.
        browseSelectKey_ = QStringLiteral("_pcfilter");
        populatePcGames(/*runRemap=*/false);   // a filter cannot alter the library the remap is derived from
        emit browseItemsChanged(false);        // re-sync a themed browse view (else its selection desyncs)
        browseSelectKey_.clear();              // cleared AFTER the re-sync reads it (the favourite idiom)
    }, window());
}

// ---- The source picker --------------------------------------------------------------------------------

// Is this row a MERGED PC game? The id is the test, not the mime: "pcgame" is ALSO the routing kind of a
// downloaded PC game (a Recent/Downloads row whose url is its exe), and those launch by path exactly as they
// always did. Only an id minted by pcgame::itemId names a game with no launch encoded in it.
// The retro system a game leaf belongs to, or empty when this is not a retro game. Mirrors how the filter
// resolves a system: the item's own hint first, then its file extension. "pc" is deliberately excluded —
// a romhack patches a console ROM, and a PC game has no dump for one to target.
static QString retroSystemFor(const MediaItem& it, const QString& consoleName)
{
    // A game leaf is recognised by EITHER field: `mime` carries the routing kind for a synthetic row while
    // `type` carries it for a catalog row, and a ROM reached through Recents arrives with whichever its
    // producer set. Gating on one of them only meant the verb never appeared on rows built by the other.
    const bool gameish = it.type == QStringLiteral("game") || it.mime == QStringLiteral("game");
    if (!gameish) return QString();
    if (it.id.startsWith(QStringLiteral("pcgame:"))) return QString();   // a merged PC game has no dump
    QString sys = it.systemHint.trimmed().toLower();
    if (sys.isEmpty() && !it.url.isEmpty())
    {
        const QFileInfo fi(it.url);
        // The ROM library is laid out <roms>/<system>/<game>, so the PARENT FOLDER is the system — and it is
        // the only signal that survives archiving. Most ROMs here are .7z/.zip, whose extension says
        // "archive" and nothing about the console, so trying the extension first would answer for the
        // handful of loose ROMs and silently give up on the rest.
        if (const GameSystem* s = SystemCatalog::byId(fi.absoluteDir().dirName().toLower())) sys = s->id;
        else if (const GameSystem* s2 = SystemCatalog::forExtension(fi.suffix().toLower())) sys = s2->id;
    }
    // Nothing on the item itself said which console — which is the NORMAL case for a game that has not been
    // downloaded yet. A catalog row carries a title and an id and no more: `systemHint` is stamped later, by
    // the resolve/play path, and `url` only exists once there is a file. The console is not unknown though —
    // it is the page we drilled in from, which is the same signal the play path uses to pick an emulator.
    if (sys.isEmpty() && !consoleName.isEmpty())
        if (const GameSystem* s = SystemCatalog::forConsoleName(consoleName)) sys = s->id;
    if (sys == QStringLiteral("pc")) return QString();
    return sys;
}

static bool isMergedPcGame(const MediaItem& it)
{
    return it.id.startsWith(QStringLiteral("pcgame:"));
}

QVector<pcgame::PcGameSource> HomeView::pcSourcesForId(const QString& itemId) const
{
    if (!itemId.startsWith(QStringLiteral("pcgame:"))) return {};
    // Rebuild the whole folder and take this game's sources out of it. Deliberately the SAME builder the
    // folder uses rather than a hand-rolled "find every copy of this title": the grouping rule, the label
    // disambiguation and the source ordering are all in that builder, and a second implementation of them
    // here would drift — and would drift SILENTLY, since the only symptom is a picker that offers different
    // rows than the tile did. The cost is one library scan per Play, which is what a launch already pays.
    const MediaCatalog cat = pcLibraryCatalog(QString(), QString());
    for (const MediaItem& i : cat.items) if (i.id == itemId) return i.pcSources;
    return {};
}

QVector<pcgame::PcGameSource> HomeView::pcSourcesFor(const MediaItem& it) const
{
    // A live tile carries its sources; a row rebuilt from a store (a favourite, a recent, a search result)
    // does not, because MediaItem::pcSources is not persisted — on purpose, since sources are machine state
    // and a stored copy would eventually name an install that is gone.
    if (!it.pcSources.isEmpty()) return it.pcSources;
    return pcSourcesForId(it.id);
}

void HomeView::playPcGame(const MediaItem& it)
{
    const QVector<pcgame::PcGameSource> sources = pcSourcesFor(it);
    if (sources.isEmpty())
    {
        // Not a failure to launch — the game genuinely is not in this machine's library any more (uninstalled
        // from every launcher, download deleted). Say so, rather than doing nothing.
        showToast(tr("“%1” isn't in your PC library any more — install it again, or re-download it.")
                      .arg(it.title), kFeedbackLong);
        return;
    }
    const int auto_ = pcgame::pickAutoSource(sources);
    if (auto_ >= 0) { launchPcSource(it, sources.at(auto_)); return; }

    // Ambiguous (several ready) or unsafe (none ready) -> ask. A NavMenu from the nav kit: an in-window child
    // overlay, controller-navigable, never a QDialog.
    QStringList rows;
    rows.reserve(sources.size());
    for (const pcgame::PcGameSource& s : sources)
    {
        // A not-ready row SAYS so. Without it the two states are indistinguishable and the user presses what
        // looks like Play and gets a download — the exact outcome pickAutoSource refuses to cause silently.
        QString row = s.label.isEmpty() ? tr("Unnamed source") : s.label;
        if (!s.ready) row = tr("%1  —  needs downloading first").arg(row);
        // The stream picker's one-row budget, for the same reason: a downloaded copy's label is a release
        // name, NavMenu word-wraps, and an unelided row occupies two lines and stops the list being scannable.
        if (row.size() > StremioTranslate::kMaxDescribeChars)
            row = row.left(StremioTranslate::kMaxDescribeChars - 1).trimmed() + QChar(0x2026);
        rows << row;
    }
    // THE FIX ROW (issue #44). This menu is where a wrong merge announces itself: the user pressed Play on
    // one game and was handed a list of copies, and if two of them are actually different games this list is
    // the moment they find out. Offering the cure anywhere else means remembering the problem and going to
    // look for a settings screen. Last, after every playable row, so it can never be pressed by reflex.
    //
    // It rides here as well as on the game's own page because this menu is reachable from EVERY layout — a
    // favourite, a Recent row, the XMB inline chooser — while the detail page is not.
    const int fixRow = rows.size();
    rows << tr("⚙   Wrong game? Fix this entry…");
    const MediaItem copy = it; // the overlay outlives items_ (a repopulate can run under it)
    const QVector<pcgame::PcGameSource> keep = sources;
    // Built on a fresh event-loop turn: in the themed modes this can run inside the QML view's `activated`
    // handler, and building/showing widgets from there is best deferred (the game-menu precedent).
    QMetaObject::invokeMethod(this, [this, copy, keep, rows, fixRow] {
        new NavMenu(tr("Play “%1” from:").arg(copy.title), rows, [this, copy, keep, fixRow](int row) {
            if (row == fixRow)
            {
                // A fresh turn again: this runs from inside the picker's own callback, and the fix opens
                // its own overlays on top of the one that is closing.
                QMetaObject::invokeMethod(this, [this, copy] {
                    if (fixPcGameEntry(copy)) refreshAfterPcMergeFix();
                }, Qt::QueuedConnection);
                return;
            }
            if (row >= 0 && row < keep.size()) launchPcSource(copy, keep.at(row));
        }, window());
    }, Qt::QueuedConnection);
}

// ---- The merge override, spent from the entry it is about (issue #44) -----------------------------------
//
// pcgame::setOverride shipped with no caller. The design named it "the escape hatch that makes a fuzzy
// heuristic acceptable to ship — without it, a wrong merge has no cure", and a settings screen is the one
// place it could not usefully live: the user is looking at ONE tile that is two games (or two tiles that are
// one), and the correction has to be on that tile. So it is an action on the game's own page, beside Play,
// and a row on the source picker — which is where a wrong merge announces itself, as two copies of a game
// you only own once.

// How many entries a "separate" would actually produce, by the same key the id builder uses. Offering an
// action that would visibly do nothing is worse than not offering it: the user presses it, reads a warning
// about losing history, accepts, and the folder looks identical.
static QStringList pcSeparationTags(const QVector<pcgame::PcGameSource>& sources)
{
    QStringList out;
    for (const pcgame::PcGameSource& s : sources)
    {
        // sourceName is the launcher's OWN name for this copy, verbatim — the field the merge did not touch
        // and the same string the catalog derived this copy's id from.
        const QString t = pcgame::separationTag(s.sourceName);
        if (!t.isEmpty() && !out.contains(t)) out << t;
    }
    return out;
}

bool HomeView::fixPcGameEntry(const MediaItem& it)
{
    if (!isMergedPcGame(it)) return false;
    const QVector<pcgame::PcGameSource> sources = pcSourcesFor(it);
    const QStringList tags = pcSeparationTags(sources);
    const QString norm = pcgame::normalizeTitle(it.title);

    // Is there already a verdict about this key? Only then is "undo" offered — a row that always shows and
    // usually does nothing teaches people to ignore it.
    bool hasVerdict = false;
    for (const pcgame::MergeVerdict& v : pcgame::overrides())
        if (v.a == norm || v.b == norm) { hasVerdict = true; break; }

    enum Act { Separate, Fuse, Undo };
    QVector<int> acts;
    QStringList  rows;
    if (tags.size() >= 2)
    {
        rows << tr("✂   Not one game — split into %1 entries").arg(tags.size());
        acts << Separate;
    }
    rows << tr("🔗   This is the same game as…");
    acts << Fuse;
    if (hasVerdict)
    {
        rows << tr("↩   Undo my correction for this game");
        acts << Undo;
    }

    const int row = NavMenu::pick(tr("Fix “%1”").arg(it.title), rows, window());
    if (row < 0 || row >= acts.size()) return false;

    if (acts.at(row) == Separate)
    {
        // Say plainly what a split can and cannot restore. It CANNOT re-divide what was already summed:
        // records key on a hash of the entry's id, and nothing anywhere recorded which copy earned which
        // hour. Implying otherwise would be the worse failure, because it is discovered after the fact.
        if (NavConfirm::ask(tr("Split “%1”?").arg(it.title),
                            tr("This entry was built from %1 differently-named copies. Splitting gives each "
                               "copy its own entry from now on.\n\n"
                               "Play time, marks and your star were already added together on this one entry "
                               "and CANNOT be divided back up — nothing recorded which copy earned which "
                               "hour. The new entries start fresh; the combined history stays where it is.")
                                .arg(tags.size()),
                            { tr("Split them"), tr("Cancel") }, /*focusIndex=*/1, /*cancelIndex=*/1,
                            window()) != 0)
            return false;
        // The SELF-pair: both copies normalise to the same key (that is why they merged), so the verdict is
        // recorded against that key rather than against a second title that does not exist.
        pcgame::setOverride(it.title, it.title, false);
        showToast(tr("Split “%1”. Each copy now has its own entry.").arg(it.title), kFeedbackLong);
        return true;
    }

    if (acts.at(row) == Undo)
    {
        // clearOverrideKeys, NOT clearOverride: `v.a`/`v.b` are the keys the verdict is STORED under, and
        // clearOverride normalises what it is given. normalizeTitle is not a fixed point — a stored key like
        // "batman arkham city goty" re-normalises to "batman arkham city" — so handing stored keys to the
        // raw-title entry point removed a pair nobody had written. QSettings::remove no-ops, the toast below
        // still said "Undone.", and the wrong fuse survived every retry with the loser side's favourites,
        // marks and play time stranded under an id nothing looks up again. See PcGameId.h.
        //
        // The WHOLE COMPONENT, not only the edges naming `norm`: undo restores the grouping this entry had,
        // and a chain A—B—C undone from A that left B—C behind (or left B's own self-pair standing) would
        // leave the user looking at an entry still grouped differently than before, under a toast saying it
        // was not.
        QSet<QString> group;
        for (const QString& k : pcgame::fusedKeys(norm)) group.insert(k);
        group.insert(norm);
        for (const pcgame::MergeVerdict& v : pcgame::overrides())
            if (group.contains(v.a) || group.contains(v.b)) pcgame::clearOverrideKeys(v.a, v.b);
        showToast(tr("Undone. “%1” groups the way it did before.").arg(it.title), kFeedbackLong);
        return true;
    }

    // FUSE. The other entries in the library, closest-looking first — the whole point is that the two
    // spellings are similar, so an alphabetical wall of the entire library buries the row being looked for.
    const MediaCatalog all = pcLibraryCatalog(QString(), QString());
    QStringList others;
    for (const MediaItem& i : all.items)
        if (i.id != it.id && !i.title.isEmpty()) others << i.title;
    if (others.isEmpty())
    {
        showToast(tr("There's no other PC game to merge “%1” with.").arg(it.title), kFeedbackLong);
        return false;
    }
    others = pcgame::rankMergeCandidates(it.title, others);
    const int pick = NavMenu::pick(tr("“%1” is the same game as:").arg(it.title), others, window());
    if (pick < 0 || pick >= others.size()) return false;
    const QString other = others.at(pick);

    // ---- The two ways a fuse would be ACCEPTED and then not happen. Both are refused BEFORE the confirm,
    //      because a destructive-sounding dialog followed by a success toast and an unchanged folder is
    //      worse than being told no: the user retries it, and every retry fails the same silent way.

    // A title that normalises to NOTHING — "GOTY", "!!!" — is real here: mergeKey has a whole
    // "pcgame:rawtitle/" fallback for it. setOverride returns early on such a side, so the confirm and the
    // toast would both fire over a verdict that was never written. The SPLIT direction is already guarded
    // (a title with no normalised form yields no separation tags, so the row is not offered); this is the
    // same guard on the direction that lacked it.
    const QString myNorm    = pcgame::normalizeTitle(it.title);
    const QString otherNorm = pcgame::normalizeTitle(other);
    if (myNorm.isEmpty() || otherNorm.isEmpty())
    {
        showToast(tr("“%1” can't be merged — one of these names is all punctuation or edition wording, so "
                     "there's no name left to match on.")
                      .arg(myNorm.isEmpty() ? it.title : other),
                  kFeedbackLong);
        return false;
    }

    // A side the user has already SPLIT. effectiveItemId applies SEPARATE before FUSE — a key cannot be both
    // too coarse and too fine — so the separated side would not move at all while the other side was dragged
    // onto a canonical id no separated copy carries: three entries, one of them keyed on a game that is not
    // there, after a confirm describing the migration and a toast saying they are now one entry. Refused
    // rather than silently un-splitting the other entry, which is a destructive change to a tile the user is
    // not looking at.
    const bool meSeparated    = pcgame::overrideSaysSeparate(it.title);
    const bool otherSeparated = pcgame::overrideSaysSeparate(other);
    if (meSeparated || otherSeparated)
    {
        showToast(tr("“%1” is currently split into separate entries, so it can't be merged with anything "
                     "yet. Undo that split first, then merge.")
                      .arg(meSeparated ? it.title : other),
                  kFeedbackLong);
        return false;
    }

    // WHICH history survives is decidable up front, so it is stated rather than left to be discovered.
    //
    // The surviving key is the minimum of the whole FUSED COMPONENT, not the smaller of these two titles.
    // Those differ exactly when one side was already fused with something smaller — and then the pairwise
    // answer names an entry whose records strand under its old id immediately after the dialog promised they
    // were kept. A wrong data-loss disclosure on the confirm whose entire purpose is that disclosure, so the
    // rule lives in pcgame::fuseSurvivorTitle where it is derived once and probe-tested.
    QStringList libraryTitles = others;
    libraryTitles << it.title;
    const QString keeps = pcgame::fuseSurvivorTitle(it.title, other, libraryTitles);
    QString body;
    if (keeps == it.title || keeps == other)
    {
        const QString loses = (keeps == it.title) ? other : it.title;
        body = tr("They become one entry from now on, with every way to launch either copy on it.\n\n"
                  "Play time, marks and stars recorded so far were kept per entry: the merged entry "
                  "keeps “%1”'s, and “%2”'s stay behind under its old entry. New activity is counted "
                  "together.").arg(keeps, loses);
    }
    else if (!keeps.isEmpty())
    {
        // The third-entry case: one of these two is already part of a group that “%3” anchors.
        body = tr("They become one entry from now on, with every way to launch either copy on it.\n\n"
                  "NEITHER of these keeps its recorded history: one of them was already merged into "
                  "“%3”, and that entry is the one the play time, marks and stars stay on. Both “%1”'s "
                  "and “%2”'s stay behind under their old entries. New activity is counted together.")
                   .arg(it.title, other, keeps);
    }
    else
    {
        body = tr("They become one entry from now on, with every way to launch either copy on it.\n\n"
                  "NEITHER of these keeps its recorded history: they join a group anchored by a copy that "
                  "is no longer in your library, so “%1”'s and “%2”'s play time, marks and stars stay "
                  "behind under their old entries. New activity is counted together.")
                   .arg(it.title, other);
    }
    if (NavConfirm::ask(tr("Merge “%1” and “%2”?").arg(it.title, other), body,
                        { tr("Merge them"), tr("Cancel") }, /*focusIndex=*/1, /*cancelIndex=*/1,
                        window()) != 0)
        return false;
    pcgame::setOverride(it.title, other, true);
    showToast(tr("“%1” and “%2” are now one entry.").arg(it.title, other), kFeedbackLong);
    return true;
}

// Show the result of a verdict. The entry that was being looked at may not exist any more — splitting
// replaces it with one per copy — so a page showing it has to be left, not refreshed in place.
void HomeView::refreshAfterRomInstall()
{
    // The scraped-card cache is keyed by folder, and the install just wrote a gamelist entry into one, so it
    // has to be dropped or the new game shows up without its name and art.
    GamelistStore::clearCache();
    loadTop();
    emit browseItemsChanged(false);   // re-sync a themed browse view (else its selection/metadata desync)
}

void HomeView::refreshAfterPcMergeFix()
{
    if (atPcGamesConsole()) { populatePcGames(); emit browseItemsChanged(false); return; }
    // On the game's own detail level: pop back to the folder, whose loadTop() re-runs populatePcGames (and
    // therefore the remap, which is what actually moves the records onto the new ids).
    if (stack_.size() > 1 && stack_.last().detail) { goBack(); return; }
    // Reached from a favourite on Home, or some other level entirely: nothing here to rebuild. The folder
    // rebuilds from the verdict the next time it is opened.
}

// The entry's OWN id, resolved from a row index right now. A caller that has to defer the fix past the
// current event — the themed action row must, or it rebuilds the browse model under the live delegate that
// is still emitting — resolves the index here, synchronously, and carries this instead. See
// fixPcGameEntryById.
QString HomeView::pcGameIdAt(int browseIndex) const
{
    if (browseIndex < 0 || browseIndex >= browseRowMap_.size()) return QString();
    return items_[browseRowMap_[browseIndex]].id;
}

// The fix, addressed by the entry's id rather than by a row index.
//
// A row index is only meaningful against the browseRowMap_ that produced it, and the themed detail defers
// this verb by a full event loop turn — the async owned-games re-present can land in that window and rebuild
// the map, so the stale index would resolve to a DIFFERENT game and split or merge it. The file's own
// precedent for every other library-management verb is to act on a stable key for exactly this reason.
//
// An id that is no longer in items_ means the entry went away while the call was queued: return false, which
// the caller already treats as "nothing happened, leave the page alone".
bool HomeView::fixPcGameEntryById(const QString& itemId)
{
    if (itemId.isEmpty()) return false;
    for (const MediaItem& i : items_)
        if (i.id == itemId) { const MediaItem copy = i; return fixPcGameEntry(copy); }
    return false;
}

void HomeView::launchPcSource(const MediaItem& it, const pcgame::PcGameSource& s)
{
    // Route by the source's OWN launcher, rebuilding the tile the per-launcher folders used to hand over. That
    // is not nostalgia: openLibraryItem's steam:// / Epic-URI / goggame / battlenetgame branches are the
    // launch paths this app has always used, each with its own Recent bookkeeping and (for the exe routes) the
    // monitored process that banks play time. A merged item that invented a fifth route would be the only
    // untested one.
    //
    // The records this launch accrues land under the PER-LAUNCHER id, and the remap moves the hashed ones —
    // marks, consumption, play time, the star, the resume position — onto the merged id on the next refresh
    // of this folder. The RECENT is NOT among them: applyRemap does not touch RecentStore, so this launch's
    // Recent row stays under its per-launcher key and relaunches by its own path/kind, exactly as it did
    // before the merge. That is benign (it is how every per-launcher Recent has always worked) but it is not
    // the migration, and this comment used to claim it was.
    MediaItem m;
    m.title        = it.title;
    m.thumbnailUrl = it.thumbnailUrl;
    m.type         = QStringLiteral("game");
    m.systemHint   = QStringLiteral("pc");

    if (s.kind == pcgame::PcGameSource::Downloaded || s.kind == pcgame::PcGameSource::AddonAvailable)
    {
        // A downloaded copy re-opens exactly as its Downloads row does: through PcGameStore's remembered
        // install, falling back to the recorded path. An AddonAvailable row has no local file yet, so the
        // same call is its download/install handoff — MainWindow asks or re-fetches. This is the user
        // explicitly choosing that row, which is what makes starting a download here legitimate.
        //
        // NOTE — AddonAvailable has NO PRODUCER anywhere in the tree today (pcGamesCatalog mints
        // LauncherInstalled / LauncherOwned / Downloaded only), so that half of this arm is written but
        // UNEXERCISED: nothing in the app, and no probe, has ever taken it. Read it as untested code, not
        // as covered behaviour.
        emit openRecent(s.exePath, QStringLiteral("pcgame"),
                        s.addonItemId.isEmpty() ? it.id : s.addonItemId, it.title, it.thumbnailUrl);
        return;
    }
    // THE ID THIS LAUNCH BANKS ITS RECORDS UNDER — built by pcgame::legacyLaunchId and nowhere else, so it
    // is the same string populatePcGames feeds remapTable as this copy's candidate. Built by hand here it
    // came apart exactly once and invisibly: the Battle.net arm used the MERGED display title, which for a
    // code-less title that loses the title contest to another launcher is not the name the remap keys on, so
    // the play time accrued under an id nothing would ever migrate. probe_browse pins the equality.
    const QString legacyId = pcgame::legacyLaunchId(s);

    if (s.launcher == QStringLiteral("steam"))
    {
        m.id = legacyId;
        m.mime = QStringLiteral("steamgame");
        m.url = s.launchUrl;   // rungameid to play, or install/<appid> for an owned-not-installed row
    }
    else if (s.launcher == QStringLiteral("epic"))
    {
        m.id = legacyId;
        m.mime = QStringLiteral("epicgame");
        m.url = s.launchUrl;
    }
    else if (s.launcher == QStringLiteral("gog"))
    {
        m.id = legacyId;
        m.mime = QStringLiteral("goggame");
        m.url = s.exePath;     // DRM-free: the monitored launchPcExe path
    }
    else if (s.launcher == QStringLiteral("battlenet"))
    {
        // Both routes on one mime, told apart by the url, exactly as openLibraryItem expects: a coded title
        // carries battlenet://<code>, a code-less one carries its best-effort exe. A code-less title keys on
        // BATTLE.NET'S OWN NAME for it (legacyLaunchId's fallback), which is what battleNetGamesCatalog's id
        // did, what the Recent dispatch re-resolves on, and what the remap can actually migrate.
        m.id = legacyId;
        m.mime = QStringLiteral("battlenetgame");
        m.url = s.launchUrl.isEmpty() ? s.exePath : s.launchUrl;
    }
    else
    {
        // A launcher source from a store with no route here (nothing produces one today). Fall back to
        // whatever it carries rather than silently doing nothing.
        m.id = it.id;
        m.mime = QStringLiteral("pcgame");
        m.url = s.launchUrl.isEmpty() ? s.exePath : s.launchUrl;
    }
    if (m.url.isEmpty())
    {
        showToast(tr("“%1” has no way to launch from %2.").arg(it.title, s.label), kFeedbackLong);
        return;
    }
    // Defensive: every source pcGamesCatalog builds carries either a launchId or the launcher's own name, so
    // legacyLaunchId cannot come back empty for one. Launching under an EMPTY id would key this session's
    // records on "" — the failure the remap's rule 1 exists to prevent — so fall back to the merged id, which
    // is at least the id the tile itself uses.
    if (m.id.isEmpty()) m.id = it.id;
    emit openItem(m);
}

// ---- Playlists: synthetic (addon-less) levels rooted at each catalogue's "Playlists" folder --------------

// A stable key for the catalogue at the root of the browse stack (its source addon + catalog id + type).
QString HomeView::currentCatalogKey() const
{
    if (stack_.isEmpty()) return QStringLiteral("native||");
    const Level& b = stack_.first();
    const QString aid = b.addon ? b.addon->manifest.id : QStringLiteral("native");
    return aid + QStringLiteral("|") + b.catalogId + QStringLiteral("|") + b.catalogType;
}

LoadedAddon* HomeView::addonForKey(const QString& catalogKey) const
{
    const QString aid = catalogKey.section(QLatin1Char('|'), 0, 0);
    if (aid.isEmpty() || aid == QStringLiteral("native")) return nullptr;
    for (LoadedAddon* s : mgr_->sources()) if (s->manifest.id == aid) return s;
    return nullptr;
}

// The bucket the current catalogue classifies into — the key playlists filter/create on (playlists widened
// from per-catalogue to per-category). Segment 2 of the catalogKey is the catalogType the oracle maps.
QString HomeView::currentCategoryKey() const
{
    return mediaCategory(currentCatalogKey().section(QLatin1Char('|'), 2, 2));
}

void HomeView::openPlaylistsLevel(const QString& categoryKey, bool asRoot)
{
    if (asRoot)
    {
        stack_.clear();      // opened from the bucket column: the list is the root (Back -> bucket column)
        recentView_ = false; // leave the home recents view, else activateItem routes rows through its recents path
    }
    if (xmbMode_) { atXmbRoot_ = false; if (xmb_) xmb_->setAtRoot(false); }
    Level lvl;
    lvl.addon = nullptr; lvl.detail = true; lvl.title = tr("Playlists");
    lvl.item.id = QStringLiteral("_playlists");
    lvl.item.type = QStringLiteral("_playlists");
    lvl.item.expandable = true;
    lvl.item.mime = QStringLiteral("playlists:") + categoryKey; // so loadTop() repopulates on Back
    stack_.push_back(lvl);
    populatePlaylists(categoryKey);
}

void HomeView::populatePlaylists(const QString& categoryKey)
{
    showSyntheticCatalog(browse::playlistsCatalog(PlaylistStore::forCategory(categoryKey), categoryKey));
}

void HomeView::openPlaylistLevel(const QString& playlistId)
{
    Playlist p;
    if (!PlaylistStore::get(playlistId, p)) return;
    if (xmbMode_) { atXmbRoot_ = false; if (xmb_) xmb_->setAtRoot(false); }
    Level lvl;
    // The playlist level is addon-less: category playlists may be mixed-source, so each item resolves its OWN
    // addon (playlistItemsCatalog stamps every row's sourceAddonId, which activateItem uses when the level has
    // no addon). Local-file entries re-open by path regardless.
    lvl.addon = nullptr;
    lvl.detail = true; lvl.title = p.name;
    lvl.item.id = QStringLiteral("pl:") + p.id;
    lvl.item.type = QStringLiteral("_playlist");
    lvl.item.expandable = true;
    lvl.item.mime = QStringLiteral("playlist:") + playlistId; // so loadTop() repopulates on Back
    stack_.push_back(lvl);
    populatePlaylistItems(playlistId);
}

void HomeView::populatePlaylistItems(const QString& playlistId)
{
    Playlist p; PlaylistStore::get(playlistId, p);
    showSyntheticCatalog(browse::playlistItemsCatalog(p));
}

void HomeView::createPlaylistInteractive(const QString& categoryKey)
{
    const QString name = Osk::getText(tr("Playlist name:"), QString(), QLineEdit::Normal, window()).trimmed();
    if (name.isEmpty()) return; // covers backed-out (null) too
    PlaylistStore::create(categoryKey, name);
    populatePlaylists(categoryKey); // we're on the playlists level -> refresh it (also fires browseItemsChanged)
}

// ---- Live TV (#75, increment 2) ------------------------------------------------------------------------
// The saved-IPTV-sources shelf and one source's channel list. The sources shelf is pure (the store's list ->
// liveTvSourcesCatalog); a source's channels are FETCHED fresh on open (a stale channel list across sessions
// is the thing this feature exists to avoid), parsed with the increment-1 parseM3u, and shown sectioned by
// group. A light in-session cache (liveTvEntries_/liveTvCacheSourceId_) backs Back and the favourite re-render
// so neither re-hits the network.

void HomeView::openLiveTvSourcesLevel()
{
    if (xmbMode_) { atXmbRoot_ = false; if (xmb_) xmb_->setAtRoot(false); }
    Level lvl;
    lvl.addon = nullptr; lvl.detail = true; lvl.title = tr("Live TV");
    lvl.item.id = QStringLiteral("_livetvsources");
    lvl.item.type = QStringLiteral("_livetvsources");
    lvl.item.expandable = true;
    lvl.item.mime = QStringLiteral("livetvsources:"); // so loadTop() repopulates on Back
    stack_.push_back(lvl);
    populateLiveTvSources();
}

void HomeView::populateLiveTvSources()
{ showSyntheticCatalog(browse::liveTvSourcesCatalog(IptvSourceStore::list())); }

void HomeView::openLiveTvChannelsLevel(const QString& sourceId)
{
    IptvSource src;
    if (!IptvSourceStore::get(sourceId, src)) return; // removed out from under us
    if (xmbMode_) { atXmbRoot_ = false; if (xmb_) xmb_->setAtRoot(false); }
    Level lvl;
    lvl.addon = nullptr; lvl.detail = true; lvl.title = src.name.isEmpty() ? src.url : src.name;
    lvl.item.id = QStringLiteral("iptvsrc:") + sourceId;
    lvl.item.type = QStringLiteral("_livetvchannels");
    lvl.item.expandable = true;
    lvl.item.mime = QStringLiteral("livetvchannels:") + sourceId; // so loadTop() repopulates on Back
    stack_.push_back(lvl);
    // Refresh on OPEN: drop any prior cache and fetch fresh.
    liveTvEntries_.clear();
    liveTvCacheSourceId_.clear();
    fetchLiveTvChannels(src);
}

void HomeView::populateLiveTvChannels(const QString& sourceId)
{
    IptvSource src;
    if (!IptvSourceStore::get(sourceId, src)) { showLiveTvError(QString()); return; }
    // Re-show from the in-session cache when it belongs to this source (Back, or a favourite re-render); only
    // fetch when the cache is empty or for a different source.
    if (liveTvCacheSourceId_ == sourceId && !liveTvEntries_.isEmpty())
    {
        showLiveTvChannels(src);   // re-render from cache with the current guide's now/next + Guide row (#75 inc 3)
        return;
    }
    fetchLiveTvChannels(src);
}

// Show a readable one-row error instead of a crash or a blank grid (type "info" is non-actionable).
void HomeView::showLiveTvError(const QString& name)
{
    MediaCatalog c;
    c.title = name.isEmpty() ? tr("Live TV") : name;
    MediaItem info;
    info.type = QStringLiteral("info");
    info.title = tr("Couldn't load this source. Check the URL, then try again.");
    c.items.push_back(info);
    showSyntheticCatalog(c);
}

void HomeView::fetchLiveTvChannels(const IptvSource& src)
{
    const int gen = ++liveTvFetchGen_;   // supersede any in-flight fetch; a stale reply is dropped below
    const QString sourceId = src.id;
    const QString srcUrl = src.url;
    const QString name = src.name.isEmpty() ? src.url : src.name;

    // A loading placeholder while the fetch is in flight.
    {
        MediaCatalog c; c.title = name;
        MediaItem info; info.type = QStringLiteral("info"); info.title = tr("Loading channels…");
        c.items.push_back(info);
        showSyntheticCatalog(c);
    }

    // Common tail: parse the fetched text, cache it, and show — or show a readable error. Guarded on the
    // generation, so a reply that arrives after the user navigated away (or re-opened) changes nothing.
    const IptvSource srcCopy = src;   // captured by value: the async replies outlive the caller's reference
    auto deliver = [this, gen, srcCopy, sourceId, srcUrl, name](const QString& text, bool ok) {
        if (gen != liveTvFetchGen_) return; // superseded
        if (!ok || text.isEmpty()) { showLiveTvError(name); return; }
        QVector<M3uEntry> entries = StreamResolver::parseM3u(text, srcUrl);
        if (entries.isEmpty()) { showLiveTvError(name); return; }
        // #203: A CHANNEL LIST IS THE ONE THING THAT CAN RE-IDENTIFY A LEGACY ROW, so every time one arrives
        // the favourites and playlist entries written by an older build get another chance at a durable,
        // credential-free name. Idempotent and free when there is nothing to repair — LiveTvMigrate.h says why
        // this is driven by the data rather than by a startup stamp. Before the render, so the ★ mark below is
        // computed against the ids the store now holds.
        LiveTvMigrate::withChannels(browse::liveTvChannels(entries));
        {
            QStringList clashes;
            LiveTvIdentity::idsFor(browse::liveTvChannels(entries), &clashes);
            // Ids only, never a url: this line is written to a log file (§ the credential rule).
            if (!clashes.isEmpty())
                qInfo("live tv: %d channel identit%s shared by more than one entry in this source; the first "
                      "in playlist order resolves (first: %s)", int(clashes.size()),
                      clashes.size() == 1 ? "y is" : "ies are", qUtf8Printable(clashes.first()));
        }
        liveTvEntries_ = entries;
        liveTvCacheSourceId_ = sourceId;
        // A change of source drops a stale guide until this source's EPG (re)loads (#75 inc 3).
        if (liveTvGuideSourceId_ != sourceId) { liveTvGuide_ = xmltv::Guide(); liveTvGuideSourceId_.clear(); }
        showLiveTvChannels(srcCopy);
        // Kick the EPG fetch — async, daily-cached, degrades to no-EPG. The playlist's own #EXTM3U url-tvg is
        // the fallback guide url when the source has no manual epgUrl.
        fetchLiveTvEpg(srcCopy, StreamResolver::m3uHeaderTvgUrl(text));
    };

    if (!srcUrl.contains(QStringLiteral("://")))
    {
        // A local .m3u/.m3u8 file path: read it directly, no network.
        QFile f(srcUrl);
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) deliver(QString::fromUtf8(f.readAll()), true);
        else                                               deliver(QString(), false);
        return;
    }

    // A remote playlist: GET it to a buffer (the app's shared nam_), same request shape as the other fetches.
    QNetworkRequest req{ QUrl(srcUrl) };
    req.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(AppBrand::kUserAgent));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = nam_->get(req);
    connect(reply, &QNetworkReply::finished, this, [reply, deliver] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) { deliver(QString(), false); return; }
        deliver(QString::fromUtf8(reply->readAll()), true);
    });
}

// Render the cached channel list (liveTvEntries_) with the current guide's now/next subtitles, prepending a
// "Guide (today)" row when there is EPG to build a grid from. The single render path for both the fresh fetch
// and the cache/favourite re-render, so now/next and the Guide row appear identically on every route.
void HomeView::showLiveTvChannels(const IptvSource& src)
{
    const QString name = src.name.isEmpty() ? src.url : src.name;
    const bool haveGuide = (liveTvGuideSourceId_ == src.id) && !liveTvGuide_.programmes.isEmpty();

    QHash<QString, QString> nowNext;
    if (haveGuide)
        nowNext = browse::liveTvNowNextByTvgId(liveTvEntries_, liveTvGuide_, QDateTime::currentDateTimeUtc());

    MediaCatalog cat = browse::liveTvChannelsCatalog(name, liveTvEntries_, FavoritesStore::list(), nowNext);
    if (haveGuide)
    {
        MediaItem guide;
        guide.id         = QStringLiteral("_livetvguide:") + src.id;
        guide.type       = QStringLiteral("_livetvguide");
        guide.title      = tr("\U0001F4FA  Guide (today)");
        guide.mime       = QStringLiteral("livetvguide:") + src.id;   // activation opens the grid level
        guide.expandable = true;
        cat.items.prepend(guide);
    }
    showSyntheticCatalog(cat);
}

// Resolve, fetch (daily-cached on disk) and parse this source's XMLTV EPG, then re-render the channel list with
// now/next. Async and best-effort: a missing/failed/empty feed degrades to no-EPG (the channels still list and
// play), never an error row. The manual per-source epgUrl wins; the playlist's own url-tvg header is the
// fallback. Generation-guarded so a reply landing after the user moved on changes nothing.
void HomeView::fetchLiveTvEpg(const IptvSource& src, const QString& headerTvgUrl)
{
    const QString epgUrl = !src.epgUrl.isEmpty() ? src.epgUrl : headerTvgUrl;
    if (epgUrl.isEmpty()) return;   // no guide declared for this source

    const int gen = ++liveTvEpgFetchGen_;
    const QString sourceId = src.id;
    const IptvSource srcCopy = src;

    // Parse + adopt a guide document, then re-render if we are still on this source.
    auto apply = [this, gen, sourceId, srcCopy](const QByteArray& xml) {
        if (gen != liveTvEpgFetchGen_) return;             // superseded
        if (xml.isEmpty()) return;
        xmltv::Guide gd = xmltv::parseXmltv(xml);
        if (gd.programmes.isEmpty()) return;               // nothing usable -> leave the list as-is
        liveTvGuide_ = gd;
        liveTvGuideSourceId_ = sourceId;
        if (liveTvCacheSourceId_ == sourceId) showLiveTvChannels(srcCopy);
    };

    // Daily cache: <dataDir>/epg_cache/<sourceId>.xml, with a per-source yyyy-MM-dd stamp in the ini. A stamp
    // dated today (and a present file) is reused; anything older or missing refetches.
    const QString cacheDir  = AppPaths::dataDir() + QStringLiteral("/epg_cache");
    const QString cacheFile = cacheDir + QStringLiteral("/") + QString(sourceId).replace(
                                  QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]")), QStringLiteral("_"))
                              + QStringLiteral(".xml");
    const QString stampKey  = QStringLiteral("epgcache/") + sourceId + QStringLiteral("/date");
    const QString today     = QDate::currentDate().toString(Qt::ISODate);

    if (settingsStore().value(stampKey).toString() == today && QFile::exists(cacheFile))
    {
        QFile f(cacheFile);
        if (f.open(QIODevice::ReadOnly)) { apply(f.readAll()); return; }
    }

    QNetworkRequest req{ QUrl(epgUrl) };
    req.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(AppBrand::kUserAgent));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = nam_->get(req);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, gen, cacheDir, cacheFile, stampKey, today, apply] {
        reply->deleteLater();
        if (gen != liveTvEpgFetchGen_) return;
        if (reply->error() != QNetworkReply::NoError) return;      // degrade to no-EPG
        const QByteArray xml = xmltv::gunzip(reply->readAll());    // .xml.gz -> xml; plain xml passes through
        if (!xml.isEmpty())
        {
            QDir().mkpath(cacheDir);
            QFile f(cacheFile);
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            {
                f.write(xml); f.close();
                settingsStore().setValue(stampKey, today);         // stamp only after a successful write
            }
        }
        apply(xml);
    });
}

// The "Guide (today)" row -> a channels×time grid for today, built against the source-agnostic Programme model
// (#75 inc 3; #179 supplies the same model from a computed schedule). Uses the already-loaded liveTvEntries_ +
// liveTvGuide_ — no fetch here.
void HomeView::openLiveTvGuideLevel(const QString& sourceId)
{
    IptvSource src;
    if (!IptvSourceStore::get(sourceId, src)) return;   // removed out from under us
    if (xmbMode_) { atXmbRoot_ = false; if (xmb_) xmb_->setAtRoot(false); }
    Level lvl;
    lvl.addon = nullptr; lvl.detail = true;
    lvl.title = (src.name.isEmpty() ? src.url : src.name) + tr(" — Guide");
    lvl.item.id = QStringLiteral("_livetvguidegrid:") + sourceId;
    lvl.item.type = QStringLiteral("_livetvguidegrid");
    lvl.item.expandable = true;
    lvl.item.mime = QStringLiteral("livetvguide:") + sourceId;   // so loadTop() repopulates on Back
    stack_.push_back(lvl);
    populateLiveTvGuide(sourceId);
}

// Build + show the grid from the already-loaded entries + guide (no stack push). The open path pushes the level
// then calls this; loadTop() calls it directly on Back so the level is not duplicated.
void HomeView::populateLiveTvGuide(const QString& sourceId)
{
    IptvSource src;
    if (!IptvSourceStore::get(sourceId, src)) return;
    const QDateTime nowUtc   = QDateTime::currentDateTimeUtc();
    const QDateTime dayStart = QDateTime(QDate::currentDate(), QTime(0, 0), Qt::LocalTime).toUTC();
    const QDateTime dayEnd   = dayStart.addDays(1);
    showSyntheticCatalog(browse::liveTvGuideCatalog(src.name.isEmpty() ? src.url : src.name,
                                                    liveTvEntries_, liveTvGuide_, nowUtc, dayStart, dayEnd));
}

bool HomeView::promptForLiveTvSource()
{
    const QString name = Osk::getText(tr("Source name:"), QString(), QLineEdit::Normal, window()).trimmed();
    if (name.isEmpty()) return false; // covers backed-out (null) too
    const QString url = Osk::getText(tr("Playlist URL or file path:"), QString(),
                                     QLineEdit::Normal, window()).trimmed();
    if (url.isEmpty()) return false;
    IptvSource s; s.name = name; s.url = url;   // epgUrl stays empty (reserved for increment 3)
    IptvSourceStore::add(s);
    return true;
}

void HomeView::addIptvSourceInteractive()
{
    if (!promptForLiveTvSource()) return;
    populateLiveTvSources(); // we're on the sources level -> refresh it (also fires browseItemsChanged)
}

void HomeView::removeIptvSourceInteractive(const QString& sourceId, const QString& name)
{
    const int choice = NavConfirm::ask(tr("Remove source"),
        tr("Remove “%1” from Live TV?").arg(name),
        { tr("Cancel"), tr("Remove") }, /*focusIndex*/ 0, /*cancelIndex*/ 0, window());
    if (choice != 1) return;
    IptvSourceStore::remove(sourceId);
    populateLiveTvSources(); // on the sources level -> refresh
}

void HomeView::toggleLiveTvChannelFavorite(const MediaItem& it)
{
    if (FavoritesStore::isFavorite(it.id))
        FavoritesStore::remove(it.id);
    else
    {
        // Rebuild the FavoriteItem from the cached M3uEntry (its clean title, logo and stream url) so the
        // star's ★-prefixed display title is never what gets stored. Fall back to the tile if the cache missed.
        bool built = false;
        const QVector<QString> ids = browse::liveTvChannelIds(liveTvEntries_);
        for (int i = 0; i < ids.size(); ++i)
            if (ids.at(i) == it.id)
            { FavoritesStore::add(browse::liveTvChannelFavorite(liveTvEntries_, i)); built = true; break; }
        if (!built)
        {
            FavoriteItem f;
            f.itemId = it.id; f.title = it.title; f.type = QStringLiteral("livetv");
            // The identity, never the url (#203) — the same rule liveTvChannelFavorite follows, restated here
            // because this arm is what runs when the cache missed and it must not be the one that leaks.
            f.thumbnailUrl = it.thumbnailUrl; f.path = it.id; f.kind = QStringLiteral("livetv");
            FavoritesStore::add(f);
        }
    }
    if (!liveTvCacheSourceId_.isEmpty()) populateLiveTvChannels(liveTvCacheSourceId_); // re-render the ★ mark
}

// ---- OPDS book catalogs (issue #146) ----------------------------------------------------------------------
// The Reading catalogue's "Book Servers" folder lists the user's saved OPDS catalogs (browse::opdsCatalogsList);
// opening one FETCHES its root feed fresh (a stale feed across sessions is the thing this avoids), parseOpds
// turns the Atom XML into an OpdsFeed, and browse::opdsCatalog renders it as drill rows (sub-shelves) + book
// items. A book item is DOWNLOADED with the catalog's device-local auth by the same remote-document path every
// addon book uses (emit openItem -> MainWindow::fetchRemoteDocumentThenOpen), then opened in the reader.
//
// AUTH: the password lives in OpdsCatalogStore, device-local (CloudSync carves the "opds/" prefix out of the
// synced bundle). It is turned into an "Authorization: Basic …" header by opdsBasicAuth at FETCH time only —
// here, never in a builder, never logged. currentOpdsCatalogId_ remembers which catalog's creds a sub-feed row
// or book item belongs to across a drill-in (they carry only a url), mirroring liveTvCacheSourceId_.

void HomeView::openOpdsCatalogsLevel()
{
    if (xmbMode_) { atXmbRoot_ = false; if (xmb_) xmb_->setAtRoot(false); }
    Level lvl;
    lvl.addon = nullptr; lvl.detail = true; lvl.title = tr("Book Servers");
    lvl.item.id = QStringLiteral("_opdscatalogs");
    lvl.item.type = QStringLiteral("_opdscatalogs");
    lvl.item.expandable = true;
    lvl.item.mime = QStringLiteral("opdscatalogs:"); // so loadTop() repopulates on Back
    stack_.push_back(lvl);
    populateOpdsCatalogs();
}

void HomeView::populateOpdsCatalogs()
{ showSyntheticCatalog(browse::opdsCatalogsList(OpdsCatalogStore::list())); }

// ---- RECOMPS (issue #248, increment a) -------------------------------------------------------------------
// `Games → Recomps`: the browse surface over the native-port catalogue #233 already ships. The rows, their
// grouping and their state come from core/RecompRows.h, which is pure and probe-driven; everything here is
// the two things that header deliberately cannot do — gather this machine's facts, and project rows onto
// MediaItems. Row activation goes to the SAME MainWindow::showNativePort the game row's *Native port* verb
// goes to, so there is one implementation of Install / Play / Homepage / Remove and not two.
void HomeView::openRecompsLevel()
{
    if (xmbMode_) { atXmbRoot_ = false; if (xmb_) xmb_->setAtRoot(false); }
    Level lvl;
    lvl.addon = nullptr; lvl.detail = true; lvl.title = tr("Recomps");
    lvl.item.id = QStringLiteral("_recomps");
    lvl.item.type = QStringLiteral("_recomps");
    lvl.item.expandable = true;
    lvl.item.mime = QStringLiteral("recomps:"); // so loadTop() repopulates on Back
    stack_.push_back(lvl);
    populateRecomps();
}

// The state label, per row. Here rather than in RecompRows.h because these are user-visible strings and that
// header is deliberately translation-free — and because a label is the one thing about a state that is a
// presentation choice rather than a fact.
static QString recompStateLabel(recomps::State s)
{
    switch (s)
    {
        case recomps::State::NotInstalled:    return HomeView::tr("not installed");
        case recomps::State::NeedsRom:        return HomeView::tr("needs ROM");
        case recomps::State::Installed:       return HomeView::tr("installed");
        case recomps::State::UpdateAvailable: return HomeView::tr("update available");
        // Reserved for the self-compiled tier (#248 increment c). deriveState never returns them today; the
        // cases exist so adding that tier is a compile error here rather than a silent blank label.
        case recomps::State::Building:        return HomeView::tr("building…");
        case recomps::State::Ready:           return HomeView::tr("ready");
    }
    return QString();
}

void HomeView::populateRecomps()
{
    // THE LIBRARY, ONCE. `needs ROM` asks the same question of every row, and a per-row scan would walk the
    // ROMs tree once per catalogue entry. Both sources count as "the user has this game": the ROM library
    // proper, and anything already recorded as downloaded (a ROM that arrived through the app lives there and
    // may sit outside the library root entirely).
    QVector<recomps::LibraryRom> library;
    for (const RomLibrary::SystemGroup& g : RomLibrary::scan())
        for (const RomLibrary::Rom& r : g.roms)
            library.push_back({ r.systemId, r.title, r.path });
    for (const DownloadedItem& d : DownloadsStore::list())
        if (d.kind == QStringLiteral("game") && !d.system.isEmpty())
            library.push_back({ d.system, d.title, d.path });

    const QVector<recomps::Row> rows = recomps::buildRows(
        NativePorts::all(),
        [&library](const ExternalEmulator& e) {
            recomps::Facts f;
            f.installed    = EmulatorManager::isInstalled(e);
            f.libraryMatch = recomps::libraryMatches(e, library);
            // Only meaningful for an install that exists; asking otherwise would read a folder that is not there.
            if (f.installed) f.installedTag = NativePorts::readInstalledTag(EmulatorManager::installDir(e));
            f.catalogueTag = e.port.releaseTag;
            return f;
        },
        [](const QString& sysId) {
            const GameSystem* s = SystemCatalog::byId(sysId);
            return s ? s->name : QString();
        });

    MediaCatalog cat;
    cat.title = tr("Recomps");
    cat.hasMore = false;
    for (const recomps::Row& r : rows)
    {
        MediaItem it;
        if (r.kind == recomps::Row::Kind::SystemHeader)
        {
            // The same non-activatable section-label shape browse::liveTvChannelsCatalog uses, so both layouts
            // already know how to draw it and activateItem already knows to ignore it.
            it.id    = QStringLiteral("_recomphdr:") + r.systemId;
            it.type  = QStringLiteral("_recompheader");
            it.title = r.title;
            cat.items.push_back(it);
            continue;
        }
        if (r.kind == recomps::Row::Kind::Error)
        {
            // #174: a catalogue that cannot be read is an ERROR ROW, not an empty section. An empty grid says
            // "there are no recomps", which is a different and false statement.
            it.id    = QStringLiteral("_recompserror");
            it.type  = QStringLiteral("info");
            it.title = tr("The recomp catalogue could not be read.");
            it.subtitle = tr("Nothing has been installed or removed.");
            cat.items.push_back(it);
            continue;
        }
        it.id    = QStringLiteral("recomp:") + r.portId;
        it.type  = QStringLiteral("_recompport");
        it.mime  = QStringLiteral("recompport:") + r.portId;   // activation resolves the port from this
        it.title = r.title;
        // The second line carries everything a person needs to decide, in the order they need it: where this
        // machine stands, who made it, under what terms, and which tier it is. The upstream is credited by its
        // OWN name — never the recompilation toolchain's brand, whose developers asked exactly that of a
        // third-party launcher (#233).
        QStringList bits{ recompStateLabel(r.state) };
        if (!r.creditedName.isEmpty()) bits << r.creditedName;
        if (!r.license.isEmpty())      bits << r.license;
        bits << (r.tier == recomps::Tier::PreBuilt ? tr("pre-built") : tr("self-compiled"));
        it.subtitle = bits.join(QStringLiteral(" · "));
        cat.items.push_back(it);
    }
    showSyntheticCatalog(cat);
}

void HomeView::refreshRecompsIfShown()
{
    if (stack_.isEmpty() || !stack_.last().detail) return;
    if (stack_.last().item.type != QStringLiteral("_recomps")) return;
    populateRecomps();
    emit browseItemsChanged(false);   // re-sync a themed browse view (else its selection/metadata desync)
}

void HomeView::openOpdsCatalog(const QString& catalogId)
{
    OpdsCatalog c;
    if (!OpdsCatalogStore::get(catalogId, c)) return;    // removed out from under us
    currentOpdsCatalogId_ = catalogId;                   // the auth context for this catalog's feeds + books
    openOpdsFeedLevel(c.url, c.name.isEmpty() ? c.url : c.name);
}

void HomeView::openOpdsFeedLevel(const QString& feedUrl, const QString& title)
{
    if (feedUrl.isEmpty()) return;
    if (xmbMode_) { atXmbRoot_ = false; if (xmb_) xmb_->setAtRoot(false); }
    Level lvl;
    lvl.addon = nullptr; lvl.detail = true; lvl.title = title;
    lvl.item.id = QStringLiteral("opdsfeed:") + feedUrl;
    lvl.item.type = QStringLiteral("_opdsfeedlvl");
    lvl.item.expandable = true;
    // Carry BOTH the catalog id (its auth) and the feed url so loadTop() can re-fetch on Back. The id is a uuid
    // and a url has no newline, so '\n' is a safe separator; the title rides the Level's own title field.
    lvl.item.mime = QStringLiteral("opdsfeedlvl:") + currentOpdsCatalogId_ + QLatin1Char('\n') + feedUrl;
    stack_.push_back(lvl);
    fetchOpdsFeed(currentOpdsCatalogId_, feedUrl, title);
}

void HomeView::populateOpdsFeed(const QString& catalogId, const QString& feedUrl, const QString& title)
{
    currentOpdsCatalogId_ = catalogId;   // restore the auth context (a Back may cross catalogs)
    fetchOpdsFeed(catalogId, feedUrl, title);
}

// A readable one-row error instead of a crash or a blank grid (type "info" is non-actionable).
void HomeView::showOpdsError(const QString& title)
{
    MediaCatalog c;
    c.title = title.isEmpty() ? tr("Book Servers") : title;
    MediaItem info;
    info.type = QStringLiteral("info");
    info.title = tr("Couldn't load this catalog. Check the URL and sign-in, then try again.");
    c.items.push_back(info);
    showSyntheticCatalog(c);
}

void HomeView::fetchOpdsFeed(const QString& catalogId, const QString& feedUrl, const QString& title)
{
    const int gen = ++opdsFetchGen_;   // supersede any in-flight fetch; a stale reply is dropped below

    // A loading placeholder while the fetch is in flight.
    {
        MediaCatalog c; c.title = title.isEmpty() ? tr("Book Servers") : title;
        MediaItem info; info.type = QStringLiteral("info"); info.title = tr("Loading…");
        c.items.push_back(info);
        showSyntheticCatalog(c);
    }

    // Render a fetched feed body, or a readable error. Best-effort parse: a truncated feed yields whatever
    // entries closed; only a body that parses to NOTHING (a 404 HTML page, garbage) is treated as a failure.
    auto deliver = [this, gen, title, feedUrl](const QByteArray& body, bool ok) {
        if (gen != opdsFetchGen_) return;   // superseded by a newer navigation
        if (!ok) { showOpdsError(title); return; }
        const OpdsFeed feed = parseOpds(body, feedUrl);
        if (feed.entries.isEmpty() && feed.title.isEmpty()) { showOpdsError(title); return; }
        showSyntheticCatalog(browse::opdsCatalog(feed));
    };

    // A local .xml file path (symmetry with the Live TV surface's local-playlist support): read it directly.
    if (!feedUrl.contains(QStringLiteral("://")))
    {
        QFile f(feedUrl);
        if (f.open(QIODevice::ReadOnly)) deliver(f.readAll(), true);
        else                             deliver(QByteArray(), false);
        return;
    }

    // Device-local HTTP basic auth, built at fetch time and attached to THIS request only — never logged. An
    // open catalog (no username) sends none. NetHeaderApply drops the header if the feed cross-origin-redirects.
    StreamHeaders::Headers headers;
    OpdsCatalog cat;
    if (OpdsCatalogStore::get(catalogId, cat) && !cat.username.isEmpty())
    {
        const QString auth = opdsBasicAuth(cat.username, cat.password);
        if (!auth.isEmpty()) headers.insert(QStringLiteral("Authorization"), auth);
    }
    QNetworkRequest req{ QUrl(feedUrl) };
    req.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(AppBrand::kUserAgent));
    QNetworkReply* reply = NetHeaderApply::get(nam_, req, headers, feedUrl, {});
    connect(reply, &QNetworkReply::finished, this, [reply, deliver] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) { deliver(QByteArray(), false); return; }
        deliver(reply->readAll(), true);
    });
}

void HomeView::addOpdsCatalogInteractive()
{
    const QString name = Osk::getText(tr("Catalog name:"), QString(), QLineEdit::Normal, window()).trimmed();
    if (name.isEmpty()) return;  // covers backed-out (null) too
    const QString url = Osk::getText(tr("OPDS feed URL:"), QString(), QLineEdit::Normal, window()).trimmed();
    if (url.isEmpty()) return;
    // Optional HTTP basic auth: a blank username means an open catalog, and only then is a password asked for.
    // The password is NOT trimmed (leading/trailing spaces can be significant) and NEVER logged.
    const QString user = Osk::getText(tr("Username (optional):"), QString(),
                                      QLineEdit::Normal, window()).trimmed();
    QString pass;
    if (!user.isEmpty())
        pass = Osk::getText(tr("Password:"), QString(), QLineEdit::Password, window());
    OpdsCatalog c; c.name = name; c.url = url; c.username = user; c.password = pass;
    OpdsCatalogStore::add(c);
    populateOpdsCatalogs();  // on the catalogs level -> refresh (also fires browseItemsChanged)
}

// Add a Subsonic music server (#193). The OSK flow addOpdsCatalogInteractive established, with two extra
// questions this protocol makes necessary and one rule about the password.
//
// THE PASSWORD IS NEVER ECHOED, NEVER TRIMMED AND NEVER LOGGED. Not trimmed because leading and trailing
// spaces are significant in a password and silently eating them produces a credential that fails for a
// reason nobody can see; entered through QLineEdit::Password so it is not readable over somebody's shoulder
// on a television. It goes straight into the device-local store and is read again only when a request is
// built (SubsonicClient), where it becomes a per-request salted token.
//
// HTTPS IS THE DEFAULT AND PLAIN HTTP IS AN EXPLICIT ANSWER, not a silent downgrade: a plain-http address
// with the box unticked is REFUSED by Subsonic::checkUrl with a sentence saying exactly that, rather than
// the password being sent in clear because the URL happened to say http.
void HomeView::addMusicServerInteractive()
{
    const QString name = Osk::getText(tr("Server name:"), QString(), QLineEdit::Normal, window()).trimmed();
    if (name.isEmpty()) return;  // covers backed-out (null) too
    const QString url = Osk::getText(tr("Server address (https://...):"), QString(),
                                     QLineEdit::Normal, window()).trimmed();
    if (url.isEmpty()) return;
    const QString user = Osk::getText(tr("Username:"), QString(), QLineEdit::Normal, window()).trimmed();
    if (user.isEmpty()) return;
    const QString pass = Osk::getText(tr("Password:"), QString(), QLineEdit::Password, window());
    if (pass.isEmpty()) return;

    SubsonicServer s;
    s.name = name; s.url = url; s.username = user; s.password = pass;
    if (Subsonic::checkUrl(url, /*allowPlainHttp*/ false) == Subsonic::UrlVerdict::InsecureRefused)
    {
        // The explicit choice. Asked ONLY when it is actually needed, and phrased as the risk it is.
        const int go = NavConfirm::ask(tr("Send the password unencrypted?"),
            tr("That address is plain HTTP, so your username and password will be sent over the network "
               "unencrypted. Use https:// instead if your server supports it."),
            { tr("Cancel"), tr("Send unencrypted") }, /*focusIndex*/ 0, /*cancelIndex*/ 0, window());
        if (go != 1) return;                 // backing out ADDS NOTHING: no half-configured server
        s.allowPlainHttp = true;
    }
    // The legacy plaintext parameter, for servers too old for the token scheme. Asked last and answered
    // "no" by default: it is a downgrade, and a client that offered it first would train people into it.
    s.legacyAuth = NavConfirm::ask(tr("Sign-in method"),
        tr("Some older servers only accept the password as plain text rather than the modern salted token. "
           "Choose the modern one unless signing in fails."),
        { tr("Modern (recommended)"), tr("Old plain-text") }, /*focusIndex*/ 0, /*cancelIndex*/ 0,
        window()) == 1;

    SubsonicServerStore::add(s);
    populateMusicServers();  // on the servers level -> refresh (also fires browseItemsChanged)
}

// Remove a saved music server. The Live TV shape, and the one extra sentence this feature owes: what the
// user is actually giving up is the SIGN-IN, and nothing on the server itself is touched.
void HomeView::removeMusicServerInteractive(const QString& serverId, const QString& name)
{
    const int choice = NavConfirm::ask(tr("Remove music server"),
        tr("Remove “%1” and forget its sign-in? Nothing on the server itself is changed.").arg(name),
        { tr("Cancel"), tr("Remove") }, /*focusIndex*/ 0, /*cancelIndex*/ 0, window());
    if (choice != 1) return;
    SubsonicServerStore::remove(serverId);   // fires the change hook -> the Music tab re-evaluates
    populateMusicServers();                  // on the servers level -> refresh
}

// ---- JELLYFIN, N SERVERS (issue #160) -----------------------------------------------------------------
//
// ONE manager for the whole list, because add / switch off / remove are three verbs about one list and
// three separate settings rows would put the list itself nowhere. Every leaf goes through the nav kit
// (NavMenu / Osk / NavConfirm), so it is controller-, keyboard- and mouse-reachable with no separate window.
//
// Both callers reach this a QUEUED TURN past their own activation (the themed settings row defers with
// invokeMethod; the classic button is an ordinary widget signal), so blocking on the nav kit's nested loops
// here is safe — the #28 / #211 discipline. Where a nested loop would otherwise open INSIDE a network
// reply's emission, it is deferred again; connectJellyfinServerInteractive says so at each site.
void HomeView::manageJellyfinServersInteractive()
{
    const QList<JellyfinServer> servers = JellyfinServerStore::list();
    QStringList rows;
    rows << tr("➕ Connect a Jellyfin server…");
    for (const JellyfinServer& s : servers)
    {
        const QString name = s.name.trimmed().isEmpty() ? tr("(unnamed)") : s.name;
        // The URL is shown because it is how a person tells two servers apart when both are called
        // "Jellyfin". The token is not shown, and there is no row that could reveal it.
        rows << (s.enabled ? tr("%1 — %2").arg(name, s.url)
                           : tr("%1 — %2  (switched off)").arg(name, s.url));
    }
    const int pick = NavMenu::pick(tr("Jellyfin servers"), rows, window());
    if (pick < 0) return;                                     // Back
    if (pick == 0) { connectJellyfinServerInteractive(); return; }

    const JellyfinServer& s = servers.at(pick - 1);
    const QString name = s.name.trimmed().isEmpty() ? tr("(unnamed)") : s.name;
    const int action = NavMenu::pick(name,
        { s.enabled ? tr("⏸ Switch this server off") : tr("▶ Switch this server on"),
          tr("🗑 Remove this server") }, window());
    if (action == 0)
    {
        // SWITCHING OFF IS NOT A REMOVAL. Its rows disappear from the merged library and its sign-in stays
        // exactly where it is, which is what makes "get the friend's 8,000 films out of the way for this
        // evening" a one-press decision the user can undo.
        JellyfinServerStore::setEnabled(s.id, !s.enabled);
    }
    else if (action == 1)
    {
        const int go = NavConfirm::ask(tr("Remove Jellyfin server"),
            tr("Remove “%1” and forget its sign-in? Nothing on the server itself is changed, and anything "
               "you have already watched from it stays remembered in case you connect it again.").arg(name),
            { tr("Cancel"), tr("Remove") }, /*focusIndex*/ 0, /*cancelIndex*/ 0, window());
        if (go != 1) return;
        JellyfinServerStore::remove(s.id);      // fires the change hook -> the merged library rebuilds
    }
}

// THE ORDER OF THE QUESTIONS IS THE DESIGN. Address first, then "who is this server?" against
// /System/Info/Public, and ONLY THEN a username and a password:
//   * a server whose identity cannot be read is never asked for a password, because there would be nothing
//     to qualify its rows with — adding it would write ids nothing can ever resolve (Jellyfin.h);
//   * and the plain-HTTP question is asked BEFORE anything is sent, not after, because a Jellyfin sign-in
//     POSTs the password.
void HomeView::connectJellyfinServerInteractive()
{
    const QString url = Osk::getText(tr("Server address (https://...):"), QString(),
                                     QLineEdit::Normal, window()).trimmed();
    if (url.isEmpty()) return;                 // covers backed-out (null) too

    bool allowPlainHttp = false;
    if (Jellyfin::checkUrl(url, false) == Jellyfin::UrlVerdict::InsecureRefused)
    {
        // The explicit choice, phrased as the risk it is. Backing out ADDS NOTHING: no half-configured
        // server, and no password sent.
        const int go = NavConfirm::ask(tr("Send the password unencrypted?"),
            tr("That address is plain HTTP, so your username and password will be sent over the network "
               "unencrypted. Use https:// instead if your server supports it."),
            { tr("Cancel"), tr("Send unencrypted") }, /*focusIndex*/ 0, /*cancelIndex*/ 0, window());
        if (go != 1) return;
        allowPlainHttp = true;
    }
    if (Jellyfin::checkUrl(url, allowPlainHttp) != Jellyfin::UrlVerdict::Ok)
    {
        NavConfirm::ask(tr("Jellyfin"), tr("That is not a server address."), { tr("OK") }, 0, 0, window());
        return;
    }

    QPointer<HomeView> self(this);
    JellyfinClient::instance().fetchPublicInfo(url, allowPlainHttp, /*budgetMs*/ 10000,
        [self, url, allowPlainHttp](const Jellyfin::PublicInfo& info, const QString& error) {
            if (!self) return;
            // WE ARE INSIDE QNetworkReply::finished's EMISSION. Osk and NavConfirm each spin a nested event
            // loop, and a nested loop inside an outer emission is the #28 / #211 crash family — so the rest
            // of the flow is deferred a turn past it, exactly as the themed settings row defers into here.
            QMetaObject::invokeMethod(self.data(), [self, url, allowPlainHttp, info, error] {
                if (!self) return;
                if (!error.isEmpty() || !info.ok)
                {
                    // The error is one of OUR sentences (JellyfinClient renders them from the NetworkError
                    // enum), never Qt's — which would embed the url, and this app's Jellyfin urls carry a
                    // credential.
                    NavConfirm::ask(tr("Jellyfin"),
                                    error.isEmpty() ? tr("That address answered, but it is not a Jellyfin "
                                                         "server.") : error,
                                    { tr("OK") }, 0, 0, self->window());
                    return;
                }

                const QString user = Osk::getText(tr("Username:"), QString(), QLineEdit::Normal,
                                                  self->window()).trimmed();
                if (user.isEmpty()) return;
                // NEVER ECHOED, NEVER TRIMMED, NEVER LOGGED. Not trimmed because leading and trailing spaces
                // are significant in a password and eating them silently produces a sign-in that fails for a
                // reason nobody can see; entered as QLineEdit::Password so it is not readable over somebody's
                // shoulder on a television. It goes to the transport and is not held.
                const QString pass = Osk::getText(tr("Password:"), QString(), QLineEdit::Password,
                                                  self->window());
                if (pass.isEmpty()) return;

                JellyfinClient::instance().authenticate(url, allowPlainHttp, user, pass, /*budgetMs*/ 20000,
                    [self, url, allowPlainHttp, info](const Jellyfin::AuthResult& res,
                                                      const QString& authError) {
                        if (!self) return;
                        // Deferred past the reply's emission again, for the same reason.
                        QMetaObject::invokeMethod(self.data(), [self, url, allowPlainHttp, info, res, authError] {
                            if (!self) return;
                            if (!authError.isEmpty() || !res.ok)
                            {
                                NavConfirm::ask(tr("Jellyfin"),
                                                authError.isEmpty() ? tr("That server refused the sign-in.")
                                                                    : authError,
                                                { tr("OK") }, 0, 0, self->window());
                                return;
                            }
                            JellyfinServer s;
                            // THE SERVER'S OWN Id, not a uuid we mint and not the url — see
                            // JellyfinServerStore.h. It is what every row from this server is qualified with.
                            s.id             = info.serverId;
                            // The server's own name by default, which is what the user calls it everywhere
                            // else; they never have to invent one.
                            s.name           = info.serverName.trimmed().isEmpty()
                                                   ? tr("Jellyfin") : info.serverName;
                            s.url            = url;
                            s.allowPlainHttp = allowPlainHttp;
                            s.userId         = res.userId;
                            s.userName       = res.userName;
                            s.token          = res.token;   // device-local, under "jellyfin/"; never synced
                            if (!JellyfinServerStore::add(s))
                            {
                                NavConfirm::ask(tr("Jellyfin"),
                                    tr("That server did not give an identity this app can use, so its "
                                       "items could not be told apart from another server's."),
                                    { tr("OK") }, 0, 0, self->window());
                                return;
                            }
                            NavConfirm::ask(tr("Jellyfin"),
                                tr("“%1” is connected. Its library appears alongside your own, with each "
                                   "row labelled by the server it came from.").arg(s.name),
                                { tr("OK") }, 0, 0, self->window());
                        }, Qt::QueuedConnection);
                    });
            }, Qt::QueuedConnection);
        });
}

void HomeView::openOpdsBook(const MediaItem& it)
{
    // Attach this catalog's device-local auth to THIS open only (built here via opdsBasicAuth, never logged),
    // then hand the item to the main window's remote-document path: it downloads the acquisition href to the
    // cache — dropping the auth on any cross-origin redirect, per NetHeaderApply — and opens it in the reader.
    // The book item already carries url = the acquisition href and mime = its content-type, which is what that
    // path needs to pick the file extension (a Calibre-web download href often has no ".epub" suffix).
    MediaItem book = it;
    OpdsCatalog cat;
    if (OpdsCatalogStore::get(currentOpdsCatalogId_, cat) && !cat.username.isEmpty())
    {
        const QString auth = opdsBasicAuth(cat.username, cat.password);
        if (!auth.isEmpty()) book.requestHeaders.insert(QStringLiteral("Authorization"), auth);
    }
    emit openItem(book);
}

// Saved filter presets (#63) — the "＋ New filter…" row on a games surface opens this manager: create a new
// preset, or rename/delete an existing one. Every leaf goes through the nav kit (NavMenu / Osk / NavConfirm),
// so it is controller/keyboard/mouse-reachable with no separate window. We are already a turn past the QML
// `activated` emission (the row queued us), so blocking on those nested loops here is safe (issue #28).
void HomeView::createFilterPresetInteractive()
{
    const QVector<FilterPreset> presets = FilterPresetStore::list();
    QStringList rows;
    rows << tr("➕ Create a new filter…");
    for (const FilterPreset& p : presets) rows << (tr("✎ ") + p.name);
    const int pick = NavMenu::pick(tr("Saved filters"), rows, window());
    if (pick < 0) return;                       // Back
    if (pick == 0) { buildFilterPreset(); return; }
    const QString name = presets[pick - 1].name;
    const int action = NavMenu::pick(name, { tr("✎ Rename…"), tr("🗑 Delete filter") }, window());
    if (action == 0) renameFilterPresetInteractive(name);
    else if (action == 1) deleteFilterPresetInteractive(name);
}

// Walk the dimensions the reliably-available stores answer for — system, favourite, played, status, tag, and
// (issue #196) composer and conductor — as a sequence of single-choice NavMenu picks (Back on any step
// cancels the whole build), then name it with the Osk. AND across the chosen dimensions; "Any" leaves a
// dimension unconstrained. A step whose values cannot be enumerated from the rows in view is SKIPPED rather
// than shown empty, which is what lets one builder serve a console folder and a classical album page without
// asking either of them about the other's dimensions. The genre / player-count / release-decade dimensions
// the evaluator ALSO supports are deferred from this builder (they need per-value enumeration off the
// scrape) — see the report; the model, the store and the shelves already handle them.
void HomeView::buildFilterPreset()
{
    gamefilter::Filter f;

    // System: the distinct systems among the games in view; "Any system" leaves it cross-system.
    QStringList systems;
    for (const MediaItem& it : items_)
    {
        if (it.type.startsWith(QLatin1Char('_')) || it.type == QStringLiteral("rechdr")
            || it.type == QStringLiteral("info"))
            continue;
        for (const QString& s : gameFactsFor(it).systems)
            if (!s.isEmpty() && !systems.contains(s)) systems << s;
    }
    if (!systems.isEmpty())
    {
        QStringList sysRows; sysRows << tr("Any system");
        for (const QString& s : systems) sysRows << s.toUpper();
        const int s = NavMenu::pick(tr("New filter — System"), sysRows, window());
        if (s < 0) return;
        if (s > 0) f.systems = { systems[s - 1] };
    }

    const int fav = NavMenu::pick(tr("Favourite?"),
        { tr("Any"), tr("★ Favourites only"), tr("Non-favourites") }, window());
    if (fav < 0) return;
    if (fav == 1) f.favorite = gamefilter::Tri::Yes;
    else if (fav == 2) f.favorite = gamefilter::Tri::No;

    const int pl = NavMenu::pick(tr("Played?"), { tr("Any"), tr("Unplayed"), tr("Played") }, window());
    if (pl < 0) return;
    if (pl == 1) f.played = gamefilter::Tri::No;   // Unplayed == playtime 0
    else if (pl == 2) f.played = gamefilter::Tri::Yes;

    // Status rows map to ItemMarks::Completion ints (None 0, InProgress 1, Finished 2, Abandoned 3, Planned 4).
    const int st = NavMenu::pick(tr("Status?"),
        { tr("Any"), tr("Planned"), tr("In progress"), tr("Finished"), tr("Abandoned") }, window());
    if (st < 0) return;
    if (st > 0)
    {
        static const int kComp[] = { 0,
            static_cast<int>(ItemMarks::Completion::Planned),
            static_cast<int>(ItemMarks::Completion::InProgress),
            static_cast<int>(ItemMarks::Completion::Finished),
            static_cast<int>(ItemMarks::Completion::Abandoned) };
        f.completions = { kComp[st] };
    }

    const QStringList tags = ItemMarks::tagVocab();
    if (!tags.isEmpty())
    {
        QStringList tagRows; tagRows << tr("Any tag"); tagRows << tags;
        const int t = NavMenu::pick(tr("Tag?"), tagRows, window());
        if (t < 0) return;
        if (t > 0) f.tags = { tags[t - 1] };
    }

    // COMPOSER and CONDUCTOR (issue #196, part 2) — the two dimensions that make "all Bach conducted by
    // Gardiner" a saved filter. Enumerated from the rows in view exactly as System is above, which is what
    // keeps this builder honest in both directions: a games surface offers neither step (no row carries a
    // composer, so the list is empty and the pick is skipped), and a classical surface offers only the names
    // that are actually on the shelf, so a filter built here cannot come out matching nothing.
    for (int dim = 0; dim < 2; ++dim)
    {
        const bool wantComposer = (dim == 0);
        QStringList values;
        for (const MediaItem& it : items_)
        {
            if (it.type.startsWith(QLatin1Char('_')) || it.type == QStringLiteral("rechdr")
                || it.type == QStringLiteral("info"))
                continue;
            const gamefilter::GameFacts facts = gameFactsFor(it);
            for (const QString& v : (wantComposer ? facts.composers : facts.conductors))
                if (!v.isEmpty() && !values.contains(v, Qt::CaseInsensitive)) values << v;
        }
        if (values.isEmpty()) continue;
        QStringList rows; rows << (wantComposer ? tr("Any composer") : tr("Any conductor"));
        rows << values;
        const int v = NavMenu::pick(wantComposer ? tr("New filter — Composer") : tr("New filter — Conductor"),
                                    rows, window());
        if (v < 0) return;
        if (v > 0)
        {
            if (wantComposer) f.composers  = { values[v - 1] };
            else              f.conductors = { values[v - 1] };
        }
    }

    // Name it (the summary seeds the OSK). An empty/backed-out name cancels; a repeat name upserts (save()'s
    // own rule), which is the natural "update this filter" gesture.
    const QString name = Osk::getText(tr("Filter name:"), f.describe(), QLineEdit::Normal, window()).trimmed();
    if (name.isEmpty()) return;
    FilterPresetStore::save({ QString(), name, f, 0 });  // id assigned by save() (stable, name-independent)
    loadTop();  // rebuild the level so the new shelf appears among the folders (also fires browseItemsChanged)
    showToast(tr("Saved filter “%1”.").arg(name), kFeedbackShort);
}

void HomeView::renameFilterPresetInteractive(const QString& name)
{
    const QString next = Osk::getText(tr("Rename filter:"), name, QLineEdit::Normal, window()).trimmed();
    if (next.isEmpty() || next == name) return;
    if (!FilterPresetStore::rename(name, next))
    { showToast(tr("A filter named “%1” already exists.").arg(next), kFeedbackShort); return; }
    loadTop();
}

void HomeView::deleteFilterPresetInteractive(const QString& name)
{
    if (NavConfirm::ask(tr("Delete filter"), tr("Delete the saved filter “%1”?").arg(name),
                        { tr("Delete"), tr("Cancel") }, /*focusIndex=*/1, /*cancelIndex=*/1, window()) != 0)
        return; // Cancel / Back
    FilterPresetStore::remove(name);
    loadTop();
}

// A playlist row's action menu (the game-item-menu precedent): Open (default row, drills as before) / Play
// random / Rename / Delete. An in-window NavMenu overlay — controller + keyboard + mouse, no separate window.
void HomeView::showPlaylistMenu(const QString& playlistId)
{
    Playlist p;
    if (!PlaylistStore::get(playlistId, p)) return; // deleted out from under us
    // "Start channel" (shuffle-bag random autoplay) is offered ONLY for video/audio playlists — games play
    // random but don't chain. The row's presence is gated on the playlist's categoryKey, so a games list simply
    // has no channel row. It's inserted after Play random; the trailing indices shift when it's present.
    const bool canChannel = (p.categoryKey == QStringLiteral("video") || p.categoryKey == QStringLiteral("audio"));
    QStringList rows = { tr("▶   Open"), tr("🔀   Play random") };
    if (canChannel) rows << tr("📺   Start channel");
    rows << tr("✎   Rename…") << tr("🗑   Delete playlist");
    new NavMenu(p.name, rows, [this, playlistId, canChannel](int row) {
        // Map the chosen row to an action; the channel row (index 2) exists only when canChannel, so everything
        // below it shifts up by one when it's absent.
        enum { Open = 0, Random = 1 };
        if (row == Open)   { openPlaylistLevel(playlistId); return; }
        if (row == Random) { playRandomFromPlaylist(playlistId); return; }
        if (canChannel && row == 2) { emit startChannelRequested(playlistId); return; }
        const int tail = canChannel ? row - 1 : row; // 2 = Rename, 3 = Delete (in the no-channel numbering)
        if (tail == 2) renamePlaylistInteractive(playlistId);
        else if (tail == 3) deletePlaylistInteractive(playlistId);
    }, window());
}

// Uniform-random pick over a playlist's entries, opened through the SAME per-entry path a row activation uses
// (built via playlistItemsCatalog so the picked row is byte-identical to the one the grid would activate).
void HomeView::playRandomFromPlaylist(const QString& playlistId)
{
    Playlist p;
    if (!PlaylistStore::get(playlistId, p) || p.items.isEmpty())
    { showToast(tr("This playlist is empty."), kFeedbackShort); return; }
    auto cat = browse::playlistItemsCatalog(p); // exactly the rows the playlist level shows
    const int idx = int(QRandomGenerator::global()->bounded(cat.items.size()));
    openResolvedItem(cat.items[idx], /*levelAddon=*/nullptr); // playlist level is addon-less: entries self-resolve
}

// Air one channel pick: resolve + open the entry at `index` through the SAME per-entry path a row activation
// uses (playlistItemsCatalog -> openResolvedItem), so a channel pick behaves byte-identically to opening that
// row by hand. MainWindow owns the shuffle bag + the natural-end chain and calls this for each pick.
HomeView::ChannelAir HomeView::playChannelItem(const QString& playlistId, int index, int gen)
{
    Playlist p;
    if (!PlaylistStore::get(playlistId, p)) return ChannelAir::Detoured;   // gone -> MainWindow skips/exits
    auto cat = browse::playlistItemsCatalog(p);      // 1:1 with p.items, same order (see SyntheticCatalogs)
    if (index < 0 || index >= cat.items.size()) return ChannelAir::Detoured;
    return openResolvedItem(cat.items[index], /*levelAddon=*/nullptr, /*forChannel=*/true, gen);
}

bool HomeView::channelItemPlaysDirectly(const QString& playlistId, int index)
{
    Playlist p;
    if (!PlaylistStore::get(playlistId, p)) return false;
    auto cat = browse::playlistItemsCatalog(p);
    if (index < 0 || index >= cat.items.size()) return false;
    const MediaItem& it = cat.items[index];
    if (it.mime.startsWith(QStringLiteral("localgame:"))) return true; // local file -> plays
    if (!it.url.isEmpty()) return true;                                // already carries a url -> plays
    LoadedAddon* addon = it.sourceAddonId.isEmpty() ? nullptr : mgr_->sourceById(it.sourceAddonId);
    // A remote leaf that will async-resolve a /stream (same gate openResolvedItem uses to attempt a resolve).
    if (!it.expandable && addon && addon->transport == LoadedAddon::RemoteHttp && !addon->stremio
        && it.type != QStringLiteral("platform") && !isInfoPageType(it.type)) return true;
    return false; // info-page movie/episode, container, or stream-less -> detail-page detour
}

// The path behind a local entry, read off the SAME catalog row playChannelItem would air (see the header for
// why only this shape answers). `kind` is the entry's recorded media kind ("audio", "video", "game"…) — the
// same string openRecent dispatches on, handed back so the caller routes on what the app WILL do with the
// file rather than on a second guess made from its extension.
QString HomeView::channelItemLocalPath(const QString& playlistId, int index, QString* kind)
{
    if (kind) kind->clear();
    Playlist p;
    if (!PlaylistStore::get(playlistId, p)) return QString();
    auto cat = browse::playlistItemsCatalog(p);
    if (index < 0 || index >= cat.items.size()) return QString();
    const MediaItem& it = cat.items[index];
    // "localgame:<kind>" is the mime playlistItemsCatalog stamps on an entry that carries a path, and the ONE
    // mime openResolvedItem re-opens by path. A store-game entry (steam:/epic:/gog:/bnet:) is not one of them
    // even though some of them carry a url, and a url that is really a link is not a file.
    static const QString kPrefix = QStringLiteral("localgame:");
    if (!it.mime.startsWith(kPrefix) || it.url.isEmpty()) return QString();
    if (it.url.contains(QStringLiteral("://"))) return QString();   // openRecent routes a link, not a file
    if (kind) *kind = it.mime.mid(kPrefix.size());
    return it.url;
}

// Rename via the OSK, prefilled with the current name; PlaylistStore::rename persists it and we refresh the
// list we're standing on. Empty / unchanged name is a no-op (covers a backed-out OSK too).
void HomeView::renamePlaylistInteractive(const QString& playlistId)
{
    Playlist p;
    if (!PlaylistStore::get(playlistId, p)) return;
    const QString name = Osk::getText(tr("Rename playlist:"), p.name, QLineEdit::Normal, window()).trimmed();
    if (name.isEmpty() || name == p.name) return;
    PlaylistStore::rename(playlistId, name);
    populatePlaylists(p.categoryKey); // we're on the playlists level -> re-render with the new name
}

// Delete behind a Cancel-focused confirm (the house shape for a destructive action: Back / default focus is
// the safe Cancel). Only the playlist bookkeeping is removed — files on disk are never touched.
void HomeView::deletePlaylistInteractive(const QString& playlistId)
{
    Playlist p;
    if (!PlaylistStore::get(playlistId, p)) return;
    const QString msg = tr("Delete “%1”? Its %n item(s) are removed from the playlist (files on disk are left in place).",
                           "", int(p.items.size())).arg(p.name);
    if (NavConfirm::ask(tr("Delete playlist"), msg, { tr("Delete"), tr("Cancel") },
                        /*focusIndex=*/1, /*cancelIndex=*/1, window()) != 0)
        return; // Cancel / Back
    PlaylistStore::remove(playlistId);
    populatePlaylists(p.categoryKey); // refresh the list we're standing on (the row disappears)
}

void HomeView::addItemToPlaylistInteractive(const MediaItem& it)
{
    if (it.type.startsWith(QLatin1Char('_'))) return; // a synthetic row (Playlists/New), not real media
    // The other two non-media types in the vocabulary. Every type NOT starting '_' is real media except these
    // two: "info" (a guidance row — an addon error, "Loading channels…", the Music category's "no music folder
    // yet" explanation) and "rechdr" (a Recent section divider). Both are SELECTABLE on the themed grid browse
    // view — browseItems() flushes a lone "info" row into browseRowMap_ with no `header` flag precisely so the
    // themed column can sit on it and show why the level is empty — so R lands on one on any empty level, and
    // without this it would file a PlaylistEntry with an empty itemId and the guidance text as its title: a
    // permanent, unopenable tile. Guarded HERE rather than at the call sites so every caller (themed grid,
    // XMB-in-catalog, the detail view's verb, classic "P") is covered by the one test.
    if (it.type == QStringLiteral("info") || it.type == QStringLiteral("rechdr")) return;
    if (atRecentsLevel() || atDownloadsLevel()) { showToast(tr("Open a catalogue item to add it to a playlist."), kFeedbackLong); return; }
    const QString key = currentCategoryKey(); // the whole category's playlists are offered, not just this catalogue's
    QVector<Playlist> pls = PlaylistStore::forCategory(key);
    QStringList opts;
    for (const Playlist& p : pls) opts << p.name;
    opts << tr("➕ New playlist…");
    const int row = NavMenu::pick(tr("Add “%1” to:").arg(it.title), opts, window()); // in-window picker
    if (row < 0) return;
    QString plid, plname;
    if (row == pls.size()) // the "New playlist…" row
    {
        const QString name = Osk::getText(tr("Playlist name:"), QString(), QLineEdit::Normal, window()).trimmed();
        if (name.isEmpty()) return;
        plid = PlaylistStore::create(key, name); plname = name;
    }
    else { plid = pls[row].id; plname = pls[row].name; }
    if (plid.isEmpty()) return;
    PlaylistEntry e;
    // Stamp the source addon. Store games have no LoadedAddon; tag them by id prefix so the playlist tile knows
    // its provenance (steam/epic/gog) — mirrors the steamgame add path, extended to epic:/gog:.
    auto storeAddonForId = [](const QString& id) -> QString {
        if (id.startsWith(QStringLiteral("steam:"))) return QStringLiteral("steam");
        if (id.startsWith(QStringLiteral("epic:")))  return QStringLiteral("epic");
        if (id.startsWith(QStringLiteral("gog:")))   return QStringLiteral("gog");
        if (id.startsWith(QStringLiteral("bnet:")))  return QStringLiteral("battlenet");
        return QString();
    };
    e.addonId = (!stack_.isEmpty() && stack_.last().addon) ? stack_.last().addon->manifest.id
              : storeAddonForId(it.id);
    e.itemId = it.id; e.title = it.title; e.subtitle = it.subtitle;
    e.type = it.type; e.thumbnailUrl = it.thumbnailUrl; e.expandable = it.expandable;
    // GOG tiles carry their resolved exe in `url`; persist it into the entry path so playlistItemsCatalog can
    // ride it back onto the tile and the monitored launchPcExe path can run it (steam/epic launch by id, no url).
    // A Battle.net tile takes the SAME line for both of its routes: a code-less tile carries its exe in `url`
    // (the GOG shape), and a coded one has an EMPTY url, so this propagates emptiness and the builder restores it.
    if (it.id.startsWith(QStringLiteral("gog:")) || it.id.startsWith(QStringLiteral("bnet:"))) e.path = it.url;
    // A LIVE TV CHANNEL FILES ITS IDENTITY IN BOTH FIELDS (#203) — never `it.url`, which is the provider's
    // signed stream. `kind` is what playlistItemsCatalog turns into the tile's "localgame:<kind>" mime, which
    // is the one route openResolvedItem re-opens a row by its own record on; without it the tile would be
    // unopenable. openRecent recognises the identity and resolves the url against this device's sources.
    else if (it.type == QStringLiteral("livetv"))
    { e.path = it.id; e.kind = QStringLiteral("livetv"); }
    PlaylistStore::addItem(plid, e);
    showToast(tr("Added “%1” to “%2”.").arg(it.title, plname), kFeedbackShort);
}

// DEFERRED A TURN (issue #28), and the deferral belongs HERE rather than at the call sites because all three
// of them are the same shape: the themed detail view's "playlist" verb, the XMB inline chooser's row 2, and
// "P" on a highlighted row. Each reaches this from inside a live QML delegate's own signal emission, and
// addItemToPlaylistInteractive spins nested event loops (NavMenu::pick, then Osk::getText on the "New
// playlist…" row) — which flush pending DeferredDeletes under the delegate that called us.
//
// The row index is resolved to a MediaItem COPY first, synchronously. A turn is a whole event-loop cycle, and
// an async re-present during it can rebuild browseRowMap_ underneath the same index — this verb WRITES a
// playlist entry, so a stale index would quietly file the wrong item and the user would have no reason to
// look. (The item was copied anyway: addItemToPlaylistInteractive takes a reference into items_ and then
// re-enters modal loops, during which that vector can be rebuilt.)
void HomeView::addBrowseItemToPlaylist(int browseIndex)
{
    if (browseIndex < 0 || browseIndex >= browseRowMap_.size()) return;
    const int row = browseRowMap_[browseIndex];
    if (row < 0 || row >= items_.size()) return;
    const MediaItem copy = items_[row];
    QMetaObject::invokeMethod(this, [this, copy] { addItemToPlaylistInteractive(copy); },
                              Qt::QueuedConnection);
}

// ---- Recents: a per-catalogue "Recent" folder (its recently-opened items, resumed on open) ----------------

bool HomeView::atRecentsLevel() const
{
    return !stack_.isEmpty() && stack_.last().detail
        && stack_.last().item.type == QStringLiteral("_recents");
}

// The RecentItem kind that belongs to the catalogue at the root (its bucket mapped to a recent kind).
QString HomeView::catalogRecentKind() const
{
    const QString cat = stack_.isEmpty() ? QString() : mediaCategory(stack_.first().catalogType);
    if (cat == QStringLiteral("audio"))   return QStringLiteral("audio");
    if (cat == QStringLiteral("game"))    return QStringLiteral("game");
    if (cat == QStringLiteral("reading")) return QStringLiteral("document");
    return QStringLiteral("video");
}

// Which of the three reading catalogues the root is — "book" | "comic" | "manga", empty for anything else.
// catalogRecentKind() above answers "which store rows", this answers "which of them"; the Recent/Downloaded
// markers carry it in the same slot a games console uses, and core::matchesReadingScope applies it. They are
// two questions because Books, Comics and Manga are three catalogues sharing ONE routing kind: a reading row
// is filed as "document" whichever it came from, so the kind alone showed all three the same list.
QString HomeView::catalogReadingForm() const
{
    return stack_.isEmpty() ? QString() : core::readingForm(stack_.first().catalogType);
}

void HomeView::openRecentsLevel(const QString& marker) // marker = "<kind>" or "<kind>|<system>"
{
    if (xmbMode_) { atXmbRoot_ = false; if (xmb_) xmb_->setAtRoot(false); }
    Level lvl;
    lvl.addon = nullptr; lvl.detail = true; lvl.title = tr("Recent");
    lvl.item.id = QStringLiteral("_recents");
    lvl.item.type = QStringLiteral("_recents");
    lvl.item.expandable = true;
    lvl.item.mime = QStringLiteral("recents:") + marker; // so loadTop() repopulates on Back
    stack_.push_back(lvl);
    populateRecents(marker);
}

// Show a locally built (addon-less) catalog level: reset paging state and hand it to the grid.
void HomeView::showSyntheticCatalog(const MediaCatalog& cat)
{
    pendingReqId_ = -1; loading_ = false; hasMore_ = false; currentPage_ = 1;
    hideMeta();
    if (carouselMode_ || xmbMode_) grid_->hide(); else grid_->show();
    populate(cat, /*append*/ false);
}

void HomeView::populateRecents(const QString& marker)
{ showSyntheticCatalog(browse::recentsCatalog(RecentStore::list(), marker)); }

bool HomeView::atDownloadsLevel() const
{
    return !stack_.isEmpty() && stack_.last().detail
        && stack_.last().item.type == QStringLiteral("_downloads");
}

bool HomeView::atFavoritesLevel() const
{
    return !stack_.isEmpty() && stack_.last().detail
        && stack_.last().item.type == QStringLiteral("_favorites");
}

void HomeView::openDownloadsLevel(const QString& marker)
{
    if (xmbMode_) { atXmbRoot_ = false; if (xmb_) xmb_->setAtRoot(false); }
    Level lvl;
    lvl.addon = nullptr; lvl.detail = true; lvl.title = tr("Downloaded");
    lvl.item.id = QStringLiteral("_downloads");
    lvl.item.type = QStringLiteral("_downloads");
    lvl.item.expandable = true;
    lvl.item.mime = QStringLiteral("downloads:") + marker; // so loadTop() repopulates on Back
    stack_.push_back(lvl);
    populateDownloads(marker);
}

void HomeView::populateDownloads(const QString& marker)
{ showSyntheticCatalog(browse::downloadsCatalog(DownloadsStore::list(), marker)); }

void HomeView::openLocalLibraryLevel(const QString& marker)
{
    if (xmbMode_) { atXmbRoot_ = false; if (xmb_) xmb_->setAtRoot(false); }
    Level lvl;
    lvl.addon = nullptr; lvl.detail = true; lvl.title = tr("Local Library");
    lvl.item.id = QStringLiteral("_locallib");
    lvl.item.type = QStringLiteral("_locallib");
    lvl.item.expandable = true;
    lvl.item.mime = QStringLiteral("locallib:") + marker; // so loadTop() repopulates on Back
    stack_.push_back(lvl);
    populateLocalLibrary(marker);
}

void HomeView::populateLocalLibrary(const QString& /*marker*/)
{ showSyntheticCatalog(browse::localLibraryCatalog(LocalLibrary::index().all())); }

void HomeView::onLocalLibraryChanged()
{
    if (stack_.isEmpty()) return;
    const auto& top = stack_.last();
    if (top.item.type == QStringLiteral("_locallib")) {
        populateLocalLibrary(top.item.mime.mid(QStringLiteral("locallib:").size()));
        return;
    }
    // Only a catalogue root shows the synthetic folders; refresh there so the newly-non-empty library
    // surfaces its folder. Anywhere else (detail/search/other), the folder appears on the next navigation
    // to a root (the present-check runs on every populate) — do NOT reload the user's current level.
    if (!top.detail && top.query.isEmpty() && !recentView_)
        loadTop();
}

// ---- Trakt "Airing Soon" (#23): the Home shelf + the video-catalogue folder ------------------------------
//
// THE gate. Every Trakt surface in this file asks this one question and shows nothing when the answer is an
// empty catalog — which it always is unless a Trakt account is BOTH configured and connected, because
// traktCal_ is only ever filled behind calendarAvailable(). Belt and braces on purpose: the availability
// check is re-asserted here rather than trusted to have been asserted when traktCal_ was loaded, so a future
// path that fills traktCal_ without checking still cannot make the shelf appear on an unconfigured install.
MediaCatalog HomeView::traktCalendarItems() const
{
    if (!TraktClient::calendarAvailable()) return MediaCatalog{};
    return browse::traktCalendarCatalog(traktCal_, QDateTime::currentDateTimeUtc()); // UTC, like the entries
}

void HomeView::openTraktCalendarLevel()
{
    if (xmbMode_) { atXmbRoot_ = false; if (xmb_) xmb_->setAtRoot(false); }
    Level lvl;
    lvl.addon = nullptr; lvl.detail = true; lvl.title = tr("Airing Soon");
    lvl.item.id = QStringLiteral("_traktcal");
    lvl.item.type = QStringLiteral("_traktcal");
    lvl.item.expandable = true;
    lvl.item.mime = QStringLiteral("traktcal:"); // so loadTop() repopulates on Back
    stack_.push_back(lvl);
    populateTraktCalendar();
}

void HomeView::populateTraktCalendar()
{ showSyntheticCatalog(traktCalendarItems()); }

// ---- Trakt "You Missed" (#25): the Home shelf + the video-catalogue folder --------------------------------
//
// The same gate, re-asserted for the same reason, over the SAME cached calendar. Nothing new is fetched:
// #23's fetch now asks for the lookback window as well as the week ahead (MainWindow::refreshTraktCalendar),
// so both surfaces read one array and the pure rule decides which entries belong to which.
//
// The two local-state lookups the rule needs are supplied HERE, at the one place that is allowed to touch
// both stores, and nothing about either store reaches TraktMissed.cpp:
//
//   * "what does the app already know about this episode" — ItemMarks, under the SAME key the Trakt
//     watched-backfill writes (the stream id), which is why importing your history clears these rows. Both
//     non-Unmarked answers mean the same thing to this rule (TraktMissed.h says why), and `hidden` is
//     folded in beside completion: hiding an item is as explicit a statement as marking it.
//   * "has this show been waved away, and through when" — MissedDismiss.
MediaCatalog HomeView::traktMissedItems(int maxRows) const
{
    if (!TraktClient::calendarAvailable()) return MediaCatalog{};
    const QVector<trakt::MissedRow> rows = trakt::planMissed(
        traktCal_, QDateTime::currentDateTimeUtc(), trakt::kMissedLookbackDays,
        [](const QString& streamId) {
            const ItemMarks::Marks m = ItemMarks::get(streamId);
            if (m.completion == ItemMarks::Completion::Finished) return trakt::LocalState::Watched;
            if (m.completion != ItemMarks::Completion::None || m.hidden) return trakt::LocalState::OtherExplicit;
            return trakt::LocalState::Unmarked;
        },
        [](const QString& showKey) { return MissedDismiss::through(showKey); });
    return browse::traktMissedCatalog(rows, maxRows);
}

void HomeView::openTraktMissedLevel()
{
    if (xmbMode_) { atXmbRoot_ = false; if (xmb_) xmb_->setAtRoot(false); }
    Level lvl;
    lvl.addon = nullptr; lvl.detail = true; lvl.title = tr("You Missed");
    lvl.item.id = QStringLiteral("_traktmissed");
    lvl.item.type = QStringLiteral("_traktmissed");
    lvl.item.expandable = true;
    lvl.item.mime = QStringLiteral("traktmissed:"); // so loadTop() repopulates on Back
    stack_.push_back(lvl);
    populateTraktMissed();
}

void HomeView::populateTraktMissed()
{ showSyntheticCatalog(traktMissedItems(0)); }   // the FOLDER is uncapped; only the shelf is a glance

void HomeView::showTraktMissedMenu(MediaItem it)
{
    const QString showKey = browse::traktMissedShowKeyOf(it.mime);
    const qint64  through = browse::traktMissedThroughOf(it.mime);
    const QStringList rows = {
        tr("▶   Play %1").arg(it.subtitle.section(QStringLiteral(" · "), 0, 0)),
        tr("✓   I'm caught up — stop showing this"),
    };
    new NavMenu(it.title, rows, [this, it, showKey, through](int row) {
        if (row == 0)
        {
            resolvePlay(nullptr, it, QString(), QString(), it.imdbStreamId, QStringLiteral("series"));
            return;
        }
        if (row != 1) return;
        // Fails CLOSED: a row whose marker did not carry both halves does nothing rather than filing a
        // dismissal under an empty key or through the epoch. Neither can happen from a row this build
        // produced — probe_browse pins the round trip — so this is the guard for a row some LATER build
        // produced that this one is still rendering after an update.
        if (showKey.isEmpty() || through <= 0) return;
        MissedDismiss::dismissThrough(showKey, through);
        // Say what it did AND what it did not do. "Stop showing this" reads as unfollow, and the one thing
        // a user must not have to discover by waiting a week is that the show comes back when it airs.
        showToast(tr("Caught up on “%1”. It will reappear when a new episode airs.").arg(it.title),
                  kFeedbackLong);
        if (recentView_) renderRecents();   // the Home shelf loses the row
        else             loadTop();         // ...or the folder does
        emit browseItemsChanged(false);     // re-sync a themed browse view (else its selection desyncs)
    }, window());
}

// The watchlist / collection folders. Same gate as the calendar, re-asserted here rather than trusted to
// have been asserted when the vectors were filled, so a future path that fills them without checking
// still cannot make a folder appear on an unconfigured install.
MediaCatalog HomeView::traktListItems(const QString& which) const
{
    if (!TraktClient::calendarAvailable()) return MediaCatalog{};
    return which == QStringLiteral("collection")
               ? browse::traktListCatalog(traktCollection_, tr("Trakt Collection"))
               : browse::traktListCatalog(traktWatchlist_,  tr("Trakt Watchlist"));
}

// "Is there a folder to draw?" — the same gate and the same admissibility rule as traktListItems, with
// none of the building. The folder list is rebuilt on every navigation into the video root and this is
// the only question it asks of these lists; building a catalog of thousands of rows and sorting it in
// full just to compare the result against zero is what this replaces.
bool HomeView::traktListHasRows(const QString& which) const
{
    if (!TraktClient::calendarAvailable()) return false;
    return browse::traktListHasRows(which == QStringLiteral("collection") ? traktCollection_
                                                                          : traktWatchlist_);
}

void HomeView::openTraktListLevel(const QString& which)
{
    if (xmbMode_) { atXmbRoot_ = false; if (xmb_) xmb_->setAtRoot(false); }
    const bool coll = (which == QStringLiteral("collection"));
    Level lvl;
    lvl.addon = nullptr; lvl.detail = true;
    lvl.title = coll ? tr("Trakt Collection") : tr("Trakt Watchlist");
    lvl.item.id = QStringLiteral("_traktlist:") + which;
    lvl.item.type = QStringLiteral("_traktlist");
    lvl.item.expandable = true;
    // The marker carries WHICH list, so loadTop() repopulates the right one on Back.
    lvl.item.mime = QStringLiteral("traktlist:") + which;
    stack_.push_back(lvl);
    populateTraktList(which);
}

void HomeView::populateTraktList(const QString& which)
{ showSyntheticCatalog(traktListItems(which)); }

void HomeView::onTraktListsChanged()
{
    // Re-read rather than being handed the lists: TraktClient wrote the caches, and reading them back is
    // what makes the offline path and the just-fetched path the same code. Disconnected => empty.
    const bool on = TraktClient::calendarAvailable();
    traktWatchlist_  = on ? TraktClient::cachedWatchlist()  : QVector<TraktListEntry>{};
    traktCollection_ = on ? TraktClient::cachedCollection() : QVector<TraktListEntry>{};
    if (stack_.isEmpty()) return;
    const auto& top = stack_.last();
    if (top.item.type == QStringLiteral("_traktlist"))
    { populateTraktList(top.item.mime.section(QLatin1Char(':'), 1)); return; }
    // Only a catalogue root shows the synthetic folders — refresh there so a folder appears (or, after a
    // disconnect, disappears). Anywhere else it settles on the next navigation. Same rule as the calendar.
    if (!top.detail && top.query.isEmpty()) loadTop();
}

void HomeView::onTraktCalendarChanged()
{
    // Re-read rather than being handed a list: TraktClient wrote the cache, and reading it back is what
    // makes the offline path and the just-fetched path the same code. Disconnected => empty, unconditionally.
    traktCal_ = TraktClient::calendarAvailable() ? TraktClient::cachedCalendar() : QVector<CalendarEntry>{};
    // A DISCONNECT arrives on this signal too, and it must take the lists with it — otherwise the
    // watchlist folder survives the unlink until something else happens to refresh it.
    if (!TraktClient::calendarAvailable())
    { traktWatchlist_.clear(); traktCollection_.clear(); }
    if (recentView_) { renderRecents(); emit browseItemsChanged(false); return; } // the Home shelf
    if (stack_.isEmpty()) return;
    const auto& top = stack_.last();
    if (top.item.type == QStringLiteral("_traktcal")) { populateTraktCalendar(); return; }
    if (top.item.type == QStringLiteral("_traktmissed")) { populateTraktMissed(); return; }
    // Only a catalogue root shows the synthetic folders — refresh there so the folder appears (or, after a
    // disconnect, disappears). Anywhere else it settles on the next navigation to a root; do NOT reload the
    // level the user is standing in. Same rule as onLocalLibraryChanged.
    if (!top.detail && top.query.isEmpty()) loadTop();
}

void HomeView::openFavoritesLevel(const QString& system)
{
    if (xmbMode_) { atXmbRoot_ = false; if (xmb_) xmb_->setAtRoot(false); }
    Level lvl;
    lvl.addon = nullptr; lvl.detail = true; lvl.title = tr("Favorites");
    lvl.item.id = QStringLiteral("_favorites");
    lvl.item.type = QStringLiteral("_favorites");
    lvl.item.expandable = true;
    lvl.item.mime = QStringLiteral("favorites:") + system; // so loadTop() repopulates on Back
    stack_.push_back(lvl);
    populateFavorites(system);
}

void HomeView::populateFavorites(const QString& system)
{ showSyntheticCatalog(browse::favoritesCatalog(FavoritesStore::list(), system)); }

// ---- Homebrew: a per-console folder of what the configured servers have for it ---------------------------
//
// The same synthetic-level shape as the three folders beside it, with one difference that runs through all of
// this: those read a local store, this asks the network. So the level is pushed first and filled in when the
// answers arrive, and "this console has none" is an ordinary empty level rather than a folder that was never
// offered. See the row's own comment in populate() for why it cannot be gated any tighter than that.
//
// A title's id is a fully host-namespaced media id, so a row plays through the ordinary stream route of the
// server that minted it — sourceAddonId names that server, and openResolvedItem takes it from there. There is
// no homebrew download path, deliberately: a second one would be a second thing to keep correct, for nothing.
void HomeView::openHomebrewLevel(const QString& system)
{
    if (xmbMode_) { atXmbRoot_ = false; if (xmb_) xmb_->setAtRoot(false); }
    Level lvl;
    lvl.addon = nullptr; lvl.detail = true; lvl.title = tr("Homebrew");
    lvl.item.id = QStringLiteral("_homebrew");
    lvl.item.type = QStringLiteral("_homebrew");
    lvl.item.expandable = true;
    lvl.item.mime = HomebrewClient::levelMime(system); // so loadTop() repopulates on Back
    stack_.push_back(lvl);
    populateHomebrew(system);
}

// Back out of a played title lands here: re-fetch page one. Unlike the Live TV channel level there is no
// in-session cache to fall back on — a page of homebrew is small, and re-asking is refresh-on-open, which is
// the same rule the OPDS feed level follows.
void HomeView::populateHomebrew(const QString& system)
{ fetchHomebrew(system, {}, /*append*/ false); }

void HomeView::fetchHomebrew(const QString& system, const QVector<HomebrewMore>& more, bool append)
{
    const int gen = ++homebrewFetchGen_;   // supersede any in-flight fetch; a stale reply is dropped below
    if (!append) homebrewRows_.clear();
    homebrewMore_.clear();                 // the continuations are replaced wholesale by this round's answers

    // Which server to ask, and with which cursor. No continuation means the first page from every configured
    // server; a "More…" row means exactly the servers that said they had more, each with its own opaque token.
    QVector<HomebrewMore> requests = more;
    if (requests.isEmpty())
        for (const QString& base : (mgr_ ? mgr_->remoteSourceUrls() : QStringList()))
            requests.push_back({ base, QString() });

    if (requests.isEmpty()) { showHomebrewPage(system); return; }   // nothing configured: an empty level

    // A loading placeholder while the fetches are in flight, so a slow server never looks like a dead folder.
    if (!append)
    {
        MediaCatalog c; c.title = tr("Homebrew");
        MediaItem info; info.type = QStringLiteral("info"); info.title = tr("Loading…");
        c.items.push_back(info);
        showSyntheticCatalog(c);
    }

    // Every reply lands here. `outstanding` is shared by the lambdas so the page renders once, after the last
    // server has answered — a server that is down or has no homebrew source contributes nothing and never
    // stops the others being shown, which is the same rule the romhack flow holds.
    auto outstanding = std::make_shared<int>(requests.size());
    auto seen = std::make_shared<QSet<QString>>();
    for (const MediaItem& row : homebrewRows_) seen->insert(row.id);

    for (const HomebrewMore& req : requests)
    {
        // The addon that owns this base URL, so an activated row resolves its stream through the right server.
        QString addonId;
        if (mgr_)
            for (LoadedAddon* a : mgr_->sources())
                if (a->transport == LoadedAddon::RemoteHttp && a->baseUrl == req.base)
                    { addonId = a->manifest.id; break; }

        const QString url = HomebrewClient::listUrl(req.base, system, req.cursor);
        QNetworkRequest nr{ QUrl(url) };
        nr.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(AppBrand::kUserAgent));
        nr.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        QNetworkReply* reply = nam_->get(nr);
        const QString base = req.base;
        connect(reply, &QNetworkReply::finished, this,
                [this, reply, gen, system, base, addonId, outstanding, seen] {
            reply->deleteLater();
            if (gen != homebrewFetchGen_) return;   // superseded by a newer navigation
            if (reply->error() == QNetworkReply::NoError)
            {
                const HomebrewPage page = HomebrewClient::parseList(reply->readAll());
                for (const HomebrewTitle& t : page.items)
                {
                    if (seen->contains(t.id)) continue;   // the same title from two servers is still one title
                    seen->insert(t.id);
                    MediaItem m;
                    m.id = t.id;
                    m.type = QStringLiteral("game");
                    m.title = t.title.isEmpty() ? t.id : t.title;
                    m.subtitle = t.subtitle();
                    m.thumbnailUrl = t.imageUrl;
                    m.systemHint = system;
                    m.sourceAddonId = addonId;   // whose /stream resolves it
                    homebrewRows_.push_back(m);
                }
                if (page.hasMore()) homebrewMore_.push_back({ base, page.nextCursor });
            }
            if (--(*outstanding) > 0) return;   // still waiting on another server
            showHomebrewPage(system);
        });
    }
}

void HomeView::showHomebrewPage(const QString& system)
{
    MediaCatalog c;
    c.title = tr("Homebrew");
    c.items = homebrewRows_;
    if (homebrewRows_.isEmpty())
    {
        // The row was offered without knowing whether there was anything behind it (populate() says why), so
        // this is a normal outcome, not a failure. It reads the same whether the console has no homebrew or
        // every server was unreachable, because to someone browsing those are the same thing.
        MediaItem info;
        info.type = QStringLiteral("info");
        info.title = tr("No homebrew here yet.");
        c.items.push_back(info);
    }
    else if (!homebrewMore_.isEmpty())
    {
        // The trailing "More…" row carries every outstanding continuation. Its type starts with '_', so the
        // themed layouts drill it rather than offering it a Play/Favorite chooser.
        MediaItem more;
        more.id = QStringLiteral("_homebrewmore");
        more.type = QStringLiteral("_homebrewmore");
        more.title = tr("More…");
        more.expandable = true;
        more.mime = HomebrewClient::moreMime(system, homebrewMore_);
        c.items.push_back(more);
    }
    // The level's childRow is left exactly as it was: activating the "More…" row already recorded ITS index,
    // and the appended page's first row lands there — so the next page opens where the reader was standing
    // rather than back at the top. On a fresh open it is -1 and the level opens at the top, as it should.
    showSyntheticCatalog(c);
}

// Extract one game's queryable facts (issue #63) for the pure filter evaluator, from the SAME per-game stores
// every other surface reads: FavoritesStore (★), ItemMarks (hidden/tags/completion, cache-backed), PlayStats
// (playtime), and the item's already-in-memory scraped metadata (art.meta) for genre/players/release year.
// The scraped fields are best-effort: a game with no scrape simply carries empty genres / zero counts and so
// never matches a scraped-field dimension — issue #63's stated scope. No disk read is added on this hot path
// (art.meta is what the display already loaded), so it is as cheap to run over a whole console as shelfMatches.
gamefilter::GameFacts HomeView::gameFactsFor(const MediaItem& it) const
{
    gamefilter::GameFacts g;
    const QString key = MetaCache::keyFor(it);
    g.favorite = FavoritesStore::isFavorite(key);
    const ItemMarks::Marks m = ItemMarks::get(key);
    g.hidden      = m.hidden;
    g.tags        = m.tags;
    g.completion  = static_cast<int>(m.completion);
    g.playSeconds = PlayStats::get(PlayStats::identity(key, it.url)).totalSeconds;
    // System id, in the same id space the filter builder offers: the console we are drilled into (authoritative
    // for an addon-catalog game whose row carries no id), else the item's own hint, else the file extension.
    QString sys;
    if (!stack_.isEmpty() && stack_.last().item.type == QStringLiteral("platform"))
    {
        const QString cn = stack_.last().item.title.trimmed();
        if (const GameSystem* s = SystemCatalog::forConsoleName(cn)) sys = s->id;
        else if (cn.toLower().contains(QStringLiteral("pc")) || cn.compare(QStringLiteral("windows"), Qt::CaseInsensitive) == 0)
            sys = QStringLiteral("pc");
    }
    if (sys.isEmpty()) sys = it.systemHint;
    if (sys.isEmpty() && !it.url.isEmpty())
        if (const GameSystem* s = SystemCatalog::forExtension(QFileInfo(it.url).suffix().toLower())) sys = s->id;
    if (!sys.isEmpty()) g.systems = { sys.toLower() };
    // Scraped facts from the item's in-memory metadata bundle (GamelistStore / the game aggregator write these
    // keys — see GamelistStore.cpp). Parsed by the pure helpers so the extraction is itself probe-covered.
    const QVariantMap& meta = it.art.meta;
    g.genres      = gamefilter::splitGenres(meta.value(QStringLiteral("genre")).toString());
    g.maxPlayers  = gamefilter::parseMaxPlayers(meta.value(QStringLiteral("players")).toString());
    QString yr = meta.value(QStringLiteral("releasedate")).toString();
    if (yr.isEmpty()) yr = meta.value(QStringLiteral("year")).toString();
    g.releaseYear = gamefilter::parseYear(yr);
    // The classical credits of a MUSIC track (issue #196, part 2), read from the SAME in-memory bundle and
    // by the same rule as everything above it. MusicCatalogs::trackRow puts the ALREADY-SPLIT lists here
    // (browse/MusicCatalogs.cpp says why there are two keys); the display string beside them is for a
    // theme's meta panel and is deliberately not what this reads, because re-splitting it would be a second
    // copy of a split only the scan could make. A game, and a track with no such tag, carries neither key
    // and gets two empty lists — which match no dimension, exactly as an unscraped genre does.
    g.composers  = meta.value(QStringLiteral("composers")).toStringList();
    g.conductors = meta.value(QStringLiteral("conductors")).toStringList();
    return g;
}

// The current level's real items that belong on a shelf: favorites (favshelf:), a pinned tag (tagshelf:<tag>),
// or hidden (hiddenshelf:). Hidden items are excluded from the favorites/tag shelves (a hidden item stays
// hidden everywhere unless Show-hidden is on, in which case isHiddenItem() is false and it can appear); the
// hidden shelf is the one surface that lists them (and only shows while Show-hidden is on). Marks are
// cache-backed, so this is O(1) per candidate.
QVector<MediaItem> HomeView::shelfMatches(const MediaItem& folder) const
{
    QVector<MediaItem> out;
    const QString& mime = folder.mime;
    for (const MediaItem& it : items_)
    {
        if (it.type == QStringLiteral("rechdr") || it.type == QStringLiteral("info")
            || it.type.startsWith(QLatin1Char('_')))
            continue;
        if (mime == QStringLiteral("hiddenshelf:"))
        {
            if (ItemMarks::get(MetaCache::keyFor(it)).hidden) out.push_back(it);
            continue;
        }
        if (isHiddenItem(it)) continue; // fav/tag shelves never surface a hidden item
        if (mime == QStringLiteral("favshelf:"))
        {
            if (FavoritesStore::isFavorite(MetaCache::keyFor(it))) out.push_back(it);
        }
        else if (mime.startsWith(QStringLiteral("tagshelf:")))
        {
            if (ItemMarks::get(MetaCache::keyFor(it)).tags.contains(mime.mid(9))) out.push_back(it);
        }
        else if (mime.startsWith(QStringLiteral("presetshelf:")))
        {
            // A saved filter preset (#63): the pure evaluator over this game's extracted facts. Hidden games
            // were already excluded above (same rule as the fav/tag shelves), so a preset never surfaces one
            // unless Show-hidden is on.
            const FilterPreset p = FilterPresetStore::get(mime.mid(QStringLiteral("presetshelf:").size()));
            if (gamefilter::matches(p.filter, gameFactsFor(it))) out.push_back(it);
        }
    }
    return out;
}

// Drill a shelf folder: snapshot the intersection from the PARENT level's items into the pushed Level (so Back
// re-shows it with no re-fetch), inheriting the parent's addon so an addon-catalog item still resolves on open.
void HomeView::openShelfLevel(const MediaItem& folder)
{
    if (xmbMode_) { atXmbRoot_ = false; if (xmb_) xmb_->setAtRoot(false); }
    const QVector<MediaItem> matches = shelfMatches(folder);
    Level lvl;
    lvl.addon = stack_.isEmpty() ? nullptr : stack_.last().addon; // resolve item opens via the parent addon
    lvl.detail = true;
    lvl.title = folder.title;
    lvl.item.id = folder.id;
    lvl.item.type = folder.type;   // "_favshelf" | "_tagshelf" | "_hiddenshelf" (loadTop rebuilds from synthItems)
    lvl.item.expandable = true;
    lvl.item.mime = folder.mime;
    lvl.synthItems = matches;
    stack_.push_back(lvl);
    MediaCatalog cat;
    cat.items = matches;
    showSyntheticCatalog(cat);
}

// The ONE ingress every row of items_ passes through. Composites the user's correction to a wrong scrape
// (issue #24) so every surface items_ feeds — the poster grid, the carousel, the XMB column, the themed
// browse model, search results, the Home recents/favourites/Trakt rows, and the detail card that opens from
// any of them — shows the fix without each of them knowing about it. Only display fields move: keyFor()
// reads id/url, so the item's identity (and therefore which correction is its own) is untouched on every
// later pass, and the correction cannot follow the wrong item.
//
// The pre-correction copy is kept for the one caller that must NOT see the composite: the metadata editor,
// whose baseline is what the SCRAPER said and whose "typed back what the scraper found -> store nothing"
// comparison runs against it. The composite overwrites in place, and for a catalog row that was never saved
// to MetaCache the scraped title then exists nowhere else — the editor would offer the user's own edit as
// the thing it overrides, and retyping the visible value would CLEAR the correction. Only rows that
// actually carry one are kept, so the map is bounded by the corrections on screen, not by the catalog.
MediaItem HomeView::correctedRow(const MediaItem& src)
{
    const QString key = MetaCache::keyFor(src);
    const MetaOverrides::Override ov = MetaOverrides::get(key);
    if (ov.isEmpty()) return src;   // nothing to composite, and nothing displaced worth keeping
    preCorrection_.insert(key, src);
    MediaItem it = src;
    MetaOverrides::applyTo(ov, it);
    return it;
}

// That same row as the providers gave it: the stashed pre-correction copy when this row carries a
// correction, else the row itself (which the composite left untouched).
MediaItem HomeView::scrapedRow(const MediaItem& shown) const
{
    return preCorrection_.value(MetaCache::keyFor(shown), shown);
}

// The chapters either side of `currentId`, from the level this view last listed. `currentId` not being in
// that list yields an invalid run, which every consumer reads as "no neighbours" — so a chapter opened from
// somewhere no chapter list was ever browsed behaves exactly as it did before this feature existed.
// `catalogLane` is what the CALLER is about to open: a manga chapter resolves to page images, a comic
// issue is searched for at a file provider. The list and its order are identical either way — only the
// lane and the series name differ — so both go through one builder.
ChapterRun HomeView::chapterRunFor(const QString& currentId, bool catalogLane) const
{
    ChapterRun run = ChapterOrder::fromChapterItems(chapterList_, currentId);
    // The container, on BOTH remote lanes. It used to be attached for the Catalog one alone, because the
    // only consumer was that lane's provider search — but a chapter arrival now writes a Recents row, and
    // "Vol. 1 · Ch. 4" with no series and no cover is a row nobody can identify. Inert on the Chapters
    // lane otherwise: every other reader of seriesTitle is gated on Lane::Catalog.
    run.seriesTitle = chapterSeriesTitle_;
    run.seriesThumb = chapterSeriesThumb_;
    run.seriesAddonId = chapterSeriesAddonId_;
    // The entry TYPE, so a crossing can ask the addon for the next chapter's pages without assuming what
    // kind of serial this is (#188). Only meaningful on the Chapters lane; the Catalog lane's entries are
    // comic issues, which reach a file provider instead.
    if (!catalogLane) run.entryType = chapterEntryType_;
    if (catalogLane) run.lane = ChapterRun::Lane::Catalog;
    return run;
}

void HomeView::renderRecents()
{
    ++generation_;             // invalidate stale thumbnail loads
    atCarouselLanding_ = false;
    if (carousel_) carousel_->hide(); // Home is always the recents list, even in carousel layout
    applyGridMode(/*recentList*/ true);
    grid_->clear();
    items_.clear();
    preCorrection_.clear();  // the pre-correction stash is per rendered page, like items_ itself
    thumbQueue_.clear();
    grid_->show();
    settingsStore().sync(); // pick up resume positions written by the player since the last render

    const QSize iconSz(44, 44);

    // A non-selectable header row that spans the list width.
    auto addHeader = [this](const QString& label) {
        MediaItem hdr;
        hdr.type = QStringLiteral("rechdr");
        hdr.title = label; // carried into browseItems() so the themed column can draw it as a section divider
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

    // ---- The home as a SEQUENCE OF SHELVES (issue #161) --------------------------------------------------
    // Every shelf below is BUILT first and DRAWN second, because the profile's row list decides the order,
    // which shelves appear and how many items each may show — and none of that can be decided while widgets
    // are already going onto the list. Nothing about what a shelf CONTAINS changed here; the builders are the
    // bodies that used to run inline, moved behind a rowId.
    //
    // The default is load-bearing: with no stored list, `available` is built by walking
    // homerows::defaultShelfOrder() and the planner hands it straight back, so an untouched profile gets the
    // exact sequence — recently-played groups, "You Missed", "Airing Soon", "★ Favorites" — this function
    // produced before #161. probe_homerows pins both halves of that.
    struct Group { QString header; QVector<MediaItem> items; };  // a shelf's rows, under an optional divider
    enum RowStyle { StyleResume, StyleWhen, StylePlain };         // how a row's label/icon is drawn
    struct Shelf { QString rowId; RowStyle style = StylePlain; QVector<Group> groups; int count = 0; };
    QVector<Shelf> shelves;
    auto pushShelf = [&shelves](const QString& rowId, RowStyle style, QVector<Group> groups) {
        int n = 0;
        for (const Group& g : groups) n += int(g.items.size());
        if (n == 0) return;   // an empty producer is not an available row — exactly today's "no rows, no header"
        shelves.push_back({ rowId, style, std::move(groups), n });
    };

    // "continue" — the recently-played shelf, bucketed into groups (media type, per-console for games) and
    // kept in newest-first group order. Its per-group dividers are the reason a shelf is a list of groups.
    auto buildContinue = [this]() {
        QVector<Group> out;
        QStringList order;
        QHash<QString, QVector<RecentItem>> groups;
        for (const RecentItem& r : RecentStore::list())
        {
            const QString key = recentGroupKey(r);
            if (!groups.contains(key)) order << key;
            groups[key].push_back(r);
        }
        for (const QString& key : order)
        {
            // Map the group's recents to MediaItems and drop the hidden ones first, so a group whose every item
            // is hidden contributes no orphan header (same rule as the Favorites section).
            Group g;
            g.header = recentGroupLabel(key);
            for (const RecentItem& r : groups[key])
            {
                MediaItem it;
                it.url = r.path;                         // re-open target
                it.id = r.key;                           // stable resume key (streamed items); also read by XMB/carousel
                it.mime = r.kind;                        // routing kind (video/audio/document/game)
                it.type = browse::iconTypeForKind(r.kind); // drives the placeholder icon
                // The real poster (streamed media records it), else a placeholder — the locally cached copy
                // (saved when the item was downloaded) wins so the shelf renders offline. Scraped-side read:
                // correctedRow below puts the user's corrected poster on top and keeps this as its baseline.
                it.thumbnailUrl = MetaCache::scrapedImage(r.key.isEmpty() ? r.path : r.key, r.thumb);
                it.title = r.title.isEmpty() ? QFileInfo(r.path).completeBaseName() : r.title;
                if (isHiddenItem(it)) continue;          // hidden mark drops the recent row (and search/shelves elsewhere)
                // RecentStore holds the title as it was when the item was played, so a recents row is a scraped
                // source too — and this is the surface the app LANDS on. Without the ingress composite Home
                // showed a corrected poster beside an uncorrected title, on both the list and the XMB column
                // (fillXmbFromItems reads items_).
                g.items.push_back(correctedRow(it));
            }
            if (!g.items.isEmpty()) out.push_back(g);
        }
        return out;
    };

    // "trakt:missed" — Trakt "You Missed" (#25): the episodes of your followed shows that already aired and
    // you have not seen. COMPLETELY ABSENT unless a Trakt account is configured AND connected — the catalog is
    // empty otherwise, and an empty shelf is never pushed, so an install that never heard of Trakt renders
    // exactly the rows it rendered before this existed. No shelf, no header, no placeholder, no hint. BOUNDED
    // at trakt::kMissedShelfMax: this is the one shelf whose length is driven by how long the user has been
    // away, and a strip you have to scroll has stopped being a glance. The folder under the video catalogue is
    // where the whole backlog lives.
    auto buildTraktMissed = [this]() {
        QVector<Group> out;
        Group g;
        g.header = tr("You Missed");
        for (const MediaItem& raw : traktMissedItems(trakt::kMissedShelfMax).items)
            g.items.push_back(correctedRow(raw));   // Trakt's title is a scrape like any other
        if (!g.items.isEmpty()) out.push_back(g);
        return out;
    };

    // "trakt:calendar" — Trakt "Airing Soon" (#23): the episodes of your followed shows still to air this
    // week. Same absent-unless-there-is-something rule as the shelf above, for the same reason, and a calendar
    // whose every episode has already aired leaves no orphan divider.
    auto buildTraktCalendar = [this]() {
        QVector<Group> out;
        Group g;
        g.header = tr("Airing Soon");
        for (const MediaItem& raw : traktCalendarItems().items)
            g.items.push_back(correctedRow(raw));   // Trakt's own copy of the title is a scrape like any other
        if (!g.items.isEmpty()) out.push_back(g);
        return out;
    };

    // "favorites" — the per-profile, starred-media shelf. Its rows are built (and hidden-filtered) before the
    // header is decided, so a profile whose every favourite is hidden gets no lingering "★ Favorites" divider.
    auto buildFavorites = [this]() {
        QVector<Group> out;
        Group g;
        g.header = tr("★ Favorites");
        for (const FavoriteItem& f : FavoritesStore::list())
        {
            MediaItem it;
            it.id = f.itemId;
            it.type = f.type;
            it.title = f.title;
            it.subtitle = f.subtitle;
            it.thumbnailUrl = MetaCache::scrapedImage(f.itemId, f.thumbnailUrl); // offline-first artwork
            it.expandable = f.expandable;
            it.mime = QStringLiteral("fav:") + f.addonId; // marks a favourite + carries its source addon
            if (isHiddenItem(it)) continue;               // hidden mark hides it from the Favorites shelf too
            // The favourite's title/subtitle are the copy FavoritesStore saved when it was starred, so this
            // shelf is a scraped source like any other and needs the same ingress composite the catalog rows
            // get — otherwise Home showed a corrected poster (displayImage already ran it) beside an
            // uncorrected title, on the screen the app lands on.
            g.items.push_back(correctedRow(it));
        }
        if (!g.items.isEmpty()) out.push_back(g);
        return out;
    };

    // "downloads" — the fully-downloaded items, the DownloadsStore rows the per-catalogue "Downloaded" folder
    // is built from. An OPT-IN shelf (homerows::isOptInShelf): it is only built when the row list asks for it,
    // because the planner appends any producible row the list has not heard of, and an always-available
    // producer here would grow a Downloads shelf on every untouched profile in the world. A row whose file is
    // gone is skipped for the same reason the Downloaded folder skips it — the entry outlives the file.
    auto buildDownloads = [this]() {
        QVector<Group> out;
        Group g;
        g.header = tr("⬇ Downloaded");
        for (const DownloadedItem& d : DownloadsStore::list())
        {
            if (d.path.isEmpty() || !QFileInfo::exists(d.path)) continue;
            MediaItem it;
            it.url = d.path;
            it.id = d.key.isEmpty() ? d.path : d.key;
            it.mime = d.kind;
            it.type = browse::iconTypeForKind(d.kind);
            it.systemHint = d.system;
            it.thumbnailUrl = MetaCache::scrapedImage(it.id, d.thumb);
            it.title = d.title.isEmpty() ? QFileInfo(d.path).completeBaseName() : d.title;
            if (isHiddenItem(it)) continue;
            g.items.push_back(correctedRow(it));
        }
        if (!g.items.isEmpty()) out.push_back(g);
        return out;
    };

    // "playlist:<id>" — one saved playlist, rendered from the SAME builder its own level uses
    // (browse::playlistItemsCatalog), so a row picked here is byte-identical to the one that level would
    // activate and opens through the entry's own add-on (activateItem falls back to MediaItem::sourceAddonId
    // when the level names none). Opt-in, for the reason spelled out on Downloads above.
    auto buildPlaylist = [this](const QString& id) {
        QVector<Group> out;
        Playlist p;
        if (!PlaylistStore::get(id, p)) return out;   // deleted here: no producer, so the entry is skipped
        Group g;
        g.header = p.name;
        for (const MediaItem& raw : browse::playlistItemsCatalog(p).items)
        {
            const MediaItem it = correctedRow(raw);
            if (isHiddenItem(it)) continue;
            g.items.push_back(it);
        }
        if (!g.items.isEmpty()) out.push_back(g);
        return out;
    };

    const QVector<homerows::Row> rowList = HomeRowStore::list();

    // The built-in shelves, in the order the home has always produced them. The order comes from
    // homerows::defaultShelfOrder() rather than from four calls in sequence, so the sequence a probe pins is
    // literally the sequence drawn here.
    for (const QString& id : homerows::defaultShelfOrder())
    {
        if (id == QStringLiteral("continue"))            pushShelf(id, StyleResume, buildContinue());
        else if (id == QStringLiteral("trakt:missed"))   pushShelf(id, StyleWhen,   buildTraktMissed());
        else if (id == QStringLiteral("trakt:calendar")) pushShelf(id, StyleWhen,   buildTraktCalendar());
        else if (id == QStringLiteral("favorites"))      pushShelf(id, StylePlain,  buildFavorites());
    }
    // The opt-in shelves — built ONLY for a row the list actually names (see homerows::isOptInShelf). Their
    // position on screen comes from the list, not from the order they are built in here.
    for (const homerows::Row& r : rowList)
    {
        if (!r.visible || !homerows::isOptInShelf(r.rowId)) continue;
        if (r.rowId == QStringLiteral("downloads")) pushShelf(r.rowId, StylePlain, buildDownloads());
        else if (r.rowId.startsWith(QStringLiteral("playlist:")))
            pushShelf(r.rowId, StylePlain, buildPlaylist(r.rowId.mid(QStringLiteral("playlist:").size())));
    }
    // "preset:<name>" — a #63 saved filter as a home shelf, evaluated over the rows the home ALREADY holds.
    // That corpus is deliberate and it is the whole reason this is cheap: the preset shelves inside a games
    // console filter that console's items, and a home shelf that fetched a console to filter it would turn the
    // landing screen into a network wait. So a preset row here answers "of what is on my home screen, which
    // matches this filter?" — which is what the pinned-preset request on #161 asks for — and it is documented
    // as that. Built last so every other shelf's rows are in the corpus.
    {
        QVector<MediaItem> corpus;
        for (const Shelf& s : shelves)
            for (const Group& g : s.groups) corpus << g.items;
        for (const homerows::Row& r : rowList)
        {
            if (!r.visible || !r.rowId.startsWith(QStringLiteral("preset:"))) continue;
            const QString name = r.rowId.mid(QStringLiteral("preset:").size());
            if (!FilterPresetStore::exists(name)) continue;   // deleted: no producer, the entry is skipped
            const FilterPreset p = FilterPresetStore::get(name);
            Group g;
            g.header = QStringLiteral("▦ ") + name;
            QSet<QString> seen;
            for (const MediaItem& it : corpus)
            {
                const QString k = MetaCache::keyFor(it);
                if (seen.contains(k)) continue;
                if (!gamefilter::matches(p.filter, gameFactsFor(it))) continue;
                seen.insert(k);
                g.items.push_back(it);
            }
            if (!g.items.isEmpty()) pushShelf(r.rowId, StylePlain, { g });
        }
    }

    // Order / hide / cap, then draw. `available` is in the app's own order; the planner returns the render
    // plan (see HomeRows.h for every rule it applies).
    QVector<homerows::Available> available;
    available.reserve(shelves.size());
    for (const Shelf& s : shelves) available.push_back({ s.rowId, s.count });
    for (const homerows::Planned& p : homerows::plan(available, rowList))
    {
        const Shelf* shelf = nullptr;
        for (const Shelf& s : shelves) if (s.rowId == p.rowId) { shelf = &s; break; }
        if (!shelf) continue;
        int shown = 0;
        for (const Group& g : shelf->groups)
        {
            // The cap counts ITEMS across the whole shelf, so a capped "continue" keeps its newest group
            // whole rather than taking N from each console. A group the cap empties takes its divider with
            // it — the same rule an all-hidden group has always followed.
            QVector<MediaItem> take;
            for (const MediaItem& it : g.items)
            {
                if (p.cap > 0 && shown >= p.cap) break;
                take.push_back(it);
                ++shown;
            }
            if (take.isEmpty()) continue;
            if (!g.header.isEmpty()) addHeader(g.header);
            for (const MediaItem& it : take)
            {
                items_.push_back(it);
                QString label = QStringLiteral("  ") + it.title;
                if (shelf->style == StyleResume)
                {
                    // "Continue watching": a percentage in the row text and a resume bar on the (small) icon.
                    const double frac = rowFraction(it); // "how far in" — a movie/episode by its key, an audiobook by its own carried fraction (#139 inc 2)
                    if (frac >= 0.0) label += QStringLiteral("    ·  %1%").arg(int(frac * 100.0));
                }
                else if (shelf->style == StyleWhen)
                {
                    // The air day / episode code rides in the row text: the Home list is a list, not a poster
                    // grid, and "Show S01E04" alone does not say WHEN, which is the entire point of the shelf.
                    label += QStringLiteral("    ·  ") + it.subtitle;
                }
                auto* w = new QListWidgetItem(label, grid_);
                w->setSizeHint(QSize(0, 52));
                if (shelf->style == StyleResume)
                    w->setIcon(iconWithProgress(defaultIcon(it.type, iconSz).pixmap(iconSz), rowFraction(it)));
                else
                    w->setIcon(defaultIcon(it.type, iconSz));
            }
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
    if (grid_->isVisible()) takeFocus(grid_);
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
        if (top.item.mime == QStringLiteral("pcgames:console")) return QString(); // PC games aren't ROM files
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
            if (k == Qt::Key_Backspace || k == Qt::Key_Escape) { goBack(); return true; }
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
            if (k == Qt::Key_Backspace || k == Qt::Key_Escape) { goBack(); return true; }
            return false;
        }
        // --- Detail page: the action button (Favorite, or Play for a Steam game) ---
        // ANY button on the detail action row, not just Play and Favorite. The old test named those two, and
        // Left/Right swapped between exactly them — so "⬇ Download" and "🔀 Choose source…" have always been
        // MOUSE-ONLY on this page, which is the defect class issue #40 is open about, on a controller-and-TV
        // -first app. Adding "⚙ Fix this entry…" (#44) to a row a D-pad cannot traverse would have made that
        // three. The row is walked in its real left-to-right order instead, so every visible action is
        // reachable and a button added later is reachable by construction.
        const QVector<QWidget*> actionRowBtns{ playBtn_, favBtn_, downloadBtn_, sourceBtn_, pcFixBtn_, manualBtn_ };
        if (actionRowBtns.contains(static_cast<QWidget*>(obj)))
        {
            if (k == Qt::Key_Up)   { focusChromeRow(); return true; }
            if (k == Qt::Key_Down) // drop into the child column (container detail), if any is shown
            {
                const bool col = (xmb_ && xmb_->isVisible()) || (carousel_ && carousel_->isVisible())
                                 || (grid_->isVisible() && grid_->count() > 0);
                if (col) focusContent();
                return true;
            }
            if (k == Qt::Key_Left || k == Qt::Key_Right)
            {
                // Only the VISIBLE ones: the row's membership changes per item (Download and Choose source
                // are revealed by requestMeta/showMeta), and stepping onto a hidden button would strand the
                // cursor somewhere with nothing drawn.
                QVector<QWidget*> shown;
                for (QWidget* w : actionRowBtns) if (w && w->isVisible()) shown << w;
                const int at = shown.indexOf(static_cast<QWidget*>(obj));
                if (at >= 0 && shown.size() > 1)
                {
                    // Wrapping, like the type-button row: with three or more actions a non-wrapping row
                    // makes the far end a long walk back.
                    const int step = (k == Qt::Key_Right) ? 1 : shown.size() - 1;
                    takeFocus(shown.at((at + step) % shown.size()));
                }
                return true;
            }
            if (k == Qt::Key_Return || k == Qt::Key_Enter || k == Qt::Key_Space)
            { if (auto* b = qobject_cast<QPushButton*>(obj)) b->click(); return true; }
            if (k == Qt::Key_Backspace || k == Qt::Key_Escape) { goBack(); return true; }
            return false;
        }

        // Backspace acts as the Back button when focus is on a tab or the grid.
        if (k == Qt::Key_Backspace || k == Qt::Key_Escape) { goBack(); return true; }

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

            // "P" adds the focused catalog item to a playlist (the Playlists folder sits atop the catalogue).
            //
            // DEFERRED A TURN, and the deferral is load-bearing rather than tidy: the controller's R button
            // injects this key from inside pollMenuPad, which is padNavTimer_'s OWN slot, and
            // addItemToPlaylistInteractive spins nested event loops (NavMenu::pick, then Osk::getText on the
            // "New playlist…" row). A QTimer never re-enters its own slot while that slot is on the stack, so
            // opening the picker synchronously here would freeze the pad poll for the picker's whole life —
            // with no keyboard on the couch it could not be moved, chosen or dismissed. Queueing lets the poll
            // return first, so the nested loop runs from an ordinary event-loop turn with padNavTimer_ free to
            // tick and drive it. Same reasoning as pollMenuPad's Start deferral and the same shape as this
            // verb's themed sibling (addBrowseItemToPlaylist).
            //
            // The item is resolved to a COPY before the turn. Two reasons, and the second is the stronger one
            // — DO NOT delete this copy as redundant with the lambda's by-value capture:
            //   1. A turn is a whole event-loop cycle, an async re-present during it can rebuild items_
            //      underneath the same row, and this verb WRITES a playlist entry — a stale index would
            //      quietly file the wrong item.
            //   2. USE-AFTER-FREE. addItemToPlaylistInteractive takes `const MediaItem&`, and the old code
            //      bound that reference straight to items_[row]. Inside, it re-enters NavMenu::pick and then
            //      Osk::getText — two nested event loops — and goes on reading `it` (title, id, subtitle,
            //      type, url) AFTER them. Any rebuild of items_ during those loops reallocates the vector and
            //      leaves the reference dangling. The copy severs it from the container entirely.
            if (ke->key() == Qt::Key_P && !recentView_)
            {
                const int row = grid_->currentRow();
                if (row >= 0 && row < items_.size() && !items_[row].type.startsWith(QLatin1Char('_'))
                    && items_[row].type != QStringLiteral("info"))
                {
                    const MediaItem copy = items_[row];
                    QMetaObject::invokeMethod(this, [this, copy] { addItemToPlaylistInteractive(copy); },
                                              Qt::QueuedConnection);
                }
                return true;
            }

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
                        takeFocus(detailActionButton()); // container detail -> action button
                    else if (carouselMode_)     showCarousel();        // back up to the carousel
                    else if (activeTypeButton_) takeFocus(activeTypeButton_); // to the tabs
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

    // A local game added to a playlist re-opens by path (recovers its console from the Recent/Downloads store).
    if (it.mime.startsWith(QStringLiteral("localgame:")))
    { emit openRecent(it.url, it.mime.mid(10), it.id, it.title, it.thumbnailUrl); return; }

    // A Trakt "Airing Soon" row, on EITHER surface — the Home shelf has no level context to key off, so the
    // rule keys on the row's own mime marker and covers both. It carries no url and belongs to no addon, so
    // it plays through the IMDB stream bridge (which also serves it off disk first, if the episode is
    // already in the local library). An unplayable row SAYS why rather than doing nothing: without this it
    // would fall through to openResolvedItem and push a detail level with no addon behind it.
    // A Trakt watchlist/collection row, on EITHER surface. Like the calendar rows these carry no url and
    // belong to no addon, so the branch has to claim them before the generic "a file is associated" test.
    //
    // The two kinds go different ways ON PURPOSE. A MOVIE plays through the IMDB stream bridge, exactly as
    // a calendar row does. A SHOW carries no stream id at all (SyntheticCatalogs.h says why: a bare show id
    // cannot resolve, so a "play" would only ever spin and fail); it hands its title to the app's own
    // cross-addon search instead, which is how the user reaches the real season/episode browser.
    if (it.mime == QLatin1String(browse::kTraktListShowMime))
    {
        if (it.title.trimmed().isEmpty())
        {
            showToast(tr("Trakt gave no title for this show, so there is nothing to search for."),
                      kFeedbackLong);
            return;
        }
        searchEverything(it.title);
        return;
    }
    if (it.mime == QLatin1String(browse::kTraktListMovieMime))
    {
        if (it.imdbStreamId.isEmpty())
        {
            showToast(tr("No source for \u201C%1\u201D \u2014 Trakt has no IMDB id for it.").arg(it.title),
                      kFeedbackLong);
            return;
        }
        resolvePlay(nullptr, it, QString(), QString(), it.imdbStreamId, QStringLiteral("movie"));
        return;
    }
    // A Trakt "You Missed" row, on EITHER surface (#25). It opens a MENU rather than playing straight away,
    // because the row needs a second verb — "I'm caught up" — and a folder row is the only control every one
    // of this app's four layouts can reach with a D-pad. Play is row 0, so the couch gesture is unchanged
    // except for one extra press, exactly as the Recent/Downloads game rows already work.
    //
    // Deferred a turn, like showGameItemMenu below: this can be reached from a themed `activated` handler,
    // and opening a nested overlay under a live QML delegate is the crash that idiom exists to avoid.
    if (browse::isTraktMissedMime(it.mime))
    {
        const MediaItem copy = it;
        QMetaObject::invokeMethod(this, [this, copy] { showTraktMissedMenu(copy); }, Qt::QueuedConnection);
        return;
    }
    if (it.mime == QStringLiteral("trakt:cal"))
    {
        if (it.imdbStreamId.isEmpty())
        {
            showToast(tr("No source for “%1” — Trakt doesn't have an IMDB id for this show.").arg(it.title),
                      kFeedbackLong);
            return;
        }
        resolvePlay(nullptr, it, QString(), QString(), it.imdbStreamId, QStringLiteral("series"));
        return;
    }

    // Games in the Recent / Downloaded lists open a small action menu (Play / Favorite / Add to playlist /
    // Uninstall) instead of launching straight away, so they can be managed from the couch. Play is the default.
    const bool isGame = (it.mime == QStringLiteral("game") || it.mime == QStringLiteral("pcgame"));
    // Open the overlay on a fresh event-loop turn: in the themed modes activateItem runs inside the QML view's
    // `activated` signal handler, and building/showing widgets from there is best deferred.
    auto queueMenu = [this](const MediaItem& g, bool dl) {
        const MediaItem copy = g;
        QMetaObject::invokeMethod(this, [this, copy, dl] { showGameItemMenu(copy, dl); }, Qt::QueuedConnection);
    };

    if (recentView_)
    {
        if (it.type == QStringLiteral("rechdr")) return;                 // a group header, not actionable
        if (it.mime.startsWith(QStringLiteral("fav:"))) { openFavorite(it); return; } // a favourite -> detail
        if (isGame && !it.url.isEmpty()) { queueMenu(it, /*isDownloads*/false); return; }
        if (!it.url.isEmpty()) emit openRecent(it.url, it.mime, resumeKeyFor(it), it.title, it.thumbnailUrl); // a recent -> re-open
        return;
    }

    // A catalogue's synthetic Recent folder: activating a row re-opens it where you left off (resume), just
    // like the Home recents list. Intercept before the generic url path below (recents carry a url too).
    if (atRecentsLevel() && it.type != QStringLiteral("_recents"))
    {
        if (isGame) { queueMenu(it, /*isDownloads*/false); return; }
        emit openRecent(it.url, it.mime, resumeKeyFor(it), it.title, it.thumbnailUrl);
        return;
    }
    // A catalogue's synthetic Downloaded folder: rows are local files, re-opened the same way as recents.
    if (atDownloadsLevel() && it.type != QStringLiteral("_downloads"))
    {
        if (isGame) { queueMenu(it, /*isDownloads*/true); return; }
        emit openRecent(it.url, it.mime, resumeKeyFor(it), it.title, it.thumbnailUrl);
        return;
    }
    // A console's synthetic Favorites folder: favourited games, same action menu as recents.
    if (atFavoritesLevel() && it.type != QStringLiteral("_favorites"))
    {
        if (isGame) { queueMenu(it, /*isDownloads*/false); return; }
        emit openRecent(it.url, it.mime, resumeKeyFor(it), it.title, it.thumbnailUrl);
        return;
    }

    if (it.type == QStringLiteral("_open"))
    {
        emit requestOpenFile(it.url); // url carries the kind: video/audio/document/game
        return;
    }
    // A LOCAL LEAF — a row whose file this machine already has, which belongs to no addon. THE SAME TABLE
    // the themed surface's playThemedLeaf reads (browse::localLeafRoute), so the two layouts cannot answer
    // this differently; they had already drifted three ways when the table was written, and each drift was a
    // category that played on one layout and said "Nothing to play" on the other. LeafRoute.h says why.
    //
    // Claimed AHEAD of the generic "a file is associated" branch below, because two of the routes are not
    // "open this url": an OPDS book's url is an acquisition href that must be fetched with the catalog's own
    // device-local auth first, and a track's url would queue its CONTAINING FOLDER — one disc of a multi-disc
    // set, which is not the album. Adding a route here means adding it to playThemedLeaf too; the
    // `themed local-leaf routing parity` gate fails the build if you don't.
    switch (const browse::LeafRoute lr = browse::localLeafRoute(it); lr.play)
    {
        case browse::LeafPlay::OpenFile:   emit openItem(it); return;
        case browse::LeafPlay::OpdsBook:   openOpdsBook(it); return;   // re-emits openItem with the auth header
        case browse::LeafPlay::MusicAlbum: emit playMusicAlbumRequested(lr.key, it.url); return;
        case browse::LeafPlay::AudiobookBook: emit playAudiobookRequested(lr.key, it.url, -1); return;
        case browse::LeafPlay::NotLocal:   break;                      // an addon's row: fall through
    }
    if (!it.url.isEmpty())
    {
        emit openItem(it); // a file is associated with this item -> the main window plays it
        return;
    }

    stack_.last().childRow = row; // remember where we drilled in, so Back restores this position

    // A marks shelf (Favorites / a pinned tag / Hidden) drills into this level's matching items.
    if (it.type == QStringLiteral("_favshelf") || it.type == QStringLiteral("_tagshelf")
        || it.type == QStringLiteral("_hiddenshelf") || it.type == QStringLiteral("_presetshelf"))
        { openShelfLevel(it); return; }

    // The "＋ New filter…" row on a games surface: build + name + save a preset (#63). Deferred a turn like
    // the "New playlist…" row for the same reason — createFilterPresetInteractive spins NavMenu/Osk nested
    // loops, and doing that from inside the QML view's own `activated` handler is what issue #28 warns against.
    if (it.type == QStringLiteral("_newpreset"))
    {
        QMetaObject::invokeMethod(this, [this] { createFilterPresetInteractive(); }, Qt::QueuedConnection);
        return;
    }

    // The synthetic PC Games console drills into the local library natively (not via the addon).
    if (it.mime == QStringLiteral("pcgames:console")) { openPcGamesConsole(it); return; }

    // The synthetic Recent folder drills into this catalogue's recently-opened items.
    if (it.type == QStringLiteral("_recents"))
        { openRecentsLevel(it.mime.mid(QStringLiteral("recents:").size())); return; }

    // The synthetic Downloaded folder drills into this catalogue's (or console's) fully-downloaded items.
    if (it.type == QStringLiteral("_downloads"))
        { openDownloadsLevel(it.mime.mid(QStringLiteral("downloads:").size())); return; }

    // The synthetic Favorites folder (inside a console) drills into that console's favourited games.
    if (it.type == QStringLiteral("_favorites"))
        { openFavoritesLevel(it.mime.mid(QStringLiteral("favorites:").size())); return; }

    // The synthetic Homebrew folder (inside a console) drills into what the configured servers have for it,
    // and its trailing "More…" row appends the next page in place — the continuations ride the row's marker,
    // opaque, so this side never has to understand a cursor.
    if (it.type == QStringLiteral("_homebrew"))
        { openHomebrewLevel(HomebrewClient::levelSystem(it.mime)); return; }
    if (it.type == QStringLiteral("_homebrewmore"))
        { fetchHomebrew(HomebrewClient::moreSystem(it.mime), HomebrewClient::moreCursors(it.mime),
                        /*append*/ true); return; }

    // The synthetic Local Library folder drills into this machine's scanned local videos.
    if (it.type == QStringLiteral("_locallib"))
        { openLocalLibraryLevel(it.mime.mid(QStringLiteral("locallib:").size())); return; }

    // A Photos folder row (#102) drills into that folder's image grid. (An image tile carries a url and was
    // already claimed by the generic "a file is associated" branch above, which routes it to the viewer.)
    if (it.type == QStringLiteral("_photofolder"))
        { openPhotoFolderLevel(it.mime.mid(QStringLiteral("photofolder:").size())); return; }

    // Music (#74): an artist row drills into their albums, an album row into its tracks, and the "Play album"
    // row at the top of a track list hands the whole album to PlaybackSession. (A TRACK row carries a url and
    // was claimed above, ahead of the generic file branch — see the comment there.)
    if (it.type == QString::fromLatin1(browse::kMusicArtistType))
        { openMusicArtistLevel(browse::musicKeyOf(it.mime, browse::kMusicArtistPrefix)); return; }
    if (it.type == QString::fromLatin1(browse::kMusicAlbumType))
        { openMusicAlbumLevel(browse::musicKeyOf(it.mime, browse::kMusicAlbumPrefix)); return; }
    // The classical view (#196, part 2). All three types start with '_', so the themed XMB sends them down
    // this ordinary browse path rather than to its per-leaf action chooser - which is what makes the
    // dimension reachable there at all, exactly as it is for the multi-album verbs below.
    if (it.type == QString::fromLatin1(browse::kMusicComposersType))
        { openMusicComposersLevel(); return; }
    // Music servers (#193, increment 5). Same idiom, same reason the composer types use it: all three start
    // with '_', so the themed XMB sends them down this ordinary browse path rather than to its per-leaf
    // action chooser, which is what makes the dimension reachable on the layout most people run. The "add"
    // row is deferred a turn, exactly like "_newopds" and "_newlivetv" below, because it spins Osk nested
    // loops and then rebuilds this very level's model - and NavMenu::pick is a nested event loop, so doing
    // that inside a QML activation is how crash #28 is reproduced.
    if (it.type == QString::fromLatin1(browse::kMusicServersType))
        { openMusicServersLevel(); return; }
    if (it.type == QString::fromLatin1(browse::kMusicServerType))
        { openMusicServerLevel(browse::musicKeyOf(it.mime, browse::kMusicServerPrefix)); return; }
    if (it.type == QString::fromLatin1(browse::kMusicAddServerType))
    {
        QMetaObject::invokeMethod(this, [this] { addMusicServerInteractive(); }, Qt::QueuedConnection);
        return;
    }
    // ONE LIBRARY ACROSS SOURCES (#194). All three start with '_', so the themed XMB sends them down this
    // ordinary browse path rather than to its per-leaf action chooser - the same idiom the composer and
    // server doors use, and the reason these verbs are reachable on the layout most people run.
    if (it.type == QString::fromLatin1(browse::kMusicAltSourceType))
        { playMusicAlbumFromSource(browse::musicKeyOf(it.mime, browse::kMusicAltSourcePrefix)); return; }
    // Both overrides are DEFERRED A TURN, for the reason the "add a music server" row above already gives:
    // they rebuild this very level's model under the still-live delegate whose emission called us, and the
    // join one additionally spins NavMenu::pick, which is a nested event loop (issue #28).
    if (it.type == QString::fromLatin1(browse::kMusicUnmergeType))
    {
        const QString k = browse::musicKeyOf(it.mime, browse::kMusicUnmergePrefix);
        QMetaObject::invokeMethod(this, [this, k] { unmergeAlbumInteractive(k); }, Qt::QueuedConnection);
        return;
    }
    if (it.type == QString::fromLatin1(browse::kMusicMergeAlbumType))
    {
        const QString k = browse::musicKeyOf(it.mime, browse::kMusicMergeAlbumPrefix);
        QMetaObject::invokeMethod(this, [this, k] { mergeAlbumInteractive(k); }, Qt::QueuedConnection);
        return;
    }
    // Audiobooks (#139). Every one of these types starts with '_', so the themed XMB sends them down this
    // ordinary browse path rather than to its per-leaf action chooser — which is what makes the whole
    // category reachable on the layout this app is actually used through, exactly as it is for the music
    // doors above. (A book PART carries a url and was claimed by the local-leaf table, ahead of the generic
    // file branch, so pressing one plays the BOOK from that part rather than opening one loose file.)
    if (it.type == QString::fromLatin1(browse::kAudiobookAuthorType))
        { openAudiobookAuthorLevel(browse::audiobookKeyOf(it.mime, browse::kAudiobookAuthorPrefix)); return; }
    if (it.type == QString::fromLatin1(browse::kAudiobookNarratorsType))
        { openAudiobookNarratorsLevel(); return; }
    if (it.type == QString::fromLatin1(browse::kAudiobookNarratorType))
        { openAudiobookNarratorLevel(browse::audiobookKeyOf(it.mime, browse::kAudiobookNarratorPrefix)); return; }
    if (it.type == QString::fromLatin1(browse::kAudiobookSeriesListType))
        { openAudiobookSeriesListLevel(); return; }
    if (it.type == QString::fromLatin1(browse::kAudiobookSeriesType))
        { openAudiobookSeriesLevel(browse::audiobookKeyOf(it.mime, browse::kAudiobookSeriesPrefix)); return; }
    if (it.type == QString::fromLatin1(browse::kAudiobookBookType))
        { openAudiobookBookLevel(browse::audiobookKeyOf(it.mime, browse::kAudiobookBookPrefix)); return; }
    if (it.type == QString::fromLatin1(browse::kAudiobookPlayType))
    {
        // Empty start path = "from the top": openAudiobook falls back to part one, and PlaybackSession's
        // ordinary resume then puts the listener back where they stopped.
        emit playAudiobookRequested(browse::audiobookKeyOf(it.mime, browse::kAudiobookPlayPrefix), QString(),
                                    -1);
        return;
    }
    if (it.type == QString::fromLatin1(browse::kAudiobookChaptersType))
    {
        // DEFERRED A TURN, for the reason the music merge rows above already give: this opens a NavMenu,
        // which is a nested event loop, and we are standing inside the emission of the still-live delegate
        // that was activated (issue #28 / #211). The book key is resolved BEFORE the turn — it names the
        // book and so cannot be invalidated by a re-present, unlike an index.
        const QString k = browse::audiobookKeyOf(it.mime, browse::kAudiobookChaptersPrefix);
        QMetaObject::invokeMethod(this, [this, k] { openAudiobookChapters(k); }, Qt::QueuedConnection);
        return;
    }
    // The reading library (#134). Same '_'-prefixed shape as the audiobook doors above, and for the same
    // reason: the themed XMB sends those down this ordinary browse path rather than to its per-leaf action
    // chooser, which is what makes the whole category reachable on the layout this app is actually used
    // through. A BOOK row is not in this list because it is a real leaf - it carries a url and was already
    // claimed by the local-leaf table at the top of this function, which opens it in its reader.
    if (it.type == QString::fromLatin1(browse::kBookAuthorType))
        { openBookAuthorLevel(browse::bookKeyOf(it.mime, browse::kBookAuthorPrefix)); return; }
    if (it.type == QString::fromLatin1(browse::kBookSeriesListType))
        { openBookSeriesListLevel(); return; }
    if (it.type == QString::fromLatin1(browse::kBookSeriesType))
        { openBookSeriesLevel(browse::bookKeyOf(it.mime, browse::kBookSeriesPrefix)); return; }
    if (it.type == QString::fromLatin1(browse::kMusicComposerType))
        { openMusicComposerLevel(browse::musicKeyOf(it.mime, browse::kMusicComposerPrefix)); return; }
    if (it.type == QString::fromLatin1(browse::kMusicWorkType))
        { openMusicWorkLevel(browse::musicKeyOf(it.mime, browse::kMusicWorkPrefix)); return; }
    if (it.type == QString::fromLatin1(browse::kMusicPlayAlbumType))
    {
        // Empty start path = "from the top": openMusicAlbum falls back to track 1.
        emit playMusicAlbumRequested(browse::musicKeyOf(it.mime, browse::kMusicPlayAlbumPrefix), QString());
        return;
    }
    // The MULTI-ALBUM verbs. They land here — and not in playThemedLeaf — on EVERY layout including the
    // themed XMB, because their types start with '_': the XMB's activation splits on exactly that, sending
    // "_" rows down this ordinary browse path and only real media leaves to the per-leaf action chooser.
    // That is what makes an hour of music across records reachable on the themed surface, which is the
    // surface that had no multi-album queue at all.
    if (it.type == QString::fromLatin1(browse::kMusicPlayArtistType))
    {
        playMusicArtistQueue(browse::musicKeyOf(it.mime, browse::kMusicPlayArtistPrefix), false);
        return;
    }
    if (it.type == QString::fromLatin1(browse::kMusicShuffleArtistType))
    {
        playMusicArtistQueue(browse::musicKeyOf(it.mime, browse::kMusicShuffleArtistPrefix), true);
        return;
    }
    if (it.type == QString::fromLatin1(browse::kMusicShuffleAllType))
    {
        emit playMusicQueueRequested(QString(), true);   // no key: the whole library
        return;
    }

    // The synthetic Airing Soon folder drills into the connected Trakt account's calendar.
    if (it.type == QStringLiteral("_traktcal")) { openTraktCalendarLevel(); return; }
    // ...and the synthetic You Missed folder into what already aired on it (#25).
    if (it.type == QStringLiteral("_traktmissed")) { openTraktMissedLevel(); return; }
    if (it.type == QStringLiteral("_traktlist"))
    { openTraktListLevel(it.mime.section(QLatin1Char(':'), 1)); return; }

    // Synthetic playlist navigation (no addon): the Playlists folder, a playlist, or the New-playlist entry.
    if (it.type == QStringLiteral("_playlists"))
        { openPlaylistsLevel(it.mime.mid(QStringLiteral("playlists:").size())); return; }
    if (it.type == QStringLiteral("_playlist"))
    {
        // A playlist row opens an action menu (Open / Play random / Rename / Delete) rather than drilling
        // straight in — Open (the default row) drills exactly as before. Deferred a turn so the themed QML
        // view doesn't build the overlay from inside its own `activated` handler (the game-menu pattern).
        const QString pid = it.mime.mid(QStringLiteral("playlist:").size());
        QMetaObject::invokeMethod(this, [this, pid] { showPlaylistMenu(pid); }, Qt::QueuedConnection);
        return;
    }
    // Deferred a turn, for the reason its two immediate siblings above and below already give —
    // this was the one branch in the chain that never was (issue #28). In the themed modes it
    // runs inside the QML view's own `activated` handler, and createPlaylistInteractive spins a
    // NESTED EVENT LOOP (Osk::getText) and then rebuilds this very level's model
    // (populatePlaylists -> browseItemsChanged -> setProperty("items")) under the still-live
    // delegate whose emission called us. A nested loop also flushes pending DeferredDeletes at
    // an arbitrary point, which is how the destruction lands mid-flight inside other work.
    //
    // The category key is resolved to a stable id HERE, synchronously: it names the bucket, so
    // unlike a row index it cannot be invalidated by an async re-present during the turn.
    if (it.type == QStringLiteral("_newplaylist"))
    {
        const QString catKey = it.mime.mid(QStringLiteral("newplaylist:").size());
        QMetaObject::invokeMethod(this, [this, catKey] { createPlaylistInteractive(catKey); },
                                  Qt::QueuedConnection);
        return;
    }

    // Live TV (#75 inc 2). The "Live TV" folder opens the saved-sources shelf; a saved source drills into its
    // channels, freshly FETCHED on open; a section header is inert; the "add a source" row opens the name/URL
    // prompt — deferred a turn, exactly like "_newplaylist" above, because addIptvSourceInteractive spins an
    // Osk nested loop and then rebuilds this very level's model.
    if (it.type == QStringLiteral("_livetv")) { openLiveTvSourcesLevel(); return; }
    if (it.type == QStringLiteral("_livetvheader")) return;   // a section label: not activatable
    if (it.type == QStringLiteral("_guideprog")) return;      // a guide programme cell: display-only (#75 inc 3)
    if (it.type == QStringLiteral("_livetvsource"))
        { openLiveTvChannelsLevel(it.mime.mid(QStringLiteral("livetvsource:").size())); return; }
    if (it.type == QStringLiteral("_livetvguide"))            // the "Guide (today)" row -> the channels×time grid
        { openLiveTvGuideLevel(it.mime.mid(QStringLiteral("livetvguide:").size())); return; }
    if (it.type == QStringLiteral("_newlivetv"))
    {
        QMetaObject::invokeMethod(this, [this] { addIptvSourceInteractive(); }, Qt::QueuedConnection);
        return;
    }

    // Recomps (#248 inc a). The Games "Recomps" folder opens the section; a system header is inert; a port row
    // opens the SAME card the game row's *Native port* verb opens — one implementation of the verbs, reached
    // from two places. Deferred a turn for the reason the themed *Native port* arm already gives: that card
    // spins a nested event loop (NavConfirm::ask), and a nested loop inside the QML delegate's own `activated`
    // emission is crash #28. The port id is resolved HERE, synchronously, because it names the entry and so —
    // unlike a row index — cannot be invalidated by a repopulate during the turn.
    if (it.type == QStringLiteral("_recomps")) { openRecompsLevel(); return; }
    if (it.type == QStringLiteral("_recompheader")) return;   // a section label: not activatable
    if (it.type == QStringLiteral("_recompport"))
    {
        const QString pid = it.mime.mid(QStringLiteral("recompport:").size());
        const MediaItem target = it;
        QMetaObject::invokeMethod(this, [this, target, pid] { emit nativePortRequested(target, pid); },
                                  Qt::QueuedConnection);
        return;
    }

    // OPDS book catalogs (#146). The "Book Servers" folder opens the saved-catalogs shelf; a saved catalog
    // fetches + renders its ROOT feed; a navigation row drills into a sub-feed (carrying the same catalog's
    // auth, held in currentOpdsCatalogId_); the "add" row opens the name/URL/creds prompt — deferred a turn,
    // exactly like "_newlivetv" above, because addOpdsCatalogInteractive spins Osk nested loops then rebuilds
    // this very level's model. (A book item, type "opdsbook", was already claimed above, ahead of the generic
    // url branch, so it can download with auth.)
    if (it.type == QStringLiteral("_opdscatalogs")) { openOpdsCatalogsLevel(); return; }
    if (it.type == QStringLiteral("_opdscatalog"))
        { openOpdsCatalog(it.mime.mid(QStringLiteral("opdscatalog:").size())); return; }
    if (it.type == QStringLiteral("_opdsfeed"))
        { openOpdsFeedLevel(it.mime.mid(QStringLiteral("opdsfeed:").size()), it.title); return; }
    if (it.type == QStringLiteral("_newopds"))
    {
        QMetaObject::invokeMethod(this, [this] { addOpdsCatalogInteractive(); }, Qt::QueuedConnection);
        return;
    }

    // The PC Games folder's launcher filter row. Deferred a turn for the same reason every other overlay
    // here is: in the themed modes this runs inside the QML view's own `activated` handler.
    if (it.type == QStringLiteral("_pcfilter"))
    {
        QMetaObject::invokeMethod(this, [this] { showPcLauncherFilterMenu(); }, Qt::QueuedConnection);
        return;
    }

    // A generic leaf/container: resolve + open through the shared per-entry path (also reused by Play-random
    // over a playlist, so a random pick resolves identically to activating that item's row).
    openResolvedItem(it, stack_.last().addon);
}

// Open a single item through the per-entry resolution path — the generic tail of activateItem, shared with
// Play-random. Local-file entries re-open by path; a remote leaf resolves its /stream and plays; info-page
// types (movies/episodes, comics/books) and stream-less/container items open a detail page instead.
HomeView::ChannelAir HomeView::openResolvedItem(const MediaItem& it, LoadedAddon* levelAddon,
                                                bool forChannel, int channelGen)
{
    // A local game/file entry (Recent/Downloaded item added to a playlist) re-opens by path.
    if (it.mime.startsWith(QStringLiteral("localgame:")))
    { emit openRecent(it.url, it.mime.mid(10), it.id, it.title, it.thumbnailUrl); return ChannelAir::Played; }
    // A file is already associated (a local video/audio) -> the main window plays it directly.
    if (!it.url.isEmpty()) { emit openItem(it); return ChannelAir::Played; }

    LoadedAddon* addon = levelAddon;
    if (!addon && !it.sourceAddonId.isEmpty()) addon = mgr_->sourceById(it.sourceAddonId); // per-entry / cross-addon

    // A remote leaf (a track, etc.) carries no url in the catalog - its source comes from the /stream
    // endpoint, fetched on open: resolve and open it directly. Movies/episodes (Play) and comics/manga/books
    // (Read) instead open an info page with a button that resolves on demand, like Stremio items - skip those.
    const bool infoPageType = isInfoPageType(it.type);
    if (!it.expandable && addon && addon->transport == LoadedAddon::RemoteHttp && !addon->stremio
        && it.type != QStringLiteral("platform") && !infoPageType)
    {
        const MediaItem item = it; // copy for the async callback
        const bool fileProvider = !addon->stremio; // Allarr-style provider: supports alternate sources (?n=)
        // #224: the id of the addon that is about to serve this play, taken NOW and carried as a string. A
        // LoadedAddon* would be the obvious capture and is the wrong one — AddonManager::reload() clears the
        // unique_ptr vector that owns them, so a pointer held across an async /stream can dangle. One read,
        // used by both the retry record and the capture below, so the two cannot name different addons.
        const QString resolvedBy = addon->manifest.id;
        lastPlay_ = { resolvedBy, item, false, {}, {}, 0 };
        showToast(tr("Finding a source for “%1”…").arg(it.title), 0);
        mgr_->resolveStream(addon, item, [this, addon, item, fileProvider, forChannel, channelGen, resolvedBy](
                                             const QString& url, const QString& mime,
                                             const StreamHeaders::Headers& headers) {
            hideToast();
            if (!url.isEmpty())
            {
                MediaItem m = item; m.url = url; m.mime = mime; m.nextSourceCapable = fileProvider;
                m.requestHeaders = headers;   // the headers this url's host requires (usually none)
                // #224: name the addon that served this play. MediaItem::sourceAddonId is what lets the
                // Recents row this play writes be RE-MINTED later; without it that row can only replay a
                // link whose credential #200's scrub has already removed from the ini — which is #224.
                // Only when the item does not already name one: a cross-addon search row or a playlist row
                // was stamped with the addon whose ID SPACE its id lives in, and that is the addon a re-mint
                // has to ask, not necessarily whoever answered this time.
                if (m.sourceAddonId.isEmpty()) m.sourceAddonId = resolvedBy;
                if (forChannel) emit channelPickResolved(channelGen, m); // MainWindow gates on gen, then plays
                else            emit openItem(m);
            }
            // No stream: a channel SKIPS this pick (never dumps the viewer on a detail page mid-channel); a
            // normal open shows its metadata instead.
            else if (forChannel) emit channelPickDetoured(channelGen);
            else                 openDetailLevel(addon, item);
        });
        return ChannelAir::Pending;
    }

    // No file yet: this opens a detail page (info-page movie/episode, container, stream-less item). For a
    // channel that's a DETOUR, not playback — report it so the channel skips instead of stranding the viewer
    // on an info page and wedging the chain.
    if (forChannel) return ChannelAir::Detoured;
    // Its metadata header describes the item; for a container (show/season/album/console) it also drills in.
    openDetailLevel(addon, it);
    return ChannelAir::Detoured;
}

void HomeView::seedNextSourceFromRecipe(const MediaItem& item, const QString& route, const QString& imdbType)
{
    // WHOLESALE, NOT FIELD BY FIELD — `lastPlay_ = {}` first, so a re-mint starts a NEW swap chain rather
    // than inheriting the last browsed item's. `attempt` is the field that matters: the fresh link this
    // re-mint just opened IS attempt 0 (remintAndOpen resolves with /*attempt=*/0 on both legs), so the
    // first press of "Issue with Streaming" must ask for ?n=1. Carrying an old count over would skip past
    // releases of a title the user has not tried at all, and silently — the swap has no "back".
    lastPlay_ = {};
    // The item remintAndOpen rebuilt from the row, carried whole: it holds the recipe fields the re-resolve
    // needs AND the title/artwork/resume identity the row it re-opens is filed under. Seeding only id+type
    // would have the swapped-to play write a Recents row with no name — RecentStore falls back to the url's
    // file name — so a source swap would rename the user's Continue Watching row to a hash off a CDN.
    lastPlay_.item = item;
    // Which resolve answers a swap, decided by the row's own route and never re-derived: "imdb" fans out
    // across every installed stream provider (no addon owns the answer), "direct" asks the one addon that
    // knows this id space. imdbType is the row's sourceType because resolveStreamByImdb takes the STREMIO
    // type ("movie"/"series") — item.type on an episode leaf is "episode", which it does not accept.
    if (route == QLatin1String("imdb")) { lastPlay_.viaImdb = true; lastPlay_.imdbType = imdbType; lastPlay_.imdbId = item.imdbStreamId; }
    else                                { lastPlay_.addonId = item.sourceAddonId; }
}

void HomeView::requestNextSource()
{
    // Nothing opened from a file provider yet (or it came from a Stremio source, which has no ?n=).
    if (!lastPlay_.viaImdb && lastPlay_.addonId.isEmpty()) { emit nextSourceResult(false, tr("No alternate source to try.")); return; }

    const int attempt = lastPlay_.attempt + 1; // advance only on success, so a failed try can be repeated
    const MediaItem item = lastPlay_.item;
    // #224: the addon serving the alternate source, when there IS a single one. The imdb leg below fans out
    // across every installed stream provider, so no addon owns that answer and it records sourceRoute="imdb"
    // instead. Held as a string, not a LoadedAddon*, because reload() frees those (see openResolvedItem).
    const QString resolvedBy = lastPlay_.viaImdb ? QString() : lastPlay_.addonId;
    // RESOLVED HERE, USED HERE, STORED NOWHERE. The context may have been seeded an hour ago; a reload() in
    // between (installing or removing an add-on) would have invalidated any pointer kept across it. Null
    // when the source has since been uninstalled, which resolveStream answers with an empty url — i.e. the
    // "No other source available" message below, not a crash.
    LoadedAddon* src = (!lastPlay_.viaImdb && mgr_) ? mgr_->sourceById(lastPlay_.addonId) : nullptr;

    auto onResolved = [this, item, attempt, resolvedBy](const QString& url, const QString& mime,
                                                        const StreamHeaders::Headers& headers) {
        if (url.isEmpty()) { emit nextSourceResult(false, tr("No other source available for “%1”.").arg(item.title)); return; }
        lastPlay_.attempt = attempt;
        MediaItem m = item; m.url = url; m.mime = mime; m.nextSourceCapable = true;
        m.requestHeaders = headers;
        // #224: the swapped-to source writes its own Recents row, so it needs the same recipe as the first
        // play — a row minted here without it dead-ends on a scrubbed link exactly like one from the play
        // button. Never overwrites an id the item already carries (see openResolvedItem).
        if (m.sourceAddonId.isEmpty()) m.sourceAddonId = resolvedBy;
        emit nextSourceResult(true, QString());
        emit openItem(m); // re-opens in the right view (player/reader); resume keys on the stable id
    };

    if (lastPlay_.viaImdb) mgr_->resolveStreamByImdb(lastPlay_.imdbType, lastPlay_.imdbId, onResolved, attempt);
    else                   mgr_->resolveStream(src, item, onResolved, attempt);
}

// ---- download crawl: resolve one item (or a whole series/season) to files, queued to MainWindow ----------

void HomeView::startDownload()
{
    if (stack_.isEmpty() || !stack_.last().detail) return;
    if (dlBusy_) { showToast(tr("A download is already being prepared…"), kFeedbackLong); return; }
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
                                  : tr("Preparing download for “%1”…").arg(top.item.title), 0);
    dlNext();
}

void HomeView::dlNext()
{
    if (dlQueue_.isEmpty())
    {
        dlBusy_ = false;
        // Hand the outcome to whoever is waiting on this crawl, once. Taken first so a callback that starts
        // another crawl is not immediately cleared by this one.
        if (dlDone_) { auto cb = dlDone_; dlDone_ = nullptr; cb(dlQueued_ > 0); }
        // Mixed outcome: a success confirmation vs. an error. Classify by outcome (mirrors J22's
        // Google-Drive split): the crawl-came-up-empty branch is error-class and must be read.
        showToast(dlQueued_ > 0 ? tr("Queued %1 item(s) to download — they’ll appear in Recent.").arg(dlQueued_)
                                : tr("Nothing here could be downloaded."),
                  dlQueued_ > 0 ? kFeedbackShort : kFeedbackLong);
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

// The copy of `it` already on this machine, or empty. Its own function because the open path and the
// download crawl are two nearly identical blocks, and putting this rule inline once meant putting it in the
// wrong one of them — it read as correct in review and never ran.
//
// Both stores are consulted: a copy arrives by two routes that did not know about each other. The Download
// verb records one under Downloads, and simply opening a remote document leaves its fetched copy under
// Recent. A store can also outlive the file it names, so the path is checked before it is trusted.
QString HomeView::localCopyForItem(const MediaItem& it) const
{
    const QString storeKind = (it.type == QStringLiteral("audiobook")) ? QStringLiteral("audio")
                            : (it.type == QStringLiteral("game"))      ? QString()   // games route by console
                                                                       : QStringLiteral("document");
    if (storeKind.isEmpty()) return QString();

    QVector<CatalogMatch::LocalCopy> have;
    for (const DownloadedItem& d : DownloadsStore::list())
        have.push_back({ d.path, d.title, d.kind, d.key });
    for (const RecentItem& r : RecentStore::list())
        have.push_back({ r.path, r.title, r.kind, r.key });

    const QString local = CatalogMatch::localCopyFor(it.id, it.title, storeKind, have);
    return (!local.isEmpty() && QFileInfo::exists(local)) ? local : QString();
}

void HomeView::dlResolveLeaf(const DlNode& node)
{
    const MediaItem it = node.item;
    // Can't pull as a single file: a store-launcher game (Steam/Epic/GOG), or a page-based manga chapter.
    if (it.mime == QStringLiteral("steamgame") || it.mime == QStringLiteral("epicgame")
        || it.mime == QStringLiteral("goggame") || it.mime == QStringLiteral("battlenetgame")
        || isReadableChapter(it.type)) { dlNext(); return; }

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
        // The title to JUDGE a result by, without the words that only help FIND one. An issue's own title is
        // "#5", so the thing being looked for is the volume it belongs to.
        const QString wantTitle = (it.type == QStringLiteral("comic_issue")) ? node.parentTitle : it.title;
        // The DOWNLOAD crawl takes the whole-release link, unchanged: `found.parts` is the queue a PLAY
        // would build, and downloading a book's forty parts one file at a time is a separate feature (#214
        // rules it out of this increment by name).
        mgr_->resolveDocumentByQuery(query, wantTitle, catType, [this, it](const AddonManager::DocFind& found) {
            if (!found.url.isEmpty()) dlEmit(it, found.url, found.mime);
            dlNext();
        });
        return;
    }
    if (node.addon && node.addon->transport == LoadedAddon::RemoteHttp) // file provider OR Stremio: its /stream
    {
        // The DOWNLOAD crawl, not playback: it writes the file to disk with the normal HTTP client — which is
        // exactly why the source's headers have to ride along (#59). They are declared for THIS url and go
        // no further than it; DownloadManager re-scopes them through forPlayUrl before the request.
        //
        // ...and when the source answers that with NOTHING, a game falls back to the same title+console search
        // the local bridge above does. Asking by id is right and stays the fast path, but it only works for an
        // id the ROM source knows: a game leaf browsed from a console page or a metadata shelf carries a
        // METADATA id, and /stream for one of those correctly returns zero streams. Before this, that ended the
        // leaf silently and the crawl finished "Nothing here could be downloaded" — with the search that finds
        // the ROM sitting right there, reachable only from the other transport.
        //
        // NOT romhack-specific: this is the ordinary Download verb on any game leaf from a remote addon.
        // resolveDocumentByQuery is the right call from here — it does not resolve against `node.addon` at all,
        // it picks the first enabled remote file provider exposing a catalog of the type and searches THAT.
        // The sequence, and the rule that dlNext() runs exactly once per leaf on every one of these paths,
        // live in browse/RemoteLeafResolve.h.
        const QString parentTitle = node.parentTitle, parentType = node.parentType;
        mgr_->resolveStream(node.addon, it, [this, it, parentTitle, parentType](const QString& url, const QString& mime,
                                                                               const StreamHeaders::Headers& headers) {
            browse::RemoteLeafSinks stage1;
            // Only the DIRECT answer carries headers: they were declared for that url. The search below resolves
            // a different url off a file provider, which declares none — exactly as the local bridge emits.
            stage1.emitFound = [this, it, headers](const QString& u, const QString& m) { dlEmit(it, u, m, headers); };
            stage1.finish    = [this] { dlNext(); };
            stage1.search    = [this, it](const browse::RemoteLeafPlan& plan) {
                mgr_->resolveDocumentByQuery(plan.query, plan.wantTitle, plan.catalogType,
                                             [this, it](const AddonManager::DocFind& found) {
                    const QString u = found.url, m = found.mime;
                    // `wantTitle` already refused anything whose title is not the game asked for. That gate is
                    // the point: a fuzzy hit on the wrong game installs the patch against the wrong dump and
                    // fails much later, somewhere that looks nothing like this.
                    browse::RemoteLeafSinks stage2;
                    stage2.emitFound = [this, it](const QString& fu, const QString& fm) { dlEmit(it, fu, fm); };
                    stage2.finish    = [this] { dlNext(); };
                    browse::remoteLeafSearchDone(u, m, stage2);
                });
            };
            browse::remoteLeafResolved(url, mime, it.type, it.title, parentTitle, parentType, stage1);
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

void HomeView::dlEmit(const MediaItem& it, const QString& url, const QString& mime,
                      const StreamHeaders::Headers& headers)
{
    MediaItem m = it; m.url = url; m.mime = mime;
    // Bound to the url they were declared for, exactly as the play path binds them (#59). The default is
    // empty, so the one caller with no headers to offer — resolveDocumentByQuery, whose callback carries
    // none — keeps the previous behaviour rather than inheriting anything.
    m.requestHeaders = headers;
    // Downloading for keeps: save the catalog metadata + artwork locally (MetaCache) so this item's
    // poster and info page keep working offline. Only items with a stable id are cached — a transient
    // stream url would never match the item again. The rich detail card comes from the open info page
    // when it's showing this item; a container crawl saves each episode's own card (see onMetaReady).
    if (!it.id.isEmpty())
    {
        // The row as the PROVIDERS gave it: the cache is the scraped layer, and the correction composites
        // over it on every read. Saving the composited row would bake the user's edit in as if the scraper
        // had said it — and "reset to scraped" would then restore the edit.
        const MediaItem scraped = scrapedRow(it);
        MetaCache::saveItem(scraped);
        const QString key = MetaCache::keyFor(it);
        MetaCache::cacheImage(key, QStringLiteral("thumb"), scraped.thumbnailUrl);
        if (key == lastMetaKey_ && lastMeta_.valid)
        {
            MetaCache::saveDetail(key, lastMeta_);
            MetaCache::cacheImage(key, QStringLiteral("poster"), lastMeta_.imageUrl);
        }
    }
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
    // A leaf info page (a single movie/book/issue): ask the host to surface its detail screen. Containers
    // (a comic/TV/manga series, a console) instead drill into their children, which the themed cross shows in
    // its column - so don't surface the classic view for those.
    if (isInfoPageType(it.type) && !it.expandable) emit infoPageRequested();
}

bool HomeView::atDetailLevel() const { return !stack_.isEmpty() && stack_.last().detail; }

// One-line append to <app>/stream_debug.log, so what the browse surface CAPTURED can be compared with
// what the reader was later ARMED with. The two are a signal apart, and when they disagree nothing on
// screen says so — a boundary press just silently does nothing.
static void hvLog(const QString& msg)
{
    QFile f(AppPaths::dataDir() + QStringLiteral("/stream_debug.log"));
    if (f.open(QIODevice::Append | QIODevice::Text))
        f.write((QDateTime::currentDateTime().toString(Qt::ISODate) + QStringLiteral("  ") + msg
                 + QStringLiteral("\n")).toUtf8());
}

// Right-click on the Home list: offer to remove the Recent or Favorite under the cursor.
// Identity for a local game favourite: its stable resume key, else its path.
static QString gameFavId(const MediaItem& it) { return it.id.isEmpty() ? it.url : it.id; }

// True if `path` is a file EverythingBox downloaded (under our downloads folder or the remote cache), so it's
// safe to delete on "Uninstall". A ROM the user keeps in their own library folder is left alone.
static bool weOwnDownloadedFile(const QString& path)
{
    if (path.isEmpty() || path.contains(QStringLiteral("://"))) return false;
    const QString file = QDir::cleanPath(path);
    const QStringList ours = {
        QDir::cleanPath(AppPaths::dataDir() + QStringLiteral("/downloads")),
        QDir::cleanPath(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)),
    };
    for (const QString& base : ours)
        if (!base.isEmpty() && file.startsWith(base + QLatin1Char('/'), Qt::CaseInsensitive)) return true;
    return false;
}

// The Recent/Downloads game menu: Play (default) / Favorite / Add to playlist / Uninstall. A NavMenu from
// the nav kit — an in-window child overlay (no separate window, so the themed QML view doesn't flash), with
// controller + keyboard + mouse navigation and previous-selection restore built in.
void HomeView::showGameItemMenu(MediaItem it, bool isDownloads)
{
    const bool fav = FavoritesStore::isFavorite(gameFavId(it));
    const bool canDelete = weOwnDownloadedFile(it.url);
    // Romhacks is APPENDED after the fixed rows so their indices keep meaning what they meant, and only on a
    // retro game with a resolvable system.
    const QString romhackSystem = retroSystemFor(it, browseConsoleName());
    // ...and the native port bound to this exact game, if there is one (issue #233). Appended after Romhacks
    // for the same reason Romhacks is appended after the fixed rows: the indices above it keep their meaning.
    // Almost every game answers "" here — a port runs ONE title.
    const QString portId = nativePortIdFor(it);
    QStringList rows = {
        tr("▶   Play"),
        fav ? tr("★   Unfavorite") : tr("☆   Favorite"),
        tr("➕   Add to playlist…"),
        canDelete ? tr("🗑   Uninstall (delete file)") : tr("🗑   Remove from list"),
    };
    if (!romhackSystem.isEmpty()) rows << tr("🧩   Romhacks…");
    // Its index is TAKEN, not written down: Romhacks may or may not have claimed row 4 above, and a hard-coded
    // 5 here would fire the wrong verb on every game that has no hacks. The switch below keeps its literals
    // because those rows are fixed; this one is not.
    const int portRow = portId.isEmpty() ? -1 : int(rows.size());
    if (portRow >= 0) rows << tr("🖥   Native port…");
    new NavMenu(it.title, rows, [this, it, isDownloads, romhackSystem, portId, portRow](int row) {
        // Runs after this overlay has closed, like the romhack arm below: MainWindow answers it with a
        // confirmation card and then an install, and neither may open under a menu that is still up.
        if (portRow >= 0 && row == portRow) { emit nativePortRequested(it, portId); return; }
        switch (row)
        {
        // A merged PC game carries no url — which copy runs is decided now, from the library as it is now.
        case 0: if (isMergedPcGame(it)) playPcGame(it);
                else emit openRecent(it.url, it.mime, resumeKeyFor(it), it.title, it.thumbnailUrl);
                break;
        case 1: toggleGameFavorite(it); break;
        case 2: addGameToPlaylistInteractive(it); break;
        case 3: uninstallGameItem(it, isDownloads); break;
        // Runs after this overlay has closed (NavMenu calls back on the way out), so the romhack flow's own
        // menus open cleanly — the same shape as addGameToPlaylistInteractive above.
        case 4: noteRomhackTarget(it, stack_.isEmpty() ? nullptr : stack_.last().addon, romhackSystem);
                emit romhacksRequested(it, romhackSystem); break;
        }
    }, window());
}

void HomeView::toggleGameFavorite(const MediaItem& it)
{
    const QString id = gameFavId(it);
    if (FavoritesStore::isFavorite(id)) { FavoritesStore::remove(id); showToast(tr("Removed from Favorites."), kFeedbackShort); }
    else
    {
        // Record the console so the favourite shows inside that console's ★ Favorites folder: the
        // Recent/Downloads store entry knows it best (ambiguous extensions like .iso/.cue), and
        // localGameFavorite falls back to the ROM extension for games in neither store.
        QString hint;
        for (const DownloadedItem& d : DownloadsStore::list())
            if (d.path == it.url || (!it.id.isEmpty() && d.key == it.id)) { hint = d.system; break; }
        if (hint.isEmpty())
            for (const RecentItem& r : RecentStore::list())
                if (r.path == it.url || (!it.id.isEmpty() && r.key == it.id)) { hint = r.system; break; }
        FavoritesStore::add(browse::localGameFavorite(it, hint));
        showToast(tr("Added “%1” to Favorites.").arg(it.title), kFeedbackShort);
    }
    // Refresh the CURRENT view: the Home recents list, or the console Recent/Downloaded/Favorites level we're in.
    browseSelectKey_ = it.url.isEmpty() ? it.id : it.url; // keep the selection on this game after the re-sync
    if (recentView_) renderRecents();
    else             loadTop();
    emit browseItemsChanged(false); // re-sync a themed browse view (else its selection/metadata desync)
    browseSelectKey_.clear();
}

void HomeView::addGameToPlaylistInteractive(const MediaItem& it)
{
    const QString key = currentCategoryKey(); // game-category playlists (offered across every games catalogue)
    QVector<Playlist> pls = PlaylistStore::forCategory(key);
    QStringList opts;
    for (const Playlist& p : pls) opts << p.name;
    opts << tr("➕ New playlist…");
    const int row = NavMenu::pick(tr("Add “%1” to:").arg(it.title), opts, window()); // in-window picker
    if (row < 0) return;
    QString plid, plname;
    if (row == pls.size()) // the "New playlist…" row
    {
        const QString name = Osk::getText(tr("Playlist name:"), QString(), QLineEdit::Normal, window()).trimmed();
        if (name.isEmpty()) return;
        plid = PlaylistStore::create(key, name); plname = name;
    }
    else { plid = pls[row].id; plname = pls[row].name; }
    if (plid.isEmpty()) return;
    PlaylistEntry e;
    e.itemId = gameFavId(it); e.title = it.title; e.type = QStringLiteral("game");
    e.thumbnailUrl = it.thumbnailUrl;
    e.path = it.url; e.kind = it.mime; // re-open by path
    PlaylistStore::addItem(plid, e);
    showToast(tr("Added “%1” to “%2”.").arg(it.title, plname), kFeedbackShort);
}

void HomeView::uninstallGameItem(const MediaItem& it, bool /*isDownloads*/)
{
    const bool del = weOwnDownloadedFile(it.url);
    const QString msg = del ? tr("Delete “%1” from disk? This removes the downloaded game file.").arg(it.title)
                            : tr("Remove “%1” from the list? (The file on disk is left in place.)").arg(it.title);
    // In-window confirm card (controller-navigable); No is focused and Back cancels.
    if (NavConfirm::ask(tr("Uninstall game"), msg, { tr("Yes"), tr("No") },
                        /*focusIndex=*/1, /*cancelIndex=*/1, window()) != 0)
        return;

    if (del) QFile::remove(it.url);
    const QString keyOrPath = it.id.isEmpty() ? it.url : it.id;
    DownloadsStore::remove(keyOrPath); DownloadsStore::remove(it.url);
    RecentStore::remove(keyOrPath);    RecentStore::remove(it.url);
    MetaCache::remove(keyOrPath);      // drop its offline metadata/artwork bundle too
    clearResume(resumeKeyFor(it));
    showToast(del ? tr("Uninstalled “%1”.").arg(it.title) : tr("Removed “%1”.").arg(it.title), kFeedbackShort);

    if (recentView_) { renderRecents(); emit browseItemsChanged(false); } // Home list (+ re-sync themed browse)
    else             loadTop();        // repopulate the catalogue Recent/Downloaded level (emits its own sync)
}

void HomeView::showItemContextMenu(int row, const QPoint& globalPos)
{
    if (row < 0 || row >= items_.size()) return;
    const MediaItem& it = items_[row];
    if (it.type == QStringLiteral("rechdr") || it.type == QStringLiteral("info")) return; // a header, not actionable

    // A game in the Recent/Downloaded lists gets the full action menu (same as activating it).
    if ((it.mime == QStringLiteral("game") || it.mime == QStringLiteral("pcgame")) && !it.url.isEmpty()
        && (recentView_ || atRecentsLevel() || atDownloadsLevel()))
    { showGameItemMenu(it, atDownloadsLevel()); return; }

    // Live TV (#75 inc 2): a saved source long-presses to REMOVE it; a channel long-presses to toggle its
    // favourite. Both are outside the recentView_ list below, so they must be handled before its guard. Copied
    // by value and deferred a turn — removeIptvSourceInteractive spins a NavConfirm loop and both rebuild the
    // level, the game-menu / issue-#28 pattern.
    if (it.type == QStringLiteral("_livetvsource"))
    {
        const QString sid = it.mime.mid(QStringLiteral("livetvsource:").size());
        const QString name = it.title;
        QMetaObject::invokeMethod(this, [this, sid, name] { removeIptvSourceInteractive(sid, name); },
                                  Qt::QueuedConnection);
        return;
    }
    if (it.type == QStringLiteral("livetv"))
    {
        const MediaItem copy = it;
        QMetaObject::invokeMethod(this, [this, copy] { toggleLiveTvChannelFavorite(copy); },
                                  Qt::QueuedConnection);
        return;
    }
    // Music servers (#193 increment 5): a saved server long-presses to REMOVE it, exactly as a Live TV
    // source does — same idiom, same deferral, and the same reason it is here rather than in an Enter
    // route: Enter on a server means "show me what is on it", which is what a person wants every time
    // except one. Something you can add and never remove is not configuration.
    if (it.type == QString::fromLatin1(browse::kMusicServerType))
    {
        const QString sid = browse::musicKeyOf(it.mime, browse::kMusicServerPrefix);
        const QString name = it.title;
        QMetaObject::invokeMethod(this, [this, sid, name] { removeMusicServerInteractive(sid, name); },
                                  Qt::QueuedConnection);
        return;
    }

    // #193 increment 2: a music track or album long-presses/right-clicks to the queue verbs. Placed above the
    // recentView_ guard for the same reason Live TV's two rows are — this is a BROWSE row, and the plain
    // remove menu below only ever served the Home recents list. The row travels (not the target): the menu is
    // a nav-kit NavMenu owned by MainWindow, and it re-resolves on the far side.
    if (browse::queueTargetFor(it).ok()) { emit browseQueueMenuRequested(row); return; }

    if (!recentView_) return; // the plain remove menu below is for the Home recents/favorites list only
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
    // A favourited MERGED PC game. THE case this whole re-derivation path exists for, and the one that was
    // broken by construction: a FavoriteItem stores an id, a title and a path, and a merged PC game has no
    // path (which copy runs is decided at activation) and an id — "pcgame:hades" — that, unlike
    // "steam:1145360", encodes no launch. Reconstructing the launch FROM THE ID, which is what every other
    // branch below does, cannot work here: there is nothing in the id to reconstruct from. Favourite a PC
    // game, restart the app, press Play and nothing at all would happen.
    //
    // So the sources are rebuilt from the library as it is NOW and the same picker runs. That is also the
    // right answer rather than a workaround: the Steam copy may have been uninstalled and the GOG one moved
    // since the star was set, and a persisted source list would faithfully launch something that is gone.
    // It is checked FIRST because a merged favourite can also be reached by the path loop below once a
    // downloaded copy has stamped a path onto it.
    if (isMergedPcGame(favItem)) { playPcGame(favItem); return; }
    // A favourited local game (starred from the Recent/Downloads menu) re-opens by path — openRecent recovers
    // its console from the Recent/Downloads store.
    for (const FavoriteItem& f : FavoritesStore::list())
        if (f.itemId == favItem.id && !f.path.isEmpty())
        {
            emit openRecent(f.path, f.kind, f.itemId, f.title, f.thumbnailUrl);
            return;
        }
    // A favourited native-store game with no local file (Steam/Epic) has no source addon - reopen its native
    // info page (rooted at Home); Play rebuilds the launch URL from the id. (A GOG favourite carries its exe as
    // a path, so it re-opened via the openRecent branch above — the GogGame dispatch — and never lands here.)
    const bool isSteamFav = favItem.id.startsWith(QStringLiteral("steam:"));
    const bool isEpicFav  = favItem.id.startsWith(QStringLiteral("epic:"));
    if (isSteamFav || isEpicFav)
    {
        recentView_ = false;
        applyGridMode(/*recentList*/ false);
        styleTypeButtons(QStringLiteral("home"));
        stack_.clear();
        MediaItem mi = favItem;
        mi.mime = isSteamFav ? QStringLiteral("steamgame")
                             : QStringLiteral("epicgame"); // restore the marker (drops the "fav:" tag)
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
        showToast(tr("That favourite's source addon isn't available."), kFeedbackLong);
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
    // Every pop leaves the page a "Choose source…" request may have been made from — say so, so MainWindow can
    // invalidate it (the classic stack is otherwise invisible there; see browseLevelPopped).
    if (stack_.size() > 1) { stack_.pop_back(); emit browseLevelPopped(); loadTop(); return; }
    // A favourite opened from Home is a lone detail level -> Back returns to Home.
    if (stack_.size() == 1 && stack_.last().detail) { emit browseLevelPopped(); selectRecent(); return; }
    // In carousel layout, Back from a catalog (or Home) returns to the media-type carousel.
    if (carouselMode_ && !atCarouselLanding_) { showCarousel(); return; }
    // Nothing left to pop: we're at the home root. The host decides (the app pause menu) — this keeps the
    // one Back rule (previous screen everywhere; app menu at the home root) in the base window.
    emit backRequested();
}

void HomeView::doSearch()
{
    if (stack_.isEmpty()) return;
    const QString q = search_->text().trimmed();

    // Searching while drilled into a console scopes the search to THAT console's games (not the whole
    // catalog): keep the console level and re-run it with the query. Clearing the box restores the full
    // list. Covers addon consoles (getDetail applies the query - e.g. an IGDB name filter) and the native
    // PC library (filtered locally in populatePcGames).
    Level& cur = stack_.last();
    if (cur.detail && (cur.item.type == QStringLiteral("platform")
                       || cur.item.mime == QStringLiteral("pcgames:console")))
    {
        cur.query = q;
        cur.childRow = -1;
        // The PC folder repopulates DIRECTLY here rather than through loadTop, to say the one thing loadTop
        // cannot know: this is a filter keystroke, not a library refresh. loadTop's branch runs the id remap
        // (correct when you enter or Back into the folder); doing it per debounced keystroke is an ini pass
        // per letter over a table that cannot have changed. Everything else loadTop would do for this level
        // is the repopulate itself.
        if (cur.item.mime == QStringLiteral("pcgames:console"))
        {
            pendingRestoreRow_ = -1;               // fresh view, exactly as loadTop() does
            populatePcGames(/*runRemap*/ false);
            return;
        }
        loadTop();
        return;
    }

    // Otherwise, search re-runs the base media-type catalog with the query (drops any drill-down). Selected
    // filters on the base level are kept, so search and filters combine.
    Level base = stack_.first();
    base.detail = false;
    base.childRow = -1; // a fresh result set -> land on the first item, not the old drill position
    base.query = q;
    stack_.clear();
    stack_.push_back(base);
    loadTop();
}

void HomeView::rebuildFilterBar(const QVector<CatalogFilter>& filters)
{
    if (!filterBar_) return;
    if (filters.isEmpty() || carouselMode_ || xmbMode_) // nothing to filter, or a layout without the bar
    {
        filterBar_->setVisible(false);
        filterSig_.clear();
        return;
    }
    // A signature of the available filters, so we only rebuild the combos when the set changes (switching
    // catalogs) - not on every reload (which would recreate the combos and re-fire their change signals).
    QString sig;
    for (const CatalogFilter& f : filters) sig += f.key + QLatin1Char('#') + QString::number(f.options.size()) + QLatin1Char(';');
    if (sig == filterSig_)
    {
        // Same filter SHAPE does not mean the same LEVEL. Switching tabs (selectType clears stack_) or
        // drilling into a container installs a level whose `filters` map is empty, while these combos still
        // show the previous level's pick — the bar would name a filter the request does not carry. Re-sync
        // the indices from the live level instead of trusting the reused widgets. Signals stay blocked:
        // onFilterChanged would write the stale value straight back into the fresh level and reload.
        const QMap<QString, QString> cur = stack_.isEmpty() ? QMap<QString, QString>() : stack_.last().filters;
        for (QComboBox* c : filterCombos_)
        {
            const QSignalBlocker block(c);
            const int idx = c->findData(cur.value(c->property("filterKey").toString()));
            c->setCurrentIndex(idx >= 0 ? idx : 0);
        }
        filterBar_->setVisible(true);
        return;
    }
    filterSig_ = sig;

    for (QComboBox* c : filterCombos_) c->deleteLater();
    filterCombos_.clear();
    QLayoutItem* li;
    while ((li = filterLayout_->takeAt(0)) != nullptr) { if (li->widget()) li->widget()->deleteLater(); delete li; }

    const QMap<QString, QString> selected = stack_.isEmpty() ? QMap<QString, QString>() : stack_.last().filters;
    for (const CatalogFilter& f : filters)
    {
        auto* lbl = new QLabel(f.label + QStringLiteral(":"), filterBar_);
        lbl->setStyleSheet(QStringLiteral("color:#cfd3da;font-size:12px;"));
        filterLayout_->addWidget(lbl);
        auto* combo = new QComboBox(filterBar_);
        combo->setProperty("filterKey", f.key);
        combo->setMinimumWidth(110);
        for (const auto& opt : f.options) combo->addItem(opt.second, opt.first);
        const int idx = combo->findData(selected.value(f.key));
        combo->setCurrentIndex(idx >= 0 ? idx : 0);
        connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) { onFilterChanged(); });
        filterCombos_.push_back(combo);
        filterLayout_->addWidget(combo);
    }
    filterLayout_->addStretch(1);
    filterBar_->setVisible(true);
}

void HomeView::onFilterChanged()
{
    if (stack_.isEmpty()) return;
    QMap<QString, QString> sel;
    for (QComboBox* c : filterCombos_)
    {
        const QString key = c->property("filterKey").toString();
        const QString val = c->currentData().toString();
        if (!key.isEmpty() && !val.isEmpty()) sel.insert(key, val);
    }
    stack_.last().filters = sel;
    stack_.last().childRow = -1;
    issueRequest(false); // reload page 1 with the new filters (keeps the current search query)
}

void HomeView::loadTop()
{
    if (stack_.isEmpty()) return;
    pendingRestoreRow_ = -1; // fresh view: any in-progress "page toward the drilled item" restore is moot
    const Level& top = stack_.last();

    // Returning to the PC Games console (e.g. Back from a game's info page): repopulate natively, not via addon.
    if (top.detail && top.item.mime == QStringLiteral("pcgames:console")) { populatePcGames(); return; }
    // Returning to a cross-addon search (Back out of a result): re-run the fan-out.
    if (top.detail && top.item.type == QStringLiteral("_search"))
        { startSearch(top.item.mime.mid(QStringLiteral("search:").size())); return; }
    // Returning to a synthetic Recent level (Back from a re-opened item): rebuild it natively.
    if (top.detail && top.item.type == QStringLiteral("_recents"))
        { populateRecents(top.item.mime.mid(QStringLiteral("recents:").size())); return; }
    // Returning to a synthetic Downloaded level: rebuild it natively.
    if (top.detail && top.item.type == QStringLiteral("_downloads"))
        { populateDownloads(top.item.mime.mid(QStringLiteral("downloads:").size())); return; }
    // Returning to a synthetic Local Library level: rebuild it natively.
    if (top.detail && top.item.type == QStringLiteral("_locallib"))
        { populateLocalLibrary(top.item.mime.mid(QStringLiteral("locallib:").size())); return; }
    // Returning to the Photos category root or a photo folder (#102): re-scan and rebuild natively.
    if (top.detail && top.item.type == QStringLiteral("_photosroot")) { populatePhotos(); return; }
    if (top.detail && top.item.type == QStringLiteral("_photofolder"))
        { populatePhotoFolder(top.item.mime.mid(QStringLiteral("photofolder:").size())); return; }
    // Returning to a Music level (#74) — Back out of a played track, or out of an album: rebuild it from the
    // installed index. No rescan here (unlike Photos): a music scan is a tag parse per file, and the index is
    // refreshed by MainWindow, not by walking back up a browse stack.
    if (top.detail && top.item.type == QStringLiteral("_musicroot")) { populateMusicArtists(); return; }
    // Music servers (#193): the same Back-repopulates shape as every other synthetic music level.
    if (top.detail && top.item.type == QStringLiteral("_musicservers")) { populateMusicServers(); return; }
    if (top.detail && top.item.type == QStringLiteral("_musicserver"))
        { populateMusicServer(browse::musicKeyOf(top.item.mime, browse::kMusicServerPrefix)); return; }
    if (top.detail && top.item.type == QStringLiteral("_musicartist"))
        { populateMusicArtist(browse::musicKeyOf(top.item.mime, browse::kMusicArtistPrefix)); return; }
    if (top.detail && top.item.type == QStringLiteral("_musicalbum"))
        { populateMusicAlbum(browse::musicKeyOf(top.item.mime, browse::kMusicAlbumPrefix)); return; }
    if (top.detail && top.item.type == QStringLiteral("_musiccomposers")) { populateMusicComposers(); return; }
    if (top.detail && top.item.type == QStringLiteral("_musiccomposer"))
        { populateMusicComposer(browse::musicKeyOf(top.item.mime, browse::kMusicComposerPrefix)); return; }
    if (top.detail && top.item.type == QStringLiteral("_musicwork"))
        { populateMusicWork(browse::musicKeyOf(top.item.mime, browse::kMusicWorkPrefix)); return; }
    // Returning to an Audiobooks level (#139) — Back out of a played book, or out of an author. Same shape
    // and same reasoning as the music levels above: rebuild from the installed index, never a rescan.
    if (top.detail && top.item.type == QStringLiteral("_abroot")) { populateAudiobooks(); return; }
    if (top.detail && top.item.type == QStringLiteral("_abauthor"))
        { populateAudiobookAuthor(browse::audiobookKeyOf(top.item.mime, browse::kAudiobookAuthorPrefix)); return; }
    if (top.detail && top.item.type == QStringLiteral("_abnarrators")) { populateAudiobookNarrators(); return; }
    if (top.detail && top.item.type == QStringLiteral("_abnarrator"))
        { populateAudiobookNarrator(browse::audiobookKeyOf(top.item.mime, browse::kAudiobookNarratorPrefix)); return; }
    if (top.detail && top.item.type == QStringLiteral("_abserieslist")) { populateAudiobookSeriesList(); return; }
    if (top.detail && top.item.type == QStringLiteral("_abseries"))
        { populateAudiobookSeries(browse::audiobookKeyOf(top.item.mime, browse::kAudiobookSeriesPrefix)); return; }
    if (top.detail && top.item.type == QStringLiteral("_abbook"))
        { populateAudiobookBook(browse::audiobookKeyOf(top.item.mime, browse::kAudiobookBookPrefix)); return; }
    // Returning to a Books level (#134) - Back out of a book that was open, or out of an author. Same shape
    // and same reasoning as the levels above: rebuild from the installed index, never a rescan.
    if (top.detail && top.item.type == QStringLiteral("_bkroot")) { populateBooks(); return; }
    if (top.detail && top.item.type == QStringLiteral("_bkauthor"))
        { populateBookAuthor(browse::bookKeyOf(top.item.mime, browse::kBookAuthorPrefix)); return; }
    if (top.detail && top.item.type == QStringLiteral("_bkserieslist")) { populateBookSeriesList(); return; }
    if (top.detail && top.item.type == QStringLiteral("_bkseries"))
        { populateBookSeries(browse::bookKeyOf(top.item.mime, browse::kBookSeriesPrefix)); return; }
    // Returning to the synthetic Airing Soon level: rebuild it from the cached calendar.
    if (top.detail && top.item.type == QStringLiteral("_traktcal")) { populateTraktCalendar(); return; }
    if (top.detail && top.item.type == QStringLiteral("_traktmissed")) { populateTraktMissed(); return; }
    if (top.detail && top.item.type == QStringLiteral("_traktlist"))
    { populateTraktList(top.item.mime.section(QLatin1Char(':'), 1)); return; }
    // Returning to a console's synthetic Favorites level: rebuild it natively.
    if (top.detail && top.item.type == QStringLiteral("_favorites"))
        { populateFavorites(top.item.mime.mid(QStringLiteral("favorites:").size())); return; }
    // Returning to a console's synthetic Homebrew level (Back out of a played title): re-fetch page one from
    // the system the level's own marker names. This is why openHomebrewLevel stores that marker at all.
    if (top.detail && top.item.type == QStringLiteral("_homebrew"))
        { populateHomebrew(HomebrewClient::levelSystem(top.item.mime)); return; }
    // Returning to a marks shelf level (Favorites / pinned tag / Hidden): re-show its snapshotted intersection.
    if (top.detail && (top.item.type == QStringLiteral("_favshelf") || top.item.type == QStringLiteral("_tagshelf")
                       || top.item.type == QStringLiteral("_hiddenshelf") || top.item.type == QStringLiteral("_presetshelf")))
        { MediaCatalog c; c.items = top.synthItems; showSyntheticCatalog(c); return; }
    // Returning to a synthetic playlist level (Back out of a playlist / an item): rebuild it natively.
    if (top.detail && top.item.type == QStringLiteral("_playlists"))
        { populatePlaylists(top.item.mime.mid(QStringLiteral("playlists:").size())); return; }
    if (top.detail && top.item.type == QStringLiteral("_playlist"))
        { populatePlaylistItems(top.item.mime.mid(QStringLiteral("playlist:").size())); return; }
    // Returning to the synthetic Live TV sources shelf (#75 inc 2): rebuild it from the store.
    if (top.detail && top.item.type == QStringLiteral("_livetvsources"))
        { populateLiveTvSources(); return; }
    // Returning to a source's channels level (Back out of a played channel): re-show its channels from the
    // in-session cache (a fetch already happened when it was opened; a fresh fetch is refresh-on-OPEN, not
    // refresh-on-back), re-fetching only if that cache is gone.
    if (top.detail && top.item.type == QStringLiteral("_livetvchannels"))
        { populateLiveTvChannels(top.item.mime.mid(QStringLiteral("livetvchannels:").size())); return; }
    // Returning to the Recomps section (#248): rebuild it. NOT from a snapshot — the whole reason to come back
    // here is that something changed (a port was installed, played or removed), and a snapshot would show the
    // state that was true before the user acted.
    if (top.detail && top.item.type == QStringLiteral("_recomps")) { populateRecomps(); return; }
    // Returning to the OPDS "Book Servers" shelf (#146): rebuild it from the store.
    if (top.detail && top.item.type == QStringLiteral("_opdscatalogs")) { populateOpdsCatalogs(); return; }
    // Returning to an OPDS feed level (Back out of a book or a sub-feed): re-fetch it, restoring the catalog's
    // auth context from the level's stored "opdsfeedlvl:<catalogId>\n<feedUrl>".
    if (top.detail && top.item.type == QStringLiteral("_opdsfeedlvl"))
    {
        const QString payload = top.item.mime.mid(QStringLiteral("opdsfeedlvl:").size());
        populateOpdsFeed(payload.section(QLatin1Char('\n'), 0, 0),
                         payload.section(QLatin1Char('\n'), 1), top.item.title);
        return;
    }
    // Returning to a source's guide grid (Back within it): rebuild from the already-loaded entries + guide.
    if (top.detail && top.item.type == QStringLiteral("_livetvguidegrid"))
        { populateLiveTvGuide(top.item.mime.mid(QStringLiteral("livetvguide:").size())); return; }

    const bool container = top.detail && top.item.expandable;       // has children to drill into
    // A leaf detail page (a movie/episode info page) has no child list to filter -> hide the bar. A container
    // detail (a console's games, a series' seasons) can advertise filters via its children response.
    if (top.detail && !container && filterBar_) { filterBar_->setVisible(false); filterSig_.clear(); }
    const bool wantMeta  = top.detail && top.item.type != QStringLiteral("platform"); // console is not "media"

    if (wantMeta) requestMeta(top.item);
    else          hideMeta();

    if (top.detail && !container)
    {
        // Leaf (episode / song / movie / game / book): a metadata-only page, no child grid/carousel.
        ++generation_;
        grid_->clear();
        items_.clear();
        preCorrection_.clear();
        grid_->hide();
        if (carousel_) carousel_->hide();
        if (xmb_) xmb_->hide();
        loading_ = false; hasMore_ = false; currentPage_ = 1; pendingReqId_ = -1;
        updateChrome();
        updateStatus();
        // The grid/carousel that held focus is now hidden; park focus on the Favorite button so the
        // detail page still has a keyboard target (and Backspace routes to Back via its event filter).
        if (meta_->isVisible()) { if (QWidget* a = detailActionButton()) takeFocus(a); }
        return;
    }

    if (carouselMode_ || xmbMode_) grid_->hide(); // the carousel/XMB shows catalog items; populate() fills them
    else                           grid_->show();
    issueRequest(/*append*/ false);
}

// Resolve a leaf to a playable/readable source and open it. Pure: every bit of context comes in as an
// argument (no stack_/detail-page state), so the classic detail Play button and the themed inline Play both
// reuse it. `parentTitle` is the level we drilled in from (a comic issue's volume); `console` is the platform
// ancestor (a ROM core hint); `imdbId`/`imdbType` resolve a non-Stremio movie/episode via stream addons.
void HomeView::resolvePlay(LoadedAddon* addon, const MediaItem& it, const QString& parentTitle,
                           const QString& console, const QString& imdbId, const QString& imdbType)
{
    // Local library prefer-local: if we own this catalog item on disk, play the local file directly and
    // SKIP stream resolution (spec: owned items must play offline, no round-trip). Movies key on id (tt...),
    // episodes on imdbStreamId (tt...:S:E, unpadded — matches OwnedIndex::buildIndex).
    {
        QString lp = LocalLibrary::index().localPathFor(it.id);
        if (lp.isEmpty() && !it.imdbStreamId.isEmpty())
            lp = LocalLibrary::index().localPathFor(it.imdbStreamId);
        if (!lp.isEmpty() && QFileInfo::exists(lp)) {
            MediaItem local = it;
            local.url = lp;
            local.mime = QStringLiteral("local:video");
            emit openItem(local);
            return;
        }
    }
    // A merged PC game (the PC Games folder's tile, or a favourite/recent row rebuilt from a store): pick a
    // source and launch it. This is the classic detail page's Play button and the themed action row's, so it
    // must not assume the row still carries its sources — playPcGame re-derives when it does not.
    //
    // NOT a cross-catalog search row: SearchAggregator fans out to ADDONS only and there is no native-PC leg,
    // so a merged game cannot appear in a _search result at all. This arm would handle one correctly if a leg
    // were ever added; nothing produces one today. (Parity with the four folders it replaced — they were not
    // searchable either — so it is not a regression, just not the coverage it looks like.)
    if (isMergedPcGame(it)) { playPcGame(it); return; }
    if (it.mime == QStringLiteral("steamgame"))
    {
        MediaItem m = it;
        // An owned-not-installed tile already carries a steam://install/<appid> url — honor it; an installed
        // tile has none, so build the run URL. Both ride the same MainWindow openUrl handoff.
        if (m.url.isEmpty())
            m.url = SteamLibrary::launchUrl(it.id.mid(QStringLiteral("steam:").size()));
        emit openItem(m); // MainWindow launches the steam:// URL
        return;
    }
    if (it.mime == QStringLiteral("epicgame"))
    {
        MediaItem m = it;
        // Fire-and-forget through the Epic launcher URI, exactly like steam:// (the launcher owns the process).
        m.url = EpicLibrary::launchUrl(it.id.mid(QStringLiteral("epic:").size()));
        emit openItem(m); // MainWindow hands the com.epicgames.launcher:// URI to the OS
        return;
    }
    if (it.mime == QStringLiteral("goggame"))
    {
        // GOG games are DRM-free exes: the tile already carries the resolved exe in `url`. Hand it to
        // MainWindow, which runs it through the MONITORED launchPcExe path (recording a "goggame" Recent).
        emit openItem(it);
        return;
    }
    if (it.mime == QStringLiteral("battlenetgame"))
    {
        // Either route — URI (no url on the tile) or exe (url set) — is decided by MainWindow::openLibraryItem,
        // which owns the whole battlenetgame branch. Hand the tile over untouched.
        emit openItem(it);
        return;
    }
    if (isReadableChapter(it.type)) // a chapter leaf -> ask its addon for the pages, then open the reader
    {
        // WHICH ADDON, and whether it has said it can answer. An addon that does not declare the `pages`
        // resource is never asked for one (requestPages enforces that, and writes the outdated-addon line
        // once) — but it is still CALLED here, through the one path, so the answer and the log come from
        // the same place. All this flag decides is what to SAY about an empty result: "this source doesn't
        // supply page images" and "this chapter has none" are different facts, and a silent empty answer
        // reads as the second when it is usually the first.
        const bool supplies = mgr_->supportsPages(addon, it.type);
        const QString sourceName = addon && !addon->manifest.name.isEmpty() ? addon->manifest.name
                                                                           : tr("this source");
        if (supplies) showToast(tr("Loading “%1”…").arg(it.title), 20000);
        if (playBtn_) playBtn_->setEnabled(false);
        const QString key = it.id, title = it.title, type = it.type;
        // Captured NOW, not read back in the callback: the run is "the list this chapter was opened from",
        // and browsing on during the resolve would leave the callback reading a different level's list.
        const ChapterRun run = chapterRunFor(key);
        mgr_->requestPages(addon, type, it.id,
                           [this, key, title, run, supplies, sourceName](const QVector<AddonPage>& pages) {
            if (playBtn_) playBtn_->setEnabled(true);
            if (!pages.isEmpty()) { hideToast(); emit openImagePages(title, key, pages, run); }
            else if (!supplies)
                showToast(tr("“%1” can't be read here: %2 doesn't supply page images. A built-in add-on "
                             "that predates this is updated by reinstalling the app.")
                              .arg(title, sourceName), kFeedbackLong);
            else
                showToast(tr("No readable pages for “%1”. The source has no images for this chapter — "
                             "try another chapter, language or title.").arg(title), kFeedbackLong);
        });
        return;
    }
    // A metadata-only item browsed from a LOCAL catalog (AIO Catalog) - comic issue / book / audiobook /
    // retro game - whose actual file the file provider (Allarr) supplies. Bridge it by searching the
    // provider's catalog of that type for a query built from the title (+ its context), then open the
    // first match. A game's console (the platform we drilled in from) tags the search and picks the core.
    const bool localBridge = addon && addon->transport != LoadedAddon::RemoteHttp
        && (it.type == QStringLiteral("comic_issue") || it.type == QStringLiteral("book")
            || it.type == QStringLiteral("audiobook") || it.type == QStringLiteral("game"));
    if (localBridge)
    {
        // A copy we ALREADY HOLD, before asking a provider to go and find one. Opening a book that had
        // been downloaded ran the whole search again — the file was on disk throughout, and what you see is
        // "Finding ..." sitting there, which reads as downloading it a second time.
        // THE VOLUMES EITHER SIDE OF THIS ONE, captured before ANY of the ways out below. It is the same
        // run whether the copy is already on disk or has to be found, and it has to be attached to every
        // exit from this block or the crossing works exactly once — on the open that downloaded the file.
        // Captured NOW rather than read back in a callback, for the reason the manga lane captures now:
        // the run is "the list this issue was opened from", and a search takes long enough to browse
        // somewhere else in.
        const ChapterRun issueRun = (it.type == QStringLiteral("comic_issue"))
                                        ? chapterRunFor(it.id, /*catalogLane*/ true)
                                        : ChapterRun{};
        // The id, not the pointer: a reload rebuilds the source list and destroys every LoadedAddon in
        // it, so a pointer held across the resolve below would be a dangling one.
        const QString catalogAddonId = addon ? addon->manifest.id : QString();
        if (it.type == QStringLiteral("comic_issue"))
            hvLog(QStringLiteral("chapter: bridging \"%1\" id=%2 run=%3 entr(y/ies) index=%4 series=\"%5\"")
                      .arg(it.title, it.id).arg(issueRun.entries.size()).arg(issueRun.index)
                      .arg(issueRun.seriesTitle));

        const QString haveLocal = localCopyForItem(it);
        if (!haveLocal.isEmpty())
        {
            MediaItem m = it;
            m.url = haveLocal;          // a local path now: openLibraryItem dispatches to the file reader
            m.chapterRun = issueRun;    // ...and its neighbours, exactly as the searched-for copy gets
            if (m.sourceAddonId.isEmpty()) m.sourceAddonId = catalogAddonId;
            emit openItem(m);
            return;
        }

        const QString catType = (it.type == QStringLiteral("comic_issue")) ? QStringLiteral("comic") : it.type;
        QString query;
        if (it.type == QStringLiteral("comic_issue"))
        {
            const QRegularExpression re(QStringLiteral("#\\s*([0-9]+(?:\\.[0-9]+)?)"));
            const auto m = re.match(it.title);
            query = (parentTitle + QLatin1Char(' ') + (m.hasMatch() ? m.captured(1) : QString())).trimmed();
        }
        else if (it.type == QStringLiteral("game"))
            query = (it.title + QLatin1Char(' ') + console).trimmed(); // Allarr parses the trailing console name
        else
        {
            // Book / audiobook: "<title> <author>" (the subtitle is "Author · Year").
            const QString author = it.subtitle.section(QStringLiteral(" · "), 0, 0).trimmed();
            query = (it.title + QLatin1Char(' ') + author).trimmed();
        }
        if (query.isEmpty()) query = it.title;
        // A game's localized catalog title may not match the copy's original regional name. Queue the
        // original/alternate names (with the console suffix) as fallbacks, tried only if the provider was
        // reached but had no match under the previous name.
        QStringList queries{ query };
        // Each query is a NAME for the same work, so each needs its own title to judge results by — matching an
        // alternate name's results against the catalog's name would reject exactly the copies that name found.
        QStringList wantTitles{ (it.type == QStringLiteral("comic_issue")) ? parentTitle : it.title };
        if (it.type == QStringLiteral("game"))
            for (const QString& alt : it.altNames)
            {
                const QString q = (alt + QLatin1Char(' ') + console).trimmed();
                if (!q.isEmpty() && !queries.contains(q, Qt::CaseInsensitive)) { queries << q; wantTitles << alt; }
            }
        const bool read = (it.type == QStringLiteral("comic_issue") || it.type == QStringLiteral("book"));
        showToast(read ? tr("Finding “%1” to read…").arg(it.title) : tr("Finding “%1” to play…").arg(it.title), 0);
        if (playBtn_) playBtn_->setEnabled(false);
        const QString title = it.title;
        // Fire every candidate name at once (parallel — a multi-name miss would otherwise cost the provider's
        // ~38s budget PER name), but keep the names' PRIORITY. `queries` is ordered best-first (the catalog
        // title, then the alternate/original names), so we open the hit from the EARLIEST-ranked name — not
        // whichever network reply happens to land first (which is how the Japanese "PC Genjin 2" once beat the
        // wanted "Bonk's Revenge (USA)"). We commit the moment every name ahead of a decisive result has come
        // back. Only a plain "no match" falls through to the next name; a hit / still-caching / provider error
        // is decisive at its rank. Bounded so a title with many alternates can't flood the provider. Callbacks
        // fire on the GUI thread, so the shared state needs no locking.
        constexpr int kMaxParallelNames = 5;
        if (queries.size() > kMaxParallelNames) queries = queries.mid(0, kMaxParallelNames);
        // #214: `parts` is the ordered audio files of a multi-file audiobook release, and `noAudio` says a
        // release WAS chosen and contains none. Both ride the per-name result rather than a shared slot,
        // because up to five of these searches are in flight at once and a shared one would let the
        // fourth-ranked name's answer be opened under the first-ranked name's url.
        struct NameResult
        {
            bool done = false; QString url, mime, err; bool caching = false;
            QVector<RemoteAudiobook::Part> parts;
            bool noAudio = false;
            // #216: the release was expanded and its first part could not be linked, plus whatever the
            // provider said about that attempt. Its own outcome, because the sentence below is not the
            // one for a release nobody could find.
            bool noPartLink = false;
            QString notice;
            // #224: who resolved this name's hit, and which release. Per-name like `parts` and for the same
            // reason — five of these are in flight at once, and an identity read off a shared slot would
            // record the fourth-ranked name's release against the first-ranked name's play.
            QString releaseId, providerId;
        };
        struct MultiSearch { bool committed = false; QVector<NameResult> r; };
        auto ms = std::make_shared<MultiSearch>();
        ms->r.resize(int(queries.size()));
        auto commit = std::make_shared<std::function<void()>>();
        *commit = [this, ms, it, title, console, issueRun, catalogAddonId]() {
            if (ms->committed) return;
            for (const NameResult& q : std::as_const(ms->r))
            {
                if (!q.done) return;                     // a higher-ranked name is still in flight — wait for it
                if (!q.url.isEmpty())                     // best-ranked name that hit → open it
                {
                    ms->committed = true;
                    if (playBtn_) playBtn_->setEnabled(true);
                    // #214: the release's ordered parts ride the item. Empty for everything that is not a
                    // multi-file audiobook, which is what leaves every other kind opening exactly as before.
                    hideToast(); MediaItem m = it; m.url = q.url; m.mime = q.mime; m.systemHint = console;
                    m.bookParts = q.parts;
                    m.chapterRun = issueRun;   // the volumes either side of this one
                    // The CATALOG addon that listed this issue, not the provider that found the file:
                    // it is the one that can be asked what series this belongs to when the run has to be
                    // rebuilt from a Recent, and a Recent that does not name it stays blind forever.
                    if (m.sourceAddonId.isEmpty()) m.sourceAddonId = catalogAddonId;
                    // …AND THE PROVIDER THAT ACTUALLY FOUND THE FILE (#224), which on this route is a
                    // different addon holding a different id. The line above is what a Recents row used to
                    // re-mint by, and it cannot: a metadata catalog has no /stream, so an audiobook re-opened
                    // from Continue Watching answered "Couldn't get a fresh link" before it made a request.
                    // Both identities are needed and neither replaces the other — the catalog answers "what
                    // series is this", the provider answers "give me this again" — so the second rides its
                    // own pair of fields. Empty unless the bridge picked a leaf, which is the only case that
                    // gets here anyway; assigned unconditionally because `m` is a fresh copy of the catalog
                    // item and cannot already carry a re-mint identity of its own.
                    m.remintAddonId = q.providerId;
                    m.remintItemId  = q.releaseId;
                    emit openItem(m);
                    return;
                }
                if (q.noAudio)                            // a copy was found and there is nothing to play in it
                {
                    // #207's precedent, one level deeper: never stage a player over something that cannot be
                    // audio. "No copies were found" would be a lie here — a copy WAS found, and it is an
                    // ebook release that won an audiobook search.
                    ms->committed = true;
                    if (playBtn_) playBtn_->setEnabled(true);
                    showToast(tr("The copy of “%1” that was found has no audio files in it — it looks like an "
                                 "ebook release. Try another source.").arg(title), kFeedbackLong);
                    return;
                }
                if (!q.err.isEmpty())                     // provider unreachable at this rank → report it
                {
                    ms->committed = true;
                    if (playBtn_) playBtn_->setEnabled(true);
                    showToast(tr("Can't reach the file provider (Allarr): %1.").arg(q.err), kFeedbackLong);
                    return;
                }
                if (q.noPartLink)                         // the release was found and listed; part one was not linked
                {
                    // WHAT IS KNOWN, AND NOT A CAUSE NOBODY ESTABLISHED (#216). This used to fall into the
                    // "still caching" arm below, which was asserted here whatever the reason and was false
                    // in the report that opened the issue — the expansion a minute earlier had listed 57
                    // playable files, which a release that is still caching cannot do. Decisive at its rank
                    // like the no-audio refusal: a copy WAS found, so another spelling of the title is not
                    // what is missing.
                    ms->committed = true;
                    if (playBtn_) playBtn_->setEnabled(true);
                    showToast(q.notice.isEmpty()
                                  ? tr("“%1” was found and its parts were listed, but no link for the "
                                       "first part came back. Try again in a moment.").arg(title)
                                  : q.notice,
                              kFeedbackLong);
                    return;
                }
                if (q.caching)                            // a copy exists at this rank but is still caching
                {
                    ms->committed = true;
                    if (playBtn_) playBtn_->setEnabled(true);
                    showToast(tr("“%1” isn't ready yet — the file provider may still be caching it (large or "
                                 "less-common titles take a while). Try again in a few minutes; if it never "
                                 "appears, there may be no copy.").arg(title), kFeedbackLong);
                    return;
                }
                // this name was a plain "no match" — fall through to the next-ranked name
            }
            ms->committed = true;                          // every name came back a plain miss
            if (playBtn_) playBtn_->setEnabled(true);
            showToast(tr("No copies of “%1” were found.").arg(title), kFeedbackLong);
        };
        for (int i = 0; i < queries.size(); ++i)
        {
            mgr_->resolveDocumentByQuery(queries[i], wantTitles.value(i, it.title), catType,
                [ms, commit, i](const AddonManager::DocFind& found) {
                NameResult& q = ms->r[i];
                q.done = true; q.url = found.url; q.mime = found.mime; q.err = found.providerError;
                q.parts = found.parts; q.noAudio = found.noAudio;
                q.noPartLink = found.noPartLink; q.notice = found.notice;
                q.releaseId = found.releaseId; q.providerId = found.providerId;
                // found-but-caching (no url, no error) — and a no-audio refusal is NOT that: it is decisive
                // at its rank, so it must not be reported as "try again in a few minutes". Nor is an
                // expanded release whose first part would not link (#216): "still caching" was exactly the
                // wrong thing to say about one whose 57 files had just been listed.
                q.caching = found.url.isEmpty() && found.providerError.isEmpty() && !found.noMatches
                            && !found.noAudio && !found.noPartLink;
                (*commit)();
            });
        }
        return;
    }
    if (addon && addon->transport == LoadedAddon::RemoteHttp) // resolve via the addon's /stream
    {
        const bool fileProvider = !addon->stremio; // Allarr-style provider: supports alternate sources (?n=)
        lastPlay_ = { addon->manifest.id, it, false, {}, {}, 0 };
        // AN AUDIOBOOK RELEASE BROWSED ON THE PROVIDER'S OWN SHELF (#214). The doc-bridge above already
        // expands one it CHOSE for you; this is the other door into the same defect — the release row you
        // pressed yourself — and it has to give the same answer, or the same book plays as one arbitrary
        // chapter depending on which shelf you found it on. That divergence is the shape of every routing
        // bug LeafRoute.h exists to end.
        //
        // A Stremio addon is excluded: its /detail is a series' episodes and it has no release to expand.
        if (fileProvider && it.type == QStringLiteral("audiobook"))
        {
            showToast(tr("Looking for “%1”…").arg(it.title), 0);
            if (playBtn_) playBtn_->setEnabled(false);
            // #224: the addon about to serve this book, captured as a STRING id — never the LoadedAddon*.
            // AddonManager::reload() clears the vector<unique_ptr<LoadedAddon>> that owns them, so a pointer
            // held across a resolve this slow (a /detail expansion, then part one's link) is a dangling one.
            //
            // WHY THIS SITE WAS LEFT UNSTAMPED AND IS NOT ANY MORE. It was skipped deliberately when the
            // three video sites were stamped, because a "direct" recipe promised a resolveStream call and
            // resolveStream cannot hand back the PARTS LIST a book needs — it returns one arbitrary file,
            // which is #214's original defect (a fifteen-hour book opening at part 10). Recents' re-mint now
            // routes an audiobook row through resolveAudiobookRelease instead (MainWindow::remintAndOpen),
            // so the recipe promises the resolve that can actually keep the promise, and the row is no
            // longer dead the moment its signed link ages out.
            const QString resolvedBy = addon->manifest.id;
            mgr_->resolveAudiobookRelease(addon, it, [this, it, console, resolvedBy](const AddonManager::DocFind& found) {
                if (playBtn_) playBtn_->setEnabled(true);
                if (found.noAudio)
                {
                    // #207's precedent: never stage a player over something that cannot be audio.
                    showToast(tr("“%1” has no audio files in it — it looks like an ebook release. "
                                 "Try another source.").arg(it.title), kFeedbackLong);
                    return;
                }
                if (found.url.isEmpty())
                {
                    // The provider's own words first, wherever they came from — they are the one account of
                    // this attempt that somebody actually established. Failing those, say what is known,
                    // and no more: a release that was expanded and could not hand over its first part is a
                    // different thing from one that produced no link at all, and only the second is worth
                    // guessing "still caching" over (#216).
                    showToast(!found.notice.isEmpty() ? found.notice
                              : found.noPartLink
                                  ? tr("“%1” was found and its parts were listed, but no link for the "
                                       "first part came back. Try again in a moment.").arg(it.title)
                                  : tr("“%1” isn't ready yet — the source may still be caching. Try again "
                                       "in a few minutes; if it never appears, there may be no copy.").arg(it.title),
                              kFeedbackLong);
                    return;
                }
                hideToast();
                MediaItem m = it;
                m.url = found.url; m.mime = found.mime; m.systemHint = console;
                // nextSourceCapable stays TRUE for a single-file recording, which is one release and can
                // honestly be swapped for another. openRemoteAudiobook turns it off for a BOOK, where the
                // verb would mean swapping one part for another release's part — not a thing anyone means.
                m.nextSourceCapable = true;
                m.bookParts = found.parts;
                // Stamped only when the item does not ALREADY name an addon — the same rule the resolveStream
                // site below follows: a cross-addon-search row or a playlist row belongs to the addon whose id
                // space its id came from, and overwriting that would send the re-mint to a source that has
                // never heard of this id.
                if (m.sourceAddonId.isEmpty()) m.sourceAddonId = resolvedBy;
                emit openItem(m);
            });
            return;
        }
        const QString lookingMsg =
            it.type == QStringLiteral("game")  ? tr("Looking for the ROM for “%1”…").arg(it.title)
          : (it.type == QStringLiteral("movie") || it.type == QStringLiteral("series"))
                                               ? tr("Finding a stream for “%1”…").arg(it.title)
                                               : tr("Looking for “%1”…").arg(it.title);
        showToast(lookingMsg, 0);
        if (playBtn_) playBtn_->setEnabled(false);
        // #224: the addon about to serve this play, captured as a string id (reload() frees LoadedAddon*).
        // This is THE path the issue is about — browse a shelf, press Play — and the item built below
        // carried every field but this one, so the Recents row it writes had no re-mint recipe and could
        // only replay a link whose credential #200's scrub had already taken out of the ini.
        const QString resolvedBy = addon->manifest.id;
        mgr_->resolveStream(addon, it, [this, it, fileProvider, console, imdbId, resolvedBy](
                                           const QString& url, const QString& mime,
                                           const StreamHeaders::Headers& headers) {
            if (playBtn_) playBtn_->setEnabled(true);
            // sourceAddonId is stamped only when the item does not already name an addon: a cross-addon
            // search row or a playlist row keeps the addon whose id space its id belongs to.
            if (!url.isEmpty()) { hideToast(); MediaItem m = it; m.url = url; m.mime = mime; m.nextSourceCapable = fileProvider; m.systemHint = console; m.cfCurl = mgr_->takeStreamCurl(); m.imdbStreamId = imdbId; m.requestHeaders = headers; if (m.sourceAddonId.isEmpty()) m.sourceAddonId = resolvedBy; emit openItem(m); }
            else {
                // No link yet. Prefer the addon's own notice (e.g. Allarr just started caching the release
                // on debrid — it names the title). Otherwise, for a file provider the source may still be
                // caching; for anything else it's simply no usable link.
                const QString notice = mgr_->takeStreamNotice();
                if (!notice.isEmpty())
                    showToast(notice, kFeedbackLong);
                else if (fileProvider)
                    showToast(tr("“%1” isn't ready yet — the source may still be caching. Try again in a few "
                                 "minutes; if it never appears, there may be no copy.").arg(it.title), kFeedbackLong);
                else
                    showToast(tr("No playable source for “%1”. The addon returned no usable link.").arg(it.title), kFeedbackLong);
            }
        }, /*attempt=*/0,
        // The release already chosen for this series, if any. Only the Stremio leg reads it (a file provider
        // has no bingeGroup to match against), and only an episode has a series key at all. Without this the
        // memory would be honoured on the automatic next-episode hand-off and silently ignored the moment the
        // user picked the next episode from the list themselves.
        BingeStore::preferredGroup(bingeStore_, it.imdbStreamId.isEmpty() ? it.id : it.imdbStreamId));
        return;
    }
    if (!imdbId.isEmpty()) // a non-Stremio catalog item bridged to IMDB -> resolve via stream addons
    {
        lastPlay_ = { QString(), it, true, imdbType, imdbId, 0 };
        const bool fileProvider = mgr_->hasFileProvider(); // an alternate source is only offerable via Allarr
        showToast(tr("Finding a stream for “%1”…").arg(it.title), 0);
        if (playBtn_) playBtn_->setEnabled(false);
        // No #224 sourceAddonId stamp here, deliberately: this resolve fans out across EVERY installed stream
        // provider, so no single addon owns the answer. The item carries imdbStreamId, which is the recipe —
        // applyRemintRecipe records sourceRoute="imdb" for it and re-resolves the same way, which is the
        // route that survives the addon that happened to answer being uninstalled.
        mgr_->resolveStreamByImdb(imdbType, imdbId, [this, it, fileProvider, imdbId](
                                                        const QString& url, const QString& mime,
                                                        const StreamHeaders::Headers& headers) {
            if (playBtn_) playBtn_->setEnabled(true);
            if (!url.isEmpty()) { hideToast(); MediaItem m = it; m.url = url; m.mime = mime; m.nextSourceCapable = fileProvider; m.imdbStreamId = imdbId; m.requestHeaders = headers; emit openItem(m); }
            else showToast(tr("No sources found for “%1”. No stream addon returned a playable link "
                              "(check that Allarr is configured and returning results).").arg(it.title), kFeedbackLong);
        }, /*attempt=*/0, BingeStore::preferredGroup(bingeStore_, imdbId)); // the release already chosen, if any
        return;
    }
    showToast(tr("Nothing to play for “%1”.").arg(it.title), kFeedbackLong);
}

// ---- Triple/XMB theme: live metadata beside the cross + an inline Play/Favorite, no classic detail page ---

// The ONE way a themed row's metadata reaches the panel. Composites the user's correction (issue #24) over
// the FINISHED map, whichever source assembled it — the catalog row, the ROMs-folder gamelist.xml, this
// session's art cache, our own scrape cache, or the addon's /meta. Every one of those is a SCRAPED source
// and none of them knows about the correction, so hooking each one would be five hooks that have to agree;
// this is the same "composite over the finished map" rule themedDetailData already ends on.
//
// It is also what lets themedArtCache_ keep holding the SCRAPED map: the correction is applied on the way
// out, so a later edit — or a reset — shows without anything having to invalidate that cache.
void HomeView::emitThemedMeta(int idx, QVariantMap meta)
{
    meta.insert(QStringLiteral("index"), idx);
    if (idx >= 0 && idx < browseRowMap_.size())
    {
        const MediaItem& it = items_[browseRowMap_[idx]];
        MetaOverrides::applyTo(MetaOverrides::get(MetaCache::keyFor(it)), meta);
        // Append the DAT dump-verification badge (issue #97) here, at the single emit choke point, so it rides
        // along no matter which source (gamelist / cache / online aggregator) assembled the facts list.
        const QVariant dump = dumpStatusFact(it);
        if (dump.isValid())
        {
            QVariantList facts = meta.value(QStringLiteral("facts")).toList();
            facts << dump;
            meta.insert(QStringLiteral("facts"), facts);
        }
    }
    emit themedMetaReady(idx, meta);
}

// The local file behind a ROM item, or empty for a non-local (streaming) item. Accepts a file:// URL or a bare
// path; refuses http(s).
static QString localRomPathFor(const MediaItem& it)
{
    QString u = it.url;
    if (u.isEmpty()) return QString();
    if (u.startsWith(QStringLiteral("file:"))) u = QUrl(u).toLocalFile();
    else if (u.startsWith(QStringLiteral("http"))) return QString();
    const QFileInfo fi(u);
    return (fi.exists() && fi.isFile()) ? fi.absoluteFilePath() : QString();
}

QVariant HomeView::dumpStatusFact(const MediaItem& it)
{
    if (it.type != QStringLiteral("game") || !Settings::verifyRoms()) return QVariant();
    const QString path = localRomPathFor(it);
    if (path.isEmpty()) return QVariant();

    const HashVerify::Stamp st = HashVerify::cachedStamp(path);
    if (!st.valid)
    {
        scheduleRomVerify(it, path); // compute once in the background; it re-requests the panel when done
        return QVariant();           // nothing to show yet — Unknown is neutral, so no premature badge
    }
    QString value;
    switch (st.status)
    {
        case HashVerify::Status::Verified: value = tr("Verified"); break;
        case HashVerify::Status::Bad:      value = tr("Bad dump"); break;
        default:                           value = tr("Unknown");  break;
    }
    return QVariantMap{ { QStringLiteral("label"), tr("Dump") }, { QStringLiteral("value"), value } };
}

void HomeView::scheduleRomVerify(const MediaItem& it, const QString& romPath)
{
    if (romVerifyInFlight_.contains(romPath)) return; // already hashing this one
    romVerifyInFlight_.insert(romPath);
    const QString systemHint = it.systemHint;
    QPointer<HomeView> self(this);
    // Hashing a multi-GB ISO must never block the UI, so it rides a worker thread (Qt6::Core's global pool);
    // the result is cached by path+mtime, so this cost is paid once per ROM, then it's a cheap ini read.
    QThreadPool::globalInstance()->start([self, romPath, systemHint]() {
        // A zipped ROM is hashed from its EXTRACTED stream (ArchiveRom), but the stamp stays keyed on the
        // archive the user sees. Non-archives hash in place.
        QString hashSource;
        if (ArchiveRom::isArchive(romPath))
        {
            const QString tmp = ArchiveRom::extractToTemp(romPath);
            if (!tmp.isEmpty()) hashSource = tmp;
        }
        const HashVerify::DatDb db = HashVerify::parseDatDir(HashVerify::datsDir());
        // No DATs at all: there is nothing to verify against. Don't stamp (leave it to try again once the user
        // adds a DAT), just clear the in-flight guard.
        if (!db.isEmpty())
            HashVerify::verifyAndCache(romPath, systemHint, db, hashSource);
        if (!self) return;
        QMetaObject::invokeMethod(self, [self, romPath]() {
            if (!self) return;
            self->romVerifyInFlight_.remove(romPath);
            // If the just-verified ROM is still the selected row, re-request its panel so the badge appears.
            const int idx = self->themedMetaIndex_;
            if (idx >= 0 && idx < self->browseRowMap_.size()
                && localRomPathFor(self->items_[self->browseRowMap_[idx]]) == romPath)
                self->requestThemedMeta(idx);
        }, Qt::QueuedConnection);
    });
}

void HomeView::ensureMiximageAsync(const QString& metaKey, int idx)
{
    const Miximage::ComposePlan plan = Miximage::planForKey(metaKey); // GUI thread: MetaCache reads + cheap stats
    if (!plan.viable || plan.fresh || miximageInFlight_.contains(metaKey)) return;
    miximageInFlight_.insert(metaKey);
    const QSize canvas = Miximage::defaultCanvas(); // ini read — stays on the GUI thread
    QPointer<HomeView> self(this);
    QThreadPool::globalInstance()->start([self, plan, canvas, metaKey, idx] {
        const bool made = Miximage::composeAndSave(plan, canvas); // pure image work + QFile: worker-safe
        if (!self) return;
        QMetaObject::invokeMethod(self, [self, made, metaKey, idx] {
            if (!self) return;
            self->miximageInFlight_.remove(metaKey);
            if (!made) return;
            MetaCache::recordLocalImage(metaKey, QStringLiteral("miximage"), QStringLiteral("miximage.png"));
            self->themedArtCache_.remove(metaKey); // the session cache resolved without the card; re-resolve
            if (idx >= 0 && self->themedMetaIndex_ == idx)
                self->requestThemedMeta(idx);      // the row is still selected: refresh its panel with the card
        }, Qt::QueuedConnection);
    });
}

void HomeView::requestThemedMeta(int idx)
{
    if (idx < 0 || idx >= browseRowMap_.size() || stack_.isEmpty()) return;
    const MediaItem& it = items_[browseRowMap_[idx]];
    themedMetaIndex_ = idx;
    // Every selection change tells the scrape aggregator the user is live: its background prefetch yields
    // (finishing jobs write art/detail files on the GUI thread — sporadic hitches under a scrolling user).
    if (gameAgg_) gameAgg_->noteUserActivity();
    // A skeleton from what the catalog row already carries, shown at once; the addon /meta enriches it below.
    QVariantMap base;
    const QString metaKey = MetaCache::keyFor(it);
    base.insert(QStringLiteral("index"), idx);
    base.insert(QStringLiteral("title"), it.title);
    base.insert(QStringLiteral("subtitle"), it.subtitle);
    // Offline-first, and persist-on-hover: the meta panel's hero falls back to this image for items with no
    // richer art (consoles), so serve the cached local copy when present — and quietly cache it the first
    // time it's seen (async, idempotent, no-op for non-http), so a console's art keeps rendering after its
    // remote URL dies instead of leaving the hero black.
    { PERF_SPAN("nav.cacheImage");
      MetaCache::cacheImage(metaKey, QStringLiteral("thumb"), it.thumbnailUrl);
      base.insert(QStringLiteral("image"), MetaCache::displayImage(metaKey, it.thumbnailUrl)); }
    base.insert(QStringLiteral("type"), it.type);
    base.insert(QStringLiteral("accent"), typeColor(it.type).name()); // hero fallback tint when no art loads
    base.insert(QStringLiteral("expandable"), it.expandable);
    base.insert(QStringLiteral("favorite"), FavoritesStore::isFavorite(it.id));
    // Resolve the rich art + facts for the panel in PRIORITY ORDER, so scrolling never re-does work and the
    // ROMs folder's own data is used before the network:
    //   1) this session's page cache (already resolved while scrolling this console)
    //   2) the ROM system's gamelist.xml (EmulationStation / RetroBat data sitting in the ROMs folder)
    //   3) our own scrape cache (MetaCache)
    // Only if none of those have it do we scrape online (the aggregator, below) — gated on `resolvedRich`.
    bool resolvedRich = false;
    const auto cachedRich = themedArtCache_.constFind(metaKey);
    if (cachedRich != themedArtCache_.constEnd())
    {
        for (auto kv = cachedRich->constBegin(); kv != cachedRich->constEnd(); ++kv) base.insert(kv.key(), kv.value());
        resolvedRich = true;
    }
    else
    {
        QVariantMap rich;            // the resolved enrichment we cache per item for instant scroll-back
        MediaArt art = it.art;       // the catalog row's own art (a thumbnail) is the floor
        if (it.type == QStringLiteral("game"))
        {
            PERF_SPAN("nav.gamelist");
            const MediaDetail gl = GamelistStore::lookup(it.url); // the ROMs-folder gamelist.xml, first
            if (gl.valid)
            {
                resolvedRich = true;
                art.mergeLowerPriority(gl.art);
                if (!gl.overview.isEmpty()) rich.insert(QStringLiteral("overview"), gl.overview);
                if (!gl.subtitle.isEmpty()) rich.insert(QStringLiteral("subtitle"), gl.subtitle);
                QVariantList facts;
                for (const MediaFact& f : gl.facts)
                    facts << QVariantMap{ { QStringLiteral("label"), f.label }, { QStringLiteral("value"), f.value } };
                if (!facts.isEmpty()) rich.insert(QStringLiteral("facts"), facts);
            }
        }
        // Lazily composite this item's miximage card (issue #90) from the art already cached, so loadArt
        // surfaces the "miximage" role for a theme that prefers it. Generated on this first display request,
        // not as a batch job — and OFF the GUI thread: composing inline here was 150-418ms per nav.select
        // (the themed shelf's scroll hitch). The panel shows with the art below now; when the card lands the
        // helper refreshes this row if it's still selected. A fresh card costs one stat+stamp read (no-op).
        { PERF_SPAN("nav.mixplan"); ensureMiximageAsync(metaKey, idx); }
        { PERF_SPAN("nav.loadArt");
          const MediaArt scraped = MetaCache::loadArt(metaKey); // our own previously-scraped art backfills
          if (!scraped.isEmpty()) { art.mergeLowerPriority(scraped); resolvedRich = true; } }
        art.writeInto(rich);
        for (auto kv = rich.constBegin(); kv != rich.constEnd(); ++kv) base.insert(kv.key(), kv.value());
        if (resolvedRich) themedArtCache_.insert(metaKey, rich); // remember (and skip scraping this one)
    }
    // Play history rides on the subtitle line (beside the year), not the facts line — see Xmb.qml. Emitted
    // as its own fields so it shows straight away, even for a game with no addon /meta to enrich it below.
    {
        PERF_SPAN("nav.playstats");
        // Continue-watching, for the panel: a movie/episode you are part way through says so here, the same
        // way a game reports its play history. The row label already carries the bare percentage; this is the
        // "how much is left" half, which is what a Recents row is usually being asked.
        const QString watched = resumeSummary(resumeKeyFor(it));
        if (!watched.isEmpty()) base.insert(QStringLiteral("watched"), watched);
        const PlayStats::Stat ps = PlayStats::get(PlayStats::identity(it.id, QString()));
        if (ps.lastPlayed > 0)
        {
            base.insert(QStringLiteral("lastPlayed"), PlayStats::formatLastPlayed(ps.lastPlayed));
            if (ps.totalSeconds > 0)
                base.insert(QStringLiteral("timePlayed"), PlayStats::formatDuration(ps.totalSeconds));
        }
    }
    {
        PerfTrace::begin(QStringLiteral("nav.emit"));
        emitThemedMeta(idx, base);
        PerfTrace::end(QStringLiteral("nav.emit"), QStringLiteral("idx=%1").arg(idx)); // idx: one physical tap must show ONE new index
    }
    PerfTrace::end(QStringLiteral("nav.select"));
    // The LOCAL art/facts above are resolved + shown instantly (no debounce), so scrolling over cached /
    // gamelist rows shows the logo + metadata immediately with no plain-title flash. Only the NETWORK half
    // (online scrape + achievements + the catalog addon's /meta) is deferred to enrichThemedMeta(), fired on
    // the host's settle debounce so scrolling doesn't hit the network for every row.
    themedResolvedRich_ = resolvedRich;
}

void HomeView::enrichThemedMeta()
{
    const int idx = themedMetaIndex_;
    if (idx < 0 || idx >= browseRowMap_.size() || stack_.isEmpty()) return;
    const MediaItem& it = items_[browseRowMap_[idx]];
    const QString metaKey = MetaCache::keyFor(it);
    const bool resolvedRich = themedResolvedRich_;

    // Achievements for a game -> the live panel (earned first, so they highlight "at the front"). Retro
    // consoles use RetroAchievements; a PC game uses Steam (its schema + the local emulator's unlock file).
    // Async + best-effort; a stale result is dropped by the host's currentIndex check.
    if (it.type == QStringLiteral("game"))
    {
        QString console;
        for (int i = stack_.size() - 1; i >= 0; --i)
            if (stack_[i].item.type == QStringLiteral("platform")) { console = stack_[i].item.title.trimmed(); break; }
        const GameSystem* sys = console.isEmpty() ? nullptr : SystemCatalog::forConsoleName(console);
        const unsigned cid = (sys && !sys->extensions.isEmpty())
                             ? Achievements::consoleIdForExtension(sys->extensions.first()) : 0u;
        const int reqIdx = idx;

        // Only scrape online when the ROMs folder / our cache didn't already have it (resolvedRich). The
        // aggregator scrapes at high priority across the configured providers and merges the best of each into
        // the panel; it caches the merged result (+ writes it back to the gamelist when "keep scraped data" is
        // on) so re-hover is instant. The console's other missing games are prefetched in the background.
        if (!gameAgg_) gameAgg_ = new GameMetaAggregator(mgr_, this);
        if (!resolvedRich && gameAgg_->hasProviders())
        {
            MediaItem q;
            q.id = it.id; q.title = it.title; q.type = QStringLiteral("game");
            q.url = it.url; q.systemHint = it.systemHint; q.altNames = it.altNames; // url -> gamelist write-back
            const QString cacheKey = metaKey;
            gameAgg_->request(q, console, [this, reqIdx, cacheKey](const MediaDetail& d) {
                if (!d.valid && d.art.isEmpty()) return;
                QVariantMap m;
                if (!d.overview.isEmpty()) m.insert(QStringLiteral("overview"), d.overview);
                if (!d.subtitle.isEmpty()) m.insert(QStringLiteral("subtitle"), d.subtitle);
                QVariantList facts;
                for (const MediaFact& f : d.facts)
                    facts << QVariantMap{ { QStringLiteral("label"), f.label }, { QStringLiteral("value"), f.value } };
                if (!facts.isEmpty()) m.insert(QStringLiteral("facts"), facts);
                d.art.writeInto(m); // logo/box/hero/screenshots/videos/audio/meta -> the panel bindings
                // Cached as the SCRAPER gave it: emitThemedMeta composites the correction on the way out, so
                // this entry stays valid across an edit and a reset both.
                themedArtCache_.insert(cacheKey, m); // remember for scroll-back (no re-scrape)
                if (reqIdx != themedMetaIndex_) return; // selection moved on; cached above, just don't emit now
                emitThemedMeta(reqIdx, m);
            });
        }
        // Publish a list of { title, icon(full URL), earned } into the panel.
        auto publish = [this, reqIdx](const QVariantList& arr, int earned) {
            if (arr.isEmpty()) return;
            QVariantMap m;
            m.insert(QStringLiteral("achievements"), arr);
            m.insert(QStringLiteral("achEarned"), earned);
            m.insert(QStringLiteral("achTotal"), int(arr.size()));
            emitThemedMeta(reqIdx, m);
        };

        if (cid && RaBrowse::configured())
        {
            if (!raBrowse_) raBrowse_ = new RaBrowse(this);
            raBrowse_->fetch(it.title, cid, [publish](const QList<RaBrowse::Ach>& list) {
                QVariantList arr; int earned = 0;
                for (const RaBrowse::Ach& a : list)
                {
                    if (a.earned) ++earned;
                    arr << QVariantMap{ { QStringLiteral("title"), a.title },
                                        { QStringLiteral("icon"), QStringLiteral("https://media.retroachievements.org/Badge/") + a.badge + QStringLiteral(".png") },
                                        { QStringLiteral("earned"), a.earned } };
                }
                publish(arr, earned);
            });
        }
        else if (cid == 0 && SteamAchievements::configured())
        {
            const QString cl = console.toLower();
            const bool pc = cl == QStringLiteral("pc") || cl == QStringLiteral("windows")
                         || cl.startsWith(QStringLiteral("pc (")) || cl.startsWith(QStringLiteral("pc windows"));
            const PcGameStore::Entry e = PcGameStore::get(it.id);
            const QString gameDir = !e.exe.isEmpty() ? QFileInfo(e.exe).absolutePath() : e.dir;
            if (pc && !gameDir.isEmpty())
            {
                if (!steamAch_) steamAch_ = new SteamAchievements(this);
                steamAch_->fetch(it.title, gameDir, [publish](const QList<SteamAchievements::Ach>& list) {
                    QVariantList arr; int earned = 0;
                    for (const SteamAchievements::Ach& a : list)
                    {
                        if (a.earned) ++earned;
                        arr << QVariantMap{ { QStringLiteral("title"), a.title },
                                            { QStringLiteral("icon"), a.icon },
                                            { QStringLiteral("earned"), a.earned } };
                    }
                    publish(arr, earned);
                });
            }
        }
    }

    if (!stack_.last().addon) { themedMetaReq_ = -1; return; }
    themedMetaReqIndex_ = idx;                                   // J09: remember which row this /meta is for
    themedMetaReq_ = mgr_->requestMeta(stack_.last().addon, it); // -> onMetaReady (themed branch) enriches
}

// The themed DETAIL view asking for the ONE thing it cannot resolve locally: a bridged leaf's Stremio stream
// id, which only /meta carries (MediaItem::fromJson never parses one — MediaDetail does). The XMB gets this
// for free from its hover debounce (refreshThemedMeta -> requestThemedMeta + enrichThemedMeta); the GRID
// browse — the default browse path — has no hover fetch at all, so without this the detail's verb row stays
// frozen on what the raw catalog row could say and "Choose source…" never appears there.
//
// Deliberately narrow: only a leaf that could bridge and hasn't yet. Everything else the detail shows is
// already resolved locally, so firing a network /meta on every detail open would buy nothing.
void HomeView::requestThemedDetailMeta(int idx)
{
    if (!mgr_ || idx < 0 || idx >= browseRowMap_.size() || stack_.isEmpty() || !stack_.last().addon) return;
    const MediaItem& it = items_[browseRowMap_[idx]];
    if (it.expandable || !it.imdbStreamId.isEmpty()) return;
    // A GAME leaf's detail page enriches through the SAME machinery as the XMB hover panel (gamelist /
    // MetaCache instantly, then the aggregator's online scrape + the addon's /meta) — kBridgeable below is
    // the Stremio /meta bridge, which knows nothing about games, so game detail pages showed only the bare
    // catalog row (no overview, facts or box art). Both emit themedMetaReady, which MainWindow merges into
    // detailData while this page is open; no debounce here — the page is open NOW.
    if (it.type == QStringLiteral("game"))
    {
        requestThemedMeta(idx);     // instant: cached art / gamelist facts (also arms themedMetaIndex_)
        enrichThemedMeta();         // network: aggregator scrape + achievements
        return;
    }
    static const QSet<QString> kBridgeable = {
        QStringLiteral("movie"), QStringLiteral("series"), QStringLiteral("tv"), QStringLiteral("episode") };
    if (!kBridgeable.contains(it.type)) return;
    themedMetaIndex_    = idx;   // the staleness key onMetaReady's themed branch compares the reply against
    themedMetaReqIndex_ = idx;
    themedMetaReq_ = mgr_->requestMeta(stack_.last().addon, it);
}

// A metadata-only catalog row carries no stream id of its own; the bridge produces one when /meta lands.
// Stamp it onto the STORED row so every later read — the action-row gates and, crucially, the item
// requestChooseSource emits — sees it. (The classic page keeps the same value in playImdbId_ and stamps it
// on at emit time; the themed row has no equivalent stash, and emitting without it would send the addon's
// private catalog id to /stream.) Returns true when the id newly landed.
bool HomeView::bridgeStreamId(int idx, const QString& streamId)
{
    if (streamId.isEmpty() || idx < 0 || idx >= browseRowMap_.size()) return false;
    MediaItem& row = items_[browseRowMap_[idx]];
    if (!row.imdbStreamId.isEmpty()) return false;
    row.imdbStreamId = streamId;
    return true;
}

// A themed leaf that is a local game file (a console Recent/Downloaded/Favorites row) favourites by
// path+console like the game action menu does; its identity is gameFavId (stable key, else path), not it.id.
static bool isLocalGameLeaf(const MediaItem& it)
{
    // A MERGED PC game counts even though it has no url: it is a game that lives on THIS machine, it belongs
    // to the PC console, and starring it must stamp the same system the console's ★ Favorites folder filters
    // on. Without this the themed star writes a system-less record and the game the user just starred does
    // not appear in that folder, while the identical star pressed on its info page (which routes through
    // localGameFavorite) does — two surfaces disagreeing about one action.
    if (isMergedPcGame(it)) return true;
    return (it.mime == QStringLiteral("game") || it.mime == QStringLiteral("pcgame")) && !it.url.isEmpty();
}

bool HomeView::isThemedLeafFavorite(int idx) const
{
    if (idx < 0 || idx >= browseRowMap_.size()) return false;
    const MediaItem& it = items_[browseRowMap_[idx]];
    return FavoritesStore::isFavorite(isLocalGameLeaf(it) ? gameFavId(it) : it.id);
}

void HomeView::favoriteThemedLeaf(int idx)
{
    if (idx < 0 || idx >= browseRowMap_.size() || stack_.isEmpty()) return;
    const MediaItem it = items_[browseRowMap_[idx]]; // copy: toggleGameFavorite repopulates items_
    if (isLocalGameLeaf(it))
    {
        // A local game: same path/kind/system stamping (and level refresh) as the game action menu, so the
        // favourite lands in the console's ★ Favorites folder instead of a system-less orphan entry.
        toggleGameFavorite(it);
    }
    else if (it.type == QStringLiteral("livetv"))
    {
        // A LIVE TV CHANNEL, THE THEMED HALF OF toggleLiveTvChannelFavorite (#203). It is called out here for
        // the reason the local-game arm above is: the generic row below stamps neither `path` nor `kind`, and
        // a channel favourite needs both — openFavorite re-opens a row by its path, and `kind` is what routes
        // it. Without this the star worked, the row appeared on Home, and pressing it said the favourite's
        // source addon was missing. The two surfaces are two code paths; this is the one that was silent.
        if (FavoritesStore::isFavorite(it.id)) FavoritesStore::remove(it.id);
        else
        {
            bool built = false;
            const QVector<QString> ids = browse::liveTvChannelIds(liveTvEntries_);
            for (int i = 0; i < ids.size(); ++i)
                if (ids.at(i) == it.id)
                { FavoritesStore::add(browse::liveTvChannelFavorite(liveTvEntries_, i)); built = true; break; }
            if (!built)
            {
                FavoriteItem f;
                f.itemId = it.id; f.title = it.title; f.subtitle = it.subtitle;
                f.type = QStringLiteral("livetv"); f.thumbnailUrl = it.thumbnailUrl;
                f.path = it.id;                    // the identity, never the url — resolved at open
                f.kind = QStringLiteral("livetv");
                FavoritesStore::add(f);
            }
        }
    }
    else if (FavoritesStore::isFavorite(it.id)) FavoritesStore::remove(it.id);
    else
    {
        FavoriteItem f;
        f.addonId = stack_.last().addon ? stack_.last().addon->manifest.id : QString();
        f.itemId = it.id; f.title = it.title; f.subtitle = it.subtitle;
        f.type = it.type; f.thumbnailUrl = it.thumbnailUrl; f.expandable = it.expandable;
        FavoritesStore::add(f);
    }
    // Nudge the live panel so its heart reflects the new state.
    QVariantMap m;
    m.insert(QStringLiteral("favorite"),
             FavoritesStore::isFavorite(isLocalGameLeaf(it) ? gameFavId(it) : it.id));
    emitThemedMeta(idx, m);
}

// The per-profile marks key for the browse-item at `idx` — the SAME MetaCache::keyFor the hidden filter and
// the detail hide/status/tags verbs use, so a hide here matches the filter that drops it from the rows.
QString HomeView::themedLeafKey(int idx) const
{
    if (idx < 0 || idx >= browseRowMap_.size()) return QString();
    return MetaCache::keyFor(items_[browseRowMap_[idx]]);
}

// Re-apply the hidden filter after Show-hidden / a profile switch flipped it. The Home list rebuilds in place
// (renderRecents re-reads the stores and re-runs the filter); a catalogue level re-issues its request so the
// filter runs in populate() as the fresh items land. browseItemsChanged re-syncs any themed browse mirror.
void HomeView::reloadForFilterChange()
{
    if (recentView_) { renderRecents(); emit browseItemsChanged(false); return; }
    if (!stack_.isEmpty()) loadTop();
}

// A non-expandable info-page leaf (movie/series/book/comic/…): the themed grid browse opens the themed detail
// view for it (replacing the classic info page) instead of drilling. Games/tracks are direct-open, not this.
bool HomeView::isThemedInfoLeaf(int idx) const
{
    if (idx < 0 || idx >= browseRowMap_.size()) return false;
    const MediaItem& it = items_[browseRowMap_[idx]];
    if (it.expandable) return false;
    if (isInfoPageType(it.type)) return true;
    // A stream-less GAME leaf (an IGDB catalog row: no url, no local file) would otherwise fall through
    // browseActivate -> openResolvedItem -> openDetailLevel, which pushes a level with no children and no
    // classic page — the themed browse renders that as an EMPTY grid ("I clicked a game and nothing came
    // up"). Its verbs (Download / Favorite / Playlist) live on the themed detail page, so open that.
    // Local/downloaded game rows carry a url (or a localgame: mime) and keep their direct action-menu path.
    if (it.type == QStringLiteral("game") && it.url.isEmpty() && it.mime.isEmpty()) return true;
    // A MERGED PC game is the same shape wearing a mime: no url (which copy runs is decided by the source
    // picker at Play time) and mime "pcgame", so the test above misses it and activating a tile drilled into
    // a level with no children — an empty column. Its verbs all live on the themed detail page (Play,
    // Favorite, Playlist, and now "Fix this entry…"), which is exactly the classic path's behaviour: a
    // merged PC game's classic route is openDetailLevel too. This is what makes the merge override reachable
    // by D-pad in the default themed layout (issue #44).
    if (isMergedPcGame(it)) return true;
    return false;
}

// The themed detail view's data for the browse-item at `idx`: rich art + facts resolved from the SAME local
// sources requestThemedMeta uses (this session's page cache, the ROMs-folder gamelist.xml, then MetaCache) so
// opening detail never re-does work, plus a joined factsText and the action-row verb list. The detail elements
// bind this through dataCtx.selected. Returns an empty map for a divider/synthetic row (not a media item).
// Resolve a game leaf to its SystemCatalog system WITHOUT extracting an archive — the cheap half of
// GameLauncher::prepareCore's resolution (systemHint id/console, else the file extension). Used to gate the
// "Launch options…" detail action (issue #51): overrides only make sense for a game that resolves to a system
// with candidate cores or a standalone emulator, so a metadata-only game entry (no local file, no system)
// gets no pill and the launchopts editor is never reachable with nothing to edit.
static const GameSystem* systemForGameItem(const MediaItem& it)
{
    if (it.type != QStringLiteral("game")) return nullptr;
    const GameSystem* sys = nullptr;
    if (!it.systemHint.isEmpty())
    {
        sys = SystemCatalog::byId(it.systemHint);
        if (!sys) sys = SystemCatalog::forConsoleName(it.systemHint);
    }
    if (!sys)
    {
        const QString ext = QFileInfo(it.url).suffix().toLower();
        if (!ext.isEmpty()) sys = SystemCatalog::forExtension(ext);
    }
    // Only offer overrides where there is something to override: candidate cores (libretro) or a standalone
    // emulator. A system with neither has no lever the store could set.
    if (sys && (sys->cores.isEmpty() && sys->externalEmulator.isEmpty())) return nullptr;
    return sys;
}

// The resolved system id for a themed game leaf (empty when it isn't an override-capable game) — the dispatch
// side of the launchopts detail action reads this to build the editor's candidate lists.
QString HomeView::themedLeafSystemId(int idx) const
{
    if (idx < 0 || idx >= browseRowMap_.size()) return QString();
    const GameSystem* sys = systemForGameItem(items_[browseRowMap_[idx]]);
    return sys ? sys->id : QString();
}

QString HomeView::themedLeafGamePath(int idx) const
{
    if (idx < 0 || idx >= browseRowMap_.size()) return QString();
    const MediaItem& it = items_[browseRowMap_[idx]];
    return it.type == QStringLiteral("game") ? it.url : QString();
}

bool HomeView::themedLeafIsGame(int idx) const
{
    if (idx < 0 || idx >= browseRowMap_.size()) return false;
    return items_[browseRowMap_[idx]].type == QStringLiteral("game");
}

// The system id of the current browse LEVEL, when it is a single-console folder. Two carriers, both in the same
// id space SystemCatalog uses: a drilled-into console/platform level names the console in its title (resolved via
// forConsoleName, the authoritative rule gameFactsFor already relies on); a synthetic per-console Favorites /
// Recent / Downloaded level names the system in its mime marker ("favorites:<sys>", "recents:<kind>|<sys>",
// "downloads:<kind>|<sys>"). Empty for a multi-type level, a non-console folder, or the root.
QString HomeView::currentLevelSystemId() const
{
    if (stack_.isEmpty()) return QString();
    const Level& top = stack_.last();

    // A drilled-into console/platform folder: its title IS the console name.
    if (top.item.type == QStringLiteral("platform"))
    {
        const QString cn = top.item.title.trimmed();
        if (const GameSystem* s = SystemCatalog::forConsoleName(cn)) return s->id;
        if (const GameSystem* s = SystemCatalog::byId(cn)) return s->id;
        return QString();
    }

    // A synthetic per-console shelf carries the system in the marker after its "<prefix>:" (and after the optional
    // "<kind>|" the Recent/Downloaded markers prepend). Resolve it as an id first, then as a console name.
    auto systemFromMarker = [](QString marker) -> QString {
        const int bar = marker.indexOf(QLatin1Char('|'));
        if (bar >= 0) marker = marker.mid(bar + 1);   // "<kind>|<sys>" -> "<sys>"
        marker = marker.trimmed();
        if (marker.isEmpty()) return QString();
        if (const GameSystem* s = SystemCatalog::byId(marker)) return s->id;
        if (const GameSystem* s = SystemCatalog::forConsoleName(marker)) return s->id;
        return QString();
    };
    const QString mime = top.item.mime;
    for (const QString& prefix : { QStringLiteral("favorites:"), QStringLiteral("recents:"),
                                   QStringLiteral("downloads:") })
        if (mime.startsWith(prefix)) return systemFromMarker(mime.mid(prefix.size()));

    return QString();
}

QVariantMap HomeView::themedDetailData(int idx)
{
    QVariantMap out;
    if (idx < 0 || idx >= browseRowMap_.size() || stack_.isEmpty()) return out;
    const MediaItem& it = items_[browseRowMap_[idx]];
    if (it.type == QStringLiteral("rechdr") || it.type.startsWith(QLatin1Char('_'))) return out;

    const QString metaKey = MetaCache::keyFor(it);
    out.insert(QStringLiteral("title"), it.title);
    out.insert(QStringLiteral("subtitle"), it.subtitle);
    out.insert(QStringLiteral("type"), it.type);
    out.insert(QStringLiteral("accent"), typeColor(it.type).name());
    out.insert(QStringLiteral("expandable"), it.expandable);
    MetaCache::cacheImage(metaKey, QStringLiteral("thumb"), it.thumbnailUrl);
    out.insert(QStringLiteral("image"), MetaCache::displayImage(metaKey, it.thumbnailUrl));
    out.insert(QStringLiteral("favorite"), isThemedLeafFavorite(idx));

    QVariantList facts;
    const auto cachedRich = themedArtCache_.constFind(metaKey);
    if (cachedRich != themedArtCache_.constEnd())
    {
        for (auto kv = cachedRich->constBegin(); kv != cachedRich->constEnd(); ++kv) out.insert(kv.key(), kv.value());
        facts = out.value(QStringLiteral("facts")).toList();
    }
    else
    {
        MediaArt art = it.art;
        if (it.type == QStringLiteral("game"))
        {
            const MediaDetail gl = GamelistStore::lookup(it.url);
            if (gl.valid)
            {
                art.mergeLowerPriority(gl.art);
                if (!gl.overview.isEmpty()) out.insert(QStringLiteral("overview"), gl.overview);
                if (!gl.subtitle.isEmpty()) out.insert(QStringLiteral("subtitle"), gl.subtitle);
                for (const MediaFact& f : gl.facts)
                    facts << QVariantMap{ { QStringLiteral("label"), f.label }, { QStringLiteral("value"), f.value } };
            }
        }
        ensureMiximageAsync(metaKey, -1); // composite the card lazily OFF-thread (issue #90); see the twin above.
                                          // -1: the detail page doesn't hot-refresh — the card serves next open.
        const MediaArt scraped = MetaCache::loadArt(metaKey);
        if (!scraped.isEmpty()) art.mergeLowerPriority(scraped);
        art.writeInto(out);
        if (!facts.isEmpty()) out.insert(QStringLiteral("facts"), facts);
    }

    // EITHER path: our own scrape cache (MetaCache) backfills the overview/facts the quick sources didn't
    // carry. The session art cache (the branch above) is filled by requestThemedMeta with whatever resolved
    // LOCALLY on hover — for a game with no gamelist entry that's art only — so without this backfill a
    // hovered-then-opened item would show less than a cold-opened one (the XMB hover path hit exactly that).
    {
        const MediaDetail cd = MetaCache::cachedDetail(metaKey);
        if (cd.valid)
        {
            // A previously-cached /meta already carries the bridged stream id: adopt it now so an item whose
            // detail we have seen before offers "Choose source…" on the FIRST push, with no network at all.
            bridgeStreamId(idx, cd.imdbStreamId);
            if (!out.contains(QStringLiteral("overview")) && !cd.overview.isEmpty())
                out.insert(QStringLiteral("overview"), cd.overview);
            if (facts.isEmpty())
            {
                for (const MediaFact& f : cd.facts)
                    facts << QVariantMap{ { QStringLiteral("label"), f.label }, { QStringLiteral("value"), f.value } };
                if (!facts.isEmpty()) out.insert(QStringLiteral("facts"), facts);
            }
        }
    }

    // A single joined "Label: value  •  …" string for the detail view's facts text element. joinFactsText is
    // shared with the hover panel's merge (MainWindow's themedMetaReady) so both surfaces read identically.
    if (facts.isEmpty()) facts = out.value(QStringLiteral("facts")).toList();
    const QString factsText = joinFactsText(facts);
    if (!factsText.isEmpty()) out.insert(QStringLiteral("factsText"), factsText);

    // The action-row verbs: Play/Read and Download come from classicActionGates — the SAME predicate the
    // classic detail page's buttons use (one definition, no drift). Favourite + playlist are always offered,
    // matching the XMB inline chooser. Two themed-only adjustments the classic page never had to make:
    //   * direct-open leaves (a console ROM row, a Recents/Downloaded row — local files the themed surface
    //     plays via playThemedLeaf/openRecent) get "play" even though the classic gates, built for
    //     remote/bridged catalog leaves, don't claim them (those rows never showed the classic info page);
    //   * a file already on disk (that same set) is never offered "download" (it's already saved —
    //     downloadThemedLeaf would just toast).
    const ActionGates gates = classicActionGates(it);
    const bool localSaved = isLocalGameLeaf(it) || atRecentsLevel() || atDownloadsLevel();
    const bool directOpen = !it.expandable && (localSaved || it.type == QStringLiteral("game"));
    // A bridged (metadata-only) movie/episode whose stream id is now known DOES resolve — through the stream
    // add-ons, exactly as the classic page's showMeta reveal says. classicActionGates is evaluated on the raw
    // catalog row, which for a local-script catalog (AIO Catalog, the default Movies/TV shelf) claims neither
    // Play nor Download for a movie, so without this both verbs stay missing however often the row is
    // re-pushed. canChooseStreamSource is unchanged — the gate is not loosened, it is merely also consulted.
    const bool bridgedStream = canChooseStreamSource(it);
    QStringList verbs;
    if (gates.play || directOpen || bridgedStream) verbs << QStringLiteral("play");
    // "Choose source…" sits next to Play because it IS a play: the same resolve, with the release picked by
    // hand instead of by the auto rule. Offered only where several releases exist to choose between.
    if (bridgedStream) verbs << QStringLiteral("source");
    // "Fix this entry…" — the PC-game merge override (issue #44), next to Play for the same reason: it is
    // about THIS entry, and a wrongly merged tile can only be recognised while you are looking at it.
    if (isMergedPcGame(it)) verbs << QStringLiteral("pcfix");
    // "Romhacks…" — retro game leaves only. A hack patches one dump of one console game, so it means
    // nothing on a film and nothing on a PC game; retroSystemFor excludes both.
    if (!retroSystemFor(it, browseConsoleName()).isEmpty()) verbs << QStringLiteral("romhack");
    verbs << QStringLiteral("favorite");
    if (gates.download && !localSaved) verbs << QStringLiteral("download");
    verbs << QStringLiteral("playlist");
    // External-player one-off actions, only on leaves that resolve to VIDEO playback (audio/readers/games stay
    // built-in per spec) and only when the profile isn't restricted. The two pills have DISTINCT gates:
    //   * "Open in external player" — shown whenever a handoff target EXISTS at all (anyTarget(): a Custom path
    //     set OR a player detected), REGARDLESS of the default. This is the core one-off: it must appear even
    //     when the default is the built-in player (the Stremio hand-off case).
    //   * "Play with built-in player" — the alternative, shown only when the default IS an external player
    //     (available()), so you can override a single item back to built-in.
    static const QSet<QString> kVideoTypes = {
        QStringLiteral("movie"), QStringLiteral("series"), QStringLiteral("tv"),
        QStringLiteral("episode"), QStringLiteral("video"), QStringLiteral("link") };
    const bool audioish = it.type == QStringLiteral("audiobook") || it.type == QStringLiteral("audio")
                          || it.mime.toLower().startsWith(QStringLiteral("audio/"));
    const bool isVideoLeaf = (gates.play || directOpen) && !gates.readable && !audioish
                             && it.type != QStringLiteral("game")
                             && (kVideoTypes.contains(it.type) || it.mime.toLower().startsWith(QStringLiteral("video/")));
    if (isVideoLeaf && !ProfileStore::current().restricted)
    {
        if (ExternalPlayer::anyTarget()) verbs << QStringLiteral("external"); // one-off, any default
        if (ExternalPlayer::available()) verbs << QStringLiteral("builtin");  // alternative, default IS external
    }
    // Library-management verbs (hidden / completion status / tags) on any REAL media item — gated off the
    // synthetic folder/marker rows (type starting '_'), which carry no marks key. These act on the item's marks
    // (ItemMarks, per profile) via MainWindow's dispatch; `hidden`/`completion` below drive the pill labels.
    // Hidden is personal, not parental — offered on restricted profiles too (per spec).
    if (!it.type.startsWith(QLatin1Char('_')))
    {
        verbs << QStringLiteral("hide") << QStringLiteral("status") << QStringLiteral("tags");
        const ItemMarks::Marks marks = ItemMarks::get(metaKey);
        out.insert(QStringLiteral("hidden"), marks.hidden);
        // Stable completion token the action row maps to a label (mirrors ItemMarks' own token strings).
        QString comp = QStringLiteral("none");
        switch (marks.completion)
        {
            case ItemMarks::Completion::InProgress: comp = QStringLiteral("inProgress"); break;
            case ItemMarks::Completion::Finished:   comp = QStringLiteral("finished");   break;
            case ItemMarks::Completion::Abandoned:  comp = QStringLiteral("abandoned");  break;
            case ItemMarks::Completion::Planned:    comp = QStringLiteral("planned");    break;
            case ItemMarks::Completion::None:       break;
        }
        out.insert(QStringLiteral("completion"), comp);
        // "Fix info…" — the per-item metadata editor (issue #24). Offered on the same real-media rows as the
        // other library-management verbs: they share the requirement of a stable key to write against.
        verbs << QStringLiteral("editmeta");
        out.insert(QStringLiteral("edited"), MetaOverrides::has(metaKey)); // drives the pill's "(edited)" mark
        // "Select…" — enter bulk edit (issue #65): pick many of this level's items and apply one action
        // (favourite / hide / tag / reassign-system) to all of them. Offered on the same real-media rows as
        // the other library-management verbs; the host (MainWindow) runs the selection checklist + the loops.
        verbs << QStringLiteral("select");
    }
    // "Launch options…" (issue #51): per-game core / standalone emulator / extra args. Offered only on a game
    // that resolves to a system with something to override, so it never appears on a movie or a metadata-only
    // game entry. The host (MainWindow) opens a NavOverlay editor and writes LaunchOptionsStore.
    if (systemForGameItem(it)) verbs << QStringLiteral("launchopts");
    // "Other versions" (issue #50): reaches the region/revision variants that region-collapsing hid at scan
    // time, so a collapsed game's losers are never orphaned. Offered ONLY when collapsing is on AND this game
    // actually has sibling variants on disk (re-derived cheaply from its own folder) — otherwise the pill
    // would open an empty menu. Never shown on a metadata-only entry (systemForGameItem gates that with the
    // sibling scan, which needs a real local file).
    if (Settings::collapseRegionalDuplicates() && systemForGameItem(it)
        && !RomLibrary::otherRegionVersions(it.url).isEmpty())
        verbs << QStringLiteral("otherversions");
    out.insert(QStringLiteral("actions"), verbs);
    out.insert(QStringLiteral("readable"), gates.readable);
    // The correction composites LAST, over every source above — the session art cache, a gamelist entry and
    // the scrape cache all feed `out`, and only the finished map is common to all three.
    MetaOverrides::applyTo(MetaOverrides::get(metaKey), out);
    return out;
}

void HomeView::playThemedLeaf(int idx, int routeHint)
{
    if (idx < 0 || idx >= browseRowMap_.size() || stack_.isEmpty()) return;
    MediaItem it = items_[browseRowMap_[idx]]; // copy (async callbacks outlive items_)
    // A one-off external/built-in override rides the item through the async resolve chain, so a failed resolve
    // can't leak it onto a later play. (Local/recents leaves below resolve synchronously — MainWindow's
    // consume-once member covers them, so the hint is harmlessly ignored there.)
    it.playRouteHint = routeHint;
    // Synthetic Recent/Downloaded folders hold already-local files (no addon to resolve through) - re-open
    // them like the classic list does, instead of trying to resolve them as catalog items via resolvePlay.
    if (atRecentsLevel() || atDownloadsLevel())
    {
        // A merged PC game row has no url to re-open — it names a game, not a file (see playPcGame).
        if (isMergedPcGame(it)) { playPcGame(it); return; }
        if (!it.url.isEmpty()) emit openRecent(it.url, it.mime, resumeKeyFor(it), it.title, it.thumbnailUrl);
        return;
    }
    // A ROW THAT RE-OPENS BY ITS OWN RECORD — "localgame:<kind>", which playlistItemsCatalog stamps on every
    // entry that carries a path. Both classic entrances take this branch before anything else (activateItem
    // and openResolvedItem), and the chooser's Play did not: it fell through to the local-leaf table below,
    // which hands the value to mpv as a FILE. That was invisible while the value was a playable url and is
    // not any more (#203) — a playlist entry now names its track rather than a link, and only openRecent
    // knows how to turn a name back into something a player can open. Fifth outing for the asymmetry this
    // function's next comment already records for a music track (#74), a photo (#102), an OPDS book (#146)
    // and a Live TV channel.
    if (it.mime.startsWith(QStringLiteral("localgame:")))
    { emit openRecent(it.url, it.mime.mid(10), it.id, it.title, it.thumbnailUrl); return; }
    // A LOCAL LEAF — a row whose file this machine already has, which has no addon to resolve through. THE
    // SAME TABLE activateItem reads (browse::localLeafRoute), and the reason it is a table rather than a
    // list written out here: the themed XMB routes a media leaf's Enter through the inline action chooser,
    // and the chooser's Play lands HERE rather than in activateItem — so a kind this function did not know
    // about fell through to resolvePlay, which has no local branch, and answered "Nothing to play" for a row
    // the classic grid played perfectly. That happened to a music track (#74), a photo (#102) and an OPDS
    // book (#146) before the table existed. Adding a route means adding it to activateItem too; the
    // `themed local-leaf routing parity` gate fails the build if you don't.
    switch (const browse::LeafRoute lr = browse::localLeafRoute(it); lr.play)
    {
        case browse::LeafPlay::OpenFile:   emit openItem(it); return;
        case browse::LeafPlay::OpdsBook:   openOpdsBook(it); return;   // re-emits openItem with the auth header
        case browse::LeafPlay::MusicAlbum: emit playMusicAlbumRequested(lr.key, it.url); return;
        case browse::LeafPlay::AudiobookBook: emit playAudiobookRequested(lr.key, it.url, -1); return;
        case browse::LeafPlay::NotLocal:   break;                      // an addon's row: resolve it below
    }
    // Prefer-local: an owned catalog item plays its on-disk file directly, WITHOUT the meta-fetch/stream-
    // provider detour below (a metadata-only catalog otherwise dead-ends at "No stream source" though the file
    // is on disk). Mirrors resolvePlay's head + the classic Play route.
    {
        QString lp = LocalLibrary::index().localPathFor(it.id);
        if (lp.isEmpty() && !it.imdbStreamId.isEmpty())
            lp = LocalLibrary::index().localPathFor(it.imdbStreamId);
        if (!lp.isEmpty() && QFileInfo::exists(lp))
        {
            MediaItem local = it;
            local.url = lp;
            local.mime = QStringLiteral("local:video");
            emit openItem(local);
            return;
        }
    }
    // A row that ALREADY CARRIES its playable url and has no addon to resolve through. A Live TV channel is
    // the case that bit: its url IS the stream, so there is nothing to resolve and nothing to look up. The
    // classic surface plays these from the same generic check (activateItem, immediately after the table);
    // the themed surface had no equivalent, so every channel answered "Nothing to play" here while playing
    // perfectly in the grid.
    //
    // The local-leaf TABLE cannot cover this — these rows are not local, and localLeafRoute is by definition
    // about files this machine already has. That is also why the `themed local-leaf routing parity` gate did
    // not catch it: the asymmetry between the two surfaces was never in the table, it was in what each of
    // them does with everything the table declines.
    //
    // Placed AFTER prefer-local rather than in activateItem's position: an owned catalog item that also
    // carries a url must still play its on-disk copy, which is the entire point of the block above.
    if (!it.url.isEmpty())
    {
        emit openItem(it);
        return;
    }

    // The level's addon, ELSE the row's own — the same fallback activateItem does, and for the same reason:
    // a synthetic level carries no addon (it was not built by one), so a row that came from a server has to
    // name its own. Without this a homebrew leaf reached resolvePlay with a null addon, matched none of its
    // branches, and answered "Nothing to play" — the fourth outing for the asymmetry this function's comment
    // above already records for a music track (#74), a photo (#102) and an OPDS book (#146). Same shape every
    // time: activateItem learned something and the chooser's Play, which lands here instead, did not.
    LoadedAddon* addon = stack_.last().addon;
    if (!addon && mgr_ && !it.sourceAddonId.isEmpty()) addon = mgr_->sourceById(it.sourceAddonId);
    const QString parentTitle = stack_.last().item.title.trimmed(); // the level this leaf hangs under
    QString console;
    for (int i = stack_.size() - 1; i >= 0; --i)
        if (stack_[i].item.type == QStringLiteral("platform")) { console = stack_[i].item.title.trimmed(); break; }

    // A movie/episode from a metadata-only catalog (non-Stremio) needs its IMDB id from /meta before a stream
    // addon can resolve it - fetch that first, then resolvePlay() with the id (handled in onMetaReady).
    const bool needsImdb = addon && addon->transport != LoadedAddon::RemoteHttp && !it.expandable
        && (it.type == QStringLiteral("movie") || it.type == QStringLiteral("episode")
            || it.type == QStringLiteral("series") || it.type == QStringLiteral("tv"));
    if (needsImdb)
    {
        showToast(tr("Finding a stream for “%1”…").arg(it.title), 0);
        themedPlayAddon_ = addon; themedPlayItem_ = it; themedPlayConsole_ = console;
        themedPlayReq_ = mgr_->requestMeta(addon, it);
        return;
    }
    resolvePlay(addon, it, parentTitle, console, QString(), QString());
}

// "Download" chosen on a themed leaf: resolve its source and queue it for keeps (no playback), reusing the same
// crawl machinery as the detail-page Download button. An expandable row (a season/folder) downloads its contents.
void HomeView::downloadThemedLeaf(int idx)
{
    if (idx < 0 || idx >= browseRowMap_.size() || stack_.isEmpty()) return;
    const MediaItem it = items_[browseRowMap_[idx]];
    if (atRecentsLevel() || atDownloadsLevel()) { showToast(tr("“%1” is already saved.").arg(it.title), 4000); return; }
    if (dlBusy_) { showToast(tr("A download is already being prepared…"), kFeedbackLong); return; }

    DlNode node;
    // Level's addon else the row's own — same fallback as playThemedLeaf and activateItem. A synthetic level
    // carries no addon, so a row that came from a server names its own; without this, Download on such a row
    // crawls with a null addon and finds nothing to fetch.
    node.addon = stack_.last().addon;
    if (!node.addon && mgr_ && !it.sourceAddonId.isEmpty()) node.addon = mgr_->sourceById(it.sourceAddonId);
    node.item = it;
    node.parentTitle = stack_.last().item.title;               // the level this leaf hangs under
    node.parentType  = stack_.last().item.type;
    // The download crawl derives a game's console from parentType == "platform"; find the nearest platform level.
    for (int i = stack_.size() - 1; i >= 0; --i)
        if (stack_[i].item.type == QStringLiteral("platform"))
        { node.parentTitle = stack_[i].item.title; node.parentType = QStringLiteral("platform"); break; }

    dlQueue_.clear();
    dlQueue_.append(node);
    dlQueued_ = 0;
    dlBusy_ = true;
    showToast(it.expandable ? tr("Preparing downloads for “%1”…").arg(it.title)
                            : tr("Preparing download for “%1”…").arg(it.title), 0);
    dlNext();
}

// The classic detail page's action-visibility rules, extracted verbatim from requestMeta so the themed detail
// action row shares the ONE definition. `item` is resolved against the CURRENT drill level's addon
// (stack_.last().addon) — the same context both callers evaluate in.
HomeView::ActionGates HomeView::classicActionGates(const MediaItem& item) const
{
    ActionGates g;
    // A store game launched by URI (steam:// / com.epicgames.launcher:// / battlenet://) carries no url and no
    // addon — the launch is a client handoff in openLibraryItem, so Play must be offered on its mime alone.
    // A merged PC game is the same shape: no url, no addon, and the launch is decided by the source picker at
    // Play time — so Play has to be offered on its identity alone or its info page has no way to start it.
    const bool isStoreLaunch = (item.mime == QStringLiteral("steamgame"))
                            || (item.mime == QStringLiteral("epicgame"))
                            || (item.mime == QStringLiteral("battlenetgame"))
                            || isMergedPcGame(item);
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
    g.readable = isReadableChapter(item.type) || isRemoteReadable || isBridgedReadable;
    // Owned local file: offer Play even when no stream/debrid provider can resolve this catalog item, so the
    // on-disk copy plays from its tile. Use localPathFor (the Seam B short-circuit's own precondition), NOT
    // ownsId — ownsId is true for a series CONTAINER (owns episodes), which has no directly-playable file.
    const bool ownedPlayable =
           !LocalLibrary::index().localPathFor(item.id).isEmpty()
        || (!item.imdbStreamId.isEmpty() && !LocalLibrary::index().localPathFor(item.imdbStreamId).isEmpty());
    g.play = isStoreLaunch || isRemotePlayable || g.readable || isBridgedAudio || isBridgedGame || ownedPlayable;
    // Downloadable: a resolvable leaf (anything but a Steam launch or a page-based manga chapter), or a
    // container we can crawl (a series/season -> episodes, a comic volume -> issues).
    const bool dlLeaf = !item.expandable
        && (isRemotePlayable || isRemoteReadable || isBridgedReadable || isBridgedAudio || isBridgedGame);
    const bool dlContainer = item.expandable
        && (item.type == QStringLiteral("series") || item.type == QStringLiteral("tv")
            || item.type == QStringLiteral("season") || item.type == QStringLiteral("comic"));
    g.download = dlLeaf || dlContainer;
    return g;
}

bool HomeView::canChooseStreamSource(const MediaItem& item) const
{
    if (item.expandable) return false;                       // a container resolves nothing itself
    // Prefer-local wins over every resolve (resolvePlay and playThemedLeaf both short-circuit on it), so an
    // owned item never reaches a stream addon and has no list of releases behind it.
    if (!LocalLibrary::index().localPathFor(item.id).isEmpty()) return false;
    if (!item.imdbStreamId.isEmpty() && !LocalLibrary::index().localPathFor(item.imdbStreamId).isEmpty())
        return false;
    // The route the play would actually take:
    //   * a leaf of a Stremio catalog — resolveStream() hands it straight to resolveStremioStream();
    //   * a leaf bridged to an IMDB stream id — resolveStreamByImdb() ends at the same aggregation.
    // Anything else (a local script catalog, a file provider's own leaf, a game, a document) has one source.
    const bool stremioLeaf = !stack_.isEmpty() && stack_.last().addon && stack_.last().addon->stremio;
    const bool bridged = !item.imdbStreamId.isEmpty();
    if (!stremioLeaf && !bridged) return false;
    static const QSet<QString> kStreamable = {
        QStringLiteral("movie"), QStringLiteral("series"), QStringLiteral("tv"), QStringLiteral("episode") };
    if (!kStreamable.contains(item.type)) return false;
    const QString streamType = (item.type == QStringLiteral("movie")) ? QStringLiteral("movie")
                                                                      : QStringLiteral("series");
    return mgr_ && mgr_->hasStreamProvider(streamType);
}

void HomeView::setChooseSourceBusy(bool busy)
{
    if (sourceBtn_) sourceBtn_->setEnabled(!busy);
}

void HomeView::requestChooseSource(int idx)
{
    if (idx < 0 || idx >= browseRowMap_.size() || stack_.isEmpty()) return;
    emit chooseSourceRequested(items_[browseRowMap_[idx]]);
}

void HomeView::noteRomhackTarget(const MediaItem& it, LoadedAddon* addon, const QString& systemId) const
{
    // Shaped as a crawl node NOW, while the stack still says where we are standing: the crawl needs the addon
    // and the console, and both are properties of the page the verb was pressed on, not of the item. Built
    // exactly as downloadThemedLeaf builds one, because it is handed to exactly that crawl.
    romhackNode_ = DlNode{};
    romhackNode_.addon = addon;
    romhackNode_.item = it;
    // `systemId` is the system the verb was OFFERED on, and it rides along because the offer reads signals
    // the stack does not have: reached from Recents or a search result there is no platform level at all, and
    // the item's own hint is the only thing that knows the console. See browse/RomhackTarget.h.
    QVector<browse::CrawlLevel> levels;
    levels.reserve(stack_.size());
    for (const Level& l : stack_) levels.push_back({ l.item.title, l.item.type });
    const browse::CrawlParent parent = browse::romhackCrawlParent(levels, systemId);
    romhackNode_.parentTitle = parent.title;
    romhackNode_.parentType = parent.type;
}

// Run the remembered leaf through the ORDINARY download crawl — the same one the Download verb uses, so a base
// ROM fetched for a hack is an ordinary download in every visible way: same resolution, same queue, same
// progress, same place to cancel it.
//
// The crawl, and not a direct stream resolve: a game leaf on a console page carries a METADATA id (an igdb:
// one, say), which no ROM source can resolve. The crawl is what knows to fall back to searching the game's
// title plus its console — which is the only thing that finds the ROM.
//
// That used to be true of the LOCAL bridge only, and this comment asserted it of the crawl as a whole. A
// remote (http) source went down dlResolveLeaf's other path, which asked for the leaf's /stream by id and, on
// the empty answer a metadata id earns, emitted nothing and moved on. So the console reached the crawl (the
// fix above this one) and the crawl still came up empty. Both paths now search; see the RemoteHttp branch of
// dlResolveLeaf and browse/RemoteLeafResolve.h.
void HomeView::startRomhackBaseDownload(std::function<void(bool started)> done)
{
    if (!mgr_ || !romhackNode_.addon || romhackNode_.item.title.trimmed().isEmpty())
    { if (done) done(false); return; }
    // One crawl at a time is the existing rule; a second would clear the first's queue out from under it.
    if (dlBusy_) { if (done) done(false); return; }

    dlQueue_.clear();
    dlQueue_.append(romhackNode_);
    dlQueued_ = 0;
    dlBusy_ = true;
    dlDone_ = std::move(done);
    dlNext();
}

QString HomeView::browseConsoleName() const

{
    for (int i = stack_.size() - 1; i >= 0; --i)
    {
        const MediaItem& lvl = stack_.at(i).item;
        // The PC games console is a platform level like any other, but nothing under it is a ROM — and
        // "PC Games" must not be handed to a console-name matcher that could resolve it to some pc-98.
        if (lvl.mime == QStringLiteral("pcgames:console")) return QString();
        if (lvl.type == QStringLiteral("platform")) return lvl.title.trimmed();
    }
    return QString();
}

bool HomeView::romhackTargetAt(int idx, MediaItem* itemOut, QString* systemOut) const
{
    if (idx < 0 || idx >= browseRowMap_.size() || stack_.isEmpty()) return false;
    const MediaItem& it = items_[browseRowMap_[idx]];
    const QString sys = retroSystemFor(it, browseConsoleName());
    if (sys.isEmpty()) return false;                 // not a retro game: the verb should not have been offered
    if (itemOut) *itemOut = it;
    if (systemOut) *systemOut = sys;
    // Also called with both outputs null, purely to ask whether the verb should be OFFERED — stash only when
    // a caller is actually taking the item, or a themed repaint would overwrite a flow already in progress.
    if (itemOut) noteRomhackTarget(it, stack_.last().addon, sys);
    return true;
}

// ---- NATIVE PORTS (issue #233) -------------------------------------------------------------------------
// The port bound to THIS item's game, or "" — which is the answer for every row but the ones the catalog
// names. Asked through retroSystemFor first, so the verb inherits the same "is this a retro game, and which
// console is it on" reading the romhack verb uses: a port is bound to one game ON ONE SYSTEM, and without
// the system a same-titled game on another console would match.
//
// BOTH strings are handed on, because each knows something the other does not. `title` is all a catalog row
// that has never been downloaded carries; `url` is the file name, which is the one that carries the No-Intro
// spelling and the "(USA)" the region gate reads. NativePorts::matchesRow takes either.
QString HomeView::nativePortIdFor(const MediaItem& it) const
{
    const QString sys = retroSystemFor(it, browseConsoleName());
    if (sys.isEmpty()) return QString();
    const ExternalEmulator* p = NativePorts::portForGame(sys, it.title, it.url);
    return p ? p->id : QString();
}

bool HomeView::browseNativePort(int themedIndex, MediaItem* itemOut, QString* portIdOut) const
{
    // -1 = the classic grid, whose cursor is its own current row (an items_ index, unmapped — the same
    // asymmetry browseQueueTarget documents). >= 0 = the themed column, which indexes browseRowMap_.
    int row = -1;
    if (themedIndex < 0) { if (!grid_) return false; row = grid_->currentRow(); }
    else                 { if (themedIndex >= browseRowMap_.size()) return false; row = browseRowMap_[themedIndex]; }
    if (row < 0 || row >= items_.size()) return false;
    const QString id = nativePortIdFor(items_[row]);
    if (id.isEmpty()) return false;
    if (itemOut) *itemOut = items_[row];
    if (portIdOut) *portIdOut = id;
    return true;
}

// #193 increment 2. Both entry points end in browse::queueTargetFor, which is the ONE reading of "is this a
// music row, and what would adding it mean" — the two layouts differ only in where their cursor lives, and a
// second reading of the mime here is exactly the drift LeafRoute.h exists to prevent.
bool HomeView::queueTargetForRow(int itemsRow, browse::QueueTarget* out) const
{
    if (itemsRow < 0 || itemsRow >= items_.size()) return false;
    const browse::QueueTarget t = browse::queueTargetFor(items_[itemsRow]);
    if (!t.ok()) return false;
    if (out) *out = t;
    return true;
}

bool HomeView::browseQueueTarget(int themedIndex, browse::QueueTarget* out) const
{
    // -1 = the classic grid, whose cursor is its own current row (an items_ index; the grid's rows and
    // items_ are built together, which is why activateItem takes grid_->currentRow() unmapped).
    if (themedIndex < 0) return grid_ && queueTargetForRow(grid_->currentRow(), out);
    // The themed column indexes browseRowMap_, which skips the section headers items_ carries.
    if (themedIndex >= browseRowMap_.size()) return false;
    return queueTargetForRow(browseRowMap_[themedIndex], out);
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
    // on click. A serial's chapter leaf also gets "Read" (its pages come from the addon's `pages` resource,
    // #188). Steam games get "▶ Play". Containers get none.
    // The gates themselves live in classicActionGates() — shared with the themed detail action row.
    // Steam's store API is keyed on an appid, which a merged PC game has only inside its Steam SOURCE. Hand
    // that source's id over so a merged game's info page still gets the synopsis/genres/Metacritic the Steam
    // console's tiles used to show — a merged item has no addon behind it, so without this its page is bare.
    QString steamAppId;
    if (item.mime == QStringLiteral("steamgame")) steamAppId = item.id.mid(QStringLiteral("steam:").size());
    else if (isMergedPcGame(item))
        for (const pcgame::PcGameSource& s : pcSourcesFor(item))
            if (s.launcher == QStringLiteral("steam") && !s.launchId.isEmpty()) { steamAppId = s.launchId; break; }
    const bool isSteam = !steamAppId.isEmpty();
    const ActionGates gates = classicActionGates(item);
    playImdbId_.clear(); playStremioType_.clear(); // a bridged Play (if any) is established in showMeta()
    if (playBtn_)
    {
        playBtn_->setText(gates.readable ? tr("📖  Read") : tr("▶  Play"));
        playBtn_->setVisible(gates.play);
    }
    if (downloadBtn_) downloadBtn_->setVisible(gates.download);
    // "Choose source…" only where there is a list of releases to choose from (a Stremio-resolved leaf). A
    // bridged leaf can't be known yet — its stream id arrives with /meta — so showMeta reveals that case.
    if (sourceBtn_) sourceBtn_->setVisible(gates.play && canChooseStreamSource(item));
    // Romhacks are a retro-ROM idea: a patch targets one dump of one game. PC games are excluded by
    // retroSystemFor (their system is "pc"), and anything that is not a game has no system at all.
    if (romhackBtn_) romhackBtn_->setVisible(!retroSystemFor(item, browseConsoleName()).isEmpty());
    // "Fix this entry…" only on a merged PC game — the only kind of row whose identity is a heuristic guess
    // the user may need to overrule (issue #44).
    if (pcFixBtn_) pcFixBtn_->setVisible(isMergedPcGame(item));
    if (favBtn_)  favBtn_->setVisible(true); // favourite-able like normal media (text set above)
    // "Fix info…" needs only a stable key, not a resolvable source — a mis-scrape is exactly as wrong on an
    // item that will not play. Its label says whether this item already carries a correction.
    if (editMetaBtn_)
    {
        const QString mk = MetaCache::keyFor(item);
        editMetaBtn_->setVisible(!mk.isEmpty());
        editMetaBtn_->setText(MetaOverrides::has(mk) ? tr("✎  Info edited") : tr("✎  Fix info…"));
    }
    refreshManualButton(item); // 📖 Manual — a game whose bundle carries (or has cached) a manual (issue #89)

    layoutMetaSections(item.type); // order the text rows per the theme
    meta_->setVisible(true);
    if (isSteam)
    {
        pendingMetaReqId_ = (steamMetaSeq_ -= 1); // a unique (negative) id for the async store fetch guard
        MediaItem probe = item;
        probe.id = QStringLiteral("steam:") + steamAppId; // requestSteamMeta reads the appid off the id
        requestSteamMeta(probe, pendingMetaReqId_);
        return;
    }
    pendingMetaReqId_ = mgr_->requestMeta(stack_.last().addon, item);

    // Show the catalog poster + title right away (guarded by the request id we just set), so the info page
    // has a cover immediately - and still shows one if the addon returns no /meta at all (e.g. Allarr). A
    // valid /meta result later overrides this with the addon's own cover + facts + synopsis.
    // Built from the row BEFORE the ingress composite, because showMeta(fromProvider) takes it as the
    // editor's baseline: seeding that from the composited row would offer the user their own correction as
    // "what the scraper found", and then retyping the value on screen would CLEAR the correction instead of
    // storing it. showMeta composites on top for painting, so the card still shows the corrected cover.
    const MediaItem raw = scrapedRow(item);
    const QString cover = MetaCache::scrapedImage(MetaCache::keyFor(item), raw.thumbnailUrl);
    if (!cover.isEmpty())
    {
        MediaDetail d0; d0.title = raw.title; d0.imageUrl = cover; d0.valid = true;
        showMeta(d0);
    }
}

// Reveal "📖 Manual" for a game whose bundle carries a manual role (or that already has one on disk). The
// manual URL rides in on the aggregator scrape (ScreenScraper is the primary supplier) and is recorded in the
// MetaCache bundle WITHOUT the megabyte PDF being fetched — so this is a pure cache read: present-or-fetchable
// means the button shows, and clicking it pulls the file on demand. For a game not yet scraped in this session
// (a cold classic detail), opportunistically kick the aggregator so its manual URL lands and the button lights
// up; the callback only flips visibility, never re-enters this method, so there is no request loop.
void HomeView::refreshManualButton(const MediaItem& item)
{
    if (!manualBtn_) return;
    auto hasManual = [](const QString& key) {
        if (key.isEmpty()) return false;
        return !MetaCache::manualPath(key).isEmpty()
               || !MetaCache::loadArt(key).image(QStringLiteral("manual")).isEmpty();
    };
    const QString key = MetaCache::keyFor(item);
    manualBtn_->setEnabled(true);
    manualBtn_->setText(tr("📖  Manual"));
    manualBtn_->setVisible(item.type == QStringLiteral("game") && hasManual(key));

    // Cold detail: the manual URL may not be cached yet. Scrape (dedup + cache-aware inside the aggregator)
    // and, if a manual role arrives, reveal the button — but only while this same item is still on screen.
    if (item.type == QStringLiteral("game") && !key.isEmpty() && !hasManual(key))
    {
        if (!gameAgg_) gameAgg_ = new GameMetaAggregator(mgr_, this);
        if (gameAgg_->hasProviders())
        {
            QString console;
            for (int i = stack_.size() - 1; i >= 0; --i)
                if (stack_[i].item.type == QStringLiteral("platform")) { console = stack_[i].item.title.trimmed(); break; }
            const QString wantId = item.id;
            gameAgg_->request(item, console, [this, key, wantId, hasManual](const MediaDetail&) {
                if (!manualBtn_) return;
                if (stack_.isEmpty() || !stack_.last().detail || stack_.last().item.id != wantId) return; // moved on
                if (hasManual(key)) manualBtn_->setVisible(true);
            });
        }
    }
}

// Open the manual for `key`. If the file is already cached, hand it straight to the shared reader-open path
// (openItem -> MainWindow::openLibraryItem, which routes a .pdf to PdfView and a .cbz to ComicView, both with
// per-file page resume). Otherwise fetch it ON DEMAND — the only place the megabyte payload is pulled — with a
// small percent readout in the button label, then open it. A failed download restores the label and toasts.
void HomeView::openManualFor(const QString& key, const QString& title)
{
    auto openPath = [this, title](const QString& path) {
        if (path.isEmpty()) return;
        MediaItem it;
        it.id = QStringLiteral("manual:") + path; // a stable, non-catalog id so per-file resume keys on it
        it.url = path;
        it.title = title.isEmpty() ? tr("Manual") : tr("%1 — Manual").arg(title);
        const QString lower = path.toLower();
        if (lower.endsWith(QStringLiteral(".pdf"))) it.type = QStringLiteral("pdf");
        else if (lower.endsWith(QStringLiteral(".cbz")) || lower.endsWith(QStringLiteral(".cbr"))
                 || lower.endsWith(QStringLiteral(".cb7")) || lower.endsWith(QStringLiteral(".cbt"))
                 || lower.endsWith(QStringLiteral(".zip"))) it.type = QStringLiteral("comic");
        emit openItem(it); // reuse the reader we already ship (PdfView / ComicView) + its resume
    };

    const QString cached = MetaCache::manualPath(key);
    if (!cached.isEmpty()) { openPath(cached); return; }

    const QString url = MetaCache::loadArt(key).image(QStringLiteral("manual"));
    if (url.isEmpty()) return;
    if (manualBtn_) { manualBtn_->setEnabled(false); manualBtn_->setText(tr("⬇  0%")); }
    MetaCache::fetchManual(key, url,
        [this](qint64 received, qint64 total) {
            if (manualBtn_ && total > 0)
            {
                qint64 pct = received * 100 / total;
                pct = pct < 0 ? 0 : (pct > 100 ? 100 : pct);
                manualBtn_->setText(tr("⬇  %1%").arg(pct));
            }
        },
        [this, openPath](const QString& path) {
            if (manualBtn_) { manualBtn_->setEnabled(true); manualBtn_->setText(tr("📖  Manual")); }
            if (!path.isEmpty()) openPath(path);
            else emit toastRequested(tr("Couldn't download the manual."), 4000);
        });
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
    req.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(AppBrand::kUserAgent));
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
            mgr_->resolveStreamByImdb(stremioType, imdb, [this, it, detail](const QString& url, const QString& mime,
                                                                           const StreamHeaders::Headers& headers) {
                if (!url.isEmpty())
                {
                    dlEmit(it, url, mime, headers);
                    // The crawl fetched this item's own /meta to bridge it — save the card for offline too.
                    if (!it.id.isEmpty())
                    {
                        MetaCache::saveDetail(MetaCache::keyFor(it), detail);
                        MetaCache::cacheImage(MetaCache::keyFor(it), QStringLiteral("poster"), detail.imageUrl);
                    }
                }
                dlNext();
            });
        }
        else dlNext(); // no IMDB id -> can't resolve this one
        return;
    }
    // Triple/XMB theme: the live-panel /meta arrived -> emit the enriched fields (synopsis + facts) for it.
    if (requestId == themedMetaReq_)
    {
        themedMetaReq_ = -1;
        // J09: bind this response to the row it was REQUESTED for, not the live selection. A slow addon /meta
        // (e.g. the Audiobooks rail, which has no local gamelist/cache to short-circuit it) can land after the
        // user has scrolled on; keying off themedMetaIndex_ then painted one item's synopsis/cover onto the
        // next row. If the selection has moved, drop it — the new row fires its own /meta on settle.
        const int reqIdx = themedMetaReqIndex_;
        if (reqIdx != themedMetaIndex_) return;
        const bool rowOk = reqIdx >= 0 && reqIdx < browseRowMap_.size();
        // The themed editor's baseline, stamped with the row it is for — the themed twin of the classic
        // card's snapshot, and keyed for the same reason: a row whose addon answers with nothing must not
        // be edited against the last row that did.
        if (rowOk && detail.valid)
            themedScraped_.remember(MetaCache::keyFor(items_[browseRowMap_[reqIdx]]), detail);
        // Offline: the addon returned nothing for a row we have a downloaded bundle for — use its saved card.
        // The RAW provider reply. It is emitted through emitThemedMeta, which composites the user's
        // correction over the finished map — without that the /meta arriving a moment after the detail page
        // opened put the scraped synopsis and poster back over the correction, so the feature worked only
        // with no network at all (the offline branch below goes through cachedDetail, already composited).
        MediaDetail det = detail;
        if (!det.valid && rowOk)
            det = MetaCache::cachedDetail(MetaCache::keyFor(items_[browseRowMap_[reqIdx]]));
        QVariantMap m;
        m.insert(QStringLiteral("overview"), det.overview);
        // For games, drop "Released": the year already shows on the subtitle line, so it'd be redundant.
        // (Play history is carried on separate fields that survive this facts merge — see requestThemedMeta.)
        const bool isGame = rowOk && items_[browseRowMap_[reqIdx]].type == QStringLiteral("game");
        QVariantList facts;
        for (const MediaFact& f : det.facts)
        {
            if (isGame && f.label == tr("Released")) continue;
            facts << QVariantMap{ { QStringLiteral("label"), f.label }, { QStringLiteral("value"), f.value } };
        }
        m.insert(QStringLiteral("facts"), facts);
        if (!det.imageUrl.isEmpty()) m.insert(QStringLiteral("image"), det.imageUrl);
        if (!det.subtitle.isEmpty()) m.insert(QStringLiteral("subtitle"), det.subtitle);
        // The enriched artwork/videos/audio/meta from the provider (or the aggregator) -> the live panel:
        // selected.logo, selected.box, selected.images.screenshot, selected.videos, selected.audio, ...
        det.art.writeInto(m);
        // The bridge: this reply may carry the Stremio stream id the catalog row could not. Stamp it on, then
        // re-publish the action verbs — themedDetailData is computed ONCE when the detail page opens, so
        // without this the verb row stays frozen on the raw browse item and "Choose source…" (and, for a
        // metadata-only movie/episode, Play itself) never appears. This is the themed twin of the second
        // reveal showMeta does for the classic page's two buttons.
        if (rowOk && bridgeStreamId(reqIdx, det.imdbStreamId))
        {
            const QVariantMap fresh = themedDetailData(reqIdx);
            if (fresh.contains(QStringLiteral("actions")))
                m.insert(QStringLiteral("actions"), fresh.value(QStringLiteral("actions")));
        }
        emitThemedMeta(reqIdx, m);
        return;
    }
    // Triple/XMB theme: a themed Play that needed the IMDB id first -> resolve via stream addons now.
    if (requestId == themedPlayReq_)
    {
        themedPlayReq_ = -1;
        const QString imdb = detail.imdbStreamId;
        const QString stremioType = (themedPlayItem_.type == QStringLiteral("episode")) ? QStringLiteral("series")
                                                                                        : QStringLiteral("movie");
        if (!imdb.isEmpty() && mgr_->hasStreamProvider(stremioType))
            resolvePlay(themedPlayAddon_, themedPlayItem_, QString(), themedPlayConsole_, imdb, stremioType);
        else
        {
            hideToast();
            showToast(tr("No stream source for “%1”. No stream addon (e.g. Allarr) returned a playable link.")
                          .arg(themedPlayItem_.title), kFeedbackLong);
        }
        return;
    }
    if (requestId != pendingMetaReqId_) return; // stale (navigated away / newer item)
    if (detail.valid)
    {
        // Remember the last shown card so a Download from this info page saves it for offline (dlEmit).
        lastMeta_ = detail;
        lastMetaKey_ = MetaCache::keyFor(metaItem_);
        showMeta(detail);
        return;
    }

    // The addon returned nothing — offline, or the item is gone upstream. A previously downloaded item has
    // its card saved locally (MetaCache), so the info page still gets its synopsis/facts/cover.
    {
        const MediaDetail cached = MetaCache::cachedDetail(MetaCache::keyFor(metaItem_));
        // fromProvider=false: this is the CACHE, already composited by cachedDetail, so it must not become the
        // editor's "what the scraper found" baseline — detailScrapedValues falls back to the raw read instead.
        if (cached.valid) { showMeta(cached, /*fromProvider*/ false); return; }
    }

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

// What the providers said about the open card — the baseline the metadata editor corrects, and what a reset
// restores. The live provider reply is richer than the cache (facts, full synopsis), so re-rendering from the
// cache alone visibly STRIPPED the card on every edit; this keeps the same detail and re-composites it.
MediaDetail HomeView::detailScrapedValues() const
{
    if (stack_.isEmpty() || !stack_.last().detail) return {};
    const QString key = MetaCache::keyFor(stack_.last().item);
    // ONLY this item's own snapshot. Asking for it by key is what stops the previous card's reply from
    // standing in for an item whose addon returned nothing — which would have seeded the editor, and the
    // "typed back what the scraper found" comparison, with ANOTHER item's values, and written them into this
    // item's override. When there is none, the per-item scrape cache is the honest fallback.
    const MediaDetail snap = scrapedDetail_.forKey(key);
    if (snap.valid) return snap;
    return MetaCache::cachedDetailScraped(key);
}

// The themed detail card's values as the PROVIDERS gave them — the twin of detailScrapedValues() for the
// other surface, and the metadata editor's baseline there. Assembled from the SAME scraped sources the
// themed card itself is built from, strongest first: this card's own /meta reply, our scrape cache (which is
// also where the game aggregator writes its merged result), and the ROMs-folder gamelist.xml — over the
// catalog row as it arrived, before the ingress composite.
//
// The editor used to read cachedDetailScraped alone. For a themed card populated from a gamelist entry or
// from session data the cache never held, that showed "(none)" for every field against a visibly populated
// card — the defect 7c3f3b7 fixed for the classic surface — and retyping the value on screen stored a
// needless override, pinning the item against every later, better scrape.
MediaDetail HomeView::themedScrapedValues(int idx) const
{
    if (idx < 0 || idx >= browseRowMap_.size()) return {};
    const MediaItem& shown = items_[browseRowMap_[idx]];
    const QString key = MetaCache::keyFor(shown);
    if (key.isEmpty()) return {};
    const MediaItem raw = scrapedRow(shown);   // the row BEFORE correctedRow() composited the correction
    MediaDetail d;
    d.title    = raw.title;
    d.subtitle = raw.subtitle;
    d.imageUrl = MetaCache::scrapedImage(key, raw.thumbnailUrl);
    MediaDetail rich = themedScraped_.forKey(key);
    if (!rich.valid) rich = MetaCache::cachedDetailScraped(key);
    if (!rich.valid && shown.type == QStringLiteral("game")) rich = GamelistStore::lookup(shown.url);
    if (rich.valid)
    {
        // FILL, never blank: a richer source that simply has no subtitle must not erase the row's own.
        if (!rich.title.isEmpty())    d.title    = rich.title;
        if (!rich.subtitle.isEmpty()) d.subtitle = rich.subtitle;
        if (!rich.imageUrl.isEmpty()) d.imageUrl = rich.imageUrl;
        d.overview = rich.overview;
        d.facts    = rich.facts;
    }
    d.valid = !d.title.isEmpty() || !d.overview.isEmpty() || !d.imageUrl.isEmpty();
    return d;
}

void HomeView::refreshDetailMetaCard()
{
    if (stack_.isEmpty() || !stack_.last().detail) return;
    const QString key = MetaCache::keyFor(stack_.last().item);
    if (key.isEmpty()) return;
    themedArtCache_.remove(key);   // the hover cache short-circuits the read path; it now holds the old art
    if (editMetaBtn_)
        editMetaBtn_->setText(MetaOverrides::has(key) ? tr("✎  Info edited") : tr("✎  Fix info…"));
    const MediaDetail d = detailScrapedValues();
    if (d.valid) showMeta(d, /*fromProvider*/ false); // re-render: composites the correction over the SAME card
}

void HomeView::showMeta(const MediaDetail& scraped, bool fromProvider)
{
    const QString key = stack_.isEmpty() ? QString() : MetaCache::keyFor(stack_.last().item);
    // Remember the provider's own answer before compositing, so the editor can show what it is correcting and
    // a reset has something to go back to. A re-render (fromProvider=false) must NOT overwrite it — it is
    // handed this very value, and on the offline path it is handed an already-cached card. Stamped with the
    // item's key: the snapshot is readable only for the item it was taken for.
    if (fromProvider) scrapedDetail_.remember(key, scraped);
    MediaDetail d = scraped;
    MetaOverrides::applyTo(MetaOverrides::get(key), d);
    showMetaComposited(d);
}

void HomeView::showMetaComposited(const MediaDetail& d)
{
    QString titleHtml = QStringLiteral("<b>%1</b>").arg(d.title.toHtmlEscaped());
    if (!d.subtitle.isEmpty())
        titleHtml += QStringLiteral("<br><span style='font-size:11pt;color:#9aa3ad;'>%1</span>")
                         .arg(d.subtitle.toHtmlEscaped());
    metaTitle_->setText(titleHtml);

    QStringList rows;
    // Lead with play history (last played / time played) for a game we've launched before, above its facts.
    const PlayStats::Stat ps = PlayStats::get(PlayStats::identity(metaItem_.id, QString()));
    if (ps.lastPlayed > 0)
    {
        rows << QStringLiteral("<b>%1:</b> %2").arg(tr("Last played"), PlayStats::formatLastPlayed(ps.lastPlayed));
        if (ps.totalSeconds > 0)
            rows << QStringLiteral("<b>%1:</b> %2").arg(tr("Time played"), PlayStats::formatDuration(ps.totalSeconds));
    }
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
            // The bridge just established a stream id, so this leaf DOES resolve through the stream add-ons
            // and has a list of releases behind it — offer the picker beside the Play it just revealed.
            if (sourceBtn_)
            {
                MediaItem probe = stack_.last().item;
                probe.imdbStreamId = d.imdbStreamId;
                sourceBtn_->setVisible(canChooseStreamSource(probe));
            }
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
    req.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(AppBrand::kUserAgent));
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
    emit toastRequested(text, ms); // rendered by MainWindow as a window-level overlay (over any theme)
}

void HomeView::hideToast()
{
    emit toastHideRequested();
}

void HomeView::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    positionNowPlayingChip();   // the chip is not in any layout; it is anchored by hand (see below)
}

// ---- "Something is playing", the classic surface's half (issue #193, increment 4) --------------------------
//
// The themed half of this is a root-level overlay in ThemeView.qml driven by the declared `backgroundTrack`
// property; this is its twin for the classic grid (and for the classic carousel and XMB layouts, which are
// this same widget). The two are fed by ONE host call from the ONE predicate — MainWindow's
// musicPlayingInBackground(), which is also what draws the "Now playing — …" menu row — so the sign and the
// route back cannot come to disagree about whether anything is playing.
//
// THREE THINGS THIS DELIBERATELY IS NOT:
//
//   * not in a layout. It is a free-floating child moved by hand, because a chip that joined the top bar (or
//     any layout) would shift every other control sideways the moment a track started and back again when it
//     ended. A chrome element that reflows the UI on playback is worse than no element at all.
//   * not focusable (Qt::NoFocus). HomeView's arrow/controller navigation walks focusable children; a chip in
//     that walk would put a dead stop in the browse ring for something you did not ask to reach. The
//     deliberate routes stay Start/Menu, the pause menu, and a click here.
//   * not a second reading of the state. It renders exactly the string it is given, and hides on "".
void HomeView::setNowPlayingTrack(const QString& track)
{
    if (track.isEmpty()) { if (nowPlayingChip_) nowPlayingChip_->hide(); return; }

    if (!nowPlayingChip_)
    {
        nowPlayingChip_ = new QPushButton(this);
        nowPlayingChip_->setObjectName(QStringLiteral("nowPlayingChip"));
        nowPlayingChip_->setFocusPolicy(Qt::NoFocus);      // see above: the browse ring keeps the D-pad
        nowPlayingChip_->setCursor(Qt::PointingHandCursor);
        nowPlayingChip_->setStyleSheet(QStringLiteral(
            "#nowPlayingChip { background: rgba(14,20,30,0.80); color:#ffffff; border:2px solid #3A6FB0;"
            " border-radius:15px; padding:5px 14px; font-weight:bold; }"
            "#nowPlayingChip:hover { background: rgba(30,44,66,0.94); }"));
        connect(nowPlayingChip_, &QPushButton::clicked, this, &HomeView::nowPlayingActivated);
    }
    // Elide against a quarter of the view: a long tag would otherwise walk the chip across the screen.
    const QFontMetrics fm(nowPlayingChip_->font());
    const int cap = qMax(120, int(width() * 0.25));
    nowPlayingChip_->setText(QStringLiteral("♪  ") + fm.elidedText(track, Qt::ElideRight, cap));
    nowPlayingChip_->setToolTip(tr("Now playing: %1 — click to go back to it").arg(track));
    nowPlayingChip_->adjustSize();
    nowPlayingChip_->show();
    positionNowPlayingChip();
}

QString HomeView::nowPlayingChipText() const
{
    if (!nowPlayingChip_ || !nowPlayingChip_->isVisible()) return {};
    // Without the "♪  " prefix the label carries: a test asserts on the TRACK, and the decoration is a
    // rendering choice that should be free to change without rewriting the assertions that depend on it.
    QString t = nowPlayingChip_->text();
    const QString prefix = QStringLiteral("♪  ");
    if (t.startsWith(prefix)) t.remove(0, prefix.size());
    return t;
}

void HomeView::positionNowPlayingChip()
{
    if (!nowPlayingChip_ || nowPlayingChip_->isHidden()) return;
    // Bottom-LEFT, not bottom-right: the toast (Notifier) floats bottom-centre and the grid's scrollbar owns
    // the right edge, so this is the one corner nothing else already uses.
    nowPlayingChip_->move(16, qMax(0, height() - nowPlayingChip_->height() - 16));
    nowPlayingChip_->raise();   // above the grid/carousel, which are layout children added before it
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

    // Warm read path: if the prefetcher (or a prior visit) already parked this catalog page in AddonManager's
    // cache, serve it synchronously — no "Loading…" spinner, no request round-trip, no pendingReqId_ consumed.
    // Only the catalog path is peeked: detail results aren't prefetched and aren't keyed into the catalog cache
    // (so a detail level always misses). cachedCatalog already enforces TTL + source-enabled, so a disabled or
    // stale source misses here and falls through to today's async path below, byte-unchanged.
    if (!top.detail)
    {
        if (const auto warm = mgr_->cachedCatalog(top.addon, top.catalogId, top.query, page, top.filters))
        {
            PerfTrace::begin(QStringLiteral("catalog.load"));
            pendingReqId_ = -1;          // supersede any still-in-flight async reply so it can't clobber this view
            loading_ = false;            // a parent request may have set it; a stale true would block loadMore()
            currentPage_ = page;
            hasMore_ = warm->hasMore;
            populate(*warm, append);     // synchronous; loading_ stays false, so the spinner never appears
            PerfTrace::end(QStringLiteral("catalog.load"),
                           QStringLiteral("page=%1 n=%2 warm").arg(page).arg(warm->items.size()));
            if (!append) rebuildFilterBar(warm->filters);
            return;
        }
    }

    pendingAppend_ = append;
    pendingPage_ = page;
    loading_ = true;
    status_->setText(append ? tr("Loading more…") : tr("Loading…"));

    PerfTrace::begin(QStringLiteral("catalog.load"));
    // requestCatalog now returns -1 for a disabled/absent source; reqIds are positive, so a -1 stored here can
    // never match in onCatalogReady (the store is inert) — same as the long-standing null-source -1 return.
    pendingReqId_ = top.detail ? mgr_->requestDetail(top.addon, top.item, page, top.filters, top.query)
                               : mgr_->requestCatalog(top.addon, top.catalogId, top.query, page, top.filters);
    if (pendingReqId_ < 0)   // disabled/absent source: no async reply will ever arrive — don't wedge on Loading…
    {                        // (reachable: setEnabled emits only sourceEnabledChanged, which HomeView doesn't
                             // rebuild on, so a disabled source's tab stays live; Back/filter/scroll lands here)
        loading_ = false;
        hasMore_ = false;
        status_->setText(tr("This source is unavailable."));
        PerfTrace::end(QStringLiteral("catalog.load"), QStringLiteral("unavailable"));
    }
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
    // (Cross-addon "search everything" responses are claimed by SearchAggregator's own catalogReady handler,
    // filtered by its reqId set; they never match pendingReqId_ so they fall through here harmlessly.)

    if (requestId != pendingReqId_) return; // a superseded request (navigated away / newer page)
    loading_ = false;
    currentPage_ = pendingPage_;
    hasMore_ = cat.hasMore;
    populate(cat, pendingAppend_);
    PerfTrace::end(QStringLiteral("catalog.load"), QStringLiteral("page=%1 n=%2").arg(pendingPage_).arg(cat.items.size()));
    // Sync the filter dropdowns to whatever this response advertises (a catalog, or a container's children
    // like a console's games). Only on a fresh load - paging keeps the current filters.
    if (!pendingAppend_) rebuildFilterBar(cat.filters);
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
        preCorrection_.clear();
        clearBrowseFilter(); // a fresh level load resets the transient browse filter (it never persists across levels)
        settingsStore().sync(); // fresh resume positions for the progress bars
        // Synthetic "folder" marker rows (Recent / Downloaded / Playlists / Favorites): each drills natively via
        // its mime marker and is shown only when `present`. id and type are the same tag in every case. The root
        // group and the per-console group differ only in guard + store-scan predicate, so both feed one builder.
        struct SyntheticFolder { QLatin1String tag; QString title; QString mime; bool present; };
        auto pushFolders = [this](std::initializer_list<SyntheticFolder> folders) {
            for (const SyntheticFolder& f : folders)
            {
                if (!f.present) continue;
                MediaItem m;
                m.id = f.tag;
                m.type = f.tag;
                m.title = f.title;
                m.expandable = true;
                m.mime = f.mime;
                items_.push_back(m);
            }
        };
        // Marks shelves: after the store folders, one row per non-empty group of THIS catalog's items — a
        // Favorites shelf (only at a catalogue root; the per-console block has its own system-scoped ★ folder),
        // one per pinned tag whose members intersect the catalog, and a Hidden shelf (only while Show-hidden is
        // on). Membership hashes each candidate's keyFor through the marks cache (O(1)/item). Drilling snapshots
        // the intersection (openShelfLevel). `cat.items` is the freshly-arrived page — for a single-shot catalog
        // (a games console) that is the whole library; a paged catalog only tests the loaded page here.
        auto pushShelves = [this, &cat](bool favoritesShelf) {
            auto add = [this](const QString& type, const QString& id, const QString& title, const QString& mime) {
                MediaItem m; m.id = id; m.type = type; m.title = title; m.expandable = true; m.mime = mime;
                items_.push_back(m);
            };
            auto any = [&cat](const std::function<bool(const MediaItem&)>& pred) {
                for (const MediaItem& it : cat.items)
                    if (it.type != QStringLiteral("rechdr") && it.type != QStringLiteral("info")
                        && !it.type.startsWith(QLatin1Char('_')) && pred(it))
                        return true;
                return false;
            };
            if (favoritesShelf && any([](const MediaItem& it) {
                    return !isHiddenItem(it) && FavoritesStore::isFavorite(MetaCache::keyFor(it)); }))
                add(QStringLiteral("_favshelf"), QStringLiteral("_favshelf"), tr("★ Favorites"),
                    QStringLiteral("favshelf:"));
            for (const QString& tag : ItemMarks::pinnedTags())
                if (any([&tag](const MediaItem& it) {
                        return !isHiddenItem(it) && ItemMarks::get(MetaCache::keyFor(it)).tags.contains(tag); }))
                    add(QStringLiteral("_tagshelf"), QStringLiteral("_tagshelf:") + tag, tag,
                        QStringLiteral("tagshelf:") + tag);
            if (showHiddenItems() && any([](const MediaItem& it) {
                    return ItemMarks::get(MetaCache::keyFor(it)).hidden; }))
                add(QStringLiteral("_hiddenshelf"), QStringLiteral("_hiddenshelf"), tr("Hidden"),
                    QStringLiteral("hiddenshelf:"));
            // Saved filter presets (#63): a Games-only feature over actual GAME rows. Gate on both the games
            // kind AND the presence of a game-typed item, so this never fires on the console-LIST root (whose
            // rows are platforms, not games — a "played == 0" preset would otherwise match every console). It
            // fires inside a console folder, and on any addon catalog that presents a flat game list. One shelf
            // per preset whose filter matches a visible game (the "non-empty group" rule the shelves above use),
            // then an always-present "＋ New filter…" row so the builder is reachable even with no presets yet.
            const bool hasGameItem = any([](const MediaItem& it) {
                return it.type == QStringLiteral("game") || it.type == QStringLiteral("pcgame"); });
            // ...AND on a CLASSICAL music surface (issue #196, part 2), which is how composer/conductor
            // become usable filter fields rather than two unreachable members of a struct. The gate is the
            // presence of a row that actually CARRIES one of them, not "this is a music level": a library
            // with no COMPOSER tag anywhere — which is most libraries — gets no preset shelves and no
            // "＋ New filter…" row on any album page, so nothing about browsing pop music changes at all.
            // It is also why the gate is not merely "a preset exists": a filter saved on a classical shelf
            // must not follow the user onto a rock record and put an empty row there.
            const bool hasClassicalItem = any([this](const MediaItem& it) {
                const gamefilter::GameFacts f = gameFactsFor(it);
                return !f.composers.isEmpty() || !f.conductors.isEmpty(); });
            if ((catalogRecentKind() == QStringLiteral("game") && hasGameItem) || hasClassicalItem)
            {
                for (const FilterPreset& preset : FilterPresetStore::list())
                    if (any([this, &preset](const MediaItem& it) {
                            return !isHiddenItem(it) && gamefilter::matches(preset.filter, gameFactsFor(it)); }))
                        add(QStringLiteral("_presetshelf"), QStringLiteral("_presetshelf:") + preset.name,
                            QStringLiteral("▦ ") + preset.name, QStringLiteral("presetshelf:") + preset.name);
                add(QStringLiteral("_newpreset"), QStringLiteral("_newpreset"), tr("＋ New filter…"),
                    QStringLiteral("newpreset:"));
            }
        };
        // At a catalogue root (unfiltered, not Recents/detail): a "Recent" folder (this catalogue's recently
        // opened items, if any), a "Downloaded" folder (its fully-downloaded items — but NOT for games, which get
        // one per console below), and a "Playlists" folder (always shown; the saved playlists + a New entry).
        if (!stack_.isEmpty() && !stack_.last().detail && stack_.last().query.isEmpty() && !recentView_)
        {
            const QString rkind = catalogRecentKind();
            // Books / Comics / Manga share the routing kind "document", so each also scopes by its reading
            // form. Empty for every other catalogue, which leaves the marker and both gates exactly as they
            // were. THE GATES MUST ASK THE SAME QUESTION THE FOLDER ANSWERS: a gate on kind alone would
            // offer Comics a Recent folder because you had read a novel, and it would open onto nothing.
            const QString rform = catalogReadingForm();
            const QString rscope = rform.isEmpty() ? QString() : QLatin1Char('|') + rform;
            auto inScope = [&rform](const QString& form, const QString& path) {
                return core::matchesReadingScope(form, path, rform);
            };
            bool hasRecents = false;
            for (const RecentItem& r : RecentStore::list())
                if (r.kind == rkind && inScope(r.form, r.path)) { hasRecents = true; break; }
            bool hasDownloads = false;
            if (rkind != QStringLiteral("game"))
                for (const DownloadedItem& d : DownloadsStore::list())
                    if (d.kind == rkind && inScope(d.form, d.path)) { hasDownloads = true; break; }
            const bool isVideo = (rkind == QStringLiteral("video"));
            const bool isReading = (rkind == QStringLiteral("document")); // the Reading catalogue root (#146)
            // Trakt "Airing Soon": present ONLY when a Trakt account is configured + connected AND its
            // calendar has something still to air. traktCalendarItems() returns an empty catalog whenever
            // calendarAvailable() is false, so on an install that never linked Trakt this is plainly false
            // and pushFolders skips the row entirely — no folder, no empty row, no "connect Trakt" hint.
            const bool hasTraktCal = isVideo && !traktCalendarItems().items.isEmpty();
            // Trakt "You Missed" (#25): the same gate and the same emptiness rule, so an install with no
            // Trakt account gets no folder, and an account with nothing missed gets none either. It is
            // asked at the SHELF cap rather than uncapped, deliberately: the question is "is there at
            // least one", the answer is identical either way, and capping bounds the work this does on
            // every navigation into the video root. (traktListHasRows exists because the LIST form of
            // this question sorted thousands of rows to compare a size to zero; the missed rule is
            // bounded by the followed-show count and the cap, so it needs no such twin.)
            const bool hasTraktMissed = isVideo && !traktMissedItems(trakt::kMissedShelfMax).items.isEmpty();
            // Same gate, same reasoning: the answer is false whenever Trakt is off, so an install that
            // never linked it gets no row at all — and an account with an EMPTY watchlist gets no row
            // either, rather than a folder that opens onto nothing.
            //
            // traktListHasRows, NOT !traktListItems(...).items.isEmpty(): this runs on every navigation
            // into the video root, and the catalog form built AND FULLY SORTED the whole list — which
            // for a watchlist is thousands of rows — only to ask whether it was empty. The predicate
            // short-circuits on the first drawable row and shares its rule with the builder.
            const bool hasTraktWatchlist = isVideo && traktListHasRows(QStringLiteral("watchlist"));
            const bool hasTraktCollection = isVideo && traktListHasRows(QStringLiteral("collection"));
            pushFolders({
                { QLatin1String("_recents"),   tr("Recent"),        QStringLiteral("recents:") + rkind + rscope,             hasRecents },
                { QLatin1String("_downloads"), tr("Downloaded"),    QStringLiteral("downloads:") + rkind + QLatin1Char('|') + rform, hasDownloads },
                { QLatin1String("_locallib"),  tr("Local Library"), QStringLiteral("locallib:") + rkind,                     isVideo && !LocalLibrary::index().all().isEmpty() },
                { QLatin1String("_traktmissed"), tr("You Missed"),  QStringLiteral("traktmissed:"),                          hasTraktMissed },
                { QLatin1String("_traktcal"),  tr("Airing Soon"),   QStringLiteral("traktcal:"),                             hasTraktCal },
                { QLatin1String("_traktlist"), tr("Trakt Watchlist"),  QStringLiteral("traktlist:watchlist"),                hasTraktWatchlist },
                { QLatin1String("_traktlist"), tr("Trakt Collection"), QStringLiteral("traktlist:collection"),               hasTraktCollection },
                { QLatin1String("_playlists"), tr("Playlists"),     QStringLiteral("playlists:") + currentCategoryKey(),     true },
                // Live TV (#75 inc 2): the saved-IPTV-sources shelf. Video only, always shown — the folder's own
                // trailing "add a source" row is the primary way to add the first one, so it appears even with
                // no sources yet (the Playlists rule).
                // Only once a playlist has actually been added. An empty Live TV folder sat under Video for
                // everyone who has never used IPTV, offering a shelf whose whole content was its own "add a
                // source" row. Settings -> Live TV -> "Add a Live TV source..." is what brings it back.
                { QLatin1String("_livetv"),    tr("Live TV"),       QStringLiteral("livetv:"),
                                                             isVideo && !IptvSourceStore::list().isEmpty() },
                // Book Servers (OPDS, #146): the saved-catalogs shelf. Reading catalogue only, always shown — the
                // folder's own trailing "add a catalog" row is the primary way to add the first one, so it
                // appears even with no catalogs yet (the Playlists / Live TV rule).
                { QLatin1String("_opdscatalogs"), tr("Book Servers"), QStringLiteral("opdscatalogs:"),                       isReading },
                // Recomps (#248 inc a): the browse surface over the native-port catalogue #233 ships. Games
                // only, and shown whenever the catalogue holds an entry — which is always, since one is
                // embedded. The gate is on the CATALOGUE, not on this machine owning any of the games: the
                // section's whole job is to say what exists and where you stand with it, and "you have none
                // of these" is an answer it gives per row (`needs ROM`), not by hiding itself.
                { QLatin1String("_recomps"),   tr("Recomps"),       QStringLiteral("recomps:"),
                                                             rkind == QStringLiteral("game")
                                                                 && !NativePorts::all().isEmpty() },
            });
            { PERF_SPAN("marks.shelves"); pushShelves(/*favoritesShelf*/ true); } // Favorites + pinned-tag + (toggle) Hidden shelves
        }
        // Inside each games console folder: a "Recent", "★ Favorites" and "Downloaded" folder scoped to THIS
        // console, each shown only if its store has a matching item.
        if (!stack_.isEmpty() && stack_.last().detail && stack_.last().query.isEmpty() && !recentView_
            && stack_.last().item.type == QStringLiteral("platform"))
        {
            const QString name = stack_.last().item.title.trimmed();
            const QString low = name.toLower();
            // "pc games" is the native folder that replaced the four launcher consoles; the others are the
            // names an addon catalog gives the same platform. Without the first name the folder would be the
            // one PC console with no Recent / ★ Favorites / Downloaded sub-folders in it.
            const bool pc = low == QStringLiteral("pc (windows)") || low == QStringLiteral("pc windows")
                            || low == QStringLiteral("windows") || low == QStringLiteral("pc")
                            || low == QStringLiteral("pc games");
            QString kind, system;
            if (pc) { kind = QStringLiteral("pcgame"); system = QStringLiteral("pc"); }
            else if (const GameSystem* s = SystemCatalog::forConsoleName(name))
                { kind = QStringLiteral("game"); system = s->id; }
            if (!kind.isEmpty())
            {
                bool hasRec = false;
                for (const RecentItem& r : RecentStore::list())
                    if ((r.kind == kind || (kind == QStringLiteral("game") && r.kind == QStringLiteral("pcgame")))
                        && r.system == system) { hasRec = true; break; }
                bool hasFav = false;
                for (const FavoriteItem& f : FavoritesStore::list())
                    // Same rule as browse::favoritesCatalog, which builds the folder this gate reveals: a
                    // merged PC game has no path on purpose, and testing the path alone would hide the ★
                    // folder from a console whose favourites it would then have happily listed.
                    if ((!f.path.isEmpty() || f.itemId.startsWith(QStringLiteral("pcgame:")))
                        && f.system == system) { hasFav = true; break; }
                bool hasDown = false;
                for (const DownloadedItem& d : DownloadsStore::list())
                    if (d.kind == kind && d.system == system) { hasDown = true; break; }
                // Homebrew: offered when this console resolves to a system id AND at least one remote source
                // is configured — the same condition MainWindow::showRomhacks checks before offering that
                // flow, and the ONLY condition available here.
                //
                // THIS IS THE ONE FOLDER OF THE FOUR THAT IS NOT GATED ON HAVING CONTENT. Its three siblings
                // above each scan a local store and so can answer "is there anything behind this row" exactly,
                // for free. This one's answer lives on a server, and asking would mean a network round trip on
                // every navigation into every console — so the row is offered on configuration alone, and a
                // console with nothing opens onto an ordinary empty level (showHomebrewPage) instead of the
                // row being hidden. That is deliberate: "this console has no homebrew" and "the source is
                // down" look the same to someone browsing, and neither is worth a stall on the way in.
                const bool hasServers = mgr_ && !mgr_->remoteSourceUrls().isEmpty();

                // Homebrew asks for a PLATFORM; the three folders above ask for an EMULATOR TARGET, and for
                // one console those are not the same thing. SystemCatalog::forConsoleName deliberately
                // collapses Wii and GameCube onto "gc" (SystemCatalog.h, the has("wii")||has("gamecube")
                // arm) because both run in Dolphin — exactly right for Recent/Favorites/Downloaded, which
                // key on the system a title RUNS on, and wrong here: they are two consoles the user picked
                // between, and a homebrew source can carry titles for one and none for the other.
                //
                // Narrow on purpose: only "gc", and only when the console's own title says Wii. GameCube
                // keeps "gc" and gets an empty folder, which is the honest answer rather than Wii's titles
                // shown under its sibling's name.
                const QString hbSystem =
                    (system == QStringLiteral("gc") && low.contains(QLatin1String("wii")))
                        ? QStringLiteral("wii")
                        : system;

                pushFolders({
                    { QLatin1String("_recents"),   tr("Recent"),      QStringLiteral("recents:") + kind + QLatin1Char('|') + system,   hasRec },
                    { QLatin1String("_favorites"), tr("★ Favorites"), QStringLiteral("favorites:") + system,                            hasFav },
                    { QLatin1String("_downloads"), tr("Downloaded"),  QStringLiteral("downloads:") + kind + QLatin1Char('|') + system, hasDown },
                    { QLatin1String("_homebrew"),  tr("Homebrew"),    HomebrewClient::levelMime(hbSystem),                              hasServers },
                });
                { PERF_SPAN("marks.shelves"); pushShelves(/*favoritesShelf*/ false); } // per-console: ★ folder above already covers favorites
            }
        }
        // ...and on a CLASSICAL music level (issue #196, part 2). This is the third and last call site, and
        // it exists because the two above are the only ones there were: both are gated on a catalogue ROOT
        // or a `platform` folder, and every music level is a detail level under neither. Without it the
        // composer/conductor filter dimensions would be a model nobody could reach - the "＋ New filter…"
        // row IS the builder's only door, and a feature that renders nothing is the failure this repo has
        // shipped twice.
        //
        // THE GATE IS A ROW THAT ACTUALLY CARRIES A CLASSICAL CREDIT, not "this is music": an album of pop
        // gets no shelves, no preset rows and no builder, so browsing a record with no COMPOSER tag is
        // byte-for-byte what it was. That also means the tag/hidden shelves arrive here only alongside the
        // dimensions they came for, rather than appearing on everyone's music at once.
        //
        // IT IS ALSO NAILED TO THE MUSIC LEVELS BY NAME, and that is not belt and braces. Drilling a shelf
        // pushes a level whose items are the matches — still classical tracks — so a gate that asked only
        // "does this level hold a classical row" would put the shelf's own row back at the top of the level
        // it just opened, and the drill would look like it did nothing at all. (It did exactly that once.)
        // The two call sites above are implicitly safe from that by being a catalogue ROOT and a `platform`
        // folder, neither of which a shelf level ever is; this one has to say so.
        if (!stack_.isEmpty() && stack_.last().detail)
        {
            const QString levelType = stack_.last().item.type;
            const bool musicLevel = levelType == QStringLiteral("_musicalbum")
                                 || levelType == QStringLiteral("_musicwork")
                                 || levelType == QStringLiteral("_musicartist");
            bool classical = false;
            if (musicLevel)
                for (const MediaItem& it : cat.items)
                    if (it.type == QStringLiteral("track")
                        && (!it.art.meta.value(QStringLiteral("composers")).toStringList().isEmpty()
                         || !it.art.meta.value(QStringLiteral("conductors")).toStringList().isEmpty()))
                    { classical = true; break; }
            if (classical)
                { PERF_SPAN("marks.shelves"); pushShelves(/*favoritesShelf*/ false); }
        }
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
        // On the Games console list, surface this machine's PC library as ONE native "PC Games" console. It
        // replaced four (Steam / Epic Games / GOG / Battle.net), each of which listed its own launcher and
        // showed a game owned in two stores twice, under two ids. The same "appears when detected" rule
        // applies, now across all of them: the folder shows when ANY PC library has something in it — so a
        // machine with no PC games still sees no extra console.
        if (!stack_.isEmpty() && !stack_.last().detail && stack_.last().query.isEmpty()
            && stack_.last().catalogType == QStringLiteral("game"))
        {
            const bool anyPc =
                   (SteamLibrary::isAvailable()     && !SteamLibrary::installedGames().isEmpty())
                || (EpicLibrary::isAvailable()      && !EpicLibrary::installedGames().isEmpty())
                || (GogLibrary::isAvailable()       && !GogLibrary::installedGames().isEmpty())
                || (BattleNetLibrary::isAvailable() && !BattleNetLibrary::installedGames().isEmpty())
                // OWNED-but-not-installed on Steam counts as "has something in it". The folder LISTS these
                // (pcGamesCatalog mints a LauncherOwned source for each), so a gate that ignored them hid
                // the whole console from the user whose PC library is entirely owned-not-installed — the
                // one folder that replaced four, absent. No isAvailable() gate: this list comes from the
                // Web API, not a local install, and it is empty unless a key + SteamID are configured and
                // a fetch has already succeeded. Cached, so it is network-free on the GUI thread.
                || !SteamLibrary::ownedGamesCached(Settings::steamWebApiKey(), Settings::steamId()).isEmpty()
                || [] {
                       for (const DownloadedItem& d : DownloadsStore::list())
                           if (d.kind == QStringLiteral("pcgame")) return true;
                       return false;
                   }();
            if (anyPc)
            {
                MediaItem pc;
                pc.id = QStringLiteral("pcgames:console");
                pc.type = QStringLiteral("platform"); // a console (drills into its games)
                pc.title = tr("PC Games");
                pc.expandable = true;
                pc.mime = QStringLiteral("pcgames:console"); // marker -> drilled natively, not via the addon
                items_.push_back(pc);
            }
        }
        from = 0;
    }
    else
    {
        from = items_.size();
    }
    // Hidden-item filter: drop items the active profile has marked hidden (unless Show-hidden is on) BEFORE
    // they enter items_, so they vanish from every surface this feeds — the poster grid/carousel/XMB here, the
    // themed browse model (browseItems reads items_), AND search: both live in-catalog search and the cross-
    // addon "search everything" ride populate() (SearchAggregator::resultsAppended -> populate). Synthetic
    // folder/open rows were pushed above (not via cat.items) and are never marks-bearing, so they're untouched.
    for (const MediaItem& src : cat.items)
    {
        if (isHiddenItem(src)) continue;
        // Composite the user's correction to a wrong scrape (issue #24) ONCE, on the way in — see
        // correctedRow(), which every items_ ingress goes through.
        items_.push_back(correctedRow(src));
    }

    // A level of manga chapters is a reading run: remember it so opening one can tell the reader what follows.
    // Rebuilt from the WHOLE of items_ on every pass, so an infinite-scroll append grows the run rather than
    // replacing it with just the newest page.
    //
    // ONLY when this level is ONE container the user drilled into. A level that legitimately mixes series — a
    // cross-addon search, a "latest chapters" shelf — would otherwise build a run spanning several stories: the
    // opened chapter's id IS in that list, so the run arms, and paging forward off the end of a chapter carries
    // the reader into somebody else's story. A run spanning two series is worse than no run at all, because
    // nothing about it looks wrong until the reader is already lost in it; with no run the reader simply gets
    // "that's the last chapter", which is merely unhelpful. The test is structural, not by media type, so it
    // holds for any provider: a non-empty stack, at a detail drill-in, whose container is a real item rather
    // than one of the synthetic levels (their types start with '_' — a cross-addon search is "_search").
    chapterList_.clear();
    chapterEntryType_.clear();
    chapterSeriesTitle_.clear();
    chapterSeriesThumb_.clear();
    chapterSeriesAddonId_.clear();
    const bool oneContainer = !stack_.isEmpty() && stack_.last().detail
                              && !stack_.last().item.type.startsWith(QLatin1Char('_'));
    if (oneContainer)
    {
        // A comic ISSUE joins manga chapters here. The structural guard above is what makes that safe —
        // it is the same guard, and it is the reason a cross-addon search level, where issues of
        // unrelated series sit together, is never remembered as a run.
        for (const MediaItem& it : items_)
            if (isReadableChapter(it.type) || it.type == QStringLiteral("comic_issue"))
            {
                chapterList_.append({ it.id, it.title });
                // The type of the entries, taken from the FIRST chapter leaf rather than assumed: a level
                // is one container, so its chapters are all one type, and that type is what the pages
                // route is keyed by (#188). Comic issues do not set it — they are not read that way.
                if (chapterEntryType_.isEmpty() && isReadableChapter(it.type)) chapterEntryType_ = it.type;
            }
        // The container itself, which this level IS ("Fairy Tail") where its children are the volumes: the
        // title the Catalog lane searches a file provider by, the cover a chapter's Recents row is drawn
        // with (a chapter carries no artwork of its own), and the addon that answered for all of it, so a
        // row resumed from Recents can go back and ask the same source what comes next.
        chapterSeriesTitle_ = stack_.last().item.title;
        chapterSeriesThumb_ = stack_.last().item.thumbnailUrl;
        chapterSeriesAddonId_ = stack_.last().addon ? stack_.last().addon->manifest.id : QString();
    }
    if (!chapterList_.isEmpty())
        hvLog(QStringLiteral("chapter: captured %1 entr(y/ies) from \"%2\"")
                  .arg(chapterList_.size()).arg(chapterSeriesTitle_));

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
                w->setIcon(iconWithProgress(defaultIcon(it.type, kPoster).pixmap(kPoster), rowFraction(it)));
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
        takeFocus(search_);
        search_->deselect();   // keep the caret at the end rather than selecting all the typed text
        searchEditing_ = true; // stay in type mode (FocusOut had flipped it off)
    }

    emit browseItemsChanged(append); // let a themed browse view mirror the new items (append -> keep selection)
    prefetchThemedGames();           // scrape + cache the console's games in the background (hover stays instant)
}

// On entering a game console in a themed view, kick off a throttled background scrape of ALL its games so
// their art/metadata is cached before you hover them (instead of a per-item wait). The aggregator dedups and
// skips games already cached, and throttles to respect the providers' rate limits. No-op unless a themed view
// is showing and at least one game provider is configured.
void HomeView::prefetchThemedGames()
{
    if (!(xmbMode_ || carouselMode_)) return;         // only when a themed view (with the live panel) is up
    if (!gameAgg_) gameAgg_ = new GameMetaAggregator(mgr_, this);
    if (!gameAgg_->hasProviders()) return;
    QString console;
    for (int i = stack_.size() - 1; i >= 0; --i)
        if (stack_[i].item.type == QStringLiteral("platform")) { console = stack_[i].item.title.trimmed(); break; }
    QVector<MediaItem> games;
    for (int r = 0; r < items_.size(); ++r)
    {
        const MediaItem& g = items_[r];
        if (g.type != QStringLiteral("game")) continue;
        if (GamelistStore::has(g.url)) continue;                        // the ROMs-folder gamelist covers it
        if (!MetaCache::loadArt(MetaCache::keyFor(g)).isEmpty()) continue; // already scraped this/prev session
        games << g;
    }
    if (!games.isEmpty())
    {
        qInfo().noquote() << QStringLiteral("[gamemeta] prefetching %1 games for console '%2' (rest from gamelist/cache)")
                                 .arg(games.size()).arg(console);
        gameAgg_->prefetch(games, console);
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
        const double frac = rowFraction(it); // "how far in" for a partly-played movie/episode/audiobook
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
    if (!(search_ && search_->hasFocus())) takeFocus(carousel_); // keep typing during live search
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
    // The QListWidget grid is HIDDEN whenever a themed (QML) home drives the UI — MainWindow routes to the
    // themed pages iff a theme is installed (its own showHome check) — and in the widget XMB/carousel modes.
    // Its poster pipeline still ran for every catalog page regardless: 40 network fetches competing with the
    // theme's own art loads, then a full-res QPixmap decode + smooth-scale PER REPLY on the GUI thread. Those
    // 20-80ms hitches sprinkled over the ~15s of streaming (thumbs.page traced at 10-27s per shelf) were the
    // themed shelf's "jumpy, uneven" scrolling right after opening a console. Skip entirely while unseen: the
    // themed view resolves its own art (row icons + hover panel with caching); a switch back to the widget
    // grid (theme uninstalled) goes through refresh()/populate(), which re-runs this with the grid visible.
#ifdef EB_HAVE_QML
    // The SAME predicate MainWindow::showHomeScreen routes on — installed theme AND the user toggle
    // ("themedHome/enabled", absent = true). Bundled themes are extracted on every install, so gating on
    // hasInstalledTheme() alone would blank the classic grid (remote posters AND local console tiles) for
    // any user who chose the classic home via the Appearance toggle.
    if (ThemeEngine::hasInstalledTheme()
        && settingsStore().value(QStringLiteral("themedHome/enabled"), true).toBool()) return;
#endif
    if (carouselMode_ || xmbMode_) return;
    if (fromIndex <= 0) { thumbQueue_.clear(); perfThumbCount_ = 0; } // fresh view: drop any stale queued loads from the last one
    const bool queueWasEmpty = thumbQueue_.isEmpty();
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
                                            rowFraction(items_[i])));
            continue;
        }
        thumbQueue_.push_back(i); // remote: fetched by pumpThumbnails(), capped so we don't flood the host
        ++perfThumbCount_;
    }
    if (queueWasEmpty && !thumbQueue_.isEmpty()) PerfTrace::begin(QStringLiteral("thumbs.page"));
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
        // The row's progress, resolved NOW and carried into the reply: by the time a poster lands the model
        // may have been rebuilt under it, and the fraction belongs to the row this request was made for.
        const double itemFrac = rowFraction(items_[i]);
        const QString cacheKey = MetaCache::keyFor(items_[i]); // to persist the fetched poster (offline-first)

        QNetworkRequest req((QUrl(url)));
        req.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(AppBrand::kUserAgent));
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        QNetworkReply* reply = nam_->get(req);
        ++thumbActive_;
        connect(reply, &QNetworkReply::finished, this, [this, reply, w, gen, itemFrac, cacheKey] {
            reply->deleteLater();
            --thumbActive_;
            if (thumbQueue_.isEmpty() && thumbActive_ == 0)
                PerfTrace::end(QStringLiteral("thumbs.page"), QStringLiteral("n=%1").arg(perfThumbCount_));
            if (reply->error() == QNetworkReply::NoError) // else navigated away / failed
            {
                const QByteArray data = reply->readAll();
                QPixmap pm;
                if (pm.loadFromData(data))
                {
                    // Persist this poster so displayImage() serves it locally next visit (no re-fetch on
                    // Back / relaunch). Cheap + idempotent; a no-op once the role is cached. Cached even if
                    // we've navigated away — the bytes are valid for this key regardless of the live view.
                    // Post-redirect url so the extension guess sees the real file name (same as cacheImage).
                    MetaCache::storeImage(cacheKey, QStringLiteral("thumb"), reply->url().toString(),
                                          reply->header(QNetworkRequest::ContentTypeHeader).toString(), data);
                    if (gen == generation_) // still the same view: paint it (else just kept for the cache)
                        w->setIcon(iconWithProgress(pm.scaled(kPoster, Qt::KeepAspectRatio, Qt::SmoothTransformation),
                                                    itemFrac));
                }
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
