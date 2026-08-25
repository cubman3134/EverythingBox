// A URL AS IT MAY BE PUT ON SCREEN (issue #202). Pure, header-only, QtCore-only.
//
// THE PROBLEM THIS EXISTS FOR. A Subsonic stream url is a CREDENTIAL: the query carries `u` (the user), `t`
// (the salted token) and `s` (the salt), so anything that renders the url renders a live secret — on screen,
// in a screenshot, in a screen share, in a recording. The classic now-playing page was rendering exactly
// that, from two independent sites, and the same shape had already been fixed twice elsewhere:
//
//   #193 increment 5   resume + consumption-stats titles   (PlaybackSession::resumeDisplayTitle)
//   #200               the recents store + the uitest key  (StoredUrl, CredentialScrub)
//   #202 (this)        the screen itself
//
// One generator behind all three: `QFileInfo(<something that might be a url>).completeBaseName()`. QFileInfo
// treats a url as a filesystem path — it splits on the last '/' and then on the last '.' — so the "base
// name" of a stream url is a SLICE OF ITS QUERY, and whether that slice contains the token depends on where
// the last dot happens to fall, i.e. on the server's id format. That is not a rule anyone can reason about.
//
// ---- HOW THIS DIFFERS FROM StoredUrl, WHICH IS THE SAME QUESTION FOR A STORE --------------------------
//
// In ONE way, and it is worth being precise about which, because the temptation is to assume a display rule
// must be stricter than a storage rule and to go and invent a second derivation.
//
//   THE DERIVATION IS THE SAME RULE, DELIBERATELY. When nothing knows a title, the label is
//   StoredUrl::title's answer and not a new one: the query goes, userinfo goes, a file keeps its base name,
//   a network url is named by the last segment of its PATH. In particular a credential embedded in the path
//   (…/live/<user>/<pass>/<id>.ts, or a token as a whole segment) is NOT removed here either, exactly as
//   StoredUrl says it is not removed there. A stricter display rule was written first and thrown away: the
//   only shape it could catch that this one cannot is a token as the LAST path segment, which no source in
//   this tree produces (a debrid link ends in the file name, a Subsonic link in "stream.view", an IPTV link
//   in the channel id) — and the price was that every untitled remote row in a queue panel or a channel
//   list collapses to the same host name, which is a functional regression in exchange for a hypothetical.
//   ONE derivation, in one place, is also worth more than a marginally tighter second one that a future
//   reader has to diff against the first to find out which applies where.
//
//   WHAT DIFFERS IS THE CHOOSING. StoredUrl::title is handed one candidate and CLEANS it. A display site has
//   SEVERAL — mpv's own `media-title`, the queue's display title, the host's last-known title — and cleaning
//   the first one is the wrong move: StoredUrl::label of a signed stream url is
//   "https://music.example.com/rest/stream.view", which is safe, and is still a terrible thing to call a
//   song, and it silently outranks the perfectly good track name sitting in the next candidate. So the
//   display rule REJECTS a candidate that is a url and tries the next one; scrubbing is the last resort
//   rather than the first. That is the whole of this header, and it is the whole of issue #202: the right
//   title already existed (it is what the themed page has always shown, which is why the themed page was
//   never affected) and simply was not reaching the classic path.
//
//   AND ELISION IS NOT SCRUBBING. Menu rows elide long labels. A token in the first 40 characters is still a
//   token, so elision must happen strictly AFTER this rule has run and may never be mistaken for it. That is
//   a display-only concern and it is stated at the one call site that elides.
//
// WHAT IS DELIBERATELY NOT ATTEMPTED — and this is the failure mode that would make the cure worse than the
// disease. A title that merely LOOKS url-ish is a title. "Who Framed Roger Rabbit?" keeps its '?';
// "Whose Line Is It Anyway? The Movie" keeps its whole name; a track called "www.example.com" survives
// verbatim. isUsable() rejects a candidate on two unambiguous syntactic grounds only — it carries a url
// SCHEME ("<scheme>://"), or it has a QUERY TAIL in StoredUrl::looksLikeQueryTail's sense (a url-safe
// parameter name followed by '='). An over-eager sanitiser that eats legitimate titles is its own bug, and a
// worse one, because it is silent and it fires every day rather than on one screenshot.
#pragma once
#include "StoredUrl.h"

#include <QString>

namespace DisplayTitle
{
// Is `t` usable as a label exactly as it stands? See the header note for the two grounds and for the much
// longer list of things that are NOT grounds.
inline bool isUsable(const QString& t)
{
    if (t.isEmpty()) return false;
    // A url is not a title however clean it is. Note this is scheme() and not isNetworkUrl(): `file://…` and
    // `steam://…` are not credential-bearing, but they are not song names either, and the location they name
    // has a better label in it than its own spelling.
    if (!StoredUrl::scheme(t).isEmpty()) return false;
    // The #200 shape: "<uuid>?token=<…>", a completeBaseName() slice left in a title field by an older
    // build. No scheme, so nothing that reasons about urls would look twice at it, and it is sitting in a
    // title field on real installs today.
    return StoredUrl::label(t) == t;
}

// THE LAST RESORT: a label derived from the location itself, when nothing anywhere knows a title.
//
// StoredUrl::title's rule, unchanged and not re-implemented — a file keeps the base name it has always had,
// a network url is named by the last segment of its path (its host when it has no path), and the query,
// the fragment and any userinfo are gone in every case. The header note says why this is deliberately the
// SAME derivation the stores use and not a stricter one of its own.
inline QString fromLocation(const QString& pathOrUrl)
{
    return StoredUrl::title(QString(), pathOrUrl);
}

// THE RULE. The first usable candidate wins; a candidate that is a url is skipped rather than cleaned; if
// none is usable the label is derived from the location. Empty in, empty out — a caller with nothing at all
// to say gets nothing, and shows nothing, rather than a placeholder that would be a lie.
//
// IN BOTH OVERLOADS THE LAST ARGUMENT IS THE LOCATION and every earlier one is a candidate title. Two
// arities rather than a list because every site in the tree has one or two candidates and a QStringList at
// each of them would be noise; the invariant above is what keeps the two readable side by side.
inline QString choose(const QString& first, const QString& second, const QString& pathOrUrl)
{
    if (isUsable(first))  return first;
    if (isUsable(second)) return second;
    return fromLocation(pathOrUrl);
}

// One candidate — the common shape (a supplied title, else the link).
inline QString choose(const QString& only, const QString& pathOrUrl)
{
    return choose(only, QString(), pathOrUrl);
}
} // namespace DisplayTitle
