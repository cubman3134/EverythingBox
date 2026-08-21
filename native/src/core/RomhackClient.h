// Talking to the server's romhacks capability: what hacks exist for a retro game, and the patch behind one.
//
// The server names no source — a hack's id is an opaque key minted by whichever plugin produced it, and this
// client treats it that way: an id is only ever appended to a configured server base URL as a path segment,
// never followed as a URL of its own. That is the same rule the server and the plugins hold.
//
// Nothing here decides whether a patch fits a ROM. It cannot: this side has the ROM, but only RomPatch can
// say whether the bytes agree, and for IPS nothing can — the source publishes no checksum and no target dump
// name. `targetNote` carries whatever the author did say (a container hint, a readme excerpt) so the UI can
// show it to a person and let them judge. See RomhackInstall for what happens after a hack is chosen.
#pragma once
#include <QByteArray>
#include <QString>
#include <QVector>

struct RomhackEntry
{
    QString id;          // opaque; only the server that minted it can resolve it
    QString source;      // which provider it came from, for display beside the title
    QString title;
    QString releasedBy;
    QString version;
    QString category;    // "Translation", "Hack", …
    QString language;
    QString genre;
    QString date;

    // One line for a menu row: the title, then whatever else is worth knowing at a glance.
    QString menuLabel() const;
};

struct RomhackPatchFile
{
    QString name;        // the file's own name — how a multi-patch release tells its revisions apart
    QString format;      // "ips"/"bps"/"ups", from the patch's MAGIC BYTES, never from a listing's claim
    QByteArray bytes;
};

struct RomhackFetch
{
    QString id;
    QString version;
    QString targetNote;                 // free text for a person to read; never machine-compared
    QVector<RomhackPatchFile> patches;  // >1 means the release ships a patch per ROM revision — ask, never pick
    bool valid = false;
};

namespace RomhackClient
{
    // Pure parsers over the server's JSON, split out so they test without a network in the way. Malformed or
    // unexpected JSON yields an empty list / an invalid fetch rather than throwing: a source having nothing
    // and a source being confused both mean "no hacks" to someone browsing.
    QVector<RomhackEntry> parseList(const QByteArray& json);
    RomhackFetch parseFetch(const QByteArray& json);

    // The endpoints, built from a server base URL. `id` is percent-encoded into a path segment, so an id
    // shaped like a URL cannot redirect the request somewhere else.
    QString listUrl(const QString& base, const QString& systemId, const QString& title);
    QString fetchUrl(const QString& base, const QString& id);
}
