#include "StoredIdentity.h"
#include "AppBrand.h"
#include "AppPaths.h"
#include "ProfileStore.h"
#include "StoredUrl.h"
#include "Subsonic.h"
#include "SubsonicServerStore.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QStringList>

namespace {

QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

// The profile a "playlists/<profile>/items" key belongs to, or empty when the key is not one of those.
// Spelled by shape rather than by walking groups, for CredentialScrub's reason: the profile segment is
// variable and a group walk needs the same string surgery with more places to get it wrong.
QString profileOfPlaylistKey(const QString& k)
{
    static const QString kHead = QStringLiteral("playlists/");
    static const QString kTail = QStringLiteral("/items");
    if (!k.startsWith(kHead) || !k.endsWith(kTail)) return QString();
    const QString p = k.mid(kHead.size(), k.size() - kHead.size() - kTail.size());
    return p.contains(QLatin1Char('/')) ? QString() : p;
}

// One playlist's entries, re-identified. `changed` is set when anything moved; the array comes back whole
// unless two entries have become the SAME row (see below).
QJsonArray sweepEntries(const QJsonArray& items, const QVector<QPair<QString, QString>>& roots, bool& changed)
{
    QJsonArray out;
    QStringList seen;   // identities already emitted for THIS playlist, in order
    for (const QJsonValue& v : items)
    {
        if (!v.isObject()) { out.append(v); continue; }   // not ours to understand; carried through untouched
        QJsonObject e = v.toObject();
        const QString itemId = e.value(QStringLiteral("itemId")).toString();
        const QString path   = e.value(QStringLiteral("path")).toString();
        // Each field on its own, because they are not always the same string: an entry added from a browse
        // row carries an addon's item id and no path at all, and a GOG/Battle.net row carries an exe. Both
        // are left byte-identical by the rule — neither is a network url — so this needs no type test, which
        // is the point: a rule that has to know what kind of row it is looking at is a rule that will meet a
        // kind nobody thought of.
        const QString nId   = itemId.isEmpty() ? itemId : StoredIdentity::resolve(itemId, QString(), roots);
        const QString nPath = path.isEmpty()   ? path   : StoredIdentity::resolve(path,   QString(), roots);
        if (nId != itemId || nPath != path)
        {
            changed = true;
            if (!nId.isEmpty()) e.insert(QStringLiteral("itemId"), nId);
            if (!nPath.isEmpty()) e.insert(QStringLiteral("path"), nPath);
        }
        // TWO ENTRIES THAT HAVE BECOME ONE ROW. PlaylistEntry::itemId is the identity inside a playlist —
        // contains(), addItem()'s de-dup and removeItem() all key on it — and addItem would never have let
        // two entries share one. So a post-sweep collision is not a row being dropped, it is the store's own
        // invariant being restored: the same track, saved twice under two spellings of its url, is one track.
        // The FIRST is kept, because a playlist's order is the user's own and the earlier position is the one
        // they chose. (#200 kept the newer of a colliding pair; a recents row has a ts to compare and a
        // playlist entry has nothing but its place in the list.)
        const QString id = nId.isEmpty() ? itemId : nId;
        if (!id.isEmpty() && seen.contains(id)) { changed = true; continue; }
        if (!id.isEmpty()) seen.push_back(id);
        out.append(e);
    }
    return out;
}

} // namespace

QVector<QPair<QString, QString>> StoredIdentity::serverRoots()
{
    QVector<QPair<QString, QString>> out;
    for (const SubsonicServer& s : SubsonicServerStore::list())
    {
        const QString root = Subsonic::normalizeRoot(s.url, s.allowPlainHttp);
        if (!s.id.isEmpty() && !root.isEmpty()) out.push_back({ s.id, root });
    }
    return out;
}

QString StoredIdentity::resolve(const QString& playPath, const QString& indexHint,
                                const QVector<QPair<QString, QString>>& roots)
{
    if (playPath.isEmpty()) return playPath;
    if (!indexHint.isEmpty() && indexHint != playPath) return StoredUrl::identity(indexHint);
    const QString qualified = Subsonic::trackIdFromStreamUrl(playPath, roots);
    if (!qualified.isEmpty()) return qualified;
    return StoredUrl::identity(playPath);
}

QString StoredIdentity::forRow(const QString& playPath, const QString& indexHint)
{
    return resolve(playPath, indexHint, serverRoots());
}

bool StoredIdentity::sweepPlaylists()
{
    const QStringList keys = store().allKeys();
    if (keys.isEmpty()) return false;

    // The server list belongs to the ACTIVE profile (SubsonicServerStore is per-profile, and so are
    // playlists). Another profile's rows are still swept — they still lose their credential — but they are
    // not re-qualified, because matching one profile's url against another profile's server would mint an id
    // that resolves to nothing on the profile holding the row. They are named on a later run, as that
    // profile, which the narrow rule keeps possible: the server root and the track id are still in the
    // string. That is what "repeatable and monotone" buys.
    const QString active = ProfileStore::currentId().isEmpty() ? QStringLiteral("default")
                                                              : ProfileStore::currentId();
    const QVector<QPair<QString, QString>> roots = serverRoots();

    bool wrote = false;
    for (const QString& k : keys)
    {
        const QString profile = profileOfPlaylistKey(k);
        if (profile.isEmpty()) continue;
        const QString raw = store().value(k).toString();
        if (raw.isEmpty()) continue;
        const QJsonArray all = QJsonDocument::fromJson(raw.toUtf8()).array();
        if (all.isEmpty()) continue;

        const QVector<QPair<QString, QString>> mine = profile == active
                                                          ? roots : QVector<QPair<QString, QString>>();
        bool changed = false;
        QJsonArray out;
        for (const QJsonValue& v : all)
        {
            if (!v.isObject()) { out.append(v); continue; }
            QJsonObject p = v.toObject();
            const QJsonArray items = p.value(QStringLiteral("items")).toArray();
            if (items.isEmpty()) { out.append(p); continue; }
            const QJsonArray swept = sweepEntries(items, mine, changed);
            p.insert(QStringLiteral("items"), swept);
            // `updatedAt` is NOT touched — see the header. A playlist the user has not edited must not start
            // outranking a peer's newer copy just because this pass ran.
            out.append(p);
        }
        if (!changed) continue;
        store().setValue(k, QString::fromUtf8(QJsonDocument(out).toJson(QJsonDocument::Compact)));
        wrote = true;
    }
    if (wrote) store().sync();
    return wrote;
}
