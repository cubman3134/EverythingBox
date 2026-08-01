#include "ThemeRegistry.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

namespace {

// Win32 strips trailing '.' and ' ' from a path component before resolving it, so "Grid " is created as
// "Grid" and "con " opens the CON device. This answers "what would Windows actually resolve?" — it is a
// question, not a fix: nothing here returns the trimmed string to a caller. isPlainSegment REJECTS any
// segment this would change, so no path ever leaves this unit in a form the listing did not name.
QString trimmedForWin32(const QString& s)
{
    int end = s.size();
    while (end > 0 && (s.at(end - 1) == QLatin1Char('.') || s.at(end - 1) == QLatin1Char(' '))) --end;
    return s.left(end);
}

// Windows opens a DEVICE for these names at any extension, so a file called "con.wav" is not a file. They
// are rejected on every platform: a theme that installs on Linux and detonates on Windows is worse than
// one that is refused everywhere, and the registry is shared across both.
//
// The stem is trimmed the way Win32 trims it before the comparison, so "con " and "con .wav" are caught
// too. isPlainSegment already refuses trailing '.'/' ' outright; this is the belt to that pair of braces,
// so the device check stays correct on its own terms if that rule is ever relaxed.
bool isReservedDeviceName(const QString& segment)
{
    static const QSet<QString> kReserved = {
        QStringLiteral("con"), QStringLiteral("prn"), QStringLiteral("aux"), QStringLiteral("nul"),
        QStringLiteral("com1"), QStringLiteral("com2"), QStringLiteral("com3"), QStringLiteral("com4"),
        QStringLiteral("com5"), QStringLiteral("com6"), QStringLiteral("com7"), QStringLiteral("com8"),
        QStringLiteral("com9"),
        QStringLiteral("lpt1"), QStringLiteral("lpt2"), QStringLiteral("lpt3"), QStringLiteral("lpt4"),
        QStringLiteral("lpt5"), QStringLiteral("lpt6"), QStringLiteral("lpt7"), QStringLiteral("lpt8"),
        QStringLiteral("lpt9"),
        QStringLiteral("conin$"), QStringLiteral("conout$"), QStringLiteral("clock$") };
    const int dot = segment.indexOf(QLatin1Char('.'));
    const QString stem = trimmedForWin32(dot < 0 ? segment : segment.left(dot)).toLower();
    return kReserved.contains(stem);
}

// One path segment is plain: non-empty, not a dot-segment, nothing Win32 would silently rewrite or refuse,
// and not a reserved device name.
bool isPlainSegment(const QString& s)
{
    if (s.isEmpty()) return false;
    if (s == QLatin1String(".") || s == QLatin1String("..")) return false;

    // A trailing '.' or ' ' does not survive to disk: "theme.json " and "theme.json." both land as
    // "theme.json", so two listed paths silently overwrite each other, and a folder recorded as "Grid "
    // is not the "Grid" that uninstall would have to remove. Reject rather than trim — the padding is
    // never meaningful, and trimming would install under a name the listing did not ask for.
    if (trimmedForWin32(s) != s) return false;

    // Separators and the drive-letter colon are the traversal-shaped characters. The rest of the Win32
    // set cannot traverse — '\' and ".." are already gone — but an embedded control character (U+0000
    // reachable through a JSON unicode escape) truncates the name at the Win32 boundary, and the
    // wildcards turn a write into a baffling failure or a collision rather than an install.
    for (const QChar c : s)
    {
        switch (c.unicode())
        {
        case u'/': case u'\\': case u':': case u'<': case u'>': case u'"': case u'|': case u'?': case u'*':
            return false;
        default:
            if (c.unicode() < 0x20) return false;
        }
    }

    if (isReservedDeviceName(s)) return false;
    return true;
}

} // namespace

namespace ThemeRegistry {

bool isSafeRelPath(const QString& rel)
{
    if (rel.isEmpty()) return false;
    if (rel.contains(QLatin1Char('\\'))) return false;      // no backslash anywhere, on any platform
    if (rel.startsWith(QLatin1Char('/'))) return false;     // absolute
    // A drive letter makes it absolute on Windows and is never valid in a registry path.
    if (rel.contains(QLatin1Char(':'))) return false;
    // split with KeepEmptyParts so "a//b" and "a/" are caught as empty segments rather than silently
    // collapsing into a valid-looking path.
    const QStringList parts = rel.split(QLatin1Char('/'), Qt::KeepEmptyParts);
    for (const QString& p : parts)
        if (!isPlainSegment(p)) return false;
    return true;
}

QString Entry::folder() const
{
    if (dir.isEmpty()) return QString();
    if (!isSafeRelPath(dir)) return QString();              // covers "..", absolute, drive letter, trailing /
    const int slash = dir.lastIndexOf(QLatin1Char('/'));
    const QString last = slash < 0 ? dir : dir.mid(slash + 1);
    return isPlainSegment(last) ? last : QString();
}

QVector<Entry> parseIndex(const QByteArray& json)
{
    QVector<Entry> out;
    const QJsonObject root = QJsonDocument::fromJson(json).object();

    // "themes2" is what the registry serves; "themes" is the legacy spelling. themes2 wins outright when
    // both are present rather than merging — two keys describing the same registry is a mistake, and
    // silently concatenating them would install from whichever the author forgot to delete.
    //
    // The fallback keys off the PRESENCE of "themes2", not on it holding anything. An index that empties
    // themes2 — a takedown, a migration, a half-finished deploy — is saying "nothing to offer"; answering
    // it from the stale legacy list would serve exactly what was withdrawn. A themes2 that is present but
    // not an array is likewise an error to surface as an empty gallery, not a silent downgrade.
    const QJsonArray arr = root.contains(QStringLiteral("themes2"))
                               ? root.value(QStringLiteral("themes2")).toArray()
                               : root.value(QStringLiteral("themes")).toArray();

    for (const QJsonValue& v : arr)
    {
        if (!v.isObject()) continue;
        const QJsonObject o = v.toObject();
        Entry e;
        e.name        = o.value(QStringLiteral("name")).toString();
        e.author      = o.value(QStringLiteral("author")).toString();
        e.description = o.value(QStringLiteral("description")).toString();
        e.dir         = o.value(QStringLiteral("dir")).toString();
        for (const QJsonValue& f : o.value(QStringLiteral("formFactors")).toArray())
            if (!f.toString().isEmpty()) e.formFactors << f.toString();

        // Drop anything without a usable folder, so no caller ever holds an Entry it cannot install. This
        // is also what rejects the legacy flat "file"/"assets" shape: it has no dir.
        if (e.folder().isEmpty()) continue;
        out << e;
    }
    return out;
}

} // namespace ThemeRegistry
