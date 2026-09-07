// TAKING A JELLYFIN SERVER'S CONTENT ON A PLANE (issue #110, increment 1) — MainWindow's half.
//
// Its own translation unit for the #186 reason MainWindowJellyfin.cpp opens with, and one more: this is the
// second Jellyfin feature TU, and #83's is already a complete thought. Nothing here is about how an item is
// PLAYED FROM A SERVER; everything here is about a copy of it that lives on this disk.
//
// ==========================================================================================================
// THE FOUR THINGS THIS FILE DOES
// ==========================================================================================================
//
// 1. QUEUES A DOWNLOAD, AS AN ORDINARY DownloadManager JOB WITH NO URL IN IT. The job carries `sourceRef` —
//    the qualified id — and DownloadManager mints the link from it at the top of every start(), through the
//    minter installed by initJellyfinDownloads(). JellyfinDownload.h has the whole argument; the short form
//    is that a Jellyfin download url carries the account's access token and queue.json is not a credential
//    store. There is exactly one expression in this feature that evaluates to a url, it is inside
//    JellyfinClient::downloadUrlFor, and its one caller puts it straight into a QNetworkRequest.
//
// 2. PLAYS THE DOWNLOADED FILE INSTEAD OF THE SERVER, WHENEVER THERE IS ONE. openJellyfinItem asks
//    jellyfinLocalCopy() first, before it opens a socket at all — which is what makes a downloaded item work
//    with the aerial unplugged, and is the same prefer-local rule the local library already applies to
//    catalogue rows. The resume comes from THIS DEVICE's store (the server cannot be asked), the segments
//    tier is skipped (same reason), and the id is unchanged — so Recents, favourites and the Downloads
//    shelf all still name the item rather than the file.
//
// 3. QUEUES THE PROGRESS REPORTS THAT PLAYBACK PRODUCES, AND FLUSHES THEM WHEN THE SERVER COMES BACK.
//    OfflineProgress owns the rules (bounded, per server, collapsed to one report per item, and the
//    stale-report rule that is the whole reason this is not a naive replay). This file owns the plumbing:
//    which report goes where, and when a flush is attempted.
//
// 4. CHECKS THE STORAGE CAP AND SUGGESTS. It never deletes. checkJellyfinDownloadCap() shows what an LRU
//    pass would free and the user presses a button; the "remove after watched" toggle is the same shape.
//
// ==========================================================================================================
// WHY THE FLUSH IS TWO ROUND TRIPS PER ITEM
// ==========================================================================================================
// Ask the server what it already knows, then decide, then report. It would be cheaper to just POST the
// queued position — and that cheaper version is the bug the issue names: you watch twenty minutes on the
// plane, you also finish the episode on the television at home, and the naive flush rewinds the server to
// the plane's position on every device you own. The read is what makes the queue safe to have at all.
//
// An UNREACHABLE server ends the flush for that server and leaves everything queued. That is different from
// a server that answered "I have never heard of this item", which is a decision (apply it) — and the two
// arrive as the same empty UserState, which is why JellyfinClient::fetchUserState carries `reachable`
// separately from UserState::ok.
#include "MainWindow.h"

#include "FeedbackPolicy.h"          // kFeedbackShort / kFeedbackLong
#include "nav/NavOverlay.h"          // NavMenu / NavConfirm — the nav kit, never a QDialog
#include "../core/AppPaths.h"
#include "../core/DownloadManager.h"
#include "../core/DownloadsStore.h"
#include "../core/Jellyfin.h"
#include "../core/JellyfinClient.h"
#include "../core/JellyfinDownload.h"
#include "../core/OfflineProgress.h"
#include "../media/PlaybackSession.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStatusBar>
#include <QTimer>

#include <functional>
#include <memory>

namespace {

// The same one-line append to <app>/stream_debug.log MainWindow.cpp's mwLog does, copied for the reason
// MainWindowOpenFail.cpp's copy states: mwLog is a file-static there, and lifting it out would touch the
// busiest file in the tree for no other reason. NOTHING HERE EVER RECEIVES A URL — the only strings this
// file logs are ids, file names and byte counts, and the one expression in the whole feature that produces
// a link lives in JellyfinClient::downloadUrlFor with a single caller that hands it to a request.
void jfdLog(const QString& msg)
{
    QFile f(AppPaths::dataDir() + QStringLiteral("/stream_debug.log"));
    if (f.open(QIODevice::Append | QIODevice::Text))
        f.write((QDateTime::currentDateTime().toString(Qt::ISODate) + QStringLiteral("  ") + msg
                 + QChar(QLatin1Char('\n'))).toUtf8());
}

// The batch fetch's budget. Longer than a browse refresh because the user has explicitly asked for a
// season and is watching a spinner for it, shorter than an open because nothing is about to play.
constexpr int kBatchBudgetMs = 12000;
// The stale-check read, per item, on a flush. Short: an unreachable server must be recognised quickly and
// the whole flush abandoned, not walked item by item into a stall.
constexpr int kFlushBudgetMs = 6000;
// The one-item read that names the file. Short and optional: it decides an extension, not whether the
// download happens, so it must not be something a user waits behind.
constexpr int kFactsBudgetMs = 6000;
// How many unwatched episodes "the next few" means. Offered as a menu rather than typed, because an OSK
// for a number on a television is a worse answer than three rows.
const int kNextNChoices[] = { 1, 3, 5, 10 };

QString downloadsDir() { return AppPaths::dataDir() + QStringLiteral("/downloads"); }

} // namespace

// ---- Wiring ------------------------------------------------------------------------------------------

void MainWindow::initJellyfinDownloads()
{
    if (!dm_) return;
    // THE MINTER. A std::function, so DownloadManager never learns what a media server is and can never
    // reach a token store itself. See DownloadManager::setUrlMinter.
    dm_->setUrlMinter([](const QString& sourceRef) {
        return JellyfinClient::instance().downloadUrlFor(sourceRef);
    });
    // Anything the last session queued and could not deliver. Deferred a turn: the constructor is not a
    // place to start network requests, and a flush that ran before the server store finished loading would
    // find nothing to flush and clear nothing.
    QTimer::singleShot(0, this, [this] { flushJellyfinProgressQueues(); });
}

// ---- What this device already has --------------------------------------------------------------------

QSet<QString> MainWindow::jellyfinDownloadedIds() const
{
    QSet<QString> out;
    // The completed half: the Downloaded folder, keyed by the qualified id (that is what a Jellyfin row's
    // identity IS — Jellyfin::recordedPath). The file is checked, because a store outlives what it names.
    for (const DownloadedItem& d : DownloadsStore::list())
        if (Jellyfin::isQualified(d.key) && !d.path.isEmpty() && QFileInfo::exists(d.path))
            out.insert(d.key);
    // ...and the in-flight half, so a batch pressed twice does not queue the same season twice. Every state
    // counts, including Failed and Paused: those jobs are still in the list and Retry is what they want,
    // not a second identical job beside them.
    if (dm_)
        for (const DownloadJob& j : dm_->jobs())
            if (Jellyfin::isQualified(j.sourceRef)) out.insert(j.sourceRef);
    return out;
}

QString MainWindow::jellyfinLocalCopy(const QString& qualifiedId) const
{
    if (!Jellyfin::isQualified(qualifiedId)) return QString();
    for (const DownloadedItem& d : DownloadsStore::list())
        if (d.key == qualifiedId && !d.path.isEmpty() && QFileInfo::exists(d.path)) return d.path;
    return QString();
}

// ---- Queueing one item -------------------------------------------------------------------------------

void MainWindow::enqueueJellyfinDownload(const Jellyfin::UnionItem& item, const QString& thumb)
{
    if (!dm_ || !Jellyfin::isQualified(item.id)) return;
    if (!jellyfinLocalCopy(item.id).isEmpty())
    { notify(tr("“%1” is already downloaded.").arg(item.title), kFeedbackShort); return; }

    // THE CONTAINER FIRST, so the file is named with the extension it actually has. One small request
    // against the user's own server, and the SAME one the progress flush makes (JellyfinClient::
    // fetchItemFacts), rather than a second endpoint for a second field.
    //
    // A FAILED READ IS NOT A FAILED DOWNLOAD. If the server cannot be asked, the job is queued anyway with
    // the default extension and will resume when it can be: refusing to download something because we could
    // not name it neatly is the wrong trade, and mpv opens a file by its content in any case.
    JellyfinClient::instance().fetchItemFacts(item.id, kFactsBudgetMs,
        [this, item, thumb](const JellyfinClient::ItemFacts& facts, bool) {
            queueJellyfinJob(item, thumb, facts.container);
        });
}

void MainWindow::queueJellyfinJob(const Jellyfin::UnionItem& item, const QString& thumb,
                                  const QString& container)
{
    if (!dm_ || !Jellyfin::isQualified(item.id)) return;
    DownloadJob j;
    j.title = item.title;
    // NO URL. This is the line the whole credential design is about — see the file header and
    // DownloadJob::sourceRef. The link is minted per request, inside DownloadManager::start().
    j.sourceRef = item.id;
    j.dest = downloadsDir() + QStringLiteral("/") + JellyfinDownload::fileNameFor(item, container);
    j.kind = QStringLiteral("video");
    j.thumb = thumb;
    // The KEY IS THE QUALIFIED ID, not a job-local name: it is what DownloadsStore files the finished item
    // under, and therefore what makes the Downloads shelf's row re-open through openJellyfinItem — which is
    // what gives the offline copy its artwork, its resume and its progress reports.
    j.key = item.id;
    dm_->enqueue(j);
    notify(tr("“%1” added to Downloads. See Settings ▸ Downloads for progress.").arg(item.title),
           kFeedbackLong);
    // The ref, never a link: there is no link at this point in the program to log even carelessly.
    jfdLog(QStringLiteral("jfdownload: queued %1 -> %2")
              .arg(Jellyfin::serverOf(item.id), QFileInfo(j.dest).fileName()));
}

void MainWindow::downloadJellyfinItem(const QString& qualifiedId, const QString& title, const QString& thumb)
{
    if (!Jellyfin::isQualified(qualifiedId)) return;
    Jellyfin::UnionItem it;
    it.id = qualifiedId;
    it.title = title;
    enqueueJellyfinDownload(it, thumb);
}

// ---- The batch verbs ---------------------------------------------------------------------------------

void MainWindow::downloadJellyfinBatch(const QString& seriesRef, const QString& seasonRef,
                                       const QString& title)
{
    if (!Jellyfin::isQualified(seriesRef)) return;
    statusBar()->showMessage(tr("Looking up “%1”…").arg(title), kFeedbackShort);

    // ADDRESSED BY THE SERIES, NARROWED BY THE SEASON — which is Jellyfin's own shape
    // (/Shows/<series>/Episodes with a SeasonId filter), and the reason a season row carries both ids. The
    // filter is handed to the SERVER rather than applied to a full-series answer here: a long-running show
    // is a thousand rows, and downloading all of them to keep twelve is a request nobody asked for.
    JellyfinClient::instance().fetchEpisodes(seriesRef, seasonRef, kBatchBudgetMs,
        [this, title](const QVector<Jellyfin::UnionItem>& episodes, const QString& error) {
            if (episodes.isEmpty())
            {
                notify(error.isEmpty() ? tr("“%1” has no episodes to download.").arg(title) : error,
                       kFeedbackLong);
                return;
            }
            // A NESTED EVENT LOOP UNDER A REPLY'S EMISSION IS THE #28/#211 FAMILY, and NavMenu::pick spins
            // one. Deferred past this delivery, exactly as every other menu opened from a callback is.
            deferPastQmlEmission([this, title, episodes] {
                const QSet<QString> have = jellyfinDownloadedIds();
                const QVector<Jellyfin::UnionItem> season = JellyfinDownload::seasonBatch(episodes, have);

                QStringList rows;
                rows << tr("Download all %n missing episode(s)", "", int(season.size()));
                for (const int n : kNextNChoices)
                    rows << tr("Download the next %n unwatched", "", n);
                const int pick = NavMenu::pick(tr("Download “%1”").arg(title), rows, this);
                if (pick < 0) return;

                const QVector<Jellyfin::UnionItem> batch =
                    pick == 0 ? season
                              : JellyfinDownload::nextUnwatched(episodes, kNextNChoices[pick - 1], have);
                if (batch.isEmpty())
                {
                    // NOT AN ERROR AND SAID AS ONE SENTENCE. "Nothing happened" after pressing Download is
                    // the failure this app has shipped before; the two reasons a batch is empty (all
                    // watched, all already here) are both worth naming.
                    notify(tr("Nothing left to download for “%1” — everything is either watched or "
                              "already on this device.").arg(title), kFeedbackLong);
                    return;
                }
                for (const Jellyfin::UnionItem& e : batch) enqueueJellyfinDownload(e, QString());
                notify(tr("Queued %n episode(s) from “%1”.", "", int(batch.size())).arg(title),
                       kFeedbackLong);
                checkJellyfinDownloadCap();
            });
        });
}

// ---- Progress: the one report site -------------------------------------------------------------------

void MainWindow::reportJellyfinProgress(const QString& qualifiedId, Jellyfin::ProgressEvent ev,
                                        double seconds, const QString& playSessionId,
                                        const QString& mediaSourceId)
{
    if (!Jellyfin::isQualified(qualifiedId)) return;
    if (!jellyfinPlayingOffline_)
    {
        // Streaming from the server: it is right there, and this is the path #83 has always taken.
        JellyfinClient::instance().reportProgress(qualifiedId, ev, seconds, playSessionId, mediaSourceId);
        return;
    }
    OfflineProgress::Report r;
    r.qualifiedId     = qualifiedId;
    r.positionSeconds = seconds;
    r.ev              = int(ev);
    r.playSessionId   = playSessionId;
    r.mediaSourceId   = mediaSourceId;
    r.whenMs          = QDateTime::currentMSecsSinceEpoch();
    OfflineProgress::enqueue(r);
    // A Stop is the moment the viewing is a finished fact, so it is the natural moment to try the server.
    // Online, this makes an offline-played item behave exactly like a streamed one; offline it costs one
    // request that fails and changes nothing.
    if (ev == Jellyfin::ProgressEvent::Stop) flushJellyfinProgressQueues();
}

void MainWindow::flushJellyfinProgressQueues()
{
    const QStringList servers = OfflineProgress::serversWithPending();
    for (const QString& serverId : servers)
    {
        const QVector<OfflineProgress::Report> rows = OfflineProgress::collapse(
            OfflineProgress::pending(serverId));
        if (rows.isEmpty()) { OfflineProgress::clearServer(serverId); continue; }
        // ONE ITEM AT A TIME, IN ORDER, EACH GATED ON WHAT THE SERVER ALREADY KNOWS. Walked with a
        // self-scheduling lambda rather than a loop because every step is a round trip: a loop would fire
        // every read at once and then apply the answers in whatever order they came back, which is exactly
        // the ordering the queue exists to preserve.
        auto step = std::make_shared<std::function<void(int)>>();
        *step = [this, serverId, rows, step](int i) {
            if (i >= int(rows.size())) return;
            const OfflineProgress::Report r = rows.at(i);
            JellyfinClient::instance().fetchUserState(r.qualifiedId, kFlushBudgetMs,
                [this, serverId, rows, step, i, r](const Jellyfin::UserState& state, bool reachable) {
                    if (!reachable)
                    {
                        // THE SERVER IS NOT THERE. Nothing is decided and nothing is dropped: the whole
                        // remaining queue for this server stays exactly as it is, for the next attempt.
                        return;
                    }
                    if (OfflineProgress::shouldApply(r, state))
                        JellyfinClient::instance().reportProgress(
                            r.qualifiedId, static_cast<Jellyfin::ProgressEvent>(r.ev), r.positionSeconds,
                            r.playSessionId, r.mediaSourceId);
                    // Handed over OR decided against — either way this report is finished with. Leaving a
                    // dropped one queued would re-ask the server about it on every flush for ever.
                    QVector<OfflineProgress::Report> left = OfflineProgress::pending(serverId);
                    for (int k = left.size() - 1; k >= 0; --k)
                        if (left[k].qualifiedId == r.qualifiedId && left[k].whenMs <= r.whenMs)
                            left.remove(k);
                    OfflineProgress::replace(serverId, left);
                    (*step)(i + 1);
                });
        };
        (*step)(0);
    }
}

// ---- The cap: a suggestion, never a deletion ---------------------------------------------------------

void MainWindow::checkJellyfinDownloadCap()
{
    const int capGb = JellyfinDownload::capGb();
    if (capGb <= 0) return;                      // no cap set: nothing to say, and nothing to nag about

    QVector<JellyfinDownload::StoredItem> items;
    for (const DownloadedItem& d : DownloadsStore::list())
    {
        if (!Jellyfin::isQualified(d.key)) continue;
        const QFileInfo fi(d.path);
        if (!fi.exists()) continue;
        JellyfinDownload::StoredItem s;
        s.qualifiedId  = d.key;
        s.path         = d.path;
        s.bytes        = fi.size();
        s.downloadedMs = fi.birthTime().isValid() ? fi.birthTime().toMSecsSinceEpoch()
                                                  : fi.lastModified().toMSecsSinceEpoch();
        s.lastPlayedMs = fi.lastRead().isValid() ? fi.lastRead().toMSecsSinceEpoch() : 0;
        items.push_back(s);
    }
    const JellyfinDownload::CapVerdict v =
        JellyfinDownload::evictionSuggestion(items, qint64(capGb) * JellyfinDownload::kBytesPerGb);
    if (!v.over || v.victims.isEmpty()) return;

    // A LINE, NOT A DELETION, AND NOT A MODAL EITHER. The user has just asked for a download; interrupting
    // that with a card they must dismiss is how a helpful warning becomes the thing people turn off. The
    // Downloads settings surface is where the list lives and where removing one is a deliberate act.
    notify(tr("Downloads are using %1 GB of your %2 GB limit. %n item(s) could be freed — see "
              "Settings ▸ Downloads.", "", int(v.victims.size()))
               .arg(double(v.usedBytes) / double(JellyfinDownload::kBytesPerGb), 0, 'f', 1)
               .arg(capGb),
           kFeedbackLong);
    jfdLog(QStringLiteral("jfdownload: over cap (%1 of %2 bytes), %3 eviction candidate(s), deleting none")
              .arg(v.usedBytes).arg(v.capBytes).arg(v.victims.size()));
}
