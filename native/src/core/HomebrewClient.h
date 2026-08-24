// Talking to the server's homebrew capability: what homebrew exists for a console.
//
// The server names no source — a title's id is an opaque key minted by whichever plugin produced it, and this
// client treats it that way: an id is only ever appended to a configured server base URL as a path segment,
// never followed as a URL of its own. That is the same rule the server and the plugins hold.
//
// There is deliberately NO fetch route here, and adding one would be a mistake. A title's `id` is already a
// fully host-namespaced media id — the same kind a catalog row carries — so a homebrew row plays through the
// ordinary stream route every other remote leaf uses. A second download path would be a second thing to keep
// correct, for nothing.
//
// Nothing in this file opens a socket or owns a QObject. It is split out for the same reason RomhackClient is:
// so the URL building and the parsing test without a network in the way.
#pragma once
#include <QByteArray>
#include <QString>
#include <QVector>

// One homebrew title. Everything but `id` and `title` is optional — a source that states only a name still
// yields a usable row, and an absent field is empty rather than the word "null".
struct HomebrewTitle
{
    QString id;          // a fully host-namespaced media id; the stream route already knows how to resolve it
    QString title;
    QString author;
    QString version;
    QString description;
    QString imageUrl;

    // The second line of a row: whoever made it, and which release. Each part is dropped when the source did
    // not supply it, so a bare listing reads as a plain title rather than " · v".
    QString subtitle() const;
};

// One page of titles. `nextCursor` is opaque to this client exactly as it is to the server — whatever the
// source needs to resume — and empty when there is no more. It is carried back verbatim, never parsed.
struct HomebrewPage
{
    QVector<HomebrewTitle> items;
    QString nextCursor;
    bool hasMore() const { return !nextCursor.isEmpty(); }
};

// Where one server left off. A console's homebrew is merged from every configured server, and each has its
// own cursor, so "the next page" is a set of these rather than a single token.
struct HomebrewMore
{
    QString base;
    QString cursor;
};

namespace HomebrewClient
{
    // A pure parser over the server's JSON, so it tests without a network in the way. A body that is not the
    // expected object — an error page, a challenge body, `[]`, `null`, garbage — yields an empty page rather
    // than throwing: a console having no homebrew and a source being confused both mean "nothing here" to
    // someone browsing, and neither is worth breaking a page over.
    HomebrewPage parseList(const QByteArray& json);

    // GET {base}/homebrew/{systemId}[?cursor=...]. The system id is percent-encoded as ONE path segment, so an
    // id shaped like "https://evil/x" becomes a segment on our own server rather than a request to somewhere
    // else. The cursor is percent-encoded for the same reason and is otherwise untouched.
    QString listUrl(const QString& base, const QString& systemId, const QString& cursor = QString());

    // ---- the level markers -------------------------------------------------------------------------------
    // The folder's own level carries levelMime(system) so that after a Back out of a played title the view can
    // rebuild it from the marker alone. The "More…" row carries moreMime(...), which holds every server that
    // still has pages together with its opaque cursor. Both are written in one place and read in another; they
    // live here so the two sides cannot drift, and so a probe can state that they round-trip.
    QString levelMime(const QString& system);
    QString levelSystem(const QString& mime);   // empty unless `mime` is a homebrew level marker

    QString moreMime(const QString& system, const QVector<HomebrewMore>& more);
    QString moreSystem(const QString& mime);            // empty unless `mime` is a homebrew paging marker
    QVector<HomebrewMore> moreCursors(const QString& mime);
}
