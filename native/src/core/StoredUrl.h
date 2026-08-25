// A URL AS IT MAY BE WRITTEN DOWN (issue #200). Pure, header-only, QtCore-only.
//
// THE PROBLEM THIS EXISTS FOR. An addon-resolved stream is a SIGNED url: a debrid/provider host mints a
// one-shot link whose query carries a credential — an API token, a session key, a salted password hash.
// Playing it is fine; that credential is meant to travel once, over TLS, and expire. WRITING IT DOWN is
// not fine, and the stores that record "what was played" wrote it down verbatim:
//
//   * recent/<profile>/items       — the playback path, the display title, the artwork url and the item key
//   * resume/<hash>/title          — a display label derived from the url
//   * stats/<profile>/<dev>/items  — the same label, again
//
// All three are matched by CloudSync::isPerItemStoreKey and NONE by isDeviceLocalKey, so they ride the
// CloudMerge progress document to every device on the account. A token that authorised one playback becomes
// a durable, cleartext record — in the ini a bug report attaches, and in a zip in a third party's Drive
// folder. Confirmed live on a real install, not theorised: seven recents entries and one resume title.
//
// The project has already reasoned its way here twice, for the SOURCE urls rather than the PLAYBACK ones:
// iptv/* is device-local because the url "routinely embeds provider credentials", opds/* because of its
// basic-auth pair, and both conclude that syncing credential-bearing urls must be a deliberate opt-in and
// "not something that ships by omission". Recents shipped it by omission. The answer here is narrower than
// a carve-out, and deliberately so: recents across devices are a real feature and the merge document is
// exactly the right home for them. The CREDENTIAL is the problem, not the record — so it never gets written.
//
// WHY THE SCRUB IS ON THE WRITE AND NOT THE SYNC BOUNDARY. Filtering on the way out would leave the token in
// the local ini, and the local ini is what gets attached to bug reports and copied between machines by hand.
// A value that is never written cannot leak from a file nobody thought to look at.
//
// ---- THE TWO RULES, AND WHY THEY DIFFER ---------------------------------------------------------------
//
// location() — for anything STORED AS A PLACE SOMETHING WAS PLAYED FROM. Drops userinfo, the whole query and
// the fragment. Not a list of credential-shaped parameter names: no such list can be maintained (a debrid
// token is `token`, a Subsonic credential is `u`/`t`/`s`, a CDN signature is `sig`/`Expires`/`Policy`, and
// the next one will be spelled something else), and the value of keeping a stored playback url's query is
// nil — a signed link has expired by the time anyone clicks the row, and an unsigned one does not need it.
// So the invariant is one anybody can check by eye: NO STORED PLAYBACK LOCATION CONTAINS A QUERY STRING.
//
// artwork() — for a stored POSTER/COVER url, where the same rule would be a visible regression. Artwork is
// re-fetched on every Home paint, and real artwork urls carry real queries: the live install holds
// `https://books.google.com/books/content?id=…&printsec=frontcover&img=1&zoom=1&edge=curl&source=…`, whose
// query IS the image. Dropping it would blank the cover of every Google Books item to protect nothing — an
// artwork url is not minted by the stream-signing path this issue is about. So artwork() takes the narrow
// rule it can afford: userinfo and fragment go, and only parameters whose NAME says credential are dropped.
// The weakness of a name list is real and is accepted HERE, where the cost of over-stripping is a picture.
//
// title() — the generalisation of #193 increment 5's fix. `QFileInfo(url).completeBaseName()` was the
// display-title idiom everywhere: for a file it is exactly right, for a url it is a request. QFileInfo
// splits on the last '/' and then on the last '.', so the "base name" of a stream url is a slice of its
// QUERY — and whether that slice includes the token depends on where the last dot happens to fall, i.e. on
// the server's id format. #193 fixed it for Subsonic streams inside PlaybackSession. This is the same rule
// with no source in it: a network url is titled from its path, never from its query, and a title that is
// itself a url is scrubbed like one (playStream's last-resort `title = url` fallback is exactly that).
//
// WHAT IS DELIBERATELY NOT ATTEMPTED. A credential embedded in the PATH rather than the query
// (…/live/<user>/<pass>/<id>.ts, or a token as the first path segment) is left alone. The path is what makes
// a row re-openable and re-identifiable, no heuristic can tell a token segment from a content id, and a
// wrong guess mangles every legitimate stream url in the store. Userinfo (user:pass@) is the one
// path-adjacent credential with an unambiguous syntax, so that one IS removed.
#pragma once
#include <QChar>
#include <QFileInfo>
#include <QLatin1String>
#include <QString>
#include <QStringList>

namespace StoredUrl
{
// The scheme of `s` in lower case, or empty when `s` is not "<scheme>://…". Deliberately hand-rolled rather
// than QUrl: QUrl round-trips (and re-encodes) everything it parses, and these functions must be able to
// hand back a local Windows path — "C:\Users\me\My Videos\a b.mkv" — byte for byte.
inline QString scheme(const QString& s)
{
    const int i = s.indexOf(QLatin1String("://"));
    if (i <= 0) return QString();
    for (int j = 0; j < i; ++j)
    {
        const QChar c = s.at(j);
        const bool ok = (j == 0) ? c.isLetter()
                                 : (c.isLetterOrNumber() || c == QLatin1Char('+') || c == QLatin1Char('-')
                                    || c == QLatin1Char('.'));
        if (!ok) return QString();   // not a scheme: a title reading "Re: //something" is a title
    }
    return s.left(i).toLower();
}

// The schemes a credential can ride. An ALLOW-list of network-fetch schemes, not "everything with a scheme",
// because the launcher URIs recents also stores put their launch INSTRUCTION in the query —
// com.epicgames.launcher://apps/<AppName>?action=launch&silent=true — and stripping it would rewrite a
// launch command to protect a value that was never a credential. Those re-open from their key anyway
// (openRecent rebuilds the url from "epic:<AppName>"), so nothing is lost by leaving them verbatim.
inline bool isNetworkUrl(const QString& s)
{
    const QString sc = scheme(s);
    if (sc.isEmpty()) return false;
    return sc == QLatin1String("http")  || sc == QLatin1String("https")
        || sc == QLatin1String("rtsp")  || sc == QLatin1String("rtsps")  || sc == QLatin1String("rtspt")
        || sc == QLatin1String("rtmp")  || sc == QLatin1String("rtmps")  || sc == QLatin1String("rtmpe")
        || sc == QLatin1String("rtmpt")
        || sc == QLatin1String("mms")   || sc == QLatin1String("mmsh")
        || sc == QLatin1String("ftp")   || sc == QLatin1String("ftps")
        || sc == QLatin1String("srt");  // Secure Reliable Transport: ?passphrase=… is the whole point
}

namespace detail
{
// Index of the first '?' or '#' in `s` at or after `from`, or -1. The two are found TOGETHER because a
// fragment can precede a query in a malformed url ("…/p#f?x") and cutting only at '?' would keep the '#f?x'
// tail. Scanning for the first of either is also what makes an ENCODED '/' in the query a non-event: the cut
// is decided by the query delimiter, never by the last separator in the string.
inline int firstQueryOrFragment(const QString& s, int from)
{
    for (int i = from; i < s.size(); ++i)
        if (s.at(i) == QLatin1Char('?') || s.at(i) == QLatin1Char('#')) return i;
    return -1;
}

// End of the authority component: the first '/', '?' or '#' after "://", else the end of the string.
inline int authorityEnd(const QString& s, int from)
{
    for (int i = from; i < s.size(); ++i)
    {
        const QChar c = s.at(i);
        if (c == QLatin1Char('/') || c == QLatin1Char('?') || c == QLatin1Char('#')) return i;
    }
    return s.size();
}

// "<scheme>://" + the authority with any "user:pass@" removed, or empty when `s` is not a network url.
// lastIndexOf('@') and not indexOf: a password may legally contain a percent-encoded '@', and cutting at the
// first one would leave the tail of the password in the host position.
inline QString schemeAndHost(const QString& s, int& authEndOut)
{
    const int a = s.indexOf(QLatin1String("://")) + 3;
    const int e = authorityEnd(s, a);
    authEndOut = e;
    QString authority = s.mid(a, e - a);
    const int at = authority.lastIndexOf(QLatin1Char('@'));
    if (at >= 0) authority = authority.mid(at + 1);
    return s.left(a) + authority;
}
} // namespace detail

// A PLAYBACK LOCATION as it may be stored: scheme, host, port and path — no userinfo, no query, no fragment.
// Anything that is not a network url (a local path, a UNC path, file://, steam://, a magnet, an empty
// string) is returned byte for byte. Idempotent: location(location(x)) == location(x).
inline QString location(const QString& s)
{
    if (!isNetworkUrl(s)) return s;
    int authEnd = 0;
    const QString head = detail::schemeAndHost(s, authEnd);
    const int cut = detail::firstQueryOrFragment(s, authEnd);
    return head + (cut < 0 ? s.mid(authEnd) : s.mid(authEnd, cut - authEnd));
}

// Whether a query parameter's NAME says it carries a credential. artwork()'s rule only — location() needs no
// such judgement, which is exactly why it is the rule used for anything that matters.
//
// The single letters are the Subsonic triple (u = user, t = the salted token, s = the salt) plus p (the
// plaintext-password scheme this client refuses to use but other servers still mint). They cost the odd
// legitimate `s=<size>` on an image host — which loads at its default size — and they buy the removal of a
// music server's credentials from a cover url, which is the one artwork url in this tree known to carry any.
inline bool isCredentialParam(const QString& rawName)
{
    const QString n = rawName.trimmed().toLower();
    if (n.isEmpty()) return false;
    if (n == QLatin1String("u") || n == QLatin1String("t") || n == QLatin1String("s") || n == QLatin1String("p"))
        return true;
    static const char* const kNeedles[] = { "token", "auth", "password", "passwd", "pwd", "secret", "apikey",
                                            "api_key", "session", "signature", "credential", "salt", "jwt",
                                            "access", "sig", "key", "hmac", "policy", "expires" };
    for (const char* needle : kNeedles)
        if (n.contains(QLatin1String(needle))) return true;
    return false;
}

// AN ARTWORK URL as it may be stored: userinfo and fragment removed, and any credential-named query
// parameter dropped while the rest of the query is kept (see the header note for why this rule and not
// location()'s). A parameter with no '=' is kept unless its whole spelling reads as a credential name.
inline QString artwork(const QString& s)
{
    if (!isNetworkUrl(s)) return s;
    int authEnd = 0;
    const QString head = detail::schemeAndHost(s, authEnd);
    const int cut = detail::firstQueryOrFragment(s, authEnd);
    const QString path = cut < 0 ? s.mid(authEnd) : s.mid(authEnd, cut - authEnd);
    if (cut < 0 || s.at(cut) == QLatin1Char('#')) return head + path;   // no query (a fragment is dropped)

    const int hash = s.indexOf(QLatin1Char('#'), cut);
    const QString query = hash < 0 ? s.mid(cut + 1) : s.mid(cut + 1, hash - cut - 1);
    QStringList kept;
    const QStringList parts = query.split(QLatin1Char('&'));
    for (const QString& part : parts)
    {
        if (part.isEmpty()) continue;
        const int eq = part.indexOf(QLatin1Char('='));
        if (!isCredentialParam(eq < 0 ? part : part.left(eq))) kept.push_back(part);
    }
    return kept.isEmpty() ? head + path
                          : head + path + QLatin1Char('?') + kept.join(QLatin1Char('&'));
}

// Whether `tail` (everything after a '?') reads as a QUERY STRING rather than as prose: its first parameter
// is a name made of url-safe characters followed by '='. "Who Framed Roger Rabbit?" has no tail at all, and
// "Whose Line Is It Anyway? The Movie" has one that is words — neither is cut. "…?token=…" is.
inline bool looksLikeQueryTail(const QString& tail)
{
    for (int i = 0; i < tail.size() && i < 48; ++i)
    {
        const QChar c = tail.at(i);
        if (c == QLatin1Char('=')) return i > 0;
        if (!(c.isLetterOrNumber() || c == QLatin1Char('_') || c == QLatin1Char('-') || c == QLatin1Char('.')
              || c == QLatin1Char('~') || c == QLatin1Char('%')))
            return false;
    }
    return false;
}

// A STORED DISPLAY LABEL, scrubbed. Two shapes have been seen to leak, and the second is the one that made
// this a live finding rather than a tidy-up:
//
//   * the label IS a url — playStream's last-resort `title = url` fallback, reached when a link carries
//     neither a supplied title nor a file name. location() takes the query off it.
//   * the label is a completeBaseName() SLICE of a url: the live install's resume store held a title of the
//     shape "<uuid>?token=<36 chars>". That is not a url — it has no scheme — so nothing that reasons about
//     urls would touch it, and it sat in a synced store looking like a filename. A query tail on a label is
//     never a label, so it goes.
inline QString label(const QString& t)
{
    const QString s = location(t);
    const int q = s.indexOf(QLatin1Char('?'));
    if (q < 0) return s;
    return looksLikeQueryTail(s.mid(q + 1)) ? s.left(q) : s;
}

// THE DISPLAY LABEL a store may keep for `pathOrUrl`, given whatever title the caller already has.
//
//  * a supplied title wins, scrubbed by label() — so neither the "no title, use the link itself" fallbacks
//    nor a caller's own QFileInfo(url).completeBaseName() can smuggle a credential into the title field;
//  * a network url with no supplied title is labelled from its PATH — the last non-empty segment, else the
//    host. Never completeBaseName(), which reaches into the query (the #193 trap);
//  * anything else is a file, and keeps the completeBaseName() every call site already used.
inline QString title(const QString& given, const QString& pathOrUrl)
{
    if (!given.isEmpty()) return label(given);
    const QString clean = location(pathOrUrl);
    // A file. completeBaseName() is right for one — and label() still guards the case where a caller handed
    // us something url-shaped that carries no scheme (a cached download named after a query, say).
    if (!isNetworkUrl(clean)) return label(QFileInfo(clean).completeBaseName());
    int authEnd = 0;
    const QString head = detail::schemeAndHost(clean, authEnd);
    const QString path = clean.mid(authEnd);
    const QStringList segs = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (!segs.isEmpty()) return segs.last();
    const int a = clean.indexOf(QLatin1String("://")) + 3;
    return head.mid(a);   // no path at all: the host is the only honest label left
}

// Whether `s` would be changed by location() — "this value carries a credential we are about to remove".
// The migration's test, and the one an assertion can state without naming a token.
inline bool carriesCredential(const QString& s) { return location(s) != s; }
} // namespace StoredUrl
