#include "ThemeRegistry.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

namespace {

// Windows opens a DEVICE for these names at any extension, so a file called "con.wav" is not a file. They
// are rejected on every platform: a theme that installs on Linux and detonates on Windows is worse than
// one that is refused everywhere, and the registry is shared across both.
bool isReservedDeviceName(const QString& segment)
{
    static const QSet<QString> kReserved = {
        QStringLiteral("con"), QStringLiteral("prn"), QStringLiteral("aux"), QStringLiteral("nul"),
        QStringLiteral("com1"), QStringLiteral("com2"), QStringLiteral("com3"), QStringLiteral("com4"),
        QStringLiteral("com5"), QStringLiteral("com6"), QStringLiteral("com7"), QStringLiteral("com8"),
        QStringLiteral("com9"),
        QStringLiteral("lpt1"), QStringLiteral("lpt2"), QStringLiteral("lpt3"), QStringLiteral("lpt4"),
        QStringLiteral("lpt5"), QStringLiteral("lpt6"), QStringLiteral("lpt7"), QStringLiteral("lpt8"),
        QStringLiteral("lpt9") };
    const int dot = segment.indexOf(QLatin1Char('.'));
    const QString stem = (dot < 0 ? segment : segment.left(dot)).toLower();
    return kReserved.contains(stem);
}

// One path segment is plain: non-empty, not a dot-segment, no separator or drive-letter character, and not
// a reserved device name.
bool isPlainSegment(const QString& s)
{
    if (s.isEmpty()) return false;
    if (s == QLatin1String(".") || s == QLatin1String("..")) return false;
    if (s.contains(QLatin1Char('/')) || s.contains(QLatin1Char('\\')) || s.contains(QLatin1Char(':')))
        return false;
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
    QJsonArray arr = root.value(QStringLiteral("themes2")).toArray();
    if (arr.isEmpty()) arr = root.value(QStringLiteral("themes")).toArray();

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
