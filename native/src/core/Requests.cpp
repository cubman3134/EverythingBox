#include "Requests.h"

#include "Jellyfin.h"   // libraryRefFor() mints a QUALIFIED id and there is exactly one minter (#160)

#include <QCoreApplication>
#include <QRegularExpression>
#include <algorithm>

namespace {

// "tt0111161" or "tt0903747:1:2" (the Stremio episode stream-id shape). Anchored, and the digits are
// required: "tt" alone, or "ttsomething", is not an IMDB id and must not be treated as one.
const QRegularExpression& imdbRe()
{
    static const QRegularExpression re(QStringLiteral("^(tt\\d{6,})(?::(\\d+):(\\d+))?$"));
    return re;
}

// "tmdb:movie:603" / "tmdb:tv:1396" / "tmdb:episode:1396:1:2" — what native/addons/aiocatalog/main.js mints.
const QRegularExpression& tmdbRe()
{
    static const QRegularExpression re(
        QStringLiteral("^tmdb:(movie|tv|episode):(\\d+)(?::(\\d+):(\\d+))?$"));
    return re;
}

// The catalogue's own type words, collapsed onto the two an acquisition pipeline knows. Deliberately a
// closed list: a type this does not recognise yields an empty string and refFor() answers None, which is
// how a game, an album, a book, a photo or a synthetic folder row ends up with no Request action.
QString mediaTypeFor(const QString& type)
{
    const QString t = type.trimmed().toLower();
    if (t == QLatin1String("movie") || t == QLatin1String("film")) return requests::kMovie();
    if (t == QLatin1String("series") || t == QLatin1String("tv") || t == QLatin1String("show")
        || t == QLatin1String("season") || t == QLatin1String("episode"))
        return requests::kTv();
    return QString();
}

int groupRank(requests::Availability a)
{
    using A = requests::Availability;
    switch (a)
    {
    case A::Available:
    case A::PartiallyAvailable: return 0;   // ready to watch
    case A::Pending:
    case A::Approved:
    case A::Processing:         return 1;   // on the way
    case A::Declined:
    case A::Failed:             return 2;   // needs attention
    case A::Unknown:
    case A::NotRequested:       return 3;   // status unknown
    }
    return 3;
}

} // namespace

namespace requests {

QString MediaRef::key() const
{
    if (kind == IdKind::Imdb && !imdb.isEmpty()) return QStringLiteral("imdb:") + imdb;
    if (kind == IdKind::Tmdb && !tmdb.isEmpty())
        return QStringLiteral("tmdb:") + mediaType + QLatin1Char(':') + tmdb;
    return QString();
}

bool isRequestableType(const QString& type) { return !mediaTypeFor(type).isEmpty(); }

MediaRef refFor(const QString& itemId, const QString& imdbStreamId, const QString& type)
{
    MediaRef out;
    const QString declared = mediaTypeFor(type);

    // TMDB first, and only from the ITEM's own id: `imdbStreamId` is by definition an IMDB id and a TMDB
    // shape appearing in it would be a bug elsewhere, not a second source to read.
    const QRegularExpressionMatch tm = tmdbRe().match(itemId.trimmed());
    if (tm.hasMatch())
    {
        const QString sub = tm.captured(1);
        out.kind = IdKind::Tmdb;
        out.tmdb = tm.captured(2);
        // THE ID'S OWN WORD WINS over the row's `type`. A row can be typed loosely by the catalogue that
        // produced it ("video" on an episode, "series" on a season row), but "tmdb:episode:…" is minted by
        // the addon that knows exactly what it made, and requesting a film as a series submits the wrong
        // thing to somebody else's server.
        out.mediaType = (sub == QLatin1String("movie")) ? kMovie() : kTv();
        if (sub == QLatin1String("episode"))
        {
            const int s = tm.captured(3).toInt();
            if (s > 0) out.season = s;      // season 0 (specials) collapses to "the whole series"
        }
        return out;
    }

    // IMDB, from the item's own id or from the bridged stream id. The item's own id is preferred: a
    // catalogue whose ids ARE IMDB ids (Cinemeta) is the case where the two agree, and where they differ
    // the row's own identity is the one the rest of the app keys on.
    for (const QString& candidate : { itemId.trimmed(), imdbStreamId.trimmed() })
    {
        const QRegularExpressionMatch im = imdbRe().match(candidate);
        if (!im.hasMatch()) continue;
        out.kind = IdKind::Imdb;
        out.imdb = im.captured(1);
        const bool episodeShaped = !im.captured(2).isEmpty();
        // An episode-shaped stream id is a SERIES plus a season, whatever the row calls itself. Otherwise
        // the row's declared type decides, and a row with no usable type is not requestable: guessing
        // "movie" would submit a film request for a series.
        if (episodeShaped)
        {
            out.mediaType = kTv();
            const int s = im.captured(2).toInt();
            if (s > 0) out.season = s;
        }
        else if (!declared.isEmpty())
        {
            out.mediaType = declared;
        }
        else
        {
            out = MediaRef{};       // an id we can read, attached to a thing we cannot classify
            continue;
        }
        return out;
    }
    return out;                     // IdKind::None — the surface shows no Request action at all
}

QString availabilityLabel(Availability a)
{
    switch (a)
    {
    case Availability::Unknown:            return QCoreApplication::translate("requests", "Status unknown");
    case Availability::NotRequested:       return QCoreApplication::translate("requests", "Not requested");
    case Availability::Pending:            return QCoreApplication::translate("requests", "Waiting for approval");
    case Availability::Approved:           return QCoreApplication::translate("requests", "Approved");
    case Availability::Processing:         return QCoreApplication::translate("requests", "Being fetched");
    case Availability::PartiallyAvailable: return QCoreApplication::translate("requests", "Partly in your library");
    case Availability::Available:          return QCoreApplication::translate("requests", "In your library");
    case Availability::Declined:           return QCoreApplication::translate("requests", "Declined");
    case Availability::Failed:             return QCoreApplication::translate("requests", "Could not be fetched");
    }
    return QCoreApplication::translate("requests", "Status unknown");
}

bool isRequestable(Availability a)
{
    switch (a)
    {
    case Availability::NotRequested:
    case Availability::PartiallyAvailable:  // the missing seasons are exactly what a second request is for
    case Availability::Declined:            // asking again after a no is a decision the user may make
    case Availability::Failed:
    case Availability::Unknown:             // we could not ask; the press is still the user's to make
        return true;
    case Availability::Pending:
    case Availability::Approved:
    case Availability::Processing:
    case Availability::Available:
        return false;
    }
    return false;
}

bool isInFlight(Availability a)
{
    return a == Availability::Pending || a == Availability::Approved || a == Availability::Processing;
}

QString statusToken(Availability a)
{
    switch (a)
    {
    case Availability::Unknown:            return QStringLiteral("unknown");
    case Availability::NotRequested:       return QStringLiteral("none");
    case Availability::Pending:            return QStringLiteral("pending");
    case Availability::Approved:           return QStringLiteral("approved");
    case Availability::Processing:         return QStringLiteral("processing");
    case Availability::PartiallyAvailable: return QStringLiteral("partial");
    case Availability::Available:          return QStringLiteral("available");
    case Availability::Declined:           return QStringLiteral("declined");
    case Availability::Failed:             return QStringLiteral("failed");
    }
    return QStringLiteral("unknown");
}

Availability availabilityFromToken(const QString& token)
{
    if (token == QLatin1String("none"))       return Availability::NotRequested;
    if (token == QLatin1String("pending"))    return Availability::Pending;
    if (token == QLatin1String("approved"))   return Availability::Approved;
    if (token == QLatin1String("processing")) return Availability::Processing;
    if (token == QLatin1String("partial"))    return Availability::PartiallyAvailable;
    if (token == QLatin1String("available"))  return Availability::Available;
    if (token == QLatin1String("declined"))   return Availability::Declined;
    if (token == QLatin1String("failed"))     return Availability::Failed;
    return Availability::Unknown;   // including an empty or unrecognised token: never a silent "pending"
}

QString failureSentence(Failure f)
{
    switch (f)
    {
    case Failure::None:
        return QString();
    case Failure::NotConfigured:
        return QCoreApplication::translate("requests",
            "No request service is set up yet. Add one in Settings to ask for things you do not have.");
    case Failure::Unreachable:
        return QCoreApplication::translate("requests",
            "Your request service could not be reached from this device.");
    case Failure::TimedOut:
        return QCoreApplication::translate("requests",
            "Your request service did not answer in time.");
    case Failure::Unauthorized:
        return QCoreApplication::translate("requests",
            "Your request service refused the API key. Enter it again in Settings.");
    case Failure::Forbidden:
        return QCoreApplication::translate("requests",
            "Your request service accepted the key but will not allow this request.");
    case Failure::NotFound:
        return QCoreApplication::translate("requests",
            "Your request service does not know this title.");
    case Failure::Duplicate:
        return QCoreApplication::translate("requests",
            "This has already been asked for. It is on the Requests shelf.");
    case Failure::Malformed:
        return QCoreApplication::translate("requests",
            "Something answered at that address, but it is not a request service.");
    case Failure::RateLimited:
        return QCoreApplication::translate("requests",
            "Your request service is asking for fewer requests. Try again in a minute.");
    case Failure::ServerError:
        return QCoreApplication::translate("requests",
            "Your request service reported a problem of its own.");
    }
    return QString();
}

ActionKind actionFor(const MediaRef& ref, bool backendConfigured, bool lookupOk, Availability a)
{
    // Nothing to key on, or nothing to ask. Either way the surface shows no action rather than a broken one.
    if (!ref.ok() || !backendConfigured) return ActionKind::None;
    // THE ANTI-DUPLICATE CHECK. It is applied BEFORE anything else about the state, because "it is already
    // there" is the one answer that must never be rendered as a button that would fetch it a second time.
    if (lookupOk && a == Availability::Available) return ActionKind::InLibrary;
    if (lookupOk && isInFlight(a))                return ActionKind::Waiting;
    // A lookup that did not answer is NOT assumed to mean "not requested". The press is still offered —
    // the user may know perfectly well that they have not asked for this — but it is labelled as the
    // unknown it is, with the failure's own sentence beside it.
    if (!lookupOk) return ActionKind::Unknown;
    return ActionKind::Request;
}

QString actionLabel(ActionKind kind, Availability a)
{
    switch (kind)
    {
    case ActionKind::None:      return QString();
    case ActionKind::Request:
        // PartiallyAvailable is still a Request, and it says what it would actually do: the seasons already
        // there are not asked for again.
        return (a == Availability::PartiallyAvailable)
                   ? QCoreApplication::translate("requests", "Request what is missing")
                   : QCoreApplication::translate("requests", "Request");
    case ActionKind::InLibrary: return QCoreApplication::translate("requests", "In your library");
    case ActionKind::Waiting:   return availabilityLabel(a);
    case ActionKind::Unknown:   return QCoreApplication::translate("requests", "Request (status unknown)");
    }
    return QString();
}

QVector<ShelfGroup> shelfGroups(const QVector<StoredRequest>& rows)
{
    ShelfGroup ready{ QCoreApplication::translate("requests", "Ready to watch"), {} };
    ShelfGroup onWay{ QCoreApplication::translate("requests", "On the way"), {} };
    ShelfGroup attn { QCoreApplication::translate("requests", "Needs attention"), {} };
    ShelfGroup unk  { QCoreApplication::translate("requests", "Status unknown"), {} };
    ShelfGroup* const buckets[4] = { &ready, &onWay, &attn, &unk };

    for (const StoredRequest& r : rows)
        buckets[groupRank(availabilityFromToken(r.status))]->items.push_back(r);

    // Newest first, and STABLE — std::stable_sort so two rows stamped in the same second keep the order
    // they were stored in rather than swapping between two draws of the same shelf.
    for (ShelfGroup* g : buckets)
        std::stable_sort(g->items.begin(), g->items.end(),
                         [](const StoredRequest& a, const StoredRequest& b) {
                             return a.requestedAt > b.requestedAt;
                         });

    QVector<ShelfGroup> out;
    for (ShelfGroup* g : buckets) if (!g->items.isEmpty()) out.push_back(*g);
    return out;
}

QString libraryRefFor(const QString& serverItemId, const QStringList& configuredServerIds)
{
    if (serverItemId.trimmed().isEmpty()) return QString();
    if (configuredServerIds.size() != 1)  return QString();   // see the header: no guessing which server
    return Jellyfin::qualify(configuredServerIds.first(), serverItemId.trimmed());
}

// Issue #315. One line, and the reason it is a function rather than an `if` at the call site is that the
// call site is a 400-line builder shared by a hover and an open: written there, the rule would be a
// condition somebody edits while thinking about something else. Here it is the thing being decided.
bool fetchesStatus(StatusTrigger trigger)
{
    return trigger == StatusTrigger::DetailOpened;
}

} // namespace requests
