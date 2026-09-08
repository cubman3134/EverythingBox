// ASKING FOR SOMETHING YOU DO NOT HAVE (issue #109) — MainWindow's half.
//
// Its own translation unit for the #186 reason MainWindowJellyfin.cpp and MainWindowJellyfinDownload.cpp
// open with: MainWindow.cpp is the file every concurrent branch collides in, and this is a complete thought
// of its own.
//
// ==========================================================================================================
// EVERY SUBMISSION IS A PRESS, AND THERE IS EXACTLY ONE PLACE IT HAPPENS
// ==========================================================================================================
// A request makes SOMEBODY ELSE'S SERVER go and acquire content — it costs their bandwidth, their disk and,
// in a household with approvals, their attention. So the shape of this file is built around one rule:
//
//   * submitRequest() is the ONLY function in the tree that calls RequestBackend::submit. Nothing else may,
//     and nothing else does.
//   * It is reached from exactly one caller, requestItemInteractive(), which is reached only from a press —
//     the classic detail page's button or the themed action row's pill — and which puts a confirmation card
//     in front of it naming the title in the sentence.
//   * NOTHING RETRIES. A submission that fails is reported to the person who pressed the button and stops.
//     A timed-out POST may or may not have reached the service, and a silent retry is how one press becomes
//     two fetches on somebody else's disk.
//   * NOTHING POLLS. Statuses are fetched ON VIEW, at most once per title per session, and refreshed for the
//     shelf's own rows when the home is built. There is no timer in this file.
//   * VIEWING AN ITEM ASKS FOR NOTHING. fetchRequestStatus issues a read-only GET; it has no path to
//     submitRequest at all.
//
// ==========================================================================================================
// THE ANTI-DUPLICATE BRANCH
// ==========================================================================================================
// Before the button can be a Request it has to be established that the title is not already there. That is
// what the on-view lookup is for, and it is why the action's LABEL is its state: when the service says the
// linked server already holds the title the button reads "In your library" and OPENS it (through #83's own
// path), so there is no press on that page that could create a second copy. requests::actionFor is the rule,
// it is pure, and probe_requests drives every arm of it.
//
// ==========================================================================================================
// THE KEY IS A CREDENTIAL
// ==========================================================================================================
// It is entered with QLineEdit::Password, handed to the verify call and then to the device-local store, and
// is never echoed, never logged and never put into a message. NOTHING IN THIS FILE BUILDS A SENTENCE OUT OF
// A REQUEST: every string a user reads here comes from requests::failureSentence, a fixed table that has
// never seen a url. There is no call to QNetworkReply::errorString() anywhere in this feature.
#include "MainWindow.h"

#include "FeedbackPolicy.h"          // kFeedbackShort / kFeedbackLong
#include "HomeView.h"
#include "../theme2/ThemeEngine.h"   // rootItem() — the themed pill's detailData lives on the QML root

#include <QQuickItem>
#include <QStackedWidget>
#include "nav/NavOverlay.h"          // NavMenu / NavConfirm — the nav kit, never a QDialog
#include "nav/Osk.h"
#include "../core/Jellyseerr.h"       // the url verdicts — the plain-HTTP question, asked before the key
#include "../core/JellyseerrClient.h"
#include "../core/JellyseerrStore.h"
#include "../core/RequestBackend.h"
#include "../core/RequestStore.h"
#include "../core/Requests.h"

#include <QDateTime>
#include <QLineEdit>
#include <QPointer>
#include <QStatusBar>

namespace {

// The budgets. A status fetch runs while somebody is looking at a page and must never be the reason the page
// feels slow; a submission is a press the user is watching a spinner for and can afford to wait.
constexpr int kStatusBudgetMs = 8000;
constexpr int kSubmitBudgetMs = 20000;
constexpr int kVerifyBudgetMs = 10000;
// The shelf refresh walks the profile's own rows one at a time. Short per row: a service that is down must
// be recognised quickly and the rest left showing what they last knew, not walked into a stall.
constexpr int kShelfBudgetMs  = 6000;

// The stored-row shape for one item, built from what the press already knows. Never from a lookup: a row
// means "this profile asked for this", so only a submission may create one.
requests::StoredRequest rowFor(const MediaItem& item, const requests::MediaRef& ref,
                               const QString& backendId, const QVector<int>& seasons,
                               requests::Availability availability)
{
    requests::StoredRequest r;
    r.key = ref.key();
    r.backendId = backendId;
    r.title = item.title;
    r.thumb = item.thumbnailUrl;
    r.mediaType = ref.mediaType;
    r.imdb = ref.imdb;
    r.tmdb = ref.tmdb;
    r.seasons = seasons;
    r.requestedAt = QDateTime::currentSecsSinceEpoch();
    r.status = requests::statusToken(availability);
    return r;
}

} // namespace

// ==========================================================================================================
// WIRING
// ==========================================================================================================
void MainWindow::initRequests()
{
    // The shelf redraws when its own rows change. No network here — the store is local and the statuses on
    // it are whatever the last refresh established.
    RequestStore::setChangeHook([this] {
        if (home_) home_->refresh();
    });
    // ...and both settings surfaces' status line follows the credential, because setting a service up is
    // asynchronous (the verify round trip) and the click alone would leave the line a service behind.
    JellyseerrStore::setChangeHook([this] {
        if (requestsStatusUpdate_) requestsStatusUpdate_();
        // A service arriving or leaving changes whether the Request action exists at all, so anything on
        // screen that draws it has to be rebuilt.
        if (home_) home_->refresh();
    });
}

// ==========================================================================================================
// SETTINGS — set up a service, replace its key, or forget it
// ==========================================================================================================
void MainWindow::manageRequestServiceInteractive()
{
    const JellyseerrConfig cur = JellyseerrStore::get();
    if (cur.configured())
    {
        // The address is shown because it is how a person recognises which service this is. THE KEY IS NOT
        // SHOWN, and there is no row here that could reveal it — "replace" asks for a new one rather than
        // offering to edit the old.
        const int action = NavMenu::pick(tr("Request service"),
            { tr("Connected — %1").arg(cur.url),
              tr("🔑 Replace the API key…"),
              tr("🔗 Change the address…"),
              tr("🗑 Forget this service") }, this);
        if (action <= 0) return;                    // Back, or the read-only first row
        if (action == 3)
        {
            const int go = NavConfirm::ask(tr("Forget request service"),
                tr("Forget this request service and its API key? Requests you have already made stay on "
                   "your Requested shelf, but their status can no longer be checked."),
                { tr("Cancel"), tr("Forget") }, /*focusIndex*/ 0, /*cancelIndex*/ 0, this);
            if (go != 1) return;
            JellyseerrStore::clear();
            statusBar()->showMessage(tr("Request service forgotten."), kFeedbackShort);
            return;
        }
        if (action == 1)
        {
            // NEVER ECHOED, NEVER TRIMMED, NEVER LOGGED. Not trimmed because a key's leading and trailing
            // characters are significant and eating them silently produces a service that refuses for a
            // reason nobody can see.
            const QString key = Osk::getText(tr("API key:"), QString(), QLineEdit::Password, this);
            if (key.isEmpty()) return;
            JellyseerrConfig next = cur;
            next.apiKey = key;
            verifyAndSaveRequestService(next);
            return;
        }
        // action == 2 falls through to the address flow below, keeping the key.
    }

    const QString url = Osk::getText(tr("Request service address (https://...):"),
                                     cur.url, QLineEdit::Normal, this).trimmed();
    if (url.isEmpty()) return;                      // covers backed-out (null) too

    bool allowPlainHttp = cur.allowPlainHttp;
    if (jellyseerr::checkUrl(url, false) == jellyseerr::UrlVerdict::InsecureRefused)
    {
        // The explicit choice, phrased as the risk it is, and asked BEFORE the key is stored — the key
        // rides a header on every single call, so plain HTTP puts it on the wire in clear every time.
        // Backing out stores nothing.
        const int go = NavConfirm::ask(tr("Send the API key unencrypted?"),
            tr("That address is plain HTTP, so your API key will be sent over the network unencrypted on "
               "every request. Use https:// instead if your service supports it."),
            { tr("Cancel"), tr("Send unencrypted") }, /*focusIndex*/ 0, /*cancelIndex*/ 0, this);
        if (go != 1) return;
        allowPlainHttp = true;
    }
    if (jellyseerr::checkUrl(url, allowPlainHttp) != jellyseerr::UrlVerdict::Ok)
    {
        NavConfirm::ask(tr("Requests"), tr("That is not a service address."), { tr("OK") }, 0, 0, this);
        return;
    }

    QString key = cur.apiKey;
    if (key.isEmpty() || url != cur.url)
        key = Osk::getText(tr("API key:"), QString(), QLineEdit::Password, this);
    if (key.isEmpty()) return;

    JellyseerrConfig next;
    next.url = url;
    next.apiKey = key;
    next.allowPlainHttp = allowPlainHttp;
    verifyAndSaveRequestService(next);
}

void MainWindow::verifyAndSaveRequestService(const JellyseerrConfig& cfg)
{
    statusBar()->showMessage(tr("Checking the request service…"), kFeedbackShort);
    QPointer<MainWindow> self(this);
    JellyseerrClient::instance().verify(cfg.url, cfg.apiKey, cfg.allowPlainHttp, kVerifyBudgetMs,
                                        [self, cfg](bool ok, const QString& error) {
        if (!self) return;
        // WE ARE INSIDE QNetworkReply::finished's EMISSION. NavConfirm spins a nested event loop, and a
        // nested loop inside an outer emission is the #28 / #211 crash family — so the rest is deferred a
        // turn past it, exactly as the Jellyfin connect flow defers.
        QMetaObject::invokeMethod(self.data(), [self, cfg, ok, error] {
            if (!self) return;
            if (!ok)
            {
                // A CREDENTIAL THAT CANNOT WORK IS NOT STORED. `error` is one of our own sentences — it has
                // never seen the url and could not contain the key.
                NavConfirm::ask(self->tr("Requests"),
                                error.isEmpty() ? self->tr("That service could not be reached.") : error,
                                { self->tr("OK") }, 0, 0, self.data());
                return;
            }
            JellyseerrStore::save(cfg);   // fires the change hook -> both status lines catch up
            // Anything already looked up was looked up against a different service (or none), so those
            // answers are void.
            self->requestAsked_.clear();
            self->requestResolved_.clear();
            self->statusBar()->showMessage(self->tr("Request service set up."), kFeedbackShort);
            self->refreshRequestShelfStatuses();
        }, Qt::QueuedConnection);
    });
}

// ==========================================================================================================
// STATUS, FETCHED ON VIEW
// ==========================================================================================================
void MainWindow::fetchRequestStatus(const MediaItem& item)
{
    RequestBackend* backend = requests::configuredBackend();
    if (!backend || !home_) return;
    const requests::MediaRef ref = requests::refFor(item.id, item.imdbStreamId, item.type);
    if (!ref.ok()) return;
    const QString key = ref.key();
    // AT MOST ONCE PER TITLE PER SESSION. themedDetailData is rebuilt on every re-push of the card, so
    // without this a page that redraws would be a request per redraw — which is the polling the issue
    // explicitly rules out, arriving by accident.
    if (requestAsked_.contains(key)) return;
    requestAsked_.insert(key);

    QPointer<MainWindow> self(this);
    backend->lookup(ref, kStatusBudgetMs, [self, key, ref](const RequestLookup& l) {
        if (!self || !self->home_) return;
        requests::UiState st;
        const requests::ActionKind kind =
            requests::actionFor(ref, /*backendConfigured*/ true, l.ok, l.availability);
        // A failed lookup is UNKNOWN and says so — never a silent "not requested", which would draw a
        // Request button over a title that may well already be on its way.
        st.token = requests::statusToken(l.ok ? l.availability : requests::Availability::Unknown);
        st.label = requests::actionLabel(kind, l.availability);
        st.libraryRef = l.libraryRef;
        st.press = (kind == requests::ActionKind::Request || kind == requests::ActionKind::Unknown
                    || (kind == requests::ActionKind::InLibrary && !l.libraryRef.isEmpty()));
        self->applyRequestState(key, st);
        if (!l.resolvedId.isEmpty()) self->requestResolved_.insert(key, l.resolvedId);
        // A row this profile already owns catches up too, so the shelf and the detail page cannot disagree.
        if (l.ok) RequestStore::setStatus(key, requests::statusToken(l.availability));
        // The reason, once, on the status bar. It is one of our own sentences: no url, no key, no code.
        if (!l.ok && !l.message.isEmpty())
            self->statusBar()->showMessage(l.message, kFeedbackLong);
    });
}

// BOTH SURFACES, from one answer. HomeView owns the classic button; the themed pill lives in a QML
// ActionRow whose `detailData` only this window can re-push (the refreshAfterMetaEdit idiom). Without the
// second half a lookup that lands after the page opened leaves the pill frozen on the state it was drawn
// in before any answer existed — which was found by driving it, not by reading it.
void MainWindow::applyRequestState(const QString& key, const requests::UiState& state)
{
    if (!home_ || key.isEmpty()) return;
    home_->setRequestState(key, state);
    if (themedDetailIndex_ < 0) return;
    // Only when the themed detail page is showing THIS title. The index is checked against the item it
    // currently names rather than trusted, because the browse model can be rebuilt between the fetch and
    // the answer.
    MediaItem shown;
    if (!home_->requestTargetAt(themedDetailIndex_, &shown)) return;
    if (requests::refFor(shown.id, shown.imdbStreamId, shown.type).key() != key) return;
    if (QQuickItem* r = ThemeEngine::rootItem(stack_->currentWidget()))
        r->setProperty("detailData", home_->themedDetailData(themedDetailIndex_));
}

void MainWindow::refreshRequestShelfStatuses()
{
    RequestBackend* backend = requests::configuredBackend();
    if (!backend) return;
    const QString backendId = backend->id();
    QPointer<MainWindow> self(this);
    for (const requests::StoredRequest& r : RequestStore::list())
    {
        // A ROW IS NEVER REFRESHED BY A DIFFERENT BACKEND. Its key means something in the service it was
        // made against, and asking another one about it would file that answer against the wrong request.
        if (!r.backendId.isEmpty() && r.backendId != backendId) continue;
        requests::MediaRef ref;
        ref.mediaType = r.mediaType;
        ref.imdb = r.imdb;
        ref.tmdb = r.tmdb;
        ref.kind = !r.tmdb.isEmpty() ? requests::IdKind::Tmdb
                 : !r.imdb.isEmpty() ? requests::IdKind::Imdb
                                     : requests::IdKind::None;
        if (!ref.ok()) continue;
        const QString key = r.key;
        backend->lookup(ref, kShelfBudgetMs, [self, key](const RequestLookup& l) {
            if (!self) return;
            // A ROW WE COULD NOT REFRESH KEEPS THE LAST THING WE WERE TOLD. Overwriting it with "unknown"
            // because the service happens to be down would turn a shelf full of real statuses into a shelf
            // full of shrugs the moment somebody's box rebooted.
            if (!l.ok) return;
            RequestStore::setStatus(key, requests::statusToken(l.availability));
        });
    }
}

// ==========================================================================================================
// THE PRESS
// ==========================================================================================================
void MainWindow::requestItemInteractive(const MediaItem& item)
{
    RequestBackend* backend = requests::configuredBackend();
    if (!backend)
    {
        NavConfirm::ask(tr("Requests"), requests::failureSentence(requests::Failure::NotConfigured),
                        { tr("OK") }, 0, 0, this);
        return;
    }
    const requests::MediaRef ref = requests::refFor(item.id, item.imdbStreamId, item.type);
    if (!ref.ok()) return;                      // the gate that offered the pill should have stopped this
    const QString key = ref.key();
    const QString title = item.title.isEmpty() ? tr("this title") : item.title;

    // ASK THE SERVICE FIRST, EVERY TIME. Not for tidiness: the on-view answer may be minutes old, somebody
    // else in the household may have requested this in the meantime, and the whole point of the check is
    // that it happens immediately before the ask. It is also what supplies the season list.
    statusBar()->showMessage(tr("Checking “%1”…").arg(title), kFeedbackShort);
    QPointer<MainWindow> self(this);
    backend->lookup(ref, kStatusBudgetMs, [self, item, ref, key, title](const RequestLookup& l) {
        if (!self) return;
        // Deferred past the reply's emission: everything below opens a nested loop (#28 / #211).
        QMetaObject::invokeMethod(self.data(), [self, item, ref, key, title, l] {
            if (!self) return;
            if (!l.ok)
            {
                NavConfirm::ask(self->tr("Requests"),
                                l.message.isEmpty()
                                    ? requests::failureSentence(requests::Failure::Unreachable)
                                    : l.message,
                                { self->tr("OK") }, 0, 0, self.data());
                return;
            }
            if (!l.resolvedId.isEmpty()) self->requestResolved_.insert(key, l.resolvedId);

            // THE ANTI-DUPLICATE BRANCH. Already there: this is not a request, it is a thing to watch.
            if (l.availability == requests::Availability::Available)
            {
                if (!l.libraryRef.isEmpty())
                {
                    const int go = NavConfirm::ask(self->tr("Already in your library"),
                        self->tr("“%1” is already on your server. Open it?").arg(title),
                        { self->tr("Not now"), self->tr("Open") }, /*focusIndex*/ 1, /*cancelIndex*/ 0,
                        self.data());
                    if (go == 1) self->openJellyfinItem(l.libraryRef, item.title, item.thumbnailUrl);
                }
                else
                {
                    NavConfirm::ask(self->tr("Already in your library"),
                        self->tr("“%1” is already on your server.").arg(title),
                        { self->tr("OK") }, 0, 0, self.data());
                }
                return;
            }
            // Already asked for: say so and stop. Pressing again would be the duplicate.
            if (requests::isInFlight(l.availability))
            {
                NavConfirm::ask(self->tr("Already requested"),
                    self->tr("“%1” has already been asked for — %2. It is on your Requested shelf.")
                        .arg(title, requests::availabilityLabel(l.availability).toLower()),
                    { self->tr("OK") }, 0, 0, self.data());
                return;
            }

            QVector<int> seasons;
            if (ref.mediaType == requests::kTv())
            {
                // WHOLE SERIES OR ONE SEASON — the two shapes an *arr stack understands, and the question is
                // only asked when it is a real one. A series whose seasons we cannot enumerate, or one that
                // is entirely absent, is asked for whole.
                QVector<int> offerable;
                for (int s = 1; s <= l.seasonCount; ++s)
                    if (!l.availableSeasons.contains(s)) offerable.push_back(s);
                // The row the item itself names, if it named one (an episode collapsed to its season).
                const bool refSeasonOfferable = ref.season > 0 && offerable.contains(ref.season);

                if (!offerable.isEmpty() && l.seasonCount > 1)
                {
                    QStringList rows;
                    rows << self->tr("Everything that is missing (%n season(s))", nullptr,
                                     int(offerable.size()));
                    if (refSeasonOfferable) rows << self->tr("Season %1 only").arg(ref.season);
                    for (int s : offerable)
                        if (!refSeasonOfferable || s != ref.season)
                            rows << self->tr("Season %1").arg(s);
                    const int pick = NavMenu::pick(title, rows, self.data());
                    if (pick < 0) return;                    // Back: nothing is asked for
                    if (pick > 0)
                    {
                        // Map the picked row back to its season, so only a listed number is ever sent.
                        int idx = 1;
                        if (refSeasonOfferable)
                        {
                            if (pick == 1) seasons = { ref.season };
                            else ++idx;
                        }
                        if (seasons.isEmpty())
                        {
                            int at = idx;
                            for (int s : offerable)
                            {
                                if (refSeasonOfferable && s == ref.season) continue;
                                if (at == pick) { seasons = { s }; break; }
                                ++at;
                            }
                        }
                    }
                    // pick == 0 leaves `seasons` empty, which is "everything".
                }
                else if (refSeasonOfferable && l.seasonCount == 1)
                {
                    seasons = { ref.season };
                }
            }

            // THE CONFIRMATION. It names the title in the sentence, because the one failure mode a press
            // cannot undo is asking for the wrong thing, and it says plainly whose machine does the work.
            const QString what = seasons.isEmpty()
                ? title
                : self->tr("“%1”, season %2").arg(title).arg(seasons.first());
            const int go = NavConfirm::ask(self->tr("Request this?"),
                seasons.isEmpty()
                    ? self->tr("Ask your request service to fetch “%1”? It will be added to your library "
                               "when it arrives.").arg(title)
                    : self->tr("Ask your request service to fetch %1? It will be added to your library "
                               "when it arrives.").arg(what),
                { self->tr("Cancel"), self->tr("Request") }, /*focusIndex*/ 1, /*cancelIndex*/ 0,
                self.data());
            if (go != 1) return;                       // THE ONLY WAY PAST THIS LINE IS A PRESS
            self->submitRequest(item, ref, l.resolvedId, seasons);
        }, Qt::QueuedConnection);
    });
}

// THE ONE PLACE ANYTHING IS SUBMITTED.
void MainWindow::submitRequest(const MediaItem& item, const requests::MediaRef& ref,
                               const QString& resolvedId, const QVector<int>& seasons)
{
    RequestBackend* backend = requests::configuredBackend();
    if (!backend) return;
    const QString title = item.title.isEmpty() ? tr("this title") : item.title;
    const QString key = ref.key();

    RequestSubmission sub;
    sub.ref = ref;
    sub.resolvedId = resolvedId.isEmpty() ? requestResolved_.value(key) : resolvedId;
    sub.seasons = seasons;

    statusBar()->showMessage(tr("Requesting “%1”…").arg(title), kFeedbackShort);
    const QString backendId = backend->id();
    QPointer<MainWindow> self(this);
    backend->submit(sub, kSubmitBudgetMs, [self, item, ref, key, seasons, title, backendId]
                                          (const RequestAck& ack) {
        if (!self) return;
        QMetaObject::invokeMethod(self.data(), [self, item, ref, key, seasons, title, backendId, ack] {
            if (!self) return;
            if (!ack.ok)
            {
                // NO RETRY. The sentence is one of ours; a duplicate rejection says so in its own words and
                // points at the shelf rather than inviting another press.
                NavConfirm::ask(self->tr("Requests"),
                                ack.message.isEmpty()
                                    ? requests::failureSentence(requests::Failure::ServerError)
                                    : ack.message,
                                { self->tr("OK") }, 0, 0, self.data());
                return;
            }
            // The row is written HERE, at the moment the service accepted it — never on a lookup. See
            // RequestStore.h: a row means "this profile asked for this".
            RequestStore::add(rowFor(item, ref, backendId, seasons, ack.availability));
            // ...and the page in front of the user catches up, so the button it was pressed on stops
            // offering a press that would now be a duplicate.
            {
                requests::UiState st;
                st.token = requests::statusToken(ack.availability);
                st.label = requests::actionLabel(
                    requests::actionFor(ref, true, true, ack.availability), ack.availability);
                st.press = false;
                self->applyRequestState(key, st);
            }
            self->statusBar()->showMessage(
                seasons.isEmpty()
                    ? self->tr("Requested “%1”. It is on your Requested shelf.").arg(title)
                    : self->tr("Requested “%1”, season %2. It is on your Requested shelf.")
                          .arg(title).arg(seasons.first()),
                kFeedbackLong);
        }, Qt::QueuedConnection);
    });
}
