// TAKING AN AUDIOBOOKSHELF BOOK ON A PLANE (issue #197, offline listening) — MainWindow's half.
//
// Its own translation unit for the #186 reason every feature TU gives: MainWindow.cpp is the file every
// concurrent branch collides in. Everything that DECIDES something is in core/AbsDownload.h (which files, in
// what order, called what, the manifest, the file ref, the removal rule) and core/AbsProgressQueue.h (what a
// position kept offline is, which one wins on open); probe_absclient drives both against a fixture server.
// What is left here is plumbing, in the shape MainWindowJellyfinDownload.cpp has for a Jellyfin season and
// MainWindowSubsonicDownload.cpp for an album:
//
//   1. DOWNLOAD — fetch the expanded item (its `audioFiles` are the file list), write the book's manifest,
//      and hand DownloadManager one ordinary job per file whose `sourceRef` is a file ref and whose url is
//      EMPTY. The link is minted per request by the one minter initJellyfinDownloads installs.
//   2. RECORD — when the LAST file of a book lands, record the book once, as one ordinary Downloaded item
//      keyed by its qualified id (AbsDownload::completedBook).
//   3. OPEN — openAbsItem asks openAbsDownloaded first: a downloaded book plays from its files, through the
//      same session machinery a streamed one uses (AbsClient::adoptLocalSession), and where it opens is
//      AbsProgressQueue::pickOnOpen's decision (the server wins, unless this device's kept position is newer).
//   4. REMOVE — the files, the Downloads row and the kept position, after a nav-kit confirmation. Never the
//      server's copy.
//
// NOTHING HERE HOLDS A URL. The log lines name server ids, item ids, file counts and folder names.
#include "MainWindow.h"

#include "FeedbackPolicy.h"          // kFeedbackShort / kFeedbackLong
#include "nav/NavOverlay.h"          // NavConfirm — the nav kit, never a QDialog
#include "../core/AbsClient.h"
#include "../core/AbsDownload.h"
#include "../core/AbsProgressQueue.h"
#include "../core/AppPaths.h"
#include "../core/DownloadManager.h"
#include "../core/DownloadsStore.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStatusBar>
#include <QTimer>

namespace {

// The one-line append to <app>/stream_debug.log, copied for the reason MainWindowJellyfinDownload.cpp's jfdLog
// gives. NOTHING HERE EVER RECEIVES A URL.
void absdLog(const QString& msg)
{
    QFile f(AppPaths::dataDir() + QStringLiteral("/stream_debug.log"));
    if (f.open(QIODevice::Append | QIODevice::Text))
        f.write((QDateTime::currentDateTime().toString(Qt::ISODate) + QStringLiteral("  ") + msg
                 + QChar(QLatin1Char('\n'))).toUtf8());
}

QString downloadsDir() { return AppPaths::dataDir() + QStringLiteral("/downloads"); }

// How long opening a downloaded book waits for the server's position before deciding without it. Short: the
// book is on this disk and plays either way, and a server that has not answered in this long is not the one
// the listener is waiting on.
constexpr int kOpenBudgetMs = 4000;
// How often positions kept offline are offered to their server again while any are owed (see initAbsDownloads).
constexpr int kRetryMs = 30000;

// The jobs DownloadManager holds for one book (any state), by their file refs.
QVector<DownloadJob> jobsOfBook(const DownloadManager* dm, const QString& qualifiedId)
{
    QVector<DownloadJob> out;
    if (!dm) return out;
    for (const DownloadJob& j : dm->jobs())
    {
        const AbsDownload::FileRef r = AbsDownload::parseFileRef(j.sourceRef);
        if (r.ok && r.qualifiedBookId == qualifiedId) out.push_back(j);
    }
    return out;
}

// Re-draw the Audiobookshelf level the user is standing in (the book level's Download row becomes Remove and
// back). The browse surface already repaints on this signal; it is emitted, not invented.
void repaintAbsLevel(const QString& qualifiedId)
{
    emit AbsClient::instance().cacheChanged(Abs::serverOf(qualifiedId));
}

} // namespace

// ---- Wiring ------------------------------------------------------------------------------------------

void MainWindow::initAbsDownloads()
{
    if (!dm_) return;
    // A FINISHED FILE THAT COMPLETES ITS BOOK. The file jobs are intermediates (record == false), so the
    // generic handler files nothing for them; the BOOK is filed here, once, as the one Downloaded item.
    connect(dm_, &DownloadManager::jobCompleted, this, [this](const DownloadJob& j) {
        if (!AbsDownload::isFileRef(j.sourceRef)) return;
        DownloadedItem rec;
        if (!AbsDownload::completedBook(j, &rec)) return;
        const bool already = !AbsDownload::localManifest(rec.key, DownloadsStore::list()).isEmpty();
        // The cover the book was downloaded with, else the one MetaCache holds for it — a local file either
        // way (AbsClient::coverPath never falls back to the server's url, which carries the token).
        if (rec.thumb.isEmpty()) rec.thumb = AbsClient::instance().coverPath(rec.key);
        DownloadsStore::add(rec);
        if (already) return;   // a re-queued file of a book that was already whole: nothing new to say
        const AbsDownload::Manifest m = AbsDownload::readManifest(rec.path);
        notify(tr("Downloaded “%1” — it plays without the server now.").arg(rec.title), kFeedbackLong);
        absdLog(QStringLiteral("absdownload: %1 complete — %2 file(s) in \"%3\", recorded as one Downloaded book")
                    .arg(rec.key).arg(m.plan.files.size()).arg(m.plan.folderName));
        repaintAbsLevel(rec.key);
    });
    // The queue's own account of itself (flushes, drops, the open decision), for the log. Ids and seconds.
    connect(&AbsClient::instance(), &AbsClient::offlineNote, this, [](const QString& line) { absdLog(line); });
    // Anything the last session kept and could not deliver. Deferred a turn, for initJellyfinDownloads' reason:
    // the constructor is not a place to start network requests.
    QTimer::singleShot(0, this, [] {
        for (const QString& serverId : AbsProgressQueue::serversWithUnsent())
            AbsClient::instance().flushOfflineProgress(serverId);
    });
    // ...AND AGAIN, NOW AND THEN, WHILE ANYTHING IS STILL OWED. "The next successful request" is the moment the
    // server is back — but the browse levels are cached, so a listener who comes home and does not happen to
    // open anything new would make no request at all, and the position kept on the plane would sit here until
    // the app restarts. The first live drive of this feature showed exactly that. So while a server is owed
    // anything, the flush itself is tried every kRetryMs: one bounded GET per owed book, which fails at once
    // against a server that is still off and changes nothing. With nothing owed the tick does nothing at all.
    auto* retry = new QTimer(this);
    retry->setInterval(kRetryMs);
    connect(retry, &QTimer::timeout, this, [] {
        for (const QString& serverId : AbsProgressQueue::serversWithUnsent())
            AbsClient::instance().flushOfflineProgress(serverId);
    });
    retry->start();
}

// ---- 1. Download ---------------------------------------------------------------------------------------

void MainWindow::downloadAbsBook(const QString& qualifiedId)
{
    if (!dm_ || !Abs::isQualified(qualifiedId)) return;
    if (!AbsDownload::localManifest(qualifiedId, DownloadsStore::list()).isEmpty())
    { notify(tr("That book is already on this device."), kFeedbackShort); return; }
    for (const DownloadJob& j : jobsOfBook(dm_, qualifiedId))
        if (j.state != DownloadJob::Done)
        {
            notify(tr("That book is already downloading. See Settings ▸ Downloads for progress."), kFeedbackLong);
            return;
        }

    statusBar()->showMessage(tr("Looking up the book's files…"), kFeedbackShort);
    // THE EXPANDED ITEM, FRESH — its `media.audioFiles` is the file list, in the server's order, with each
    // file's inode and length. Fetched even when the book level has it cached: a download is a promise about
    // the files the server holds NOW.
    AbsClient::instance().fetchItem(qualifiedId, [this, qualifiedId](const AbsClient::Result& r) {
        if (!r.ok)
        {
            // The client's own sentence — a transport one of ours or the server's. Never a request.
            notify(r.message.isEmpty() ? tr("Couldn't read that book from the audiobook server.") : r.message,
                   kFeedbackLong);
            return;
        }
        const AbsDownload::Plan plan = AbsDownload::planFor(qualifiedId, AbsClient::instance().item(qualifiedId));
        if (!plan.ok)
        {
            notify(tr("That book has no audio files this app can download."), kFeedbackLong);
            return;
        }
        const QString folder = AbsDownload::folderFor(plan, downloadsDir());
        if (!QDir().mkpath(folder))
        { notify(tr("Couldn't write to the downloads folder."), kFeedbackLong); return; }

        // THE COVER, as a file in the book's folder — the bytes MetaCache already holds (never the server's
        // url, which carries the token). A book whose cover has not landed yet simply has none of its own;
        // its Downloaded row then shows MetaCache's copy, which is pinned for downloaded items.
        QString coverName;
        const QString cached = AbsClient::instance().coverPath(qualifiedId);
        if (!cached.isEmpty())
        {
            const QString suffix = QFileInfo(cached).suffix().isEmpty() ? QStringLiteral("jpg")
                                                                        : QFileInfo(cached).suffix();
            coverName = QStringLiteral("cover.") + suffix;
            QFile::remove(folder + QLatin1Char('/') + coverName);
            if (!QFile::copy(cached, folder + QLatin1Char('/') + coverName)) coverName.clear();
        }

        // THE MANIFEST FIRST — before a byte is fetched, because it is how a finished file finds its book, and
        // how the book keeps its chapters and every part's length when the server is gone.
        QFile mf(folder + QLatin1Char('/') + AbsDownload::manifestName());
        if (!mf.open(QIODevice::WriteOnly | QIODevice::Truncate)
            || mf.write(AbsDownload::manifestJson(plan, coverName)) < 0)
        { notify(tr("Couldn't write to the downloads folder."), kFeedbackLong); return; }
        mf.close();

        const QString thumb = coverName.isEmpty() ? QString() : folder + QLatin1Char('/') + coverName;
        const QVector<DownloadJob> jobs = AbsDownload::jobsFor(plan, downloadsDir(), thumb);
        if (jobs.isEmpty()) { notify(tr("That book has no audio files this app can download."), kFeedbackLong); return; }
        for (const DownloadJob& j : jobs) dm_->enqueue(j);   // a file already on disk completes synchronously

        // THIS DEVICE'S LAST KNOWN POSITION, seeded from the server now, while it can be asked — so a book
        // downloaded at home and opened on the plane starts where the listener was, not at chapter one.
        // adoptServer never overwrites a newer position this device still owes the server.
        AbsClient::instance().fetchProgress(qualifiedId, [qualifiedId](const AbsClient::Result&, const Abs::Progress& p) {
            if (p.found) AbsProgressQueue::adoptServer(qualifiedId, p);
        });

        notify(tr("“%1” added to Downloads — %n file(s). See Settings ▸ Downloads for progress.", "",
                  int(jobs.size())).arg(plan.title), kFeedbackLong);
        absdLog(QStringLiteral("absdownload: queued %1 — %2 file(s), %3 s -> \"%4\"")
                    .arg(qualifiedId).arg(jobs.size()).arg(plan.duration, 0, 'f', 1).arg(plan.folderName));
    });
}

// ---- 3. Open --------------------------------------------------------------------------------------------

bool MainWindow::openAbsDownloaded(const QString& qualifiedId, int startPart)
{
    // THE ONE PREFER-LOCAL RULE (#417's, now PreferLocal.h), behind the Audiobookshelf gate.
    const QString manifestPath = AbsDownload::localManifest(qualifiedId, DownloadsStore::list());
    if (manifestPath.isEmpty()) return false;
    const AbsDownload::Manifest m = AbsDownload::readManifest(manifestPath);
    if (!m.ok || !AbsDownload::isComplete(m))
    {
        // A copy that has lost a file is not a book. The server can still play it, so it is asked instead.
        absdLog(QStringLiteral("absdownload: %1 — the downloaded copy is incomplete; opening from the server")
                    .arg(qualifiedId));
        return false;
    }
    const Abs::Session s = AbsDownload::localSession(m);
    AbsClient::instance().adoptLocalSession(qualifiedId, s);
    statusBar()->showMessage(tr("Opening from this device…"), 4000);
    absdLog(QStringLiteral("absdownload: opening %1 from this device — %2 file(s), %3 s")
                .arg(qualifiedId).arg(s.tracks.size()).arg(s.duration, 0, 'f', 1));

    // WHERE. The server is asked — briefly — and still wins, unless this device's kept position is newer
    // (then it is sent first and used). No answer: this device's last known position.
    //
    // `gen` for playRemoteBookPart's reason: a film started while the server was being asked must not be
    // replaced by this book when the answer lands.
    //
    // ...and `ask` for the other race: a SECOND open of a downloaded book pressed while the first is still
    // asking (a double press was enough, on the first live drive). Both answers land a moment apart, before
    // either has started anything, so `gen` passes for both and the book was opened twice — the second open
    // restarting the part the first had just seeked into. Only the newest ask may start the book.
    static quint64 s_lastAsk = 0;
    const quint64 ask = ++s_lastAsk;
    const quint64 gen = remoteBookGen_;
    const QString cover = m.coverFile;
    AbsClient::instance().resolveOpenPosition(qualifiedId, kOpenBudgetMs,
        [this, qualifiedId, s, startPart, gen, ask, cover](const AbsProgressQueue::OpenPick& pick) {
            if (ask != s_lastAsk)
            {
                absdLog(QStringLiteral("absdownload: %1 — a newer open was asked for; this one is dropped")
                            .arg(qualifiedId));
                return;
            }
            if (gen != remoteBookGen_)
            {
                absdLog(QStringLiteral("absdownload: %1 — something else started while the server was asked; "
                                       "not opening").arg(qualifiedId));
                return;
            }
            startAbsBook(qualifiedId, s, startPart, pick.position, cover);
        });
    return true;
}

// ---- 4. Remove ------------------------------------------------------------------------------------------

void MainWindow::removeAbsDownload(const QString& qualifiedId)
{
    if (!Abs::isQualified(qualifiedId)) return;
    QString manifestPath = AbsDownload::localManifest(qualifiedId, DownloadsStore::list());
    const QVector<DownloadJob> jobs = jobsOfBook(dm_, qualifiedId);
    // A book still downloading has no Downloads row yet, but it does have a folder and a manifest.
    if (manifestPath.isEmpty() && !jobs.isEmpty())
        manifestPath = QFileInfo(jobs.first().dest).absolutePath() + QLatin1Char('/') + AbsDownload::manifestName();
    if (manifestPath.isEmpty())
    { notify(tr("That book isn't downloaded on this device."), kFeedbackShort); return; }
    const AbsDownload::Manifest m = AbsDownload::readManifest(manifestPath);
    const QString title = m.ok && !m.plan.title.isEmpty() ? m.plan.title : tr("this book");

    // A NESTED EVENT LOOP UNDER AN EMISSION IS THE #28/#211 FAMILY, and NavConfirm::ask spins one. This is
    // reached from a browse activation; deferred a turn, exactly as the Jellyfin remove offer is.
    deferPastQmlEmission([this, qualifiedId, manifestPath, title] {
        const int go = NavConfirm::ask(
            tr("Remove the download?"),
            tr("Remove this device's copy of “%1”?\n\nIt stays on your audiobook server — only the copy on this "
               "device is deleted.").arg(title),
            { tr("Remove"), tr("Keep") }, /*focusIndex=*/1, /*cancelIndex=*/1, this);
        if (go != 0) return;   // KEEP (and Back): nothing is touched

        // Its files cannot be deleted from under the player on every platform, and a book playing from a
        // folder that is being emptied is not a state worth having. Said, not guessed around.
        if (AbsClient::instance().isLocalSession(qualifiedId) && absBookPlaying() && absBookId_ == qualifiedId)
        {
            notify(tr("“%1” is playing from this device. Stop it first, then remove the download.").arg(title),
                   kFeedbackLong);
            return;
        }
        // The jobs first — an in-flight file must not land in a folder that has just been removed.
        if (dm_)
            for (const DownloadJob& j : jobsOfBook(dm_, qualifiedId))
            {
                if (j.state == DownloadJob::Done || j.state == DownloadJob::Failed) dm_->removeJob(j.id);
                else dm_->cancel(j.id);
            }

        using R = AbsDownload::Removal;
        const R outcome = AbsDownload::removeBook(qualifiedId, manifestPath, downloadsDir());
        switch (outcome)
        {
            case R::RefusedOutsideDownloads:
                notify(tr("“%1” is not in this app's downloads folder, so nothing was removed.").arg(title),
                       kFeedbackLong);
                absdLog(QStringLiteral("absdownload: refused to remove a folder outside the downloads folder for %1; "
                                       "nothing touched").arg(qualifiedId));
                return;
            case R::DeleteFailed:
                notify(tr("Couldn't remove the downloaded copy of “%1”. It is still on this device.").arg(title),
                       kFeedbackLong);
                absdLog(QStringLiteral("absdownload: delete failed for %1 — the Downloads entry is unchanged")
                            .arg(qualifiedId));
                return;
            case R::Removed:
            case R::AlreadyGone:
                break;
        }
        notify(outcome == R::Removed ? tr("Removed the downloaded copy of “%1”.").arg(title)
                                     : tr("“%1” was already gone from this device.").arg(title),
               kFeedbackLong);
        absdLog(QStringLiteral("absdownload: removed the downloaded copy of %1, its Downloads entry and its kept "
                               "position; the server's copy is untouched").arg(qualifiedId));
        repaintAbsLevel(qualifiedId);
    });
}
