// ThemeRegistry — the pure core of the in-app theme gallery: what a community registry index means, and
// what may be written to disk because of one.
//
// A themes2 theme is a FOLDER (theme.json plus optional sounds/ and fonts/), and a registry entry names
// that folder rather than listing files: {name, author, description, dir: "themes2/<Name>"}. The dead
// RegistryBrowser::Themes path this replaces read a "file"/"assets" pair that no live entry has ever had.
//
// Everything here is QtCore-only and network-free so probe_themereg can pin it headlessly, and so BOTH
// Appearance builders share one copy of the parts that are easy to get subtly wrong: which index key to
// read, which paths may become filenames, and how a folder lands on disk without a half-install surviving.
#pragma once
#include <QByteArray>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

namespace ThemeRegistry {

// Refuse an entry whose listing is implausible for a theme. A registry is public and anyone may open a
// pull request against one; these are the bounds beyond which we stop rather than download.
inline constexpr int    kMaxFiles     = 64;
inline constexpr qint64 kMaxFileBytes = 8 * 1024 * 1024;

struct Entry {
    QString     name;          // display text ONLY — never used as a path
    QString     author;
    QString     description;
    QString     dir;           // "themes2/<Name>", relative to the index URL's directory
    QStringList formFactors;   // advisory note on the row; does not filter

    // The install folder: the last segment of `dir`. Empty when `dir` is unusable, which is the single
    // predicate callers check — parseIndex already drops those, so an Entry in hand always has one.
    QString folder() const;
};

// Parse a registry index. Reads "themes2" (what the registry serves) and falls back to "themes" (the key
// the pre-existing code assumed) ONLY when "themes2" is absent — a present themes2 wins outright, even
// empty or malformed, so a registry that empties it is not silently answered from the legacy list.
// Entries without a usable `dir` are DROPPED, so every returned Entry has a non-empty folder().
QVector<Entry> parseIndex(const QByteArray& json);

// May this relative path become a filename? Accepts only a relative path whose every segment is plain:
// no "." or ".." segment, no leading "/", no drive letter, no backslash, no empty segment, no Windows
// reserved device name, no Windows-illegal character (< > : " | ? * and control characters), and no
// trailing "." or " " — Win32 strips those before resolving, so such a segment names something other than
// itself on disk. Rejects rather than sanitises: rewriting a hostile path guesses at intent, and no theme
// has a benign reason to ship one. No path in, no path out — every answer here is a plain yes or no.
bool isSafeRelPath(const QString& rel);

} // namespace ThemeRegistry
