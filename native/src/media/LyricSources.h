// WHICH LYRIC SOURCE WINS, and the LRCLIB protocol that feeds the last of them (issue #142, sources 2 and 3).
// Pure: QtCore only, no file I/O, no network, no keys, no state. Everything that touches a disk or a socket
// lives in LyricFetch.cpp; everything that DECIDES lives here, so the decision can be pinned in a probe with
// no player, no clock and no internet.
//
// THE PRECEDENCE, AND WHY IT IS A LIST RATHER THAN A SCORE. Issue #142 orders the three sources:
//
//   1. the .lrc SIDECAR the user put beside the file,
//   2. the EMBEDDED tag inside the file,
//   3. LRCLIB, over the network.
//
// A tier wins on EXISTENCE, not on quality: the first tier that yields a single lyric line is the answer, and
// the tiers below it are never consulted. That is deliberate and it is the whole point of the ordering — a
// user who dropped a hand-corrected Track.lrc next to a song did so to override what the file and the internet
// say, and a rule that preferred "the synced one" would silently hand their correction back to LRCLIB the
// moment the sidecar happened to be a plain sheet. Quality only breaks ties INSIDE a tier, where there is no
// user intent to respect: SYLT beats USLT, and LRCLIB's syncedLyrics beats its plainLyrics.
//
// UNSYNCED IS A RESULT, NOT A MISS. #142 says degrade, don't hide. A tier that yields only untimed lines has
// still answered — Choice::source names it, Choice::lyrics.synced is false, and the surface renders a
// scrollable sheet. The only value that means "keep looking" is NO LINES AT ALL.
//
// needsOnline() is the politeness rule in one place: it is true only when both LOCAL tiers came up empty, so a
// track that already has words never reaches the network. The caller adds the other half of the politeness —
// the setting, and the fact that this is asked when a track starts playing rather than during a library sweep.
#pragma once
#include "LrcLyrics.h"
#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QUrl>
#include <QUrlQuery>

namespace LyricSources
{
    enum class Source
    {
        None,     // no tier had a line
        Sidecar,  // <basename>.lrc beside the audio file
        Embedded, // SYLT / USLT / ©lyr / Vorbis LYRICS, out of the tag pass
        Lrclib    // lrclib.net, fetched once and cached
    };

    // The raw text each tier offered, already extracted but not yet parsed. Empty means "this tier has
    // nothing"; whitespace-only counts as nothing too, because a tagger that wrote an empty USLT frame has not
    // given the user any words and must not shadow the tier below.
    struct Candidates
    {
        QString sidecar;        // tier 1
        QString embeddedSynced; // tier 2, preferred half — AudioTags::Tags::syncedLyrics (SYLT, rendered to LRC)
        QString embeddedPlain;  // tier 2, fallback half — AudioTags::Tags::lyrics (may itself be LRC)
        QString lrclib;         // tier 3, already reduced to one text by Lrclib::bestText
    };

    struct Choice
    {
        LrcLyrics::Lyrics lyrics;
        Source            source = Source::None;
    };

    // Parse one candidate; "did this tier answer" is "did parsing produce a line".
    inline bool yields(const QString& text, LrcLyrics::Lyrics& out)
    {
        if (text.trimmed().isEmpty())
            return false;
        out = LrcLyrics::parseLrc(text);
        return !out.lines.isEmpty();
    }

    inline Choice resolve(const Candidates& c)
    {
        Choice ch;
        if (yields(c.sidecar, ch.lyrics))        { ch.source = Source::Sidecar;  return ch; }
        if (yields(c.embeddedSynced, ch.lyrics)) { ch.source = Source::Embedded; return ch; }
        if (yields(c.embeddedPlain, ch.lyrics))  { ch.source = Source::Embedded; return ch; }
        if (yields(c.lrclib, ch.lyrics))         { ch.source = Source::Lrclib;   return ch; }
        ch.lyrics = LrcLyrics::Lyrics{}; // a failed parse may have left a partial value in there
        ch.source = Source::None;
        return ch;
    }

    // True when the two LOCAL tiers gave nothing, i.e. when going online could add something. Deliberately
    // independent of the tier-3 field: the caller asks this BEFORE it has an LRCLIB answer to offer.
    inline bool needsOnline(const Candidates& c)
    {
        LrcLyrics::Lyrics scratch;
        if (yields(c.sidecar, scratch))        return false;
        if (yields(c.embeddedSynced, scratch)) return false;
        if (yields(c.embeddedPlain, scratch))  return false;
        return true;
    }

    // A stable spelling for logs and the on-page debug read-out. Not shown to a user.
    inline QString sourceId(Source s)
    {
        switch (s)
        {
        case Source::Sidecar:  return QStringLiteral("sidecar");
        case Source::Embedded: return QStringLiteral("embedded");
        case Source::Lrclib:   return QStringLiteral("lrclib");
        case Source::None:     break;
        }
        return QStringLiteral("none");
    }
}

// ---------------------------------------------------------------------------------------------------------
// LRCLIB (issue #142, source 3). https://lrclib.net — free, keyless, no account, no rate-limit registration:
// the same zero-config bar the app's other free providers clear. The whole protocol is two GETs and one JSON
// object, and both halves of it are pure, so the URL a set of tags produces and the meaning of a response are
// pinned by a probe rather than by hitting the service.
//
// TWO ENDPOINTS, IN ORDER. /api/get is an EXACT match — artist, track, album and duration all have to agree
// (the service allows a couple of seconds of drift on the duration and nothing on the names), which is what
// makes it trustworthy and also what makes it miss a great deal. So a 404 from it falls back to /api/search,
// which is fuzzy and returns an array; the first entry is taken. Two round-trips at most, on a miss, once per
// track, and only when the track is actually playing.
//
// NO OTHER PROVIDER. #142 draws this as a scope line, not a performance one: the licensed lyric services
// (Musixmatch and friends) are not a scraping problem to solve, they are content nobody here has the right to
// redistribute. LRCLIB is a community database of user-contributed files and is the only source this talks to.
// ---------------------------------------------------------------------------------------------------------
namespace Lrclib
{
    // The lookup keys, straight out of the tag pass. duration is SECONDS and 0 means "unknown" — an unknown
    // duration is left OUT of the query rather than sent as zero, which would match nothing at all.
    struct Query
    {
        QString artist;
        QString title;
        QString album;
        int     durationSec = 0;
    };

    // Artist and title are the minimum the service can answer on. A track with neither is a filename we never
    // parsed, and asking about it would be a request guaranteed to miss.
    inline bool isUsable(const Query& q)
    {
        return !q.artist.trimmed().isEmpty() && !q.title.trimmed().isEmpty();
    }

    inline QUrl getUrl(const Query& q)
    {
        QUrlQuery params;
        params.addQueryItem(QStringLiteral("artist_name"), q.artist.trimmed());
        params.addQueryItem(QStringLiteral("track_name"), q.title.trimmed());
        if (!q.album.trimmed().isEmpty())
            params.addQueryItem(QStringLiteral("album_name"), q.album.trimmed());
        if (q.durationSec > 0)
            params.addQueryItem(QStringLiteral("duration"), QString::number(q.durationSec));
        QUrl url(QStringLiteral("https://lrclib.net/api/get"));
        url.setQuery(params);
        return url;
    }

    // The fuzzy fallback. Album and duration are deliberately dropped: they are exactly the fields whose
    // disagreement made /api/get miss, so repeating them would reproduce the miss.
    inline QUrl searchUrl(const Query& q)
    {
        QUrlQuery params;
        params.addQueryItem(QStringLiteral("artist_name"), q.artist.trimmed());
        params.addQueryItem(QStringLiteral("track_name"), q.title.trimmed());
        QUrl url(QStringLiteral("https://lrclib.net/api/search"));
        url.setQuery(params);
        return url;
    }

    struct Response
    {
        bool    valid        = false; // the body parsed as a track record (a 404 body parses, and is not one)
        bool    instrumental = false;
        QString synced;               // "syncedLyrics" — LRC text
        QString plain;                // "plainLyrics"  — untimed sheet
    };

    // One record -> a Response. Both lyric fields are JSON null on a record that has neither, which
    // QJsonValue::toString() renders as an empty QString, so "the service knows this track and has no words
    // for it" and "the service has words" are told apart by the fields rather than by the presence of a body.
    inline Response fromObject(const QJsonObject& o)
    {
        Response r;
        if (o.isEmpty() || !o.contains(QStringLiteral("id")))
            return r; // a 404 body is {"code":404,"name":"TrackNotFound",…} — parses fine, is not a record
        r.valid        = true;
        r.instrumental = o.value(QStringLiteral("instrumental")).toBool();
        r.synced       = o.value(QStringLiteral("syncedLyrics")).toString();
        r.plain        = o.value(QStringLiteral("plainLyrics")).toString();
        return r;
    }

    inline Response parseGet(const QByteArray& body)
    {
        return fromObject(QJsonDocument::fromJson(body).object());
    }

    // Synced first, plain second — the tie-break INSIDE tier 3, where there is no user intent to respect.
    // An instrumental is an explicit "this track has no words", so it returns nothing and the surface shows no
    // panel; treating the empty string as a failure and retrying every play would be the wrong answer, which
    // is why the fetch layer records an instrumental as a settled miss.
    inline QString bestText(const Response& r)
    {
        if (!r.valid || r.instrumental)
            return {};
        return !r.synced.trimmed().isEmpty() ? r.synced : r.plain;
    }

    // /api/search returns an array, best match first. Entries with no words at all are skipped rather than
    // accepted — the search index carries records for tracks nobody has contributed lyrics for, and taking the
    // first of those would turn a recoverable miss into a settled one. An empty array is a clean miss, not a
    // malformed reply.
    inline Response parseSearch(const QByteArray& body)
    {
        const QJsonArray arr = QJsonDocument::fromJson(body).array();
        for (const QJsonValue& v : arr)
        {
            const Response r = fromObject(v.toObject());
            if (r.valid && !bestText(r).trimmed().isEmpty())
                return r;
        }
        return {};
    }
}
