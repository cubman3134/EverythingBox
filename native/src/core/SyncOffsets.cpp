#include "SyncOffsets.h"
#include "AppPaths.h"
#include <QSettings>

// Shares the portable mymediavault.ini with Settings (same AppPaths::dataDir() posture). A separate QSettings
// instance on the same path shares Qt's per-file cache with Settings' own store(), so the two stay coherent.
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

// Per-file key: "sync/files/<key>/<axis>". Only built for a non-empty key.
QString fileKey(const QString& key, SyncOffsets::Which w)
{
    return QStringLiteral("sync/files/") + key + QLatin1Char('/') + axis(w);
}

} // namespace

double SyncOffsets::globalDefault(Which w)
{
    // toDouble() yields 0.0 for an absent or non-numeric (corrupt) value.
    return store().value(globalKey(w), 0.0).toDouble();
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
    if (store().contains(ak)) p.audio = store().value(ak).toDouble();
    if (store().contains(sk)) p.sub   = store().value(sk).toDouble();
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
