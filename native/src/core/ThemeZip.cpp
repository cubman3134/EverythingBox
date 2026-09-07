#include "ThemeZip.h"

#include "ThemeRegistry.h"   // the caps and isSafeRelPath — one set of numbers for both install lanes

#include <QSet>
#include <QStringList>

#include <cstring>

extern "C" {
#include "miniz.h"
}

namespace {

// A member name is attacker-supplied text on its way to a label. Bounded before it gets there, the same way
// parseIndex bounds the key list it reports.
QString elide(const QString& s)
{
    return s.size() > 80 ? s.left(80) + QStringLiteral("…") : s;
}

// A copy with '\' read as a separator, used ONLY to answer "what shape is this?". Nothing normalised here
// reaches the filesystem: the name that survives is the one the archive spelled, and isSafeRelPath refuses a
// backslash in it outright. Normalising for the shape checks is what lets "..\\..\\evil" be reported as the
// traversal it is rather than as a generic bad character.
QString shapeView(const QString& name)
{
    QString v = name;
    v.replace(QLatin1Char('\\'), QLatin1Char('/'));
    return v;
}

// The single top-level directory every member sits under, or empty when there is not exactly one. This is
// the shape GitHub's "Download ZIP" and every desktop archiver produce, and stripping it is why an archive
// of "MyTheme/theme.json" installs the same as one of "theme.json".
QString soleWrapper(const QStringList& names)
{
    QString prefix;
    for (const QString& n : names)
    {
        const int slash = n.indexOf(QLatin1Char('/'));
        if (slash <= 0) return QString();                       // a member at the root: no single wrapper
        const QString head = n.left(slash);
        if (prefix.isEmpty()) prefix = head;
        else if (prefix != head) return QString();              // two different tops
    }
    return prefix;
}

} // namespace

namespace ThemeZip {

bool unpack(const QByteArray& zipBytes, QVector<QPair<QString, QByteArray>>* files, QString* error)
{
    auto fail = [error](const QString& msg) { if (error) *error = msg; return false; };
    if (files) files->clear();
    if (!files) return fail(QStringLiteral("Nothing to unpack into."));

    if (zipBytes.isEmpty())
        return fail(QStringLiteral("The download was empty."));

    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_mem(&zip, zipBytes.constData(), size_t(zipBytes.size()), 0))
        return fail(QStringLiteral("That download is not a readable .zip archive."));

    struct Member { mz_uint index; QString name; qint64 size; };
    QVector<Member> members;
    qint64 total = 0;

    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count; ++i)
    {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st))
        {
            mz_zip_reader_end(&zip);
            return fail(QStringLiteral("This archive's directory could not be read."));
        }
        if (mz_zip_reader_is_file_a_directory(&zip, i)) continue;   // directories are implied by the paths

        const QString name  = QString::fromUtf8(st.m_filename);
        const QString shape = shapeView(name);

        // (1) SYMLINK. The high 16 bits of a zip's external attributes are the Unix mode when the archive
        // was made on a Unix host; S_IFLNK is 0xA000 under the S_IFMT mask 0xF000. A Windows-made archive
        // leaves those bits zero, so this cannot misfire on one.
        if (((st.m_external_attr >> 16) & 0xF000u) == 0xA000u)
        {
            mz_zip_reader_end(&zip);
            return fail(QStringLiteral("This theme's archive contains a symbolic link (%1). Refused — a "
                                       "theme is files, and a link is a way out of the themes folder.")
                            .arg(elide(name)));
        }
        // (2) ABSOLUTE. POSIX "/x" and UNC "//host/share/x" both begin with a separator once '\' is read
        // as one, and neither is joined under anything.
        if (shape.startsWith(QLatin1Char('/')))
        {
            mz_zip_reader_end(&zip);
            return fail(QStringLiteral("This theme's archive contains an absolute path (%1). Refused.")
                            .arg(elide(name)));
        }
        // (3) DRIVE LETTER. "C:/x", "C:x" and a bare "C:" are all absolute on Windows, and cleanPath keeps
        // the "C:" through a join — which is exactly how such a member escapes.
        if (shape.size() >= 2 && shape.at(1) == QLatin1Char(':'))
        {
            mz_zip_reader_end(&zip);
            return fail(QStringLiteral("This theme's archive contains a drive-letter path (%1). Refused.")
                            .arg(elide(name)));
        }
        // (4) TRAVERSAL. Any ".." segment at all, wherever it sits: "../x", "a/../../x" and "a/.." all
        // climb, and a member that merely resolves back inside is still a listing this app will not run.
        {
            bool climbs = false;
            for (const QString& seg : shape.split(QLatin1Char('/'), Qt::KeepEmptyParts))
                if (seg == QLatin1String("..")) { climbs = true; break; }
            if (climbs)
            {
                mz_zip_reader_end(&zip);
                return fail(QStringLiteral("This theme's archive contains a path that climbs out of the "
                                           "theme folder (%1). Refused.").arg(elide(name)));
            }
        }
        // The belt to those four braces, and the rule the tree lane already installs under: no backslash,
        // no dot segment, no Windows-reserved device name, nothing Win32 would silently rewrite. A name
        // that reaches here has been proved to be a shape, not merely not-one-of-four.
        if (!ThemeRegistry::isSafeRelPath(name))
        {
            mz_zip_reader_end(&zip);
            return fail(QStringLiteral("This theme's archive contains a member this app will not write "
                                       "(%1). Refused.").arg(elide(name)));
        }

        if (st.m_uncomp_size > mz_uint64(ThemeRegistry::kMaxFileBytes))
        {
            mz_zip_reader_end(&zip);
            return fail(QStringLiteral("%1 is larger than a theme file may be.").arg(elide(name)));
        }
        total += qint64(st.m_uncomp_size);
        if (total > ThemeRegistry::kMaxTotalBytes)
        {
            mz_zip_reader_end(&zip);
            return fail(QStringLiteral("This theme unpacks to more than a theme may be."));
        }
        members << Member{ i, name, qint64(st.m_uncomp_size) };
        if (members.size() > ThemeRegistry::kMaxFiles)
        {
            mz_zip_reader_end(&zip);
            return fail(QStringLiteral("This theme's archive holds more files than a theme may have."));
        }
    }

    if (members.isEmpty())
    {
        mz_zip_reader_end(&zip);
        return fail(QStringLiteral("That archive holds no files."));
    }

    // The wrapper folder, decided over the WHOLE set rather than per member: one directory that every
    // member sits under and that no theme.json sits beside.
    QStringList names;
    names.reserve(members.size());
    for (const Member& m : members) names << m.name;
    if (!names.contains(QStringLiteral("theme.json")))
    {
        const QString wrapper = soleWrapper(names);
        if (!wrapper.isEmpty())
            for (Member& m : members) m.name = m.name.mid(wrapper.size() + 1);
    }

    // Case collisions AFTER the strip, because the strip is what can create one.
    QSet<QString> seen;
    bool hasThemeJson = false;
    for (const Member& m : members)
    {
        if (m.name.isEmpty() || !ThemeRegistry::isSafeRelPath(m.name))
        {
            mz_zip_reader_end(&zip);
            return fail(QStringLiteral("This theme's archive contains a member this app will not write "
                                       "(%1). Refused.").arg(elide(m.name)));
        }
        const QString key = m.name.toCaseFolded();
        if (seen.contains(key))
        {
            mz_zip_reader_end(&zip);
            return fail(QStringLiteral("Two files in this archive would land on the same name: %1")
                            .arg(elide(m.name)));
        }
        seen.insert(key);
        if (m.name == QLatin1String("theme.json")) hasThemeJson = true;
    }
    if (!hasThemeJson)
    {
        mz_zip_reader_end(&zip);
        return fail(QStringLiteral("That archive has no theme.json, so it is not a theme."));
    }

    // Only now is a single byte decompressed. Every refusal above happened against the central directory.
    for (const Member& m : members)
    {
        QByteArray blob;
        blob.resize(int(m.size));
        if (m.size > 0 && !mz_zip_reader_extract_to_mem(&zip, m.index, blob.data(), size_t(m.size), 0))
        {
            mz_zip_reader_end(&zip);
            return fail(QStringLiteral("%1 could not be unpacked from this archive.").arg(elide(m.name)));
        }
        *files << qMakePair(m.name, blob);
    }

    mz_zip_reader_end(&zip);
    return true;
}

bool install(const QByteArray& zipBytes, const QString& themesRoot, const QString& folder,
             const ThemeRegistry::Record& record, QString* error)
{
    QVector<QPair<QString, QByteArray>> files;
    if (!unpack(zipBytes, &files, error)) return false;
    return ThemeRegistry::installFiles(themesRoot, folder, ThemeRegistry::withRecord(files, record), error);
}

} // namespace ThemeZip
