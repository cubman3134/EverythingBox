// LookupClient — the in-book lookup's IMPURE half (issue #137): the one object in this feature that owns a
// socket. Everything it decides is decided in LookupRequest.h; this file is the network access manager, the
// timeout and the cancel.
//
// ONE LOOKUP AT A TIME, AND IT IS CANCELLABLE. A reader who selects a word, presses Define and then turns the
// page has moved on; the answer to the old question must not arrive over the new page. cancel() aborts the
// reply AND drops the callback, so a cancelled lookup produces nothing at all — not a late card, not a
// vocabulary row. Starting a second lookup cancels the first for the same reason.
//
// A LOOKUP ALWAYS RESOLVES. Every path out of here — a refused connection, a 404, a 500, an endpoint that
// accepts the socket and then says nothing — ends in exactly one callback carrying a LookupRequest::Outcome
// whose text is a sentence. The transfer timeout is what makes the last of those true: without it a card
// would sit on "Looking up…" for as long as the reader was willing to stare at it.
//
// NOTHING IS LOGGED. No URL, no body, no header. The only user text that leaves the process is the selected
// word, and only after an explicit verb press.
#pragma once
#include "LookupRequest.h"

#include <QObject>
#include <QPointer>
#include <QString>
#include <functional>

class QNetworkAccessManager;
class QNetworkReply;

class LookupClient : public QObject
{
    Q_OBJECT
public:
    explicit LookupClient(QObject* parent = nullptr);
    ~LookupClient() override;

    using Callback = std::function<void(LookupRequest::Outcome)>;

    // Ask one verb about one term. `srcLang` is the book's language (the dictionary/encyclopedia edition and
    // the translation source); `dstLang` is what a translation should come back in. `endpoint` is the
    // configured LibreTranslate-class instance and is ignored by the other two verbs. The callback runs
    // exactly once unless the lookup is cancelled, in which case it never runs.
    void start(LookupRequest::Verb verb, const QString& term, const QString& srcLang,
               const QString& dstLang, const QString& endpoint, Callback cb);

    // Drop whatever is in flight: abort the socket and forget the callback. Idempotent, and safe to call from
    // a page turn, a chapter load, the reader being hidden, or the card being dismissed.
    void cancel();

    bool busy() const;

    // How long a lookup may take before it becomes a sentence. Milliseconds; the probe shortens it so the
    // never-answers case can be driven in a test rather than waited out.
    static void setTimeoutMs(int ms);
    static int  timeoutMs();

private:
    void finish(QNetworkReply* reply);

    QNetworkAccessManager*   nam_ = nullptr;
    QPointer<QNetworkReply>  reply_;
    Callback                 cb_;
    LookupRequest::Verb      verb_ = LookupRequest::Verb::Define;
    QString                  term_;
    QString                  lang_;
};
