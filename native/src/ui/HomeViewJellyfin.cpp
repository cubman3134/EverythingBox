// THE JELLYFIN BROWSE LEVELS, AS THE HOME VIEW DRIVES THEM (issue #83, on #160's foundation).
//
// WHY THIS IS ITS OWN TRANSLATION UNIT. HomeView.cpp is nine thousand lines and is the file every
// concurrent branch in this repository ends up editing; a feature's own TU is the #186 direction and it is
// what keeps ten branches out of each other's hunks. What deliberately did NOT move here is the small set
// of lines that has to stay in HomeView.cpp, because three text gates read those functions out of that file
// by name: the "Jellyfin" folder row, the four `activateItem` arms, the themed `playThemedLeaf` arm and the
// four `loadTop` arms.
//
// ==================================================================================================
// EVERY LEVEL IS FETCHED, AND NOTHING IS CACHED
// ==================================================================================================
// The Live TV shelf keeps an in-session channel cache so that Back does not re-hit the network. This does
// not, and the difference is the source: an IPTV playlist is one big document fetched from a stranger's
// CDN, while a Jellyfin level is one small request against a box the user owns, usually on the same
// network — and a media server's library is the single thing most likely to have changed since the last
// time it was looked at (something finished on the television, something added by the scanner an hour ago).
// So Back re-fetches. If that ever proves too expensive it is a cache with an explicit invalidation, not a
// silently stale shelf.
//
// ==================================================================================================
// A SUPERSEDED REPLY CHANGES NOTHING
// ==================================================================================================
// Every fetch takes a generation (jellyfinFetchGen_) and the reply drops itself if the counter has moved —
// the liveTvFetchGen_ idiom exactly, and for the reason that one states: a reply arriving after the user
// has navigated away must not paint over the level they are now looking at. It is also what makes the
// "Loading…" placeholder safe: the placeholder IS a level render, so without the guard a slow first fetch
// would repaint a level the user had already left.
#include "HomeView.h"
#include "XmbView.h"                 // the themed column: every open…Level below leaves its root state

#include "../browse/JellyfinCatalogs.h"
#include "../core/Jellyfin.h"
#include "../core/JellyfinClient.h"
#include "../core/JellyfinServerStore.h"

#include <QStringList>

namespace {

// The per-level network budget. Generous compared with the home fan-out's, deliberately: this is an
// EXPLICIT open — the user pressed a row and is watching a "Loading…" line — so waiting is what they asked
// for, whereas the home refresh must never hold up a shelf for a server that is switched off.
// JellyfinClient.h states that split as the reason the budget is a parameter at all.
constexpr int kLevelBudgetMs = 12000;

// The home Continue Watching fan-out's budget: SHORT, for the mirror-image reason. Nothing is waiting on
// it, its rows arrive late and re-render, and a server that is off must cost the home screen nothing.
constexpr int kContinueBudgetMs = 6000;

// How many distinct servers contributed to a list — the input to the "tag a row with its server name only
// when that disambiguates" rule (JellyfinCatalogs.h). Asked of the ROWS rather than of the store, because
// what matters is what is on the screen: two servers configured and one switched off is a one-server view.
bool moreThanOneServerIn(const QVector<Jellyfin::UnionItem>& items)
{
    QString first;
    for (const Jellyfin::UnionItem& it : items)
    {
        if (it.serverId.isEmpty()) continue;
        if (first.isEmpty()) { first = it.serverId; continue; }
        if (it.serverId != first) return true;
    }
    return false;
}

} // namespace

// ---- The shared one-row levels ---------------------------------------------------------------------------

void HomeView::showJellyfinLoading(const QString& title)
{
    MediaCatalog c;
    c.title = title;
    MediaItem info;
    info.type  = QStringLiteral("info");
    info.title = tr("Loading…");
    c.items.push_back(info);
    showSyntheticCatalog(c);
}

void HomeView::showJellyfinError(const QString& title, const QString& message)
{
    MediaCatalog c;
    c.title = title;
    MediaItem info;
    info.type = QStringLiteral("info");
    // THE CLIENT'S SENTENCE, WHICH HAS NEVER SEEN A URL. JellyfinClient renders transport failures from
    // Qt's NetworkError enum into fixed sentences of its own precisely so that a message like this one can
    // be put on the screen without a token or an address going with it.
    info.title = message.isEmpty() ? tr("That server could not be read.") : message;
    c.items.push_back(info);
    showSyntheticCatalog(c);
}

// ---- Level 1: the libraries, merged across every enabled server -------------------------------------------

void HomeView::openJellyfinLevel()
{
    if (xmbMode_) { atXmbRoot_ = false; if (xmb_) xmb_->setAtRoot(false); }
    Level lvl;
    lvl.addon = nullptr; lvl.detail = true; lvl.title = tr("Jellyfin");
    lvl.item.id   = QString::fromLatin1(browse::kJellyfinRootType);
    lvl.item.type = QString::fromLatin1(browse::kJellyfinRootType);
    lvl.item.expandable = true;
    // The marker loadTop() repopulates from on the way back in — see the synthetic level Back survival gate.
    lvl.item.mime = QString::fromLatin1(browse::kJellyfinRootPrefix);
    stack_.push_back(lvl);
    populateJellyfinLibraries();
}

void HomeView::populateJellyfinLibraries()
{
    const int gen = ++jellyfinFetchGen_;
    const QString title = tr("Jellyfin");
    showJellyfinLoading(title);
    JellyfinClient::instance().fetchLibraries(kLevelBudgetMs,
        [this, gen, title](const QVector<Jellyfin::LibraryRef>& libraries, const QStringList& notes) {
            if (gen != jellyfinFetchGen_) return;   // superseded: the user has navigated away
            showSyntheticCatalog(browse::jellyfinLibrariesCatalog(libraries, notes));
        });
}

// ---- Level 2: one library's titles ------------------------------------------------------------------------

void HomeView::openJellyfinLibraryLevel(const QString& libraryRef, const QString& title)
{
    if (libraryRef.isEmpty()) return;   // a row whose marker carried no id: nothing to open
    if (xmbMode_) { atXmbRoot_ = false; if (xmb_) xmb_->setAtRoot(false); }
    Level lvl;
    lvl.addon = nullptr; lvl.detail = true; lvl.title = title;
    lvl.item.id    = libraryRef;
    lvl.item.title = title;
    lvl.item.type  = QString::fromLatin1(browse::kJellyfinLibType);
    lvl.item.expandable = true;
    lvl.item.mime  = QString::fromLatin1(browse::kJellyfinLibPrefix) + libraryRef;
    stack_.push_back(lvl);
    populateJellyfinLibrary(libraryRef, title);
}

void HomeView::populateJellyfinLibrary(const QString& libraryRef, const QString& title)
{
    const int gen = ++jellyfinFetchGen_;
    showJellyfinLoading(title);
    JellyfinClient::instance().fetchLibraryItems(libraryRef, kLevelBudgetMs,
        [this, gen, title](const QVector<Jellyfin::UnionItem>& items, const QString& error) {
            if (gen != jellyfinFetchGen_) return;
            if (!error.isEmpty()) { showJellyfinError(title, error); return; }
            // A library is addressed on ONE server, so no row here can be from another and the tag would
            // be on every row saying the same thing. The union is what qualified the ids; the tagging
            // question is separate and is answered by looking at the rows.
            showSyntheticCatalog(browse::jellyfinLibraryCatalog(title, items,
                                                                moreThanOneServerIn(items), {}));
        });
}

// ---- Level 3: a series' seasons ---------------------------------------------------------------------------

void HomeView::openJellyfinSeriesLevel(const QString& seriesRef, const QString& title)
{
    if (seriesRef.isEmpty()) return;
    if (xmbMode_) { atXmbRoot_ = false; if (xmb_) xmb_->setAtRoot(false); }
    Level lvl;
    lvl.addon = nullptr; lvl.detail = true; lvl.title = title;
    lvl.item.id    = seriesRef;
    lvl.item.title = title;
    lvl.item.type  = QString::fromLatin1(browse::kJellyfinSeriesType);
    lvl.item.expandable = true;
    lvl.item.mime  = QString::fromLatin1(browse::kJellyfinSeriesPrefix) + seriesRef;
    stack_.push_back(lvl);
    populateJellyfinSeries(seriesRef, title);
}

void HomeView::populateJellyfinSeries(const QString& seriesRef, const QString& title)
{
    const int gen = ++jellyfinFetchGen_;
    showJellyfinLoading(title);
    JellyfinClient::instance().fetchSeasons(seriesRef, kLevelBudgetMs,
        [this, gen, title, seriesRef](const QVector<Jellyfin::UnionItem>& seasons, const QString& error) {
            if (gen != jellyfinFetchGen_) return;
            if (!error.isEmpty()) { showJellyfinError(title, error); return; }
            showSyntheticCatalog(browse::jellyfinSeasonsCatalog(title, seriesRef, seasons));
        });
}

// ---- Level 4: a season's episodes -------------------------------------------------------------------------

void HomeView::openJellyfinSeasonLevel(const QString& marker, const QString& title)
{
    if (marker.isEmpty()) return;
    if (xmbMode_) { atXmbRoot_ = false; if (xmb_) xmb_->setAtRoot(false); }
    Level lvl;
    lvl.addon = nullptr; lvl.detail = true; lvl.title = title;
    lvl.item.id    = marker;
    lvl.item.title = title;
    lvl.item.type  = QString::fromLatin1(browse::kJellyfinSeasonType);
    lvl.item.expandable = true;
    lvl.item.mime  = QString::fromLatin1(browse::kJellyfinSeasonPrefix) + marker;
    stack_.push_back(lvl);
    populateJellyfinSeason(marker, title);
}

void HomeView::populateJellyfinSeason(const QString& marker, const QString& title)
{
    // "<qualified series id>\n<qualified season id>" — split on the FIRST newline only, because neither
    // half may contain one and a section() over the whole string would be a second reading of the same
    // marker free to disagree with the builder that wrote it.
    const int nl = marker.indexOf(QLatin1Char('\n'));
    const QString seriesRef = nl < 0 ? marker : marker.left(nl);
    const QString seasonRef = nl < 0 ? QString() : marker.mid(nl + 1);
    const int gen = ++jellyfinFetchGen_;
    showJellyfinLoading(title);
    JellyfinClient::instance().fetchEpisodes(seriesRef, seasonRef, kLevelBudgetMs,
        [this, gen, title](const QVector<Jellyfin::UnionItem>& episodes, const QString& error) {
            if (gen != jellyfinFetchGen_) return;
            if (!error.isEmpty()) { showJellyfinError(title, error); return; }
            showSyntheticCatalog(browse::jellyfinEpisodesCatalog(title, episodes));
        });
}

// ---- Continue Watching, merged into the home list ----------------------------------------------------------

void HomeView::refreshJellyfinContinue()
{
    // NOT ONCE PER RENDER. renderRecents runs on every Back and on every store change, and one request per
    // navigation into Home would be a request per keystroke on a controller. The in-flight latch is the
    // whole of the throttle: a fetch is running, or it is not, and the rows it left behind are drawn until
    // the next one replaces them.
    if (jellyfinContinueInFlight_) return;
    if (!JellyfinServerStore::hasServers())
    {
        // A server was removed while its rows were on screen: drop them, and re-render only if there was
        // something to drop (an unconditional re-render here would recurse through renderRecents).
        if (!jellyfinContinue_.isEmpty()) { jellyfinContinue_.clear(); renderRecents(); }
        return;
    }
    jellyfinContinueInFlight_ = true;
    JellyfinClient::instance().fetchContinueWatching(kContinueBudgetMs,
        [this](const QVector<Jellyfin::UnionItem>& items, const QStringList& notes) {
            Q_UNUSED(notes);   // the home list is not the place to explain a server being off; the browse
                               // levels carry those notes, where the user went looking for that server
            jellyfinContinueInFlight_ = false;
            const QVector<MediaItem> rows =
                browse::jellyfinContinueRows(items, moreThanOneServerIn(items));
            // RE-RENDER ONLY ON A CHANGE. The answer is usually identical to the last one, and re-rendering
            // the home list rebuilds every row and reloads every thumbnail — on a surface the user may be
            // in the middle of scrolling.
            bool same = rows.size() == jellyfinContinue_.size();
            for (int i = 0; same && i < rows.size(); ++i)
                same = rows[i].id == jellyfinContinue_[i].id
                    && rows[i].title == jellyfinContinue_[i].title
                    && rows[i].subtitle == jellyfinContinue_[i].subtitle;
            if (same) return;
            jellyfinContinue_ = rows;
            if (recentView_) renderRecents();
        });
}
