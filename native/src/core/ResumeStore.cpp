#include "ResumeStore.h"
#include "Tombstones.h"

#include <QCryptographicHash>
#include <QSettings>

namespace {

// The 10-hex-char MD5 leaf every resume site already used independently (PlaybackSession::mediaResumeKey,
// HomeView::resumeFraction/clearResume, PcGameRemap's md5Hex10). Now that a clear has to name the same item to
// the tombstone store that the merge document names it by, they read it from here.
QString hashFor(const QString& key)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Md5).toHex().left(10));
}

} // namespace

// Hashes whatever it is given, including an empty key — the three sites this replaced all did, and their
// callers guard emptiness themselves, so folding a guard in here would silently re-point an existing read.
// The WRITE verbs below guard it instead, where an empty key is genuinely meaningless.
QString ResumeStore::groupFor(const QString& key)
{
    return QStringLiteral("resume/") + hashFor(key);
}

QString ResumeStore::tombStore() { return QStringLiteral("resume"); }

QString ResumeStore::tombKey(const QString& key) { return hashFor(key); }

void ResumeStore::clear(QSettings& s, const QString& key)
{
    if (key.isEmpty()) return;
    const QString grp = groupFor(key);
    // Was there anything to clear? Every leaf, not just "pos": a row can carry a ts/title without a position
    // (the merge writes whatever fields the remote document had), and CloudMerge's own haveLocal gate reading
    // "pos" alone is what let a cleared row fall through to the wholesale write-back in the first place.
    const bool had = s.contains(grp + QStringLiteral("/pos"))   || s.contains(grp + QStringLiteral("/dur"))
                  || s.contains(grp + QStringLiteral("/ts"))    || s.contains(grp + QStringLiteral("/title"));
    s.remove(grp);      // removes the whole group
    s.sync();
    if (had) Tombstones::record(tombStore(), tombKey(key));
}

int ResumeStore::lastMarkedIndex(QSettings& s, const QStringList& keys)
{
    for (int i = keys.size() - 1; i >= 0; --i)
    {
        if (keys.at(i).isEmpty()) continue;
        if (s.contains(groupFor(keys.at(i)) + QStringLiteral("/pos"))) return i;
    }
    return -1;
}

void ResumeStore::noteResumed(const QString& key)
{
    if (key.isEmpty()) return;
    Tombstones::remove(tombStore(), tombKey(key)); // no-op when there is no tombstone (the ordinary case)
}
