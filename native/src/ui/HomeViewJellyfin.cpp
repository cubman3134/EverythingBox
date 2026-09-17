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
#include "nav/NavOverlay.h"          // #83: the Quick Connect code panel is a NavConfirm
#include "nav/Osk.h"                 // ...and the password route's prompts

#include <QBoxLayout>
#include <QEventLoop>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QStringList>
#include <QTextBrowser>

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
    // The builder probe_browse holds against the row this was opened from (#310) — the same five fields
    // this site used to set inline, so the drill pushes exactly the level it always did.
    lvl.item = browse::jellyfinSeriesLevelItem(seriesRef, title);
    stack_.push_back(lvl);
    populateJellyfinSeries(seriesRef, title);
}

void HomeView::populateJellyfinSeries(const QString& seriesRef, const QString& title)
{
    const int gen = ++jellyfinFetchGen_;
    // Every render below is followed by the header, for the reason presentJellyfinLevelHeader states.
    showJellyfinLoading(title);
    presentJellyfinLevelHeader();
    JellyfinClient::instance().fetchSeasons(seriesRef, kLevelBudgetMs,
        [this, gen, title, seriesRef](const QVector<Jellyfin::UnionItem>& seasons, const QString& error) {
            if (gen != jellyfinFetchGen_) return;
            if (!error.isEmpty()) { showJellyfinError(title, error); presentJellyfinLevelHeader(); return; }
            showSyntheticCatalog(browse::jellyfinSeasonsCatalog(title, seriesRef, seasons));
            presentJellyfinLevelHeader();
        });
}

// ---- Level 4: a season's episodes -------------------------------------------------------------------------

void HomeView::openJellyfinSeasonLevel(const QString& marker, const QString& title)
{
    if (marker.isEmpty()) return;
    if (xmbMode_) { atXmbRoot_ = false; if (xmb_) xmb_->setAtRoot(false); }
    Level lvl;
    lvl.addon = nullptr; lvl.detail = true; lvl.title = title;
    lvl.item = browse::jellyfinSeasonLevelItem(marker, title);   // see openJellyfinSeriesLevel (#310)
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
    presentJellyfinLevelHeader();
    JellyfinClient::instance().fetchEpisodes(seriesRef, seasonRef, kLevelBudgetMs,
        [this, gen, title](const QVector<Jellyfin::UnionItem>& episodes, const QString& error) {
            if (gen != jellyfinFetchGen_) return;
            if (!error.isEmpty()) { showJellyfinError(title, error); presentJellyfinLevelHeader(); return; }
            showSyntheticCatalog(browse::jellyfinEpisodesCatalog(title, episodes));
            presentJellyfinLevelHeader();
        });
}

// ---- The classic layout's Download door on a series or season level (#310) --------------------------------
//
// WHERE IT SITS, AND WHOSE SHAPE IT IS. An addon's series level on the classic layout is a detail level with
// its header card above the child grid — cover, title and the action row, whose "⬇ Download" takes the
// whole container. A Jellyfin series or season is also a detail level, but it is drawn through
// showSyntheticCatalog, which hides that card, so the level had rows and no verbs. This puts the same card
// back, narrowed to its one button that means something here. It is NOT a row in the grid: a leading tile
// would shift every episode one place along a wrapping poster grid and move where each arrow press lands.
//
// WHY THE CARD AND NOT A NEW CONTROL. Everything a D-pad needs already exists for it: Up from the grid's top
// row lands on the card's action (the container-detail rule in the grid's key filter), Down drops back into
// the grid, Left/Right walk the visible buttons, and Backspace is Back. The press is downloadBtn_'s own
// clicked -> startDownload, whose Jellyfin arm asks browse::jellyfinDownloadTargetFor of THIS level's item
// and hands MainWindow the same (kind, ref, seasonRef) the Start-menu door reads off the row — ending in
// MainWindow::downloadJellyfinBatch either way. One table, one verb, one chooser.
//
// WHY IT IS RE-APPLIED AFTER EVERY RENDER. showSyntheticCatalog calls hideMeta(), and both of these levels
// render two or three times (Loading, then the rows or the error), so a header set once would be taken down
// by the level's own next paint.
//
// Every widget the card holds is set here, not inherited from whatever page used it last: requestMeta sets
// each of its buttons explicitly on every build, and this does the same in the other direction, so neither
// page can leave a stale button showing on the other.
void HomeView::presentJellyfinLevelHeader()
{
    if (stack_.isEmpty() || !meta_ || !actionRow_ || !downloadBtn_) return;
    const MediaItem& level = stack_.last().item;
    if (!browse::jellyfinLevelOffersDownload(level)) return;

    metaTitle_->setText(level.title.toHtmlEscaped());
    metaFacts_->clear();    metaFacts_->setVisible(false);
    metaOverview_->clear(); metaOverview_->setVisible(false);
    if (metaFailure_) metaFailure_->setVisible(false);
    // No cover: a level has none of its own, and a type placeholder the size of a poster would push the
    // episodes half a screen down to show nothing. The theme's own "text" detail layout, in effect.
    metaImage_->hide();
    metaLayout_->setDirection(QBoxLayout::LeftToRight);

    for (QPushButton* b : actionRow_->findChildren<QPushButton*>(Qt::FindDirectChildrenOnly))
        b->setVisible(false);
    // The detail page's own gate, for the level's own item — classicActionGates answers Download for a
    // Jellyfin row from jellyfinDownloadTargetFor, the table every other door asks.
    downloadBtn_->setVisible(classicActionGates(level).download);
    meta_->setVisible(true);
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

// ==========================================================================================================
// SIGNING IN: QUICK CONNECT, WITH THE PASSWORD ONE PRESS AWAY (issue #83)
// ==========================================================================================================
// connectJellyfinServerInteractive (HomeView.cpp) settles the address and reads the server's identity; this
// is everything after that. It is shared by BOTH settings builders, because both open the same manager.
//
//   identity read
//        |
//   GET /QuickConnect/Enabled ---- anything but a plain `true` ---------------------------> PASSWORD
//        | true
//   CODE PANEL  "Getting a code…" -> the code, large, and one line of instruction
//        |-- approved on another app -> AuthenticateWithQuickConnect -> STORED (same path as a password)
//        |-- "Use password instead" ------------------------------------------------------> PASSWORD
//        |-- Back -----------------------------------------------------------------------> nothing added
//        |-- the server forgot the code, or five minutes passed -> "That code expired"
//        |        |-- New code -> a fresh CODE PANEL
//        |        |-- Use password instead -----------------------------------------------> PASSWORD
//        |        `-- Back ---------------------------------------------------------------> nothing added
//        `-- Quick Connect switched off / refused -> the sentence, and "Use password instead"
//
// THE POLLING BELONGS TO THE PANEL. The session is the card's QObject child and the card's closed() cancels
// it, so however the card goes away — approved, Back, "Use password instead", expired — no further request
// reaches the server. probe_jellyfin section 20 counts the server's Connect calls to prove it.
//
// No modal dialog and no window: the card is a NavConfirm, so a pad, a keyboard and a mouse all reach it on
// both layouts, and it is relabelled in place when the code arrives.

namespace {

// The card's own outcomes, past the button indexes it was built with.
constexpr int kQcAuthorized = 100;
constexpr int kQcExpired    = 101;
constexpr int kQcFailed     = 102;

// The Enabled question's budget. Short: the answer only decides which prompt comes next, and a server slow
// to say is a server the password prompt serves just as well.
constexpr int kQcRouteBudgetMs = 8000;

QString jellyfinDisplayName(const QString& serverName)
{
    return serverName.trimmed().isEmpty() ? QStringLiteral("Jellyfin") : serverName;
}

// The one place a signed-in server is stored and announced, whichever way the sign-in went. The record comes
// from JellyfinServerStore::fromSignIn, so the two routes cannot disagree about it.
void storeJellyfinSignIn(QWidget* window, const QString& url, bool allowPlainHttp, const QString& serverId,
                         const QString& serverName, const Jellyfin::AuthResult& res)
{
    Jellyfin::PublicInfo info;
    info.serverId   = serverId;
    info.serverName = serverName;
    info.ok         = Jellyfin::isServerId(serverId);
    const JellyfinServer s = JellyfinServerStore::fromSignIn(info, res, url, allowPlainHttp);
    if (!JellyfinServerStore::add(s))
    {
        NavConfirm::ask(HomeView::tr("Jellyfin"),
            HomeView::tr("That server did not give an identity this app can use, so its "
                        "items could not be told apart from another server's."),
            { HomeView::tr("OK") }, 0, 0, window);
        return;
    }
    NavConfirm::ask(HomeView::tr("Jellyfin"),
        HomeView::tr("“%1” is connected. Its library appears alongside your own, with each "
                    "row labelled by the server it came from.").arg(s.name),
        { HomeView::tr("OK") }, 0, 0, window);
}

// The code panel's message. Rich text so the code can be LARGE — it is read from across a room — and the
// code is digits from the server, escaped all the same.
QString quickConnectMessage(const QString& code)
{
    return QStringLiteral("<div align=\"center\" style=\"font-size:44px; font-weight:700;\">%1</div>"
                          "<div align=\"center\">%2</div>")
        .arg(code.toHtmlEscaped(),
             HomeView::tr("On a signed-in Jellyfin app, open Quick Connect and enter this code").toHtmlEscaped());
}

} // namespace

void HomeView::signInToJellyfinServer(const QString& url, bool allowPlainHttp, const QString& serverId,
                                      const QString& serverName)
{
    QPointer<HomeView> self(this);
    JellyfinClient::instance().fetchSignInRoute(url, allowPlainHttp, kQcRouteBudgetMs, this,
        [self, url, allowPlainHttp, serverId, serverName](JellyfinQuickConnect::Route route) {
            if (!self) return;
            // Deferred a turn: this can be inside the Enabled reply's finished() emission, and both routes
            // open the nav kit's nested loops (#28 / #211).
            QMetaObject::invokeMethod(self.data(), [self, url, allowPlainHttp, serverId, serverName, route] {
                if (!self) return;
                if (route == JellyfinQuickConnect::Route::QuickConnect)
                    self->signInToJellyfinWithQuickConnect(url, allowPlainHttp, serverId, serverName);
                else
                    self->signInToJellyfinWithPassword(url, allowPlainHttp, serverId, serverName);
            }, Qt::QueuedConnection);
        });
}

void HomeView::signInToJellyfinWithQuickConnect(const QString& url, bool allowPlainHttp,
                                                const QString& serverId, const QString& serverName)
{
    const QString name = jellyfinDisplayName(serverName);
    for (;;)
    {
        auto* card = new NavConfirm(tr("Quick Connect — %1").arg(name),
                                    tr("Getting a code from %1…").arg(name),
                                    { tr("Use password instead") }, /*focusIndex*/ 0, window());
        // Owned by the card: when the card goes, so does the polling.
        JellyfinQuickConnectSession* session =
            JellyfinClient::instance().newQuickConnectSession(url, allowPlainHttp, card);
        if (!session)
        {
            card->dismiss(0);
            signInToJellyfinWithPassword(url, allowPlainHttp, serverId, serverName);
            return;
        }

        QPointer<NavConfirm> cardGuard(card);
        Jellyfin::AuthResult result;          // THE TOKEN, once approved. Handed to the store and dropped.
        QString failure;
        int outcome = -1;
        bool closed = false;
        QEventLoop loop;
        connect(session, &JellyfinQuickConnectSession::codeReady, card,
                [card](const QString& code) { card->setMessage(quickConnectMessage(code)); });
        connect(session, &JellyfinQuickConnectSession::authorized, card,
                [card, &result](const Jellyfin::AuthResult& r) { result = r; card->dismiss(kQcAuthorized); });
        connect(session, &JellyfinQuickConnectSession::expired, card, [card] { card->dismiss(kQcExpired); });
        connect(session, &JellyfinQuickConnectSession::failed, card,
                [card, &failure](const QString& m) { failure = m; card->dismiss(kQcFailed); });
        connect(card, &NavOverlay::closed, &loop, [session, &outcome, &closed, &loop](int r) {
            // BACK, "Use password instead", approval, expiry: every way the card closes stops the polling
            // here, before anything else runs. (The session is the card's child and dies with it a turn
            // later too; this is the stop that does not wait for the deleteLater.)
            session->cancel();
            outcome = r;
            closed = true;
            loop.quit();
        });
        session->start();
        if (!closed) loop.exec();   // pad polling and the poll timer both run inside this loop

        if (outcome == kQcAuthorized)
        {
            storeJellyfinSignIn(window(), url, allowPlainHttp, serverId, serverName, result);
            return;
        }
        if (outcome == 0)
        {
            signInToJellyfinWithPassword(url, allowPlainHttp, serverId, serverName);
            return;
        }
        if (outcome == kQcExpired)
        {
            const int next = NavConfirm::ask(tr("Quick Connect — %1").arg(name), tr("That code expired."),
                                             { tr("New code"), tr("Use password instead") },
                                             /*focusIndex*/ 0, /*cancelIndex*/ -1, window());
            if (next == 0) continue;                         // a fresh code, a fresh clock
            if (next == 1) signInToJellyfinWithPassword(url, allowPlainHttp, serverId, serverName);
            return;
        }
        if (outcome == kQcFailed)
        {
            const int next = NavConfirm::ask(tr("Quick Connect — %1").arg(name), failure,
                                             { tr("Use password instead"), tr("Cancel") },
                                             /*focusIndex*/ 0, /*cancelIndex*/ 1, window());
            if (next == 0) signInToJellyfinWithPassword(url, allowPlainHttp, serverId, serverName);
            return;
        }
        return;                                              // Back: nothing is added
    }
}

// THE PASSWORD ROUTE — the prompts #160 shipped, unchanged, moved here from connectJellyfinServerInteractive
// so both routes sit side by side. Only the storing moved: it is storeJellyfinSignIn above, shared.
void HomeView::signInToJellyfinWithPassword(const QString& url, bool allowPlainHttp, const QString& serverId,
                                            const QString& serverName)
{
    const QString user = Osk::getText(tr("Username:"), QString(), QLineEdit::Normal, window()).trimmed();
    if (user.isEmpty()) return;
    // NEVER ECHOED, NEVER TRIMMED, NEVER LOGGED. Not trimmed because leading and trailing spaces
    // are significant in a password and eating them silently produces a sign-in that fails for a
    // reason nobody can see; entered as QLineEdit::Password so it is not readable over somebody's
    // shoulder on a television. It goes to the transport and is not held.
    const QString pass = Osk::getText(tr("Password:"), QString(), QLineEdit::Password, window());
    if (pass.isEmpty()) return;

    QPointer<HomeView> self(this);
    JellyfinClient::instance().authenticate(url, allowPlainHttp, user, pass, /*budgetMs*/ 20000,
        [self, url, allowPlainHttp, serverId, serverName](const Jellyfin::AuthResult& res,
                                                          const QString& authError) {
            if (!self) return;
            // Deferred past the reply's emission, for the #28 / #211 reason above.
            QMetaObject::invokeMethod(self.data(),
                [self, url, allowPlainHttp, serverId, serverName, res, authError] {
                    if (!self) return;
                    if (!authError.isEmpty() || !res.ok)
                    {
                        NavConfirm::ask(tr("Jellyfin"),
                                        authError.isEmpty() ? tr("That server refused the sign-in.")
                                                            : authError,
                                        { tr("OK") }, 0, 0, self->window());
                        return;
                    }
                    storeJellyfinSignIn(self->window(), url, allowPlainHttp, serverId, serverName, res);
                }, Qt::QueuedConnection);
        });
}
