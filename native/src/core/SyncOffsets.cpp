#include "SyncOffsets.h"
#include "AppPaths.h"
#include <QSettings>
#include <QCryptographicHash>

// Shares the portable mymediavault.ini with Settings (same AppPaths::dataDir() posture). Coherence with any
// other QSettings on the same file comes from every writer here calling sync() (flush to disk); QSettings
// reloads on access when the on-disk file has changed. We do NOT rely on a shared in-process cache.
static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/mymediavault.ini"),
                       QSettings::IniFormat);
    return s;
}

namespace {

double clamp(double secs) { return qBound(-10.0, secs, 10.0); }

// Axis leaf name, shared by the global and per-file key builders.
QString axis(SyncOffsets::Which w)
{
    return w == SyncOffsets::Which::Audio ? QStringLiteral("audio") : QStringLiteral("sub");
}

// Global default key: "sync/global/<axis>".
QString globalKey(SyncOffsets::Which w)
{
    return QStringLiteral("sync/global/") + axis(w);
}

// Per-file key: "sync/files/<token>/<axis>", where <token> is an MD5 hex of the opaque caller key. Real
// resume keys are paths/URLs — some with '/', '//', or trailing separators that QSettings would normalize
// into colliding group paths — so we hash to a flat token first (same MD5 pattern as
// PlaybackSession::mediaResumeKey and Settings' PIN). Callers guard the empty key BEFORE this, so the
// junk-key contract is unaffected. Only built for a non-empty key.
QString fileKey(const QString& key, SyncOffsets::Which w)
{
    const QString token = QString::fromLatin1(
        QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Md5).toHex());
    return QStringLiteral("sync/files/") + token + QLatin1Char('/') + axis(w);
}

} // namespace

double SyncOffsets::globalDefault(Which w)
{
    // toDouble() yields 0.0 for an absent or non-numeric (corrupt) value; clamp on read too so a value
    // written outside the API (older build, hand-edited ini, corruption) is still bounded (house style:
    // Settings::virtualPadOpacity clamps on read).
    return clamp(store().value(globalKey(w), 0.0).toDouble());
}

void SyncOffsets::setGlobalDefault(Which w, double secs)
{
    store().setValue(globalKey(w), clamp(secs));
    store().sync();
}

SyncOffsets::Pair SyncOffsets::resolve(const QString& key)
{
    Pair p;
    p.audio = globalDefault(Which::Audio);
    p.sub   = globalDefault(Which::Sub);
    if (key.isEmpty()) return p; // empty key => the globals

    const QString ak = fileKey(key, Which::Audio);
    const QString sk = fileKey(key, Which::Sub);
    if (store().contains(ak)) p.audio = clamp(store().value(ak).toDouble());
    if (store().contains(sk)) p.sub   = clamp(store().value(sk).toDouble());
    return p;
}

void SyncOffsets::savePerFile(const QString& key, Which w, double secs)
{
    if (key.isEmpty()) return; // never write a `sync/files//` junk entry
    store().setValue(fileKey(key, w), clamp(secs));
    store().sync();
}

void SyncOffsets::clearPerFile(const QString& key, Which w)
{
    if (key.isEmpty()) return;
    store().remove(fileKey(key, w));
    store().sync();
}
