#include "DecorationPack.h"

#include "ArchiveSafePath.h"   // the ONE zip-slip rule in the product — see planInstall
#include "ThemeRegistry.h"     // isSafeRelPath: the ONE "may this become a filename?" rule

#include <QCryptographicHash>
#include <QHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include <algorithm>

namespace {

const QLatin1String kManifestName("pack.json");

// The synthetic base ArchiveSafePath::join is asked about in planInstall. planInstall is pure and has no
// destination directory yet — the point is to answer "would this member escape?" before anything is
// created — so it asks the question against a fixed absolute path instead of the real one. The answer does
// not depend on which base is used: join()'s refusals are all properties of the MEMBER (absolute, drive
// spec, UNC, a ".." that climbs past the base), and a member that escapes this base escapes any base.
//
// QDir::rootPath() rather than a hardcoded "/" so the base is absolute on Windows too ("C:/"), which is
// what join()'s cleanPath comparison needs.
QString planningBase()
{
    return QDir::rootPath() + QStringLiteral("eb-decoration-plan");
}

// The first path segment of a relative member name, and the rest. Members always use '/' inside a zip, but
// a Windows-made archive can use '\', so both are normalised before splitting — the same normalisation
// ArchiveSafePath::join does, for the same reason.
void splitFirst(const QString& member, QString* head, QString* rest)
{
    QString n = member;
    n.replace(QLatin1Char('\\'), QLatin1Char('/'));
    const int slash = n.indexOf(QLatin1Char('/'));
    if (slash < 0) { *head = n; rest->clear(); return; }
    *head = n.left(slash);
    *rest = n.mid(slash + 1);
}

// Directory entries and the junk every archiver sprinkles into a zip. Skipped BEFORE the plan is formed, so
// a "__MACOSX/._snes" resource fork never counts as a top-level name and never makes a single-folder pack
// look like a two-folder one (which would silently disable the wrapper strip).
bool isIgnorableMember(const QString& member)
{
    if (member.isEmpty()) return true;
    if (member.endsWith(QLatin1Char('/')) || member.endsWith(QLatin1Char('\\'))) return true;  // a directory
    const QString lower = member.toLower();
    if (lower.startsWith(QLatin1String("__macosx/")) || lower.contains(QLatin1String("/__macosx/"))) return true;
    const QString base = QFileInfo(lower).fileName();
    if (base == QLatin1String(".ds_store") || base == QLatin1String("thumbs.db")) return true;
    if (base.startsWith(QLatin1String("._"))) return true;
    return false;
}

} // namespace

namespace DecorationPack {

bool Entry::isValid() const
{
    // `id` becomes a directory name under bezels/<system>/, so it is held to the same rule a theme folder
    // is. A single plain segment: isSafeRelPath accepts "a/b" too, which would nest a pack a level deeper
    // than removePack looks, so the separator is refused here.
    if (id.isEmpty() || id.contains(QLatin1Char('/')) || !ThemeRegistry::isSafeRelPath(id)) return false;
    if (systems.isEmpty()) return false;
    for (const QString& s : systems)
        if (s.isEmpty() || s.contains(QLatin1Char('/')) || !ThemeRegistry::isSafeRelPath(s)) return false;
    if (zip.isEmpty()) return false;
    if (!isSha256Hex(sha256)) return false;   // REQUIRED: no digest, no install — see the header
    return true;
}

bool isSha256Hex(const QString& s)
{
    if (s.size() != 64) return false;
    for (const QChar c : s)
    {
        const ushort u = c.unicode();
        const bool digit = (u >= u'0' && u <= u'9');
        const bool lower = (u >= u'a' && u <= u'f');
        if (!digit && !lower) return false;
    }
    return true;
}

QString sha256Hex(const QByteArray& bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QString bezelsRoot(const QString& dataDir)
{
    if (dataDir.isEmpty()) return QString();
    return dataDir + QStringLiteral("/bezels");
}

QString packDir(const QString& root, const QString& system, const QString& packId)
{
    if (root.isEmpty() || system.isEmpty() || packId.isEmpty()) return QString();
    if (system.contains(QLatin1Char('/')) || packId.contains(QLatin1Char('/'))) return QString();
    if (!ThemeRegistry::isSafeRelPath(system) || !ThemeRegistry::isSafeRelPath(packId)) return QString();
    return root + QLatin1Char('/') + system + QLatin1Char('/') + packId;
}

Index parseDecorations(const QByteArray& json)
{
    Index out;

    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject())
    {
        // Same sentence, same reason, as ThemeRegistry::parseIndex: unparseable bytes, or the HTML sign-in
        // page a misconfigured host serves with a 200. Not "there are no packs" by any reading.
        out.shapeError = QStringLiteral("This registry URL did not return a registry index — the response "
                                        "is not a JSON object.");
        return out;
    }
    const QJsonObject root = doc.object();

    // A document with no `decorations` key is a themes-only registry, which is every registry that exists
    // today. Understood, and empty. See the header for why this differs from the themes2 rule.
    if (!root.contains(QStringLiteral("decorations")))
        return out;

    const QJsonValue container = root.value(QStringLiteral("decorations"));
    if (!container.isArray())
    {
        out.shapeError = QStringLiteral("This registry's \"decorations\" is not a list of packs.");
        return out;
    }

    // Counted, and the FIRST reason kept, so the message below can say what was thrown away. An array that
    // yields nothing because every element was unusable is the entry shape drifting — `zip` renamed, the
    // digest dropped when someone regenerated the index — and it is exactly as invisible as the container
    // drifting if both just produce an empty list.
    int dropped = 0;
    QString firstReason;
    auto drop = [&dropped, &firstReason](const QString& why) {
        ++dropped;
        if (firstReason.isEmpty()) firstReason = why;
    };

    QSet<QString> seenIds;
    for (const QJsonValue& v : container.toArray())
    {
        if (!v.isObject()) { drop(QStringLiteral("an element that is not an object")); continue; }
        const QJsonObject o = v.toObject();

        Entry e;
        e.id      = o.value(QStringLiteral("id")).toString().trimmed();
        e.name    = o.value(QStringLiteral("name")).toString();
        e.author  = o.value(QStringLiteral("author")).toString();
        e.license = o.value(QStringLiteral("license")).toString();
        e.version = o.value(QStringLiteral("version")).toString();
        e.zip     = o.value(QStringLiteral("zip")).toString().trimmed();
        e.sha256  = o.value(QStringLiteral("sha256")).toString().trimmed();
        e.preview = o.value(QStringLiteral("preview")).toString().trimmed();
        e.size    = qint64(o.value(QStringLiteral("size")).toDouble());
        for (const QJsonValue& s : o.value(QStringLiteral("systems")).toArray())
        {
            const QString id = s.toString().trimmed();
            if (!id.isEmpty() && !e.systems.contains(id)) e.systems << id;
        }

        if (e.id.isEmpty())        { drop(QStringLiteral("an entry with no \"id\"")); continue; }
        if (e.systems.isEmpty())   { drop(QStringLiteral("an entry with no \"systems\"")); continue; }
        if (e.zip.isEmpty())       { drop(QStringLiteral("an entry with no \"zip\"")); continue; }
        if (!isSha256Hex(e.sha256)){ drop(QStringLiteral("an entry with no usable \"sha256\"")); continue; }
        if (!e.isValid())          { drop(QStringLiteral("an entry whose \"id\" or \"systems\" cannot "
                                                         "become folder names")); continue; }
        // Two entries claiming one id would install over each other and uninstall as one. Case-folded,
        // because the folders they name are one folder on Windows and on a default macOS volume.
        if (seenIds.contains(e.id.toCaseFolded()))
        { drop(QStringLiteral("a duplicate \"id\"")); continue; }
        seenIds.insert(e.id.toCaseFolded());

        out.entries << e;
    }

    if (out.entries.isEmpty() && dropped > 0)
        out.shapeError = QStringLiteral("This registry lists %1 decoration pack(s) but none of them is "
                                        "installable — the first problem was %2. Its index format has "
                                        "probably changed.").arg(dropped).arg(firstReason);
    return out;
}

Plan planInstall(const QStringList& members, const QStringList& knownSystems)
{
    Plan plan;

    // Known ids are compared case-insensitively — a pack author zipping on Windows can easily produce
    // "SNES/" — but the DESTINATION always uses the app's own spelling of the id, never the zip's, so no
    // two casings of one system can become two folders.
    QHash<QString, QString> known;
    for (const QString& s : knownSystems)
        if (!s.isEmpty()) known.insert(s.toCaseFolded(), s);

    QStringList real;   // members that are actual files
    for (const QString& m : members)
    {
        if (isIgnorableMember(m)) continue;
        // The zip-slip refusal. Asked of ArchiveSafePath::join — the same function the real extractor
        // gates every member on — so the product has one traversal rule rather than one here and one
        // there that drift. Refuse the WHOLE pack: an archive trying to escape is not something to
        // unpack the safe half of.
        if (ArchiveSafePath::join(planningBase(), m).isEmpty())
        {
            plan.error = QStringLiteral("This pack contains an unsafe file path (\"%1\") and was not "
                                        "installed.").arg(m.left(120));
            return plan;
        }
        real << m;
    }

    if (real.isEmpty())
    {
        plan.error = QStringLiteral("This pack's download contains no files.");
        return plan;
    }
    if (real.size() > kMaxMembers)
    {
        plan.error = QStringLiteral("This pack contains %1 files, more than the %2 a decoration pack may "
                                    "install.").arg(real.size()).arg(kMaxMembers);
        return plan;
    }

    // ---- the single-top-level-folder strip -----------------------------------------------------------
    // Only when EVERY file sits under one top-level folder AND that folder is not itself a system id. See
    // the header: a single-system pack's root is one folder too, and stripping it would leave every file
    // with no system and the pack refused as empty.
    QSet<QString> tops;
    bool everyMemberIsNested = true;
    for (const QString& m : real)
    {
        QString head, rest;
        splitFirst(m, &head, &rest);
        tops.insert(head);
        if (rest.isEmpty()) everyMemberIsNested = false;   // a bare file at the root: nothing to strip into
    }
    if (tops.size() == 1 && everyMemberIsNested && !known.contains(tops.values().first().toCaseFolded()))
    {
        plan.stripTop = true;
        plan.topFolder = tops.values().first();
    }

    QSet<QString> ignoredSeen;
    QSet<QString> systemsSeen;
    QSet<QString> destSeen;   // case-folded "<system>/<rel>", so two members cannot land on one file
    for (const QString& m : real)
    {
        QString name = m;
        name.replace(QLatin1Char('\\'), QLatin1Char('/'));
        if (plan.stripTop) name = name.mid(plan.topFolder.size() + 1);

        QString head, rest;
        splitFirst(name, &head, &rest);
        const QString system = known.value(head.toCaseFolded());
        if (system.isEmpty() || rest.isEmpty())
        {
            // A README at the root, art for a console this build has never heard of, a stray folder. Named
            // once each in `ignored` and logged by the caller; never a refusal.
            const QString label = head.isEmpty() ? name : head;
            if (!ignoredSeen.contains(label)) { ignoredSeen.insert(label); plan.ignored << label; }
            continue;
        }
        // `rest` becomes a path under bezels/<system>/<packId>/, so it is held to the filename rule the
        // rest of the product uses. This is belt to the traversal check above's braces: that one asks
        // whether the member can ESCAPE, this one whether every segment of it can safely BE a filename
        // (a trailing dot, a reserved device name, a wildcard) on the platform it lands on.
        if (!ThemeRegistry::isSafeRelPath(rest))
        {
            plan.error = QStringLiteral("This pack contains a file name that cannot be written to disk "
                                        "(\"%1\") and was not installed.").arg(rest.left(120));
            return plan;
        }
        const QString destKey = (system + QLatin1Char('/') + rest).toCaseFolded();
        if (destSeen.contains(destKey))
        {
            plan.error = QStringLiteral("This pack contains two files that would land on the same name "
                                        "(\"%1/%2\") and was not installed.").arg(system, rest);
            return plan;
        }
        destSeen.insert(destKey);

        Item it;
        it.member = m;
        it.system = system;
        it.rel    = rest;
        plan.items << it;
        systemsSeen.insert(system);
    }

    if (plan.items.isEmpty())
    {
        plan.error = QStringLiteral("This pack carries no bezels for any system this app knows about.");
        return plan;
    }

    plan.systems = systemsSeen.values();
    plan.systems.sort();
    plan.ignored.sort();
    return plan;
}

QByteArray packManifest(const Entry& entry)
{
    QJsonObject o;
    o.insert(QStringLiteral("id"), entry.id);
    o.insert(QStringLiteral("name"), entry.name);
    o.insert(QStringLiteral("version"), entry.version);
    o.insert(QStringLiteral("author"), entry.author);
    o.insert(QStringLiteral("license"), entry.license);
    QJsonArray sys;
    for (const QString& s : entry.systems) sys.append(s);
    o.insert(QStringLiteral("systems"), sys);
    return QJsonDocument(o).toJson(QJsonDocument::Indented);
}

QVector<Installed> installedPacks(const QString& root)
{
    QVector<Installed> out;
    if (root.isEmpty()) return out;

    QHash<QString, Installed> byId;
    const QFileInfoList systemDirs = QDir(root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo& sd : systemDirs)
    {
        const QString system = sd.fileName();
        if (system.startsWith(QLatin1Char('.'))) continue;   // the installer's staging directory
        const QFileInfoList packDirs = QDir(sd.absoluteFilePath())
                                           .entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QFileInfo& pd : packDirs)
        {
            const QString id = pd.fileName();
            if (id.startsWith(QLatin1Char('.'))) continue;
            Installed& rec = byId[id];
            rec.id = id;
            if (!rec.systems.contains(system)) rec.systems << system;
            // The manifest supplies only display fields, and only the first one found is read: the
            // directory layout is what says a pack is installed, so a pack whose pack.json was deleted is
            // still listed (under its id) and still removable.
            if (rec.name.isEmpty())
            {
                QFile f(pd.absoluteFilePath() + QLatin1Char('/') + kManifestName);
                if (f.open(QIODevice::ReadOnly))
                {
                    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
                    rec.name    = o.value(QStringLiteral("name")).toString();
                    rec.version = o.value(QStringLiteral("version")).toString();
                    rec.author  = o.value(QStringLiteral("author")).toString();
                }
            }
        }
    }

    // ONE temporary. `QVector<T>(byId.values().begin(), byId.values().end())` builds a QList, takes an
    // iterator into it, destroys it, builds a SECOND one and takes an iterator into that — two iterators
    // into two dead containers. It compiles, and it is undefined behaviour: probe_decopack caught it as a
    // wrong count on the first run and a segfault on the third.
    const QList<Installed> vals = byId.values();
    out = QVector<Installed>(vals.begin(), vals.end());
    for (Installed& i : out) i.systems.sort();
    std::sort(out.begin(), out.end(), [](const Installed& a, const Installed& b) { return a.id < b.id; });
    return out;
}

QStringList packsForSystem(const QString& root, const QString& system)
{
    if (root.isEmpty() || system.isEmpty()) return {};
    const QString dir = root + QLatin1Char('/') + system;
    QStringList ids;
    for (const QFileInfo& pd : QDir(dir).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
    {
        const QString id = pd.fileName();
        if (id.startsWith(QLatin1Char('.'))) continue;
        ids << id;
    }
    ids.sort();   // stable competition order between two packs for one system, across runs and platforms
    return ids;
}

bool removePack(const QString& root, const QString& packId, QString* error)
{
    auto fail = [error](const QString& m) { if (error) *error = m; return false; };

    if (root.isEmpty()) return fail(QStringLiteral("No decorations folder to remove from."));
    if (packId.isEmpty() || packId.contains(QLatin1Char('/')) || !ThemeRegistry::isSafeRelPath(packId))
        return fail(QStringLiteral("\"%1\" is not a decoration pack this app installed.").arg(packId));

    int removed = 0;
    QStringList stuck;
    for (const QFileInfo& sd : QDir(root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
    {
        const QString dir = sd.absoluteFilePath() + QLatin1Char('/') + packId;
        if (!QFileInfo::exists(dir)) continue;
        if (QDir(dir).removeRecursively()) ++removed;
        else stuck << sd.fileName();
    }

    if (!stuck.isEmpty())
        return fail(QStringLiteral("Removed this pack from %1 system(s), but %2 could not be deleted — "
                                   "something may still be using it.")
                        .arg(removed).arg(stuck.join(QStringLiteral(", "))));
    if (removed == 0)
        return fail(QStringLiteral("That decoration pack isn't installed."));
    return true;
}

} // namespace DecorationPack
