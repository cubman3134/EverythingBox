// Manages "download for keeps" jobs: ROMs / movies / books fetched to <app>/downloads and recorded in the
// Downloaded folder. Unlike the old fire-and-forget queue, jobs are persistent (survive a restart) and have
// state — so a download that failed or stopped part-way stays in the list to be retried or resumed. Streams
// straight to a .part file (no whole-file buffering) and resumes with an HTTP Range request when the server
// supports it, else restarts. One download runs at a time; the rest queue.
#pragma once
#include <QObject>
#include <QString>
#include <QVector>

class QNetworkAccessManager;
class QNetworkReply;
class QFile;

struct DownloadJob
{
    QString id;      // unique
    QString title;
    QString url;     // source (a debrid link may expire; retry re-uses it — a dead link just fails again)
    QString dest;    // final local path (the .part is dest + ".part")
    QString kind;    // "video" | "audio" | "document" | "game" | "pcgame"
    QString sysId;   // game system id, else empty
    QString thumb;
    QString key;     // stable identity for de-dup / recording
    qint64 received = 0;
    qint64 total = 0;
    enum State { Queued, Active, Paused, Failed, Done };
    State state = Queued;
    QString error;
};

class DownloadManager : public QObject
{
    Q_OBJECT
public:
    explicit DownloadManager(QObject* parent = nullptr);

    void enqueue(const DownloadJob& job);       // add + start (de-dups by dest); no-op if the file already exists
    const QVector<DownloadJob>& jobs() const { return jobs_; }
    bool hasActiveOrQueued() const;

    void retry(const QString& id);              // failed/paused -> queued (resumes from the .part if present)
    void pauseJob(const QString& id);           // active -> paused (keeps the .part)
    void resumeJob(const QString& id);          // paused/queued -> active
    void cancel(const QString& id);             // stop + delete the .part + drop the job
    void removeJob(const QString& id);          // drop a finished/failed job from the list
    void clearFinished();                       // drop all Done/Failed jobs

signals:
    void changed();                             // the job list or a state changed (UI should rebuild)
    void jobProgress(const QString& id);        // a job's byte counts advanced
    void jobCompleted(const DownloadJob& job);  // finished OK -> caller records it in Recent / Downloaded

private:
    void pump();                                // start the next queued job if nothing is active
    void start(int idx);
    void onReadyRead();
    void onFinished();
    void finishActive(bool ok, const QString& error, bool discardPart = false);
    int indexOf(const QString& id) const;
    int activeIndex() const;
    void save() const;
    void load();

    QVector<DownloadJob> jobs_;
    QNetworkAccessManager* nam_ = nullptr;
    QNetworkReply* reply_ = nullptr;    // the in-flight request (one at a time)
    QFile* file_ = nullptr;             // the open .part being written
    QString activeId_;
    bool restartOnHeaders_ = false;     // set when a resume was requested; cleared once we've checked the status
};
