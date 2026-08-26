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
    QString format;      // "ips"/"bps"/"ups", from the patch's MAGIC BYTES, never from a listing's claim —
                         // except "rom", which a source ASSERTS, a finished game announcing nothing
    // Where to fetch the file, RELATIVE to the server that answered this fetch. Not the bytes: a patch may
    // be a 14-byte IPS or a gigabyte-scale pre-applied disc image, and one response shape carries both only
    // if it carries neither. Never followed as given — see RomhackClient::fileUrl.
    QString url;
};

// What the source says the patch was built against. Every field is optional and any one of them may be the
// only thing stated. Unlike targetNote below, this IS machine-comparable — which is the whole point: IPS
// carries no checksum and applies cleanly to any bytes at all, so without a stated target there is nothing
// to check a ROM against and the question can only be put to the user.
struct RomhackTarget
{
    QString fileName;   // the dump's catalogued name, e.g. "Final Fantasy V (Japan).sfc"
    QString crc32;      // lowercase hex of the ORIGINAL, unpatched ROM
    QString sha1;       // lowercase hex, same
    QString region;     // a short marker ("J", "Japan") for sources that identify a release only that far

    bool isEmpty() const
    { return fileName.isEmpty() && crc32.isEmpty() && sha1.isEmpty() && region.isEmpty(); }
    // Something we can actually verify a file against, as opposed to something we can only show a person.
    bool checkable() const { return !crc32.isEmpty() || !sha1.isEmpty(); }
};

struct RomhackFetch
{
    QString id;
    QString version;
    QString targetNote;                 // free text for a person to read; never machine-compared
    RomhackTarget target;               // what it was built against, when the source stated it
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

    // Is this the kind of url a fetch is allowed to hand back — a reference RELATIVE to the server we
    // already asked? What it guarantees is that and no more: a reference out of a response is resolved
    // against the base we configured, never followed as a url in its own right — no scheme, no host, no
    // root, no climbing up. Where the request is then allowed to END UP is a separate decision, belonging
    // to the fetch site and the redirect policy it sets, not to this check — and one that has to be STATED
    // there rather than inherited, because the default (NoLessSafeRedirectPolicy) follows a cross-host 302
    // and would take the transfer off our server with no malformed reference involved at all. The same rule
    // fetchUrl() enforces for ids, in the one other shape it can arrive.
    bool isSafeRelativeFileUrl(const QString& url);

    // The absolute url to fetch a patch file from: the server that answered the fetch, plus the relative
    // reference it gave. Empty when the reference is one we will not follow, or when there is no base — and
    // an empty url is a refusal every caller already reads as "couldn't get it".
    QString fileUrl(const QString& base, const QString& relative);
}
