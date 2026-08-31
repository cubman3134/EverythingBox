// A REMOTE AUDIOBOOK RELEASE, AS A BOOK RATHER THAN AS A LINK (issue #214).
//
// THE BUG THIS EXISTS FOR, in the words of the three reports that turned out to be one. Asking for an
// audiobook resolved the chosen RELEASE to a single link and handed that to the player. A release is
// almost always MANY FILES — a folder of numbered MP3s, a set of M4B parts — and none of them is the
// book: each is a slice of it. So what played was whichever file the source happened to return first:
// a zip of the whole torrent (nothing played), a link that stalled (silence), and, most clearly, a
// 43-minute MP3 of a fifteen-hour book, which started the listener at chapter ten and then recorded
// their position against that chapter.
//
// Every layer downstream behaved correctly on the file it was given. The format gate (#207) passed it:
// it IS audio. The player played it: it IS a valid MP3. Resume recorded a position in it. The only
// thing wrong was that the file was one arbitrary slice of a book nobody asked to start in the middle
// of — which is precisely why nothing anywhere reported a fault.
//
// ---- THE MODEL IS THE LOCAL ONE, DELIBERATELY ------------------------------------------------------
//
// AudiobookLibrary (#139) already treats a folder of numbered parts as ONE BOOK: one tile, one ordered
// queue, one resume point, ordered through core/NaturalOrder. This is that model applied to a release
// somebody else is holding, and the two are the same shape on purpose:
//
//   local                                      remote (here)
//   -----                                      -------------
//   Book::files, ordered                       playableParts(), ordered
//   BookFile::path — what playback is handed    Part::id — what MINTS what playback is handed
//   one resume point, on the last part with     the same rule, over the same ResumeStore marks
//     a position
//
// The ONE difference, and it is forced: a local part is a file that will still be there tomorrow, and
// a remote part is a link that will not. So the queue cannot hold links.
//
// ---- WHY A PART IS A NAME AND NEVER A LINK ---------------------------------------------------------
//
// Five consecutive bugs (#200, #202, #203, #204 and the client half of #207) came from one instinct —
// "I have a url, I will keep it". A resolved stream url is SIGNED: it carries a credential, it is
// minted for one playback, and it expires. Writing one down leaks the credential (#200) and stores an
// identity that stops being one (#203). StoredUrl, StoredIdentity and CredentialScrub are what came
// out of that work, and the rule they settled is the rule here: A SIGNED LINK IS NEVER AN IDENTITY.
//
// An audiobook is the worst case for getting this wrong in BOTH directions:
//
//   * as an identity, because a book is listened to over days and its resume row has to survive them;
//   * as a link, because a fifteen-hour book reaches part forty days after part one was signed. A
//     queue holding forty pre-minted links plays for an hour and then dies mid-book, and what the user
//     sees is the app breaking rather than a link ageing out.
//
// So the queue holds a PART TOKEN — the book it belongs to and the file it is, and nothing else — and
// the link for a part is minted at the moment the app reaches it. The token is what resume, the
// consumption stats and the playlist row are keyed by; it is credential-free by construction (there is
// no url in it to scrub), and it means the same thing tomorrow.
//
// ---- WHAT THIS FILE DECIDES, AND WHAT IT DOES NOT --------------------------------------------------
//
// It decides WHICH of a release's files are playable and IN WHAT ORDER, as pure functions over names,
// so both can be driven headlessly (probe_remotebook) and mutation-tested. Everything else belongs to
// somebody else: the server enumerates the release (it already owns that conversation and the client
// must not grow a second one), AddonManager fetches the list, and MainWindow builds the queue.
//
// The FILTER is deliberately a whitelist of audio extensions and not "anything that is not obviously
// junk". A release contains cover.jpg, an .nfo, a .txt of the chapter list, sometimes the ebook — and
// a rule shaped as "drop the ones I have heard of" would put the next unfamiliar file into the queue
// and give the listener silence in the middle of a book. The cost of the whitelist is the opposite and
// far cheaper: an exotic codec is left out of the queue, which is visible immediately.
//
// The ORDER is core/NaturalOrder's, which is not a detail. A hand-built QCollator is INERT under the C
// locale — numeric mode is accepted, reads back true, and does nothing — so "10 - part" sorts before
// "2 - part" on any machine with LANG/LC_ALL unset, which is every CI runner and every kiosk session.
// #205 is the issue that found it; NaturalOrder.h states it at length. A book that starts at part ten
// is the exact defect this file exists to remove, so ordering it wrong here would reintroduce the bug
// with a different cause.
//
// Nothing here reads Settings, touches the filesystem or knows what a network is.
#pragma once
#include "NaturalOrder.h"

#include <QChar>
#include <QCollator>
#include <QFileInfo>
#include <QLatin1String>
#include <QString>
#include <QStringList>
#include <QVector>

#include <algorithm>

namespace RemoteAudiobook
{
// One file of a release, as the source described it.
struct Part
{
    // THE SOURCE'S OWN ITEM ID for this file — the release plus the file, which is what the server
    // mints a link from when the app reaches this part. Never a url: see the header. Empty is not a
    // part (playableParts drops it), because a row nothing can resolve is worse than one row fewer.
    QString id;
    // What the release calls the file. The ordering key AND the display title, because for a release
    // of numbered parts the file name is the only description of the part that exists.
    QString fileName;
    // Whatever the source said about it — a SIZE, for every source in this tree ("42.19 MB"). Shown as the
    // part's second line, and, since #218, read for its bytes as well: the part sizes plus one real duration
    // from mpv are what let the position bar show the whole book instead of part four of fifty-seven. Read
    // through BookTimeline::bytesFromSizeText, which answers 0 for anything that is not a size — a source
    // that puts something else here costs the book its book-scale bar and nothing else, which is the
    // fallback the issue asks for. Still never routed on: nothing is fetched, opened or keyed by it.
    QString subtitle;

    bool isValid() const { return !id.isEmpty() && !fileName.isEmpty(); }
};

// The extensions a part may have. The same set MainWindow's open-dialog filter and folder-queue scan
// use, plus the two lossless-container spellings a book rip occasionally carries. Whitelist, for the
// reason the header gives.
inline const QStringList& audioExtensions()
{
    static const QStringList kExts = {
        QStringLiteral("mp3"),  QStringLiteral("flac"), QStringLiteral("ogg"),  QStringLiteral("opus"),
        QStringLiteral("wav"),  QStringLiteral("m4a"),  QStringLiteral("m4b"),  QStringLiteral("aac"),
        QStringLiteral("wma"),  QStringLiteral("alac"), QStringLiteral("aiff"), QStringLiteral("aif"),
        QStringLiteral("ape"),  QStringLiteral("mka"),  QStringLiteral("mp4"),  QStringLiteral("oga"),
    };
    return kExts;
}

// "Is this file name one of a book's parts?" — its extension, and nothing else about it.
//
// TWO STEPS BEFORE QFileInfo, and both earn their line. The TRIM, because a release's listing carries
// whatever whitespace its packer left in ("… 01.mp3 " has the suffix "mp3 ", which matches nothing). And
// the BACKSLASH, because a release lists its files with their folders and QFileInfo does not treat '\' as
// a separator anywhere but Windows — so "Disc 1\01 - part.mp3" would keep the whole string as its file
// name on Linux and on CI. Splitting the LAST path segment out by hand would be a third step and is not
// one: QFileInfo::suffix already reads the segment after the final '/', which is also what makes a bare
// folder entry ("Disc 1/") answer no — it has no extension, so there is nothing to accept.
inline bool isAudioFileName(const QString& fileName)
{
    QString name = fileName.trimmed();
    if (name.isEmpty()) return false;
    name.replace(QLatin1Char('\\'), QLatin1Char('/'));
    const QString ext = QFileInfo(name).suffix().toLower();
    if (ext.isEmpty()) return false;
    return audioExtensions().contains(ext);
}

// THE BOOK, out of the release: every audio file it contains, in the order they are meant to be heard.
//
// Filtered THEN ordered, which is the order the two steps have to run in — a cover.jpg sorted into the
// middle of the parts and then dropped is the same answer, but an .nfo named "00 - readme.nfo" sorted
// FIRST and then dropped would leave the queue's first entry decided by a file that is not in it.
//
// Ordered by NATURAL FILE NAME and by nothing else. There is no track tag to read (these files have
// not been fetched, only named), no disc number, and no metadata of any kind — the name is all there
// is, which is also why "10" must not sort before "2" (see the header, and #205).
//
// A release whose files are all named identically keeps the order the source listed them in:
// std::stable_sort, so an equal comparison is not a reshuffle.
inline QVector<Part> playableParts(const QVector<Part>& listed)
{
    QVector<Part> parts;
    parts.reserve(listed.size());
    for (const Part& p : listed)
        if (p.isValid() && isAudioFileName(p.fileName)) parts.push_back(p);

    const QCollator collator = NaturalOrder::collator();
    std::stable_sort(parts.begin(), parts.end(), [&collator](const Part& a, const Part& b) {
        return collator.compare(a.fileName, b.fileName) < 0;
    });
    return parts;
}

// ---- THE QUEUE'S PART TOKEN -------------------------------------------------------------------------
//
// What the queue actually holds, and what resume/stats/the playlist row are keyed by: the BOOK and the
// FILE, joined. Never the part's source id, which for some sources encodes the release blob and so
// changes between searches; never a link, which expires. The book key is whatever the caller already
// uses to identify the book — the catalog item's id — so a listener who reopens the same book after a
// re-search lands on the same tokens and resumes where they were.
//
// 0x1F (UNIT SEPARATOR) is the join, the same character AudiobookLibrary's book keys use, because it
// cannot occur in a file name on any filesystem and cannot occur in a catalog id.
//
// The scheme prefix earns its place twice: it is what the play choke point recognises a token BY, and
// it makes the token not-a-url, so StoredUrl leaves it byte for byte (there is no query to scrub and
// no host to reduce to) and no store can mistake it for a location.
inline const char* kPartScheme = "ebaudiobookpart:";
inline QChar partSeparator() { return QChar(0x1F); }

inline QString partToken(const QString& bookKey, const QString& fileName)
{
    if (bookKey.isEmpty() || fileName.isEmpty()) return QString();
    return QString::fromLatin1(kPartScheme) + bookKey + partSeparator() + fileName;
}

inline bool isPartToken(const QString& s)
{
    return s.startsWith(QLatin1String(kPartScheme)) && s.contains(partSeparator());
}

// The book a token belongs to, and the file it is. Empty for anything that is not a token — every
// caller already has to handle that, because most play paths are not one.
inline QString bookKeyOfToken(const QString& token)
{
    if (!isPartToken(token)) return QString();
    const int start = QLatin1String(kPartScheme).size();
    const int sep = token.indexOf(partSeparator(), start);
    return sep < 0 ? QString() : token.mid(start, sep - start);
}

inline QString fileNameOfToken(const QString& token)
{
    if (!isPartToken(token)) return QString();
    const int sep = token.indexOf(partSeparator(), QLatin1String(kPartScheme).size());
    return sep < 0 ? QString() : token.mid(sep + 1);
}

// The display title for a part row: the file name without its extension, which is what a person reads
// on a now-playing list ("01 - Chapter One" rather than "01 - Chapter One.mp3"). Folders are dropped
// for the same reason — a listener does not need to be told which disc directory a part sits in on
// somebody else's machine. Never empty for a valid part, so no queue row is blank.
inline QString partTitle(const QString& fileName)
{
    QString leaf = fileName;
    leaf.replace(QLatin1Char('\\'), QLatin1Char('/'));
    const int slash = leaf.lastIndexOf(QLatin1Char('/'));
    if (slash >= 0) leaf = leaf.mid(slash + 1);
    const QString base = QFileInfo(leaf).completeBaseName();
    return base.isEmpty() ? leaf : base;
}
} // namespace RemoteAudiobook
