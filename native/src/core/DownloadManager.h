// Manages "download for keeps" jobs: ROMs / movies / books fetched to <app>/downloads and recorded in the
// Downloaded folder. Unlike the old fire-and-forget queue, jobs are persistent (survive a restart) and have
// state — so a download that failed or stopped part-way stays in the list to be retried or resumed. Streams
// straight to a .part file (no whole-file buffering) and resumes with an HTTP Range request when the server
// supports it, else restarts. One download runs at a time; the rest queue.
#pragma once
#include "StreamHeaders.h"

#include <QObject>
#include <QSet>
#include <QString>
#include <QVector>
#include <functional>
#include <utility>

class QNetworkAccessManager;
class QNetworkReply;
class QFile;

struct DownloadJob
{
    QString id;      // unique
    QString title;
    QString url;     // source (a debrid link may expire; retry re-uses it — a dead link just fails again).
                     // #437: EMPTY for a job that can mint its link (sourceRef); sealed at rest on Windows and
                     // kept in an owner-only file elsewhere while it can resume; cut to StoredUrl::location
                     // (no query, no fragment) the moment the job fails. See core/UrlAtRest.h.
    QString dest;    // final local path (the .part is dest + ".part")
    QString kind;    // "video" | "audio" | "document" | "game" | "pcgame"
    QString sysId;   // game system id, else empty
    QString form;    // reading downloads only: "book" | "comic" | "manga" — which reading catalogue the
                     // finished file is filed under (see core/ReadingForm.h). The reading twin of sysId,
                     // and for the same reason: kind is "document" for all three of them.
    QString thumb;
    QString key;     // stable identity for de-dup / recording
    // AN IDENTITY INSTEAD OF A URL (issue #110). Set, `url` is left EMPTY and the link is MINTED at the top
    // of every start() by DownloadManager::setUrlMinter's hook — see JellyfinDownload.h for the whole
    // argument. A Jellyfin download url carries the user's ACCESS TOKEN in its query, and this struct is
    // written to queue.json, which is an ordinary file in the app folder and not a credential store: the
    // same rule headerGated below states about proxy headers, one notch stronger, because unlike a header
    // set an id survives a restart and therefore RESUMES rather than failing.
    //
    // Persisted: the ref is the durable half. It carries no credential — "jf:<serverId>:<itemId>" is two
    // ids — and it is exactly what a restart needs in order to ask for the link again.
    QString sourceRef;
    // A link ALREADY MINTED for this job's ref, used by the next start() INSTEAD of asking the minter, and
    // cleared by it (#437). An add-on download arrives with its link freshly resolved; asking the source a
    // second time for the first transfer would be a wasted round trip, and on the imdb route possibly a
    // different release. NEVER PERSISTED: it is a credential, and it is exactly what sourceRef exists to keep
    // out of queue.json. Jellyfin, Subsonic and Audiobookshelf jobs never set it.
    QString mintedUrl;
    // The source's behaviorHints.proxyHeaders.request, declared for `url` (#59). A download is a plain HTTP
    // fetch of the very URL the player would have played, so a header-gated source that PLAYS fine used to
    // fail here with a 403 the user reads as "the download is broken".
    //
    // Per JOB, not a manager member: a member holding "the last download's headers" outlives its download,
    // and the next job — a different host — would inherit them. Same reason the resolve callback carries
    // them rather than StreamResolver holding them.
    StreamHeaders::Headers requestHeaders;
    // …and NOT persisted, which is why this exists. queue.json is an ordinary file in the app folder, not a
    // credential store, and a proxyHeader is routinely a signed-URL token or a session cookie; writing one
    // there turns a transient per-request secret into a secret at rest that outlives the download. So a
    // restart drops the values and keeps this value-free bit, and a restored job that needs headers it no
    // longer has says so instead of retrying into an unexplained 403.
    bool headerGated = false;
    // Whether finishing this job means the user asked for the FILE. Nearly always they did — a job exists
    // because someone pressed Download. A romhack patch is the exception: it is an INTERMEDIATE, streamed
    // through here for the resume, the progress and the Cancel, and the thing the user asked for is the
    // patched game that gets written afterwards. false keeps it out of Recent and the Downloaded folder,
    // where a raw .ips is noise nobody asked for; it still appears in the Downloads panel, which is the
    // entire reason it came this way.
    //
    // Not expressed as a `kind`: nothing at the recording site reads kind, so a new one there would be
    // recorded exactly like a game — a field that looks like it should have worked.
    //
    // Persisted, and ABSENT MEANS TRUE, so a queue.json written before this field existed keeps recording.
    bool record = true;
    qint64 received = 0;
    qint64 total = 0;
    enum State { Queued, Active, Paused, Failed, Done };
    State state = Queued;
    QString error;
    // #437: this plain-link job FAILED and its url's query and fragment were dropped on the spot (the part
    // that carries a debrid token or an add-on key), so the link left cannot be trusted to fetch the file
    // again. Retry cannot help. Starting the download again from the item can: it re-resolves the link, and
    // enqueue() de-dups by destination, so the fresh link RESUMES the .part. The job's error says so.
    // Persisted ("dropped"); cleared by that fresh enqueue.
    bool linkDropped = false;
};

class DownloadManager : public QObject
{
    Q_OBJECT
public:
    explicit DownloadManager(QObject* parent = nullptr);

    void enqueue(const DownloadJob& job);       // add + start (de-dups by dest); no-op if the file already exists
    // The url-minter hook (#110). Called with a job's `sourceRef` at the top of start(), and must answer
    // with a fetchable url or an empty string. Installed once by MainWindow; a manager without one simply
    // has no ref-backed jobs to run, which is what every probe and every non-app build sees.
    //
    // A std::function rather than an #include of the Jellyfin client: this class is the generic downloader
    // and knows about HTTP, files and Range. Nothing else about it should have to learn what a media server
    // is, and nothing in it should be able to reach a token store.
    //
    // ONE SLOT FOR ALL THREE SERVER SOURCES (#439): Jellyfin, Subsonic and Audiobookshelf refs all go to the
    // one minter MainWindow installs, which routes by the ref's family. So "is this job's minter here yet" is
    // one question for all three, and a source that is installed but not able to answer YET says so with
    // Mint::notYet() — distinct from an empty url, which means it CAN'T mint (the server was removed, the
    // item is gone) and fails the job. Neither "not installed" nor "not yet" fails anything: the job stays
    // Queued and waits (waitingForSource), and installing the minter or calling sourceReady() starts it.
    struct Mint
    {
        QString url;              // empty (and not notReady): this source can't mint this ref
        bool notReady = false;    // the source is installed but can't answer yet — wait, don't fail
        Mint() = default;
        Mint(const QString& u) : url(u) {}   // implicit: a minter answering with a plain url is the norm
        static Mint notYet() { Mint m; m.notReady = true; return m; }
    };
    using UrlMinter = std::function<Mint(const QString& sourceRef)>;
    // Installing it pumps the queue (#439): a ref job restored before its minter existed waits for it, and
    // starts now.
    void setUrlMinter(UrlMinter minter);
    // A minter that answered Mint::notYet() can now answer: the jobs it turned away are tried again.
    void sourceReady();
    // This Queued job has a ref and nothing that can mint it yet: its minter is not installed, or answered
    // "not yet". It is WAITING, not failed — the Downloads panel says so, and it starts on its own.
    bool waitingForSource(const DownloadJob& j) const;
    // THE ASYNCHRONOUS MINTER (#437), for refs only a network round trip can answer: an add-on download's
    // #224 recipe (core/DownloadRecipe.h), re-resolved through the add-on that served it. Owns exactly the
    // refs DownloadRecipe::isRef accepts; every other ref still goes to the synchronous minter above, so a
    // Jellyfin, Subsonic or Audiobookshelf job runs exactly as before.
    //
    // `done` may be called at once or later, at most once; an empty url means the source could not mint one.
    // The headers are the ones the source declared for THAT url (#59), used for this request and not kept.
    // Installing one pumps the queue: a restored recipe job waits for its minter rather than failing for the
    // lack of one.
    using MintDone = std::function<void(const QString& url, const StreamHeaders::Headers& headers)>;
    using AsyncUrlMinter = std::function<void(const QString& sourceRef, MintDone done)>;
    void setAsyncUrlMinter(AsyncUrlMinter minter);
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
    // The transfer itself, once `fetchUrl` is known: straight from start() for a plain job or a ref the
    // synchronous minter answers, from onMinted() for a recipe job (#437).
    void beginTransfer(int idx, const QString& fetchUrl);
    void onMinted(quint64 gen, const QString& id, const QString& url, const StreamHeaders::Headers& headers);
    // Every way a job ends Failed goes through here, because #437's rule has to hold on every one of them:
    // a plain link loses its query and fragment the moment its job can no longer resume.
    void failJob(DownloadJob& j, const QString& error);
    // Throw the .part away and fetch the file from the top, as the app's own work rather than the user's:
    // a 416 about our offset, or a re-minted link that names a different file (#437).
    void restartFromTop(int idx, const QString& why);
    void onReadyRead();
    void onFinished();
    void noteResponseHead();            // read this response's own size facts, exactly once per transfer
    void onRangeUnsatisfiable();        // a 416 answering OUR resume Range: finalise or re-derive, never stall
    // ok now means COMPLETE, decided in onFinished() against the response. This used to re-test `received`
    // against `total` here, which is a number that can predate the transfer — see onFinished().
    void finishActive(bool ok, const QString& error, bool discardPart = false);
    int indexOf(const QString& id) const;
    int activeIndex() const;
    void save() const;
    bool load();   // true when what it read has to be written back in today's shape (#437's migration)

    QVector<DownloadJob> jobs_;
    QNetworkAccessManager* nam_ = nullptr;
    QNetworkReply* reply_ = nullptr;    // the in-flight request (one at a time)
    QFile* file_ = nullptr;             // the open .part being written
    QString activeId_;
    bool restartOnHeaders_ = false;     // set when a resume was requested; cleared once we've checked the status
    // Set by the NetHeaderApply redirect hook when the origin gate refused a hop, so onFinished() can tell
    // that abort apart from the user's Cancel — both arrive as OperationCanceledError, and Qt's string for
    // it ("Operation canceled") is the one this job must NOT report. Cleared at the top of every start().
    bool redirectRefused_ = false;
    // What the RESPONSE said about its own size. The transport is the authority on how many bytes there are;
    // a number recorded before a transfer — by an earlier attempt, or read back out of queue.json — cannot
    // describe it. All three are reset by every start() and none is persisted.
    bool headSeen_ = false;             // this response's head has been read into the three fields below
    qint64 bodyExpected_ = -1;          // its Content-Length, or -1 when it declared none
    qint64 bodyReceived_ = 0;           // bytes of THIS response written so far (not the .part's total size)
    bool rangeAsked_ = false;           // we sent a resume Range, so a 416 is an answer about OUR offset
    UrlMinter minter_;                  // #110: sourceRef -> a fresh url, asked once per start()
    AsyncUrlMinter asyncMinter_;        // #437: a recipe ref -> a fresh url, later
    // #439: whether the minter that owns this ref is installed — the async one for a recipe ref, the
    // synchronous one for every other. A plain job has nothing to wait for.
    bool minterInstalledFor(const QString& sourceRef) const;
    // #439: ids of Queued jobs whose installed minter answered Mint::notYet(). In memory only — a restart
    // asks again anyway. Cleared by setUrlMinter() and sourceReady(), and per job when it leaves Queued.
    QSet<QString> notReady_;
    // A recipe job between start() and its minter's answer. It holds the slot (one download at a time) with
    // no reply yet, so pump(), pause and cancel each have to see it. mintGen_ is bumped by anything that
    // abandons the wait, so a late answer for a job paused or cancelled meanwhile is dropped.
    bool minting_ = false;
    quint64 mintGen_ = 0;
    // #437: a RE-MINTED link resuming a .part. The source is asked for the same item again, and on the imdb
    // route that can be a different release, whose bytes appended to ours would make a corrupt file that
    // finishes "successfully". So the size the source now states is compared with the size recorded for the
    // .part, and a mismatch starts the file over instead of splicing. -1 when there is nothing to compare.
    qint64 resumeTotal_ = -1;
    bool identityMismatch_ = false;
};
