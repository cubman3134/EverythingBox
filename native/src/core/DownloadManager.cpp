#include "DownloadManager.h"
#include "AppPaths.h"

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QUrl>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QUuid>

static QString queuePath() { return AppPaths::dataDir() + QStringLiteral("/downloads/queue.json"); }

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
            if (j.state == DownloadJob::Failed || j.state == DownloadJob::Paused) { j.state = DownloadJob::Queued; j.error.clear(); }
            save(); emit changed(); pump();
            return;
        }
    DownloadJob j = in;
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
        if (!file_->open(QIODevice::WriteOnly)) { delete file_; file_ = nullptr; j.state = DownloadJob::Failed; j.error = tr("Can't write to the downloads folder."); save(); emit changed(); return; }
        j.received = 0;
        restartOnHeaders_ = false;
    }

    j.state = DownloadJob::Active;
    activeId_ = j.id;

    QNetworkRequest rq{ QUrl(j.url) };
    rq.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MyMediaVault"));
    rq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    if (have > 0) rq.setRawHeader("Range", QByteArray("bytes=") + QByteArray::number(have) + "-");
    reply_ = nam_->get(rq);
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
        j.received = o.value(QStringLiteral("received")).toVariant().toLongLong();
        j.total = o.value(QStringLiteral("total")).toVariant().toLongLong();
        j.state = static_cast<DownloadJob::State>(o.value(QStringLiteral("state")).toInt());
        if (!j.id.isEmpty() && !j.dest.isEmpty()) jobs_.push_back(j);
    }
}
