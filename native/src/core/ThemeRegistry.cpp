#include "ThemeRegistry.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QUrl>

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

// Two relative paths that differ only in case are two entries in a GitHub tree and ONE file on Windows and
// on a default macOS volume, so writing both would have the second silently overwrite the first and the
// theme would ship whichever download happened to finish last. No per-path predicate can see this — it is a
// property of the SET — so the listing and the install each keep a case-folded ledger of what they have
// already accepted. Returns false when `rel` collides with something already in `seen`; the whole entry is
// then refused rather than collapsed, because there is no way to tell which of the two was meant.
bool claimCaseInsensitive(QSet<QString>& seen, const QString& rel)
{
    const QString key = rel.toCaseFolded();
    if (seen.contains(key)) return false;
    seen.insert(key);
    return true;
}

// Percent-encode a relative path for use in a URL, one segment at a time so the '/' separators survive —
// encoding the path whole turns them into %2F and every asset 404s. Encoding is a URL-side concern ONLY:
// nothing on the filesystem side ever decodes, or a name that is literally "%2e%2e" would become "..".
QString encodePathSegments(const QString& path)
{
    QStringList enc;
    for (const QString& seg : path.split(QLatin1Char('/')))
        enc << QString::fromUtf8(QUrl::toPercentEncoding(seg));
    return enc.join(QLatin1Char('/'));
}

// The one directory under themesRoot that installFiles owns rather than offers. It is a plain segment, so
// isPlainSegment would happily accept it as a theme folder name — and then the destination and the staging
// directory would be the same path. Named once here so the reservation and the staging path cannot drift.
const QLatin1String kStagingDirName(".eb-installing");

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

// An index URL is a raw file in a repo; the Trees API for the same repo and branch is what lists the folder
// an entry names. Only raw.githubusercontent.com can be translated — a user-added registry on another host
// still LISTS (parseIndex is host-agnostic), it just cannot be installed from in-app, and returning ""
// rather than a guessed URL is how the caller tells the two apart.
QString treeApiUrl(const QString& indexUrl)
{
    const QUrl u(indexUrl);
    if (u.host() != QLatin1String("raw.githubusercontent.com")) return QString();
    // /<owner>/<repo>/<branch>/<path...> — four segments minimum. Read the path still ENCODED: these
    // segments are about to be pasted into another URL, and a decoded '?' or '#' would truncate it into a
    // different request than the one intended.
    const QStringList p = u.path(QUrl::FullyEncoded).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (p.size() < 4) return QString();
    return QStringLiteral("https://api.github.com/repos/%1/%2/git/trees/%3?recursive=1")
        .arg(p[0], p[1], p[2]);
}

Listing filesUnder(const QByteArray& treeJson, const QString& dir)
{
    Listing out;
    if (dir.isEmpty() || !isSafeRelPath(dir))
    { out.error = QStringLiteral("This entry does not name a usable folder."); return out; }

    const QJsonDocument doc = QJsonDocument::fromJson(treeJson);
    if (!doc.isObject())
    { out.error = QStringLiteral("The registry's file listing could not be read."); return out; }
    const QJsonObject root = doc.object();

    // A truncated tree is an INCOMPLETE listing. Installing from one would silently omit files and produce
    // a theme that looks installed and is not, which is worse than refusing.
    if (root.value(QStringLiteral("truncated")).toBool())
    { out.error = QStringLiteral("This registry is too large to list; install this theme by hand."); return out; }

    // Collected locally and assigned to `out` only on success, so a Listing carrying an error can never also
    // be carrying files: ok() is the single question a caller has to ask.
    const QString prefix = dir + QLatin1Char('/');
    QStringList files;
    QSet<QString> seen;
    bool hasThemeJson = false;

    for (const QJsonValue& v : root.value(QStringLiteral("tree")).toArray())
    {
        if (!v.isObject()) continue;
        const QJsonObject o = v.toObject();
        if (o.value(QStringLiteral("type")).toString() != QLatin1String("blob")) continue;
        const QString path = o.value(QStringLiteral("path")).toString();
        if (!path.startsWith(prefix)) continue;             // prefix includes the '/', so a sibling whose
                                                            // name merely starts with dir cannot bleed in
        const QString rel = path.mid(prefix.size());
        if (!isSafeRelPath(rel))
        {
            out.error = QStringLiteral("This theme lists an unsafe file path and will not be installed.");
            return out;
        }
        if (!claimCaseInsensitive(seen, rel))
        {
            out.error = QStringLiteral("This theme lists two files that differ only in capitalisation.");
            return out;
        }
        // An ABSENT size is refused, not treated as zero: toDouble() on a missing value is 0, so an entry that
        // simply omits the key would sail past the cap. The Trees API always reports a size for a blob, so a
        // blob without one is a response we do not understand — and the whole point of the cap is that we do
        // not start a download whose size we have not agreed to.
        const QJsonValue size = o.value(QStringLiteral("size"));
        if (!size.isDouble())
        {
            out.error = QStringLiteral("This registry's file listing does not say how large its files are.");
            return out;
        }
        // The cap is interpolated rather than spelled out, so raising kMaxFileBytes cannot leave the message
        // stating a number the code no longer enforces.
        if (size.toDouble() > double(kMaxFileBytes))
        {
            out.error = QStringLiteral("This theme contains a file larger than %1 MB.")
                            .arg(double(kMaxFileBytes) / (1024.0 * 1024.0));
            return out;
        }
        // Checked BEFORE appending rather than after the loop: a hostile tree should stop being accumulated at
        // the limit, not be collected in full and then refused.
        if (files.size() >= kMaxFiles)
        { out.error = QStringLiteral("This theme contains more than %1 files.").arg(kMaxFiles); return out; }
        if (rel == QLatin1String("theme.json")) hasThemeJson = true;
        files << rel;
    }

    if (files.isEmpty())
    { out.error = QStringLiteral("This theme's folder is empty or missing from the registry."); return out; }
    if (!hasThemeJson)
    { out.error = QStringLiteral("This folder has no theme.json, so it is not a theme."); return out; }

    out.files = files;
    return out;
}

QString assetUrl(const QString& base, const QString& dir, const QString& rel)
{
    // `dir` is encoded as well as `rel`: a registry entry is free to name "themes2/My Grid", and an
    // unencoded space there 404s every file in the theme rather than just the oddly-named ones.
    return base + QLatin1Char('/') + encodePathSegments(dir) + QLatin1Char('/') + encodePathSegments(rel);
}

bool installFiles(const QString& themesRoot, const QString& folder,
                  const QVector<QPair<QString, QByteArray>>& files, QString* error)
{
    auto fail = [error](const QString& msg) { if (error) *error = msg; return false; };

    if (themesRoot.isEmpty())
        return fail(QStringLiteral("No themes folder to install into."));
    if (folder.isEmpty() || !isPlainSegment(folder))
        return fail(QStringLiteral("Unusable theme folder name."));
    // The staging directory is a plain segment and would pass the check above, which would make the
    // destination and the staging root the SAME path — every rename below then targets a child of itself.
    // It refuses safely today, by accident of that arithmetic; refuse it on purpose instead, so the property
    // survives a change to where staging lives.
    if (folder == kStagingDirName)
        return fail(QStringLiteral("That theme folder name is reserved."));
    if (files.isEmpty())
        return fail(QStringLiteral("Nothing was downloaded for this theme."));

    // VALIDATE EVERYTHING BEFORE WRITING ANYTHING. filesUnder has already checked these, but installFiles
    // is the function that turns a string into a filename and it does not get to assume its caller — a
    // hand-assembled file set, or a second surface added later, arrives here without ever having been a
    // listing.
    bool hasThemeJson = false;
    QSet<QString> seen;
    for (const auto& f : files)
    {
        if (!isSafeRelPath(f.first)) return fail(QStringLiteral("Unsafe file path: %1").arg(f.first));
        if (!claimCaseInsensitive(seen, f.first))
            return fail(QStringLiteral("Two downloaded files would land on the same name: %1").arg(f.first));
        if (f.first == QLatin1String("theme.json")) hasThemeJson = true;
    }
    if (!hasThemeJson) return fail(QStringLiteral("The download has no theme.json."));

    // Stage inside a directory that is not itself a theme. ThemeEngine::availableThemes() offers every
    // SUBDIRECTORY of themesRoot holding a theme.json, so staging in a sibling ("Grid.installing") would be
    // offered as a theme the instant theme.json was written into it, and a crash mid-install would leave
    // that phantom in the picker forever. One level deeper, nothing scans for it — and it is still on the
    // same filesystem as the destination, so the swap stays a rename rather than a copy.
    const QString dest  = themesRoot + QLatin1Char('/') + folder;
    const QString stage = themesRoot + QLatin1Char('/') + kStagingDirName;
    const QString tmp   = stage + QLatin1Char('/') + folder;
    const QString old   = stage + QLatin1Char('/') + folder + QStringLiteral(".replaced");

    // The message a half-completed swap produces names `old` and says the theme is safe there. Both halves of
    // that sentence have to survive the user's most likely next act, which is pressing Install again.
    auto strandedFail = [&] {
        return fail(QStringLiteral("Could not install into %1, and the theme that was there could not be "
                                   "put back. It is safe in %2 — move that folder back by hand.")
                        .arg(dest, old));
    };

    QDir(tmp).removeRecursively();                 // a previous run that died mid-install

    // `old` is NOT unconditionally residue, and deleting it on sight is how the promise above acquires a
    // one-call lifetime. Two states put a folder there, told apart by whether `dest` is occupied:
    //
    //   dest EXISTS   — a crash after a swap that succeeded. `old` is the version the user chose to replace
    //                   and the replacement is installed; it is stale, and removing it is right.
    //   dest ABSENT   — a swap that half-completed. `old` is the user's ONLY copy of that theme, exactly the
    //                   state the unwind below deliberately leaves behind.
    //
    // In the second case, put it back BEFORE staging anything. The cause of a double-rename failure is
    // usually still present when the retry runs — a parent that stopped accepting subdirectories, a name
    // another process is holding — so a retry is the same roll rather than a fresh one, and the old code's
    // opening removeRecursively() destroyed the folder its own error message had just called safe. Restoring
    // is strictly better than refusing outright: when it works the user is whole again (and this install
    // proceeds normally from a state indistinguishable from any other reinstall); when it does not, nothing
    // has been touched and the same message points at the same folder, so the retry costs nothing.
    if (QDir(old).exists() && !QDir(dest).exists())
    {
        if (!QDir().rename(old, dest))
            return strandedFail();
    }
    QDir(old).removeRecursively();

    // rmdir, not removeRecursively: it succeeds only when the staging directory is empty, so tidying up
    // after this install cannot delete another one that is still running. Declared before the first exit
    // that could leave something behind — mkpath creates the PARENTS first, so a failure on the leaf still
    // leaves an empty staging directory, and "a refusal leaves no residue" has to hold at every exit or it
    // is not a rule.
    auto clean = [&] { QDir(tmp).removeRecursively(); QDir(old).removeRecursively(); QDir().rmdir(stage); };

    if (!QDir().mkpath(tmp)) { clean(); return fail(QStringLiteral("Could not create %1").arg(tmp)); }

    for (const auto& f : files)
    {
        // The path written is the VALIDATED string itself, concatenated — never re-parsed through QFileInfo
        // or QUrl, never re-joined from split pieces, and never decoded. Validation that does not protect
        // the exact value used is not validation, and a decode step here would turn a file legitimately
        // named "%2e%2e" back into "..".
        const QString target = tmp + QLatin1Char('/') + f.first;
        const int slash = f.first.lastIndexOf(QLatin1Char('/'));
        if (slash > 0 && !QDir().mkpath(tmp + QLatin1Char('/') + f.first.left(slash)))
        { clean(); return fail(QStringLiteral("Could not create a folder for %1").arg(f.first)); }
        QFile out(target);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        { clean(); return fail(QStringLiteral("Could not write %1").arg(f.first)); }
        // The write is buffered, so a full disk can surface only when close() flushes it. An unchecked close
        // is how a TRUNCATED file gets renamed into place as a finished theme — the one outcome this whole
        // function exists to prevent.
        const bool wrote = out.write(f.second) == qint64(f.second.size());
        out.close();
        if (!wrote || out.error() != QFileDevice::NoError)
        { clean(); return fail(QStringLiteral("Could not write %1").arg(f.first)); }
    }

    // Swap in. The old folder goes to a second staging name first, so a rename that fails leaves the
    // previous theme intact rather than deleting it and then failing to put the new one there.
    const bool hadOld = QDir(dest).exists();
    if (hadOld && !QDir().rename(dest, old))
    { clean(); return fail(QStringLiteral("Could not replace the existing %1.").arg(folder)); }
    if (!QDir().rename(tmp, dest))
    {
        // At this point `old` is the ONLY copy of the theme the user already had: the rename that emptied
        // `dest` succeeded and the one that would have refilled it did not. Putting it back can fail for the
        // very same reason the install just did — the name taken again by another process, a parent that has
        // stopped accepting new subdirectories — and clean() would then delete it. Checking this return is
        // the whole difference between "refuses without having touched the existing install", which is what
        // the header promises, and silently destroying a theme while reporting only that the install failed.
        // So: drop the half-built copy, KEEP `old`, and say where it is so it can be recovered by hand.
        // strandedFail() is shared with the retry guard at the top on purpose — a message that drifts from
        // the path it names is a message that loses the folder.
        if (hadOld && !QDir().rename(old, dest))
        {
            QDir(tmp).removeRecursively();
            return strandedFail();
        }
        clean();
        return fail(QStringLiteral("Could not install into %1.").arg(dest));
    }
    clean();
    if (error) error->clear();
    return true;
}

} // namespace ThemeRegistry
