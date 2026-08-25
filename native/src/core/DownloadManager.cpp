#include "DownloadManager.h"
#include "AppBrand.h"
#include "AppPaths.h"
#include "NetHeaderApply.h"

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
static QString logSafeUrl(const QString& url)
{
    const QUrl u(url);
    if (u.scheme().isEmpty()) return QFileInfo(url).fileName(); // a local path
    const QString file = QFileInfo(u.path()).fileName();
    return u.scheme() + QStringLiteral("://") + u.host()
         + (u.port() > 0 ? QStringLiteral(":") + QString::number(u.port()) : QString())
         + QStringLiteral("/…/") + file;
}

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

DownloadManager::DownloadManager(QObject* parent) : QObject(parent)
{
    nam_ = new QNetworkAccessManager(this);
    load();
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
    if (in.url.isEmpty() || in.dest.isEmpty()) return;
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
    if (reply_) return;                         // one at a time
    for (int i = 0; i < jobs_.size(); ++i)
        if (jobs_[i].state == DownloadJob::Queued) { start(i); return; }
}

void DownloadManager::start(int idx)
{
    DownloadJob& j = jobs_[idx];
    // A gated job that came back from queue.json has its flag but not its headers (they are deliberately not
    // persisted — see DownloadJob::headerGated). Retrying it would send the request bare and take the 403
    // this whole change exists to stop, and the user would read that as the download being broken twice. Say
    // what actually happened instead; re-downloading the item resolves it afresh and refills the headers.
    if (j.headerGated && j.requestHeaders.isEmpty())
    {
        j.state = DownloadJob::Failed;
        j.error = tr("this source needs HTTP headers that aren't kept after a restart — start the download "
                     "again from the item");
        save(); emit changed();
        pump(); // this job is out of the running; don't strand the rest of the queue behind it
        return;
    }
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
            j.state = DownloadJob::Failed; j.error = tr("Can't write to the downloads folder.");
            save(); emit changed();
            pump(); // this job is out of the running; don't strand the rest of the queue behind it
            return;
        }
        j.received = 0;
        restartOnHeaders_ = false;
    }

    j.state = DownloadJob::Active;
    activeId_ = j.id;

    QNetworkRequest rq{ QUrl(j.url) };
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
    reply_ = NetHeaderApply::get(nam_, rq, j.requestHeaders, j.url, [this](bool allowed, const QUrl& to) {
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
    QString err = ok ? QString() : reply_->errorString();
    // A hop the origin gate refused arrives here as an abort, and Qt renders that as "Operation canceled" —
    // the SAME string the user's own Cancel produces, which is both wrong and the end of the trail. Name the
    // cause instead. Not persisted (save() writes no error), so this is a message and never a stored fact.
    //
    // Deliberately not discardPart: the refusal happens on the response head, before any content byte, so a
    // .part from an earlier partial download is still good and a retry can still resume from it.
    if (!ok && redirectRefused_)
        err = tr("this source sent the download on to a different site, and the HTTP headers it needs are "
                 "not sent there");
    // An HTTP error body is an error page, not our file — the .part is garbage and must be discarded so a retry
    // starts clean (a connection drop, by contrast, leaves a valid partial we can resume from).
    bool discardPart = false;
    if (ok && http >= 400) { ok = false; err = tr("the source returned HTTP %1").arg(http); discardPart = true; }

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
    jobs_[idx].total = 0;                        // it described a file this source does not serve
    finishActive(false, tr("the download had to start over"), /*discardPart=*/true);
    // finishActive() left the job Failed with its .part removed, and pumped. Queue it again so the restart is
    // the app's work and not the user's — a job that has to re-derive its own byte counts is not something to
    // report as a failure and wait on. If another job took the slot in that pump, this one waits its turn.
    if (indexOf(jobs_[idx].id) == idx && jobs_[idx].state == DownloadJob::Failed)
    {
        jobs_[idx].state = DownloadJob::Queued;
        jobs_[idx].error.clear();
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
        if (j.state == DownloadJob::Active) { j.state = DownloadJob::Failed; j.error = err; }
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
        else { j.state = DownloadJob::Failed; j.error = tr("Couldn't finalize the file."); }
    }
    activeId_.clear();
    save(); emit changed();
    pump();
}

void DownloadManager::retry(const QString& id)
{
    const int i = indexOf(id);
    if (i < 0) return;
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
    else if (jobs_[i].state == DownloadJob::Queued) { jobs_[i].state = DownloadJob::Paused; }
    save(); emit changed();
}

void DownloadManager::cancel(const QString& id)
{
    const int i = indexOf(id);
    if (i < 0) return;
    const QString part = jobs_[i].dest + QStringLiteral(".part");
    if (jobs_[i].id == activeId_ && reply_) { jobs_[i].state = DownloadJob::Paused; reply_->abort(); }
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
        arr.append(QJsonObject{
            { QStringLiteral("id"), j.id }, { QStringLiteral("title"), j.title }, { QStringLiteral("url"), j.url },
            { QStringLiteral("dest"), j.dest }, { QStringLiteral("kind"), j.kind }, { QStringLiteral("sysId"), j.sysId },
            { QStringLiteral("thumb"), j.thumb }, { QStringLiteral("key"), j.key },
            // The FLAG, never the headers. Deliberately not a loop over requestHeaders: this file is not a
            // credential store, and the values are per-request secrets. See DownloadJob::headerGated.
            { QStringLiteral("gated"), j.headerGated },
            { QStringLiteral("received"), j.received }, { QStringLiteral("total"), j.total },
            { QStringLiteral("state"), int(j.state == DownloadJob::Active ? DownloadJob::Paused : j.state) } });
    }
    QDir().mkpath(QFileInfo(queuePath()).absolutePath());
    QFile f(queuePath());
    if (f.open(QIODevice::WriteOnly)) f.write(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

void DownloadManager::load()
{
    QFile f(queuePath());
    if (!f.open(QIODevice::ReadOnly)) return;
    for (const QJsonValue& v : QJsonDocument::fromJson(f.readAll()).array())
    {
        const QJsonObject o = v.toObject();
        DownloadJob j;
        j.id = o.value(QStringLiteral("id")).toString();
        j.title = o.value(QStringLiteral("title")).toString();
        j.url = o.value(QStringLiteral("url")).toString();
        j.dest = o.value(QStringLiteral("dest")).toString();
        j.kind = o.value(QStringLiteral("kind")).toString();
        j.sysId = o.value(QStringLiteral("sysId")).toString();
        j.thumb = o.value(QStringLiteral("thumb")).toString();
        j.key = o.value(QStringLiteral("key")).toString();
        j.headerGated = o.value(QStringLiteral("gated")).toBool(); // requestHeaders stays empty — that is the point
        j.received = o.value(QStringLiteral("received")).toVariant().toLongLong();
        j.total = o.value(QStringLiteral("total")).toVariant().toLongLong();
        j.state = static_cast<DownloadJob::State>(o.value(QStringLiteral("state")).toInt());
        if (!j.id.isEmpty() && !j.dest.isEmpty()) jobs_.push_back(j);
    }
}
