#include "DownloadManager.h"
#include "AppBrand.h"
#include "AppPaths.h"
#include "NetHeaderApply.h"
#include "LogSafeText.h"       // issue #231: the ONE definition of a url as it may be LOGGED
#include "NetErrorText.h"      // issue #435: what a failed request may say on screen, and in a log
#include "StoredUrl.h"         // issue #437: the one rule for a stored link that can no longer resume
#include "UrlAtRest.h"         // issue #437: a resumable link as it may sit in queue.json, per platform
#include "DownloadRecipe.h"    // issue #437: an add-on download's #224 re-mint recipe, as a sourceRef

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QFile>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QUrl>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QUuid>

static QString queuePath() { return AppPaths::dataDir() + QStringLiteral("/downloads/queue.json"); }

// One-line append to <app>/stream_debug.log, the same file StreamResolver (srLog) and MainWindow (mwLog)
// write to — including the two redirect refusals that this file, alone of the three NetHeaderApply callers,
// used to record NOWHERE. Local and named `…Log(` on purpose: the proxy-header log-discipline gate matches
// log calls by that shape, so a shared helper under some other name would be a hole in it.
static void dlLog(const QString& msg)
{
    QFile f(AppPaths::dataDir() + QStringLiteral("/stream_debug.log"));
    if (f.open(QIODevice::Append | QIODevice::Text))
        f.write((QDateTime::currentDateTime().toString(Qt::ISODate) + QStringLiteral("  ") + msg + QStringLiteral("\n")).toUtf8());
}

// A log-safe rendering of a URL: scheme://host[:port]/…/<filename>. Drops the path's middle segments (which
// can carry an addon access token) and the query string (which can carry debrid keys), so logs never leak secrets.
// THE RULE ITSELF now lives in core/LogSafeText.h — it was written out identically here, in MainWindow.cpp,
// DownloadManager.cpp and StreamResolver.cpp, and #231 needed a fourth caller. Same rendering, one definition.
static QString logSafeUrl(const QString& url) { return LogSafeText::url(url); }

// The complete length out of a Content-Range field — "bytes 0-99/1234", or "bytes */1234" as a 416 states the
// resource's size — or -1 when the field is absent or the length is unknown ("*"). RFC 7233 §4.2.
static qint64 contentRangeLength(const QByteArray& v)
{
    const int slash = v.lastIndexOf('/');
    if (slash < 0) return -1;
    bool isNum = false;
    const qint64 n = v.mid(slash + 1).trimmed().toLongLong(&isNum);
    return (isNum && n >= 0) ? n : -1;
}

// What a failed plain-link job says once its link has lost its query (#437). The remedy it names is the one
// that works: the item re-resolves a fresh link, and enqueue() de-dups by destination, so that fresh link
// RESUMES the .part rather than starting a second copy. Retry cannot, which is why it is not offered as one.
static QString linkDroppedSentence()
{
    return DownloadManager::tr("this link can't be used again — start the download again from the item");
}

DownloadManager::DownloadManager(QObject* parent) : QObject(parent)
{
    nam_ = new QNetworkAccessManager(this);
    // #437: a queue.json from before the at-rest rules (a plain "url", or a failed job still holding its whole
    // link) is rewritten in today's shape at once, not at the next change that happens to save.
    if (load()) save();
    // Anything that was mid-flight when we last quit is now paused; resume the queue.
    for (DownloadJob& j : jobs_)
        if (j.state == DownloadJob::Active) j.state = DownloadJob::Paused;
    pump();
}

int DownloadManager::indexOf(const QString& id) const
{
    for (int i = 0; i < jobs_.size(); ++i) if (jobs_[i].id == id) return i;
    return -1;
}
int DownloadManager::activeIndex() const { return indexOf(activeId_); }
bool DownloadManager::hasActiveOrQueued() const
{
    for (const DownloadJob& j : jobs_)
        if (j.state == DownloadJob::Active || j.state == DownloadJob::Queued) return true;
    return false;
}

void DownloadManager::enqueue(const DownloadJob& in)
{
    // A job needs SOMETHING to fetch: a url, or a sourceRef the minter can turn into one (#110). A
    // ref-backed job deliberately arrives with an empty url — that is the whole point of it, so testing
    // only `url` here would silently drop every Jellyfin download.
    if ((in.url.isEmpty() && in.sourceRef.isEmpty()) || in.dest.isEmpty()) return;
    // Already downloaded? Report it complete without re-fetching.
    if (QFileInfo::exists(in.dest) && QFileInfo(in.dest).size() > 0)
    {
        DownloadJob done = in; done.state = DownloadJob::Done;
        emit jobCompleted(done);
        return;
    }
    // De-dup: if a job for this destination exists, just make sure it's (re)queued.
    for (DownloadJob& j : jobs_)
        if (j.dest == in.dest)
        {
            // …with THIS resolve's url and headers, not the ones the old job is holding. Both go stale: a
            // debrid link expires, and headers are dropped entirely by a restart (see DownloadJob). Asking
            // the item again is exactly how a user recovers a job that outlived its own credentials, so the
            // fresh answer has to win — otherwise the retry re-sends the dead one and fails identically.
            j.url = in.url;
            j.sourceRef = in.sourceRef;   // #110: the durable half; the url above is empty for these
            j.mintedUrl = in.mintedUrl;   // #437: the link this resolve just minted for that ref, if any
            j.linkDropped = false;        // #437: a fresh link is exactly what a dropped one was waiting for
            j.requestHeaders = in.requestHeaders;
            j.headerGated = !in.requestHeaders.isEmpty();
            if (j.state == DownloadJob::Failed || j.state == DownloadJob::Paused) { j.state = DownloadJob::Queued; j.error.clear(); }
            save(); emit changed(); pump();
            return;
        }
    DownloadJob j = in;
    j.headerGated = !j.requestHeaders.isEmpty(); // the value-free half, and the only half that survives a restart
    if (j.id.isEmpty()) j.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    j.state = DownloadJob::Queued;
    jobs_.push_front(j);
    save(); emit changed();
    pump();
}

void DownloadManager::pump()
{
    if (reply_ || minting_) return;             // one at a time — a job waiting for its link holds the slot
    for (int i = 0; i < jobs_.size(); ++i)
    {
        if (jobs_[i].state != DownloadJob::Queued) continue;
        // #437: a recipe job restored before its minter was installed waits for it (setAsyncUrlMinter pumps),
        // rather than failing for want of a minter and stranding nothing behind it either.
        if (DownloadRecipe::isRef(jobs_[i].sourceRef) && !asyncMinter_ && jobs_[i].mintedUrl.isEmpty()) continue;
        start(i);
        return;
    }
}

void DownloadManager::setAsyncUrlMinter(AsyncUrlMinter minter)
{
    asyncMinter_ = std::move(minter);
    pump();
}

void DownloadManager::failJob(DownloadJob& j, const QString& error)
{
    j.state = DownloadJob::Failed;
    j.error = error;
    j.mintedUrl.clear();
    // #437: A PLAIN LINK THAT CAN NO LONGER RESUME LOSES ITS QUERY AND FRAGMENT NOW. Not at the next save,
    // not when the list is cleared: a token must not outlive the transfer it was issued for, in memory or
    // in queue.json. StoredUrl::location is the one rule for a stored playback link (#200) — no list of
    // credential-shaped parameter names, because no such list can be kept. A ref-backed job has no url.
    if (j.sourceRef.isEmpty() && !j.url.isEmpty())
    {
        const QString kept = StoredUrl::location(j.url);
        if (kept != j.url)
        {
            j.url = kept;
            j.linkDropped = true;
            // One remedy, said once: a reason that already sends the user back to the item (the header-gated
            // one does) is not followed by a second sentence saying the same thing.
            if (error.isEmpty()) j.error = linkDroppedSentence();
            else if (!error.contains(tr("start the download again from the item")))
                j.error = tr("%1; %2").arg(error, linkDroppedSentence());
        }
    }
}


void DownloadManager::start(int idx)
{
    DownloadJob& j = jobs_[idx];
    const bool recipe = DownloadRecipe::isRef(j.sourceRef);
    // A gated job that came back from queue.json has its flag but not its headers (they are deliberately not
    // persisted — see DownloadJob::headerGated). Retrying it would send the request bare and take the 403
    // this whole change exists to stop, and the user would read that as the download being broken twice. Say
    // what actually happened instead; re-downloading the item resolves it afresh and refills the headers.
    //
    // …except a RECIPE job (#437), whose re-mint below answers with the headers declared for the link it
    // mints. That is the very thing this check says a restart loses, so it has nothing to refuse there.
    if (!recipe && j.headerGated && j.requestHeaders.isEmpty())
    {
        failJob(j, tr("this source needs HTTP headers that aren't kept after a restart — start the download "
                      "again from the item"));
        save(); emit changed();
        pump(); // this job is out of the running; don't strand the rest of the queue behind it
        return;
    }
    // #437: AN ADD-ON DOWNLOAD THAT KEPT ITS RECIPE INSTEAD OF ITS LINK. The first transfer uses the link its
    // resolve already minted (mintedUrl, never persisted). Every later one — a resume after a restart, a
    // retry — asks the source again, asynchronously, and holds the slot while it waits. The answer is
    // handed to onMinted, which drops it if the job was paused or cancelled in the meantime.
    if (recipe)
    {
        if (!j.mintedUrl.isEmpty())
        {
            const QString fresh = j.mintedUrl;
            j.mintedUrl.clear();              // one transfer's worth, exactly as a synchronous mint is
            beginTransfer(idx, fresh);
            return;
        }
        if (!asyncMinter_) return;            // pump() does not start one without; kept for any other caller
        j.state = DownloadJob::Active;
        j.requestHeaders.clear();             // declared for the LAST link; the mint brings this one's
        activeId_ = j.id;
        minting_ = true;
        const quint64 gen = ++mintGen_;
        const QString id = j.id;
        const QString ref = j.sourceRef;
        save(); emit changed();
        QPointer<DownloadManager> self(this);
        asyncMinter_(ref, [self, gen, id](const QString& url, const StreamHeaders::Headers& headers) {
            if (self) self->onMinted(gen, id, url, headers);
        });
        return;
    }
    // A REF-BACKED JOB HAS NO URL AND MINTS ONE HERE, once, for this request (#110). This is the site the
    // whole credential design turns on: the link exists between here and the QNetworkRequest below and
    // nowhere else — it is never assigned to `j`, so it cannot reach save(), a log line or a crash dump.
    // See DownloadJob::sourceRef and JellyfinDownload.h.
    QString fetchUrl = j.url;
    if (!j.sourceRef.isEmpty())
    {
        fetchUrl = minter_ ? minter_(j.sourceRef) : QString();
        if (fetchUrl.isEmpty())
        {
            // The honest sentence, built from NOTHING about the request. A server that has been removed or
            // signed out is the ordinary case here, and it is a state the user can fix.
            failJob(j, tr("the server this was downloaded from isn't set up on this device any more — "
                          "sign in again and start the download from the item"));
            save(); emit changed();
            pump(); // this job is out of the running; don't strand the rest of the queue behind it
            return;
        }
    }
    beginTransfer(idx, fetchUrl);
}

// A recipe job's fresh link (#437), or the source's answer that it has none.
void DownloadManager::onMinted(quint64 gen, const QString& id, const QString& url,
                               const StreamHeaders::Headers& headers)
{
    // Abandoned while it was being asked for: paused, cancelled, or the manager moved on. Nothing to do — in
    // particular, nothing to start: whatever holds the slot now got it through pump().
    if (gen != mintGen_ || !minting_ || activeId_ != id) return;
    minting_ = false;
    const int idx = indexOf(id);
    if (idx < 0) { activeId_.clear(); pump(); return; }
    DownloadJob& j = jobs_[idx];
    if (url.isEmpty())
    {
        activeId_.clear();
        // Retry is honest here, unlike a dropped plain link: the recipe is intact, and asking again may well
        // succeed (a debrid release still caching, an add-on briefly unreachable).
        failJob(j, tr("couldn't get a fresh link for this download — the source may no longer have it; retry, "
                      "or start it again from the item"));
        save(); emit changed();
        pump();
        return;
    }
    j.requestHeaders = headers;   // declared for THIS link (#59); never persisted
    beginTransfer(idx, url);
}

void DownloadManager::beginTransfer(int idx, const QString& fetchUrl)
{
    DownloadJob& j = jobs_[idx];
    const QString part = j.dest + QStringLiteral(".part");
    QDir().mkpath(QFileInfo(j.dest).absolutePath());

    // Resume from an existing .part when we have one, else start fresh.
    const qint64 have = QFileInfo::exists(part) ? QFileInfo(part).size() : 0;
    file_ = new QFile(part);
    if (have > 0 && file_->open(QIODevice::Append))
    {
        j.received = have;
        restartOnHeaders_ = true;               // may need to restart if the server ignores our Range
    }
    else
    {
        if (!file_->open(QIODevice::WriteOnly))
        {
            delete file_; file_ = nullptr;
            activeId_.clear();                  // a recipe job held the slot while it was minted (#437)
            failJob(j, tr("Can't write to the downloads folder."));
            save(); emit changed();
            pump(); // this job is out of the running; don't strand the rest of the queue behind it
            return;
        }
        j.received = 0;
        restartOnHeaders_ = false;
    }

    j.state = DownloadJob::Active;
    activeId_ = j.id;

    QNetworkRequest rq{ QUrl(fetchUrl) };
    rq.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(AppBrand::kUserAgent));
    // OUR Range, set before the source's headers and safe from them: parseProxyHeaders refuses a Range from a
    // stream precisely because this request (and the player's seeks) own it.
    if (have > 0) rq.setRawHeader("Range", QByteArray("bytes=") + QByteArray::number(have) + "-");
    // The source's own proxyHeaders, plus the same-origin redirect gate that has to accompany them (#59). A
    // download is the plain-HTTP fetch of the very URL the player would have played: without this a
    // header-gated source plays and does not download, and the 403 reads as a broken download rather than a
    // missing header. Redirect policy is chosen in there too — this used to be NoLessSafe unconditionally,
    // which on a gated job would have re-sent this source's Referer to whatever host it 302'd to.
    //
    // The hook is not decoration either. A refused hop ABORTS the reply, which reaches onFinished() as
    // OperationCanceledError — errorString "Operation canceled", a message that says the USER stopped this.
    // With no hook here (the only one of the three call sites without one) the job failed with that string
    // and nothing reached any log, so a source that plays fine and never downloads was undiagnosable from
    // the field. That is the same class of unexplained failure headerGated exists to remove for the restart
    // case; redirectRefused_ removes it for this one.
    redirectRefused_ = false;
    // Nothing is known about the response yet, and nothing may be carried over from the last one. rangeAsked_
    // records that a 416 arriving below would be an answer about the offset WE named, rather than a range the
    // source itself asked us for.
    headSeen_ = false;
    bodyExpected_ = -1;
    bodyReceived_ = 0;
    rangeAsked_ = have > 0;
    // #437: a re-minted link resuming a .part must name the file the .part is a prefix of. The recorded size
    // is the only fact about that file we kept; onReadyRead compares the 206's stated size against it.
    resumeTotal_ = (DownloadRecipe::isRef(j.sourceRef) && have > 0 && j.total > 0) ? j.total : -1;
    identityMismatch_ = false;
    reply_ = NetHeaderApply::get(nam_, rq, j.requestHeaders, fetchUrl, [this](bool allowed, const QUrl& to) {
        if (allowed)
        {
            dlLog(QStringLiteral("download: same-origin redirect -> %1, headers still apply")
                      .arg(logSafeUrl(to.toString())));
            return;
        }
        redirectRefused_ = true;
        dlLog(QStringLiteral("download: cross-origin redirect -> %1, refusing to carry this source's "
                             "headers there").arg(logSafeUrl(to.toString())));
    });
    connect(reply_, &QNetworkReply::readyRead, this, &DownloadManager::onReadyRead);
    connect(reply_, &QNetworkReply::finished, this, &DownloadManager::onFinished);
    save(); emit changed();
}

void DownloadManager::onReadyRead()
{
    if (!reply_ || !file_) return;
    const int idx = activeIndex();
    if (idx < 0) return;
    DownloadJob& j = jobs_[idx];

    // On the first data, decide whether the server honoured our Range. 206 => resuming; 200 => it's sending the
    // whole file from the top, so truncate what we had and restart the byte count. Before noteResponseHead(),
    // because the size it records is relative to what we are keeping.
    if (restartOnHeaders_)
    {
        restartOnHeaders_ = false;
        const int code = reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (code != 206)
        {
            file_->seek(0); file_->resize(0); j.received = 0;
        }
        else if (resumeTotal_ > 0)
        {
            // #437: a RE-MINTED link continuing our .part. If the file it names is not the size recorded for
            // the one we hold, it is not that file (the imdb route can pick another release), and appending
            // its tail to our head would finish "successfully" as a corrupt file. Stop before a byte of it is
            // written; onFinished starts this download over from the top instead.
            const qint64 whole = contentRangeLength(reply_->rawHeader("Content-Range"));
            if (whole >= 0 && whole != resumeTotal_)
            {
                identityMismatch_ = true;
                reply_->abort();                // -> onFinished, now or later; nothing below may run
                return;
            }
        }
    }
    noteResponseHead();

    const QByteArray chunk = reply_->readAll();
    if (!chunk.isEmpty())
    {
        file_->write(chunk);
        j.received += chunk.size();
        bodyReceived_ += chunk.size();
    }
    emit jobProgress(j.id);
}

// What this response says about its own size, read ONCE.
//
// This used to be two lines inside onReadyRead:
//
//     const qint64 remain = reply_->header(ContentLengthHeader).toLongLong();
//     if (remain > 0) j.total = j.received + remain;
//
// run on every readyRead. Content-Length is the length of this response's body — a constant, stated once in
// the head — and not a countdown of what is left, so each read after the first added bytes that had already
// been counted to the full length a second time and `total` climbed away from the truth as the file arrived.
// A 632168-byte download whose last read came in at 548567 ended up recording 548567 + 632168 = 1180735, and
// finishActive() then read the complete file as a truncated one and refused to finalise it — for good, since
// every retry reproduced the same arithmetic. Only downloads small enough to arrive in a single readyRead
// ever finished. Taking the number once is the whole of the fix.
void DownloadManager::noteResponseHead()
{
    if (headSeen_ || !reply_) return;
    headSeen_ = true;
    const QVariant len = reply_->header(QNetworkRequest::ContentLengthHeader);
    bodyExpected_ = len.isValid() ? len.toLongLong() : -1;

    const int idx = activeIndex();
    if (idx < 0) return;
    DownloadJob& j = jobs_[idx];
    // The size of the WHOLE resource, which is what the progress UI wants. A 206 states it outright in
    // Content-Range; a 200 gives it as the body length after whatever we are keeping. When the response says
    // neither, we do not know it — and 0 is how this job has always spelled "unknown", which the Downloads
    // panel renders as a busy indicator rather than a wrong percentage. Assigned on every branch, never left
    // alone: a number read back out of queue.json must not outlive the response that contradicts it.
    const qint64 whole = contentRangeLength(reply_->rawHeader("Content-Range"));
    j.total = whole >= 0 ? whole : (bodyExpected_ >= 0 ? j.received + bodyExpected_ : 0);
}

void DownloadManager::onFinished()
{
    if (!reply_) return;
    // #437: the re-minted link named a different file than the one our .part belongs to (see onReadyRead).
    if (identityMismatch_)
    {
        identityMismatch_ = false;
        const qint64 stated = contentRangeLength(reply_->rawHeader("Content-Range"));
        reply_->deleteLater(); reply_ = nullptr;
        const int idx = activeIndex();
        // Paused or cancelled in the same breath: that decision stands, and finishActive honours it.
        if (idx < 0 || jobs_[idx].state != DownloadJob::Active) { finishActive(false, QString()); return; }
        dlLog(QStringLiteral("download: the fresh link for %1 names a %2-byte file where %3 bytes were recorded "
                             "— not the file the .part belongs to; starting it over")
                  .arg(QFileInfo(jobs_[idx].dest).fileName()).arg(stated).arg(resumeTotal_));
        restartFromTop(idx, tr("the source now serves a different file for this download"));
        return;
    }
    // A transport-level success (NoError) does NOT mean the download is good: an HTTP 404/403/5xx delivers an
    // error page with NoError, and a dropped connection can end "cleanly" mid-file. Treat a >=400 status, or a
    // body shorter than the length THIS RESPONSE advertised, as a failure so we never record a broken file as
    // complete — see the completeness rule below for why the comparison is against the response and not `total`.
    const int http = reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    // A 416 answering OUR OWN resume Range is not a failed fetch — it is the source telling us there is
    // nothing at or past the offset we asked from, which is a statement about the file already on disk.
    // Handled apart, and only while the job is still running: an abort from Pause or Cancel has already
    // decided this job's fate and must not be overridden by whatever status came back with it.
    {
        const int idx = activeIndex();
        if (rangeAsked_ && http == 416 && idx >= 0 && jobs_[idx].state == DownloadJob::Active)
        { onRangeUnsatisfiable(); return; }
    }
    bool ok = reply_->error() == QNetworkReply::NoError;
    // The reason a user is shown comes from the NetworkError code, the HTTP status and the redirect flag —
    // NEVER from reply_->errorString() (#435). Qt renders an HTTP failure as "Error transferring <url> -
    // server replied: …" with the url whole, and a Jellyfin, Subsonic or Audiobookshelf download url is signed
    // in its query: that string in the Downloads panel was the user's credential on screen.
    //
    // A hop the origin gate refused arrives here as an abort, and Qt renders that as "Operation canceled" —
    // the SAME string the user's own Cancel produces, which is both wrong and the end of the trail. The mapper
    // names the cause instead, ahead of the code. Not persisted (save() writes no error), so this is a message
    // and never a stored fact.
    //
    // Deliberately not discardPart: the refusal happens on the response head, before any content byte, so a
    // .part from an earlier partial download is still good and a retry can still resume from it.
    QString err = ok ? QString() : NetErrorText::forReply(reply_, redirectRefused_);
    // An HTTP error body is an error page, not our file — the .part is garbage and must be discarded so a retry
    // starts clean (a connection drop, by contrast, leaves a valid partial we can resume from).
    bool discardPart = false;
    if (ok && http >= 400)
    { ok = false; err = NetErrorText::sentence(QNetworkReply::NoError, http); discardPart = true; }

    // The response's own account of itself, taken before the reply goes away. Also the only place a response
    // that delivered no body at all gets read, since that raises no readyRead.
    if (ok) noteResponseHead();

    // THE COMPLETENESS RULE, and the reason a whole file used to sit unfinalised. The transport is the
    // authority on how many bytes there are:
    //
    //   * error() == NoError is the CLEAN END, and it is the discriminator — not "the bytes stopped
    //     arriving", which is what the end of every download looks like. Where the response declared a
    //     Content-Length, Qt reports a body that stops short of it as the remote host having closed the
    //     connection, so a drop mid-file never reaches here as NoError. Where it declared none, the body is
    //     delimited by the close and a normal close IS the end of it.
    //   * and where a length was declared, this response's bytes must reach it. Checked here rather than
    //     trusted to Qt alone, and checked against what THIS response said — not against `total`, which can
    //     predate the transfer entirely. That distinction is the bug: a complete download whose recorded
    //     total was larger than anything the source had ever served failed as "stopped before it finished",
    //     identically on every retry, while holding every byte of the file.
    //
    // So a stop that is not a clean end still fails, and still keeps its .part to resume from.
    if (ok && restartOnHeaders_ && bodyReceived_ == 0)
    {
        // We asked to resume and the response ended without delivering a byte, so nothing ever said whether
        // the server was continuing our .part or replacing it. Finalising here would rename a partial file.
        ok = false;
        err = tr("the source sent no data for the rest of this download");
    }
    else if (ok && bodyExpected_ >= 0 && bodyReceived_ < bodyExpected_)
    {
        ok = false;
        err = tr("the download stopped before it finished (%1 of %2 bytes)")
                  .arg(bodyReceived_).arg(bodyExpected_);
    }
    // One line per real failure (not a Pause or Cancel, which have already moved the job out of Active), so a
    // bug report says why. The url through logSafeUrl, Qt's own text through NetErrorText::logText — the same
    // rule, applied to the url wherever Qt put it. Every value goes in through ONE multi-arg arg(): a url is
    // full of "%3A"-style escapes that a chained .arg() would read as placeholders.
    {
        const int idx = activeIndex();
        if (!ok && idx >= 0 && jobs_[idx].state == DownloadJob::Active)
            dlLog(QStringLiteral("download: failed %1 — %2 (network error %3, HTTP %4): %5")
                      .arg(logSafeUrl(reply_->url().toString()), err, QString::number(int(reply_->error())),
                           QString::number(http),
                           reply_->error() == QNetworkReply::NoError ? QStringLiteral("-")
                                                                     : NetErrorText::logText(reply_)));
    }
    reply_->deleteLater(); reply_ = nullptr;
    finishActive(ok, err, discardPart);
}

// A 416 in answer to the resume Range we sent. The source is stating that nothing exists at or past that
// offset and, per RFC 7233, naming the resource's real length while it does so. Two things can be true, and
// both are answerable:
//
//   * the .part already holds exactly that many bytes — the download finished, and had finished before this
//     request was ever sent. Finalise what is on disk; there is nothing left to fetch.
//   * it does not — what we hold is not a prefix of what the source serves any more (the file changed, or an
//     earlier run recorded the wrong length and wrote past the end). Throw it away and fetch from the top.
//
// Neither outcome is "fail with the same error again next time", which is what a job wedged on byte counts
// the source no longer agrees with used to do: it re-sent the same dead Range on every retry, forever. The
// restart terminates by construction rather than by a guard — the .part is gone, so the next start() finds
// nothing to resume, sends no Range, and this path cannot be reached for that job again.
void DownloadManager::onRangeUnsatisfiable()
{
    const qint64 whole = contentRangeLength(reply_->rawHeader("Content-Range"));
    reply_->deleteLater(); reply_ = nullptr;

    const int idx = activeIndex();
    if (idx < 0) { finishActive(false, tr("the source refused to continue this download")); return; }
    const QString part = jobs_[idx].dest + QStringLiteral(".part");
    const qint64 have = QFileInfo::exists(part) ? QFileInfo(part).size() : 0;

    if (whole > 0 && have == whole)
    {
        jobs_[idx].received = have;
        jobs_[idx].total = whole;
        dlLog(QStringLiteral("download: the source reports %1 is %2 bytes and every one of them is already "
                             "here — finalising without re-fetching")
                  .arg(QFileInfo(jobs_[idx].dest).fileName()).arg(whole));
        finishActive(true, QString());
        return;
    }

    dlLog(QStringLiteral("download: the source refused to resume %1 at %2 bytes and reports the file is %3 — "
                         "starting it over")
              .arg(QFileInfo(jobs_[idx].dest).fileName()).arg(have).arg(whole));
    restartFromTop(idx, tr("the download had to start over"));
}

void DownloadManager::restartFromTop(int idx, const QString& why)
{
    const QString id = jobs_[idx].id;
    // The link survives this restart, which is not a failure the user sees: finishActive() marks the job
    // Failed on its way through, and #437's rule would cut the link's query there. Held here and handed back
    // when the job is queued again, below.
    const QString url = jobs_[idx].url;
    jobs_[idx].total = 0;                        // it described a file this source does not serve
    finishActive(false, why, /*discardPart=*/true);
    // finishActive() left the job Failed with its .part removed, and pumped. Queue it again so the restart is
    // the app's work and not the user's — a job that has to re-derive its own byte counts is not something to
    // report as a failure and wait on. If another job took the slot in that pump, this one waits its turn.
    const int i = indexOf(id);
    if (i >= 0 && jobs_[i].state == DownloadJob::Failed)
    {
        jobs_[i].state = DownloadJob::Queued;
        jobs_[i].error.clear();
        jobs_[i].url = url;
        jobs_[i].linkDropped = false;
        save(); emit changed();
        pump();
    }
}

void DownloadManager::finishActive(bool ok, const QString& err, bool discardPart)
{
    const int idx = activeIndex();
    if (file_) { file_->close(); delete file_; file_ = nullptr; }
    if (idx < 0) { activeId_.clear(); pump(); return; }
    DownloadJob& j = jobs_[idx];

    if (!ok)
    {
        // Keep the .part so a retry resumes. If it was paused/cancelled we've already handled the state.
        if (j.state == DownloadJob::Active) failJob(j, err);
        if (discardPart) { QFile::remove(j.dest + QStringLiteral(".part")); j.received = 0; }
    }
    else
    {
        // No second opinion on completeness here. This used to re-test `received < total` and fail the job
        // when it held less than `total` claimed — but `total` is a recorded number that can predate the
        // transfer, and a complete file behind a wrong one failed forever. Whether the stream ended cleanly
        // and reached the length its OWN response declared is decided in onFinished(), against that response.
        QFile::remove(j.dest);
        if (QFile::rename(j.dest + QStringLiteral(".part"), j.dest))
        {
            j.state = DownloadJob::Done; j.error.clear();
            if (j.total <= 0) j.total = j.received;
            emit jobCompleted(j);
        }
        else failJob(j, tr("Couldn't finalize the file."));
    }
    activeId_.clear();
    save(); emit changed();
    pump();
}

void DownloadManager::retry(const QString& id)
{
    const int i = indexOf(id);
    if (i < 0) return;
    // #437: a plain link that lost its query when it failed cannot fetch the file again, so Retry says what
    // can instead of sending a request that would fail the same way (or, worse, fetch an error page).
    if (jobs_[i].state == DownloadJob::Failed && jobs_[i].linkDropped)
    { jobs_[i].error = linkDroppedSentence(); emit changed(); return; }
    if (jobs_[i].state == DownloadJob::Failed || jobs_[i].state == DownloadJob::Paused)
    { jobs_[i].state = DownloadJob::Queued; jobs_[i].error.clear(); save(); emit changed(); pump(); }
}

void DownloadManager::resumeJob(const QString& id) { retry(id); }

void DownloadManager::pauseJob(const QString& id)
{
    const int i = indexOf(id);
    if (i < 0) return;
    if (jobs_[i].id == activeId_ && reply_)
    {
        jobs_[i].state = DownloadJob::Paused;   // set before abort so finishActive() doesn't mark it Failed
        reply_->abort();                        // -> onFinished -> finishActive(false); .part is kept
    }
    else if (jobs_[i].id == activeId_ && minting_)
    {
        // #437: paused while its link was being minted. Abandon the answer (mintGen_), give up the slot.
        jobs_[i].state = DownloadJob::Paused;
        minting_ = false; ++mintGen_; activeId_.clear();
        save(); emit changed();
        pump();
        return;
    }
    else if (jobs_[i].state == DownloadJob::Queued) { jobs_[i].state = DownloadJob::Paused; }
    save(); emit changed();
}

void DownloadManager::cancel(const QString& id)
{
    const int i = indexOf(id);
    if (i < 0) return;
    const QString part = jobs_[i].dest + QStringLiteral(".part");
    if (jobs_[i].id == activeId_ && reply_) { jobs_[i].state = DownloadJob::Paused; reply_->abort(); }
    // #437: cancelled while its link was being minted; the answer, when it comes, belongs to nothing.
    else if (jobs_[i].id == activeId_ && minting_) { minting_ = false; ++mintGen_; activeId_.clear(); }
    // The record goes with the job, link and all: the remove() and save() below are what "a cancelled job
    // keeps no link" means, at the same instant the user asked.
    QFile::remove(part);
    jobs_.remove(indexOf(id));
    save(); emit changed();
    pump();
}

void DownloadManager::removeJob(const QString& id)
{
    const int i = indexOf(id);
    if (i < 0) return;
    if (jobs_[i].state == DownloadJob::Done || jobs_[i].state == DownloadJob::Failed || jobs_[i].state == DownloadJob::Paused)
    { jobs_.remove(i); save(); emit changed(); }
}

void DownloadManager::clearFinished()
{
    for (int i = jobs_.size() - 1; i >= 0; --i)
        if (jobs_[i].state == DownloadJob::Done || jobs_[i].state == DownloadJob::Failed) jobs_.remove(i);
    save(); emit changed();
}

void DownloadManager::save() const
{
    QJsonArray arr;
    for (const DownloadJob& j : jobs_)
    {
        if (j.state == DownloadJob::Done) continue; // completed jobs live in DownloadsStore; don't persist here
        // #437: A PLAIN JOB'S LINK, AS IT MAY SIT IN THIS FILE. (A ref-backed job has none; see below.)
        //   * a job that has FAILED keeps only StoredUrl::location — failJob() already cut it, and this is
        //     the belt for any future path that sets Failed without going through there;
        //   * on Windows the link is SEALED with DPAPI (current user) into "urlp" and "url" stays empty, so
        //     no link of any kind is readable in this file; if sealing fails, nothing is written and the job
        //     comes back unable to resume, rather than the link going out in the clear;
        //   * elsewhere it is written as it is, into a file only its owner can read (see the open below).
        // See core/UrlAtRest.h for why each platform gets what it gets.
        QString plainUrl, sealedUrl;
        if (j.sourceRef.isEmpty() && !j.url.isEmpty())
        {
            const QString u = j.state == DownloadJob::Failed ? StoredUrl::location(j.url) : j.url;
            if (UrlAtRest::sealsAtRest()) sealedUrl = UrlAtRest::seal(u);
            else                          plainUrl = u;
        }
        QJsonObject o{
            { QStringLiteral("id"), j.id }, { QStringLiteral("title"), j.title },
            // #110: THE REF, AND FOR A REF-BACKED JOB NO URL AT ALL. `j.url` is already empty for those —
            // this is not a scrub but a belt beside the braces, so that a future site which did assign one
            // still cannot write it here. A ref is two ids and carries no credential. #437's recipe refs
            // (DownloadRecipe.h) are four ids, and the same rule; `mintedUrl` is never written at all.
            { QStringLiteral("url"), plainUrl },
            { QStringLiteral("ref"), j.sourceRef },
            { QStringLiteral("dest"), j.dest }, { QStringLiteral("kind"), j.kind }, { QStringLiteral("sysId"), j.sysId }, { QStringLiteral("form"), j.form },
            { QStringLiteral("thumb"), j.thumb }, { QStringLiteral("key"), j.key },
            // The FLAG, never the headers. Deliberately not a loop over requestHeaders: this file is not a
            // credential store, and the values are per-request secrets. See DownloadJob::headerGated.
            { QStringLiteral("gated"), j.headerGated },
            { QStringLiteral("record"), j.record },
            { QStringLiteral("received"), j.received }, { QStringLiteral("total"), j.total },
            { QStringLiteral("state"), int(j.state == DownloadJob::Active ? DownloadJob::Paused : j.state) } };
        // Written only when there is something to say, so a ref-backed job's record (Jellyfin, Subsonic,
        // Audiobookshelf) is exactly what it was before #437.
        if (!sealedUrl.isEmpty()) o.insert(QStringLiteral("urlp"), sealedUrl);
        if (j.linkDropped) o.insert(QStringLiteral("dropped"), true);
        arr.append(o);
    }
    QDir().mkpath(QFileInfo(queuePath()).absolutePath());
    QFile f(queuePath());
    // #437: owner-only on POSIX (0600, and an older 0644 file is brought down to it); a plain open on Windows,
    // where the protection is the seal above.
    if (UrlAtRest::openRestricted(f)) f.write(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

bool DownloadManager::load()
{
    bool rewrite = false;
    QFile f(queuePath());
    if (!f.open(QIODevice::ReadOnly)) return false;
    for (const QJsonValue& v : QJsonDocument::fromJson(f.readAll()).array())
    {
        const QJsonObject o = v.toObject();
        DownloadJob j;
        j.id = o.value(QStringLiteral("id")).toString();
        j.title = o.value(QStringLiteral("title")).toString();
        j.sourceRef = o.value(QStringLiteral("ref")).toString();   // #110; absent for every older job
        // #437: the link, sealed ("urlp") or as it is ("url"). A plain "url" on a platform that seals is a
        // queue.json from before #437 — read ONCE, then written back sealed (the caller saves when this
        // returns true). A seal that will not open (another Windows account, another machine, a damaged
        // file) leaves a job that cannot resume, and it says so rather than fetching nothing.
        const QString sealed = o.value(QStringLiteral("urlp")).toString();
        bool unreadable = false;
        if (!sealed.isEmpty())
        {
            j.url = UrlAtRest::unseal(sealed);
            unreadable = j.url.isEmpty();
        }
        else
        {
            j.url = o.value(QStringLiteral("url")).toString();
            if (!j.url.isEmpty() && UrlAtRest::sealsAtRest()) rewrite = true;
        }
        j.linkDropped = o.value(QStringLiteral("dropped")).toBool();
        j.dest = o.value(QStringLiteral("dest")).toString();
        j.kind = o.value(QStringLiteral("kind")).toString();
        j.sysId = o.value(QStringLiteral("sysId")).toString();
        j.form = o.value(QStringLiteral("form")).toString();
        j.thumb = o.value(QStringLiteral("thumb")).toString();
        j.key = o.value(QStringLiteral("key")).toString();
        j.headerGated = o.value(QStringLiteral("gated")).toBool(); // requestHeaders stays empty — that is the point
        // Absent means true: a queue.json from before this field existed describes ordinary downloads, and
        // reading a missing key as false would silently empty the Downloaded folder for everything in flight
        // across exactly one upgrade.
        j.record = o.value(QStringLiteral("record")).toBool(true);
        j.received = o.value(QStringLiteral("received")).toVariant().toLongLong();
        j.total = o.value(QStringLiteral("total")).toVariant().toLongLong();
        j.state = static_cast<DownloadJob::State>(o.value(QStringLiteral("state")).toInt());
        if (j.sourceRef.isEmpty())
        {
            // (An empty link on a plain job is the same case: a seal that failed at save wrote nothing.)
            if (unreadable || j.url.isEmpty())
            {
                j.state = DownloadJob::Failed;
                j.linkDropped = true;
                rewrite = true;
            }
            // A FAILED job written before #437 still holds its whole link. Cut it now, on the way in, the
            // same cut failJob() makes the moment a job fails today.
            else if (j.state == DownloadJob::Failed && !j.url.isEmpty())
            {
                failJob(j, QString());
                if (j.linkDropped) rewrite = true;
            }
            // The error is not persisted, so a dropped job's sentence is restored with the flag that says it.
            if (j.state == DownloadJob::Failed && j.linkDropped) j.error = linkDroppedSentence();
        }
        if (!j.id.isEmpty() && !j.dest.isEmpty()) jobs_.push_back(j);   // url may be empty: see `ref`
    }
    return rewrite;
}
