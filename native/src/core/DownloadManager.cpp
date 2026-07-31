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
    // whole file from the top, so truncate what we had and restart the byte count.
    if (restartOnHeaders_)
    {
        restartOnHeaders_ = false;
        const int code = reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (code != 206)
        {
            file_->seek(0); file_->resize(0); j.received = 0;
        }
    }
    // Total = what we've already got + what the reply says remains (Content-Length is the remaining bytes on a 206).
    const qint64 remain = reply_->header(QNetworkRequest::ContentLengthHeader).toLongLong();
    if (remain > 0) j.total = j.received + remain;

    const QByteArray chunk = reply_->readAll();
    if (!chunk.isEmpty())
    {
        file_->write(chunk);
        j.received += chunk.size();
    }
    emit jobProgress(j.id);
}

void DownloadManager::onFinished()
{
    if (!reply_) return;
    // A transport-level success (NoError) does NOT mean the download is good: an HTTP 404/403/5xx delivers an
    // error page with NoError, and a dropped connection can end "cleanly" mid-file. Treat a >=400 status, or a
    // body shorter than the advertised size, as a failure so we never record a broken file as complete.
    const int http = reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
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
    reply_->deleteLater(); reply_ = nullptr;
    finishActive(ok, err, discardPart);
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
    else if (j.total > 0 && j.received < j.total)
    {
        // The stream ended before the whole file arrived. Keep the .part so a retry resumes from here rather
        // than recording a partial file as a finished download.
        j.state = DownloadJob::Failed;
        j.error = tr("the download stopped before it finished (%1 of %2 bytes)").arg(j.received).arg(j.total);
    }
    else
    {
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
