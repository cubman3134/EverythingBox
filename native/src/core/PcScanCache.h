// Persisted PC-launcher scans (issue #62, part 1). Every populate of the PC Games folder re-scans each store
// on disk — Steam's steamapps, Epic's manifests, GOG's registry, Battle.net's install records — and a store
// that is BRIEFLY UNREADABLE (the launcher closed, a drive not yet mounted, a manifest dir momentarily
// locked) then comes back empty, and every game it holds vanishes from the folder until the next good scan.
// This unit keeps the last SUCCESSFUL scan per source so an unreadable scan falls back to it (marked
// unavailable) instead of dropping the library.
//
// Pure and QtCore-only (the persistence is a JSON blob per source in the portable everythingbox.ini, the way
// the per-item stores keep their state), so probe_pcscan links lean and the load-bearing decision — merge()
// — is mutation-tested with no disk in the loop.
#pragma once
#include <QString>
#include <QVector>
#include <QJsonArray>

namespace pcscan
{
    // One game as the scan cache knows it: the launcher-native id (Steam appid / Epic appName / GOG id /
    // Battle.net code-or-name) and its display name. `available` is the DISPLAY state merge() stamps — true
    // for a game the current scan actually read, false for one shown from the cache because the source could
    // not be read this time. The persisted last-good cache is always all-available; the false state is a
    // property of a fallback, not something that is stored.
    struct ScanEntry
    {
        QString id;
        QString name;
        bool    available = true;
    };

    // The outcome of a fresh scan, as the CALLER classifies it — the one bit merge() cannot infer for itself.
    //   Ok         - the source was READ. Its entries are authoritative even when empty: a genuinely empty
    //                library is empty, and does NOT resurrect a stale cache.
    //   Unreadable - the scan could not read the source (launcher closed / drive absent / dir locked). Its
    //                entries are meaningless; the cache is what should still be shown.
    // The distinction is the whole point of the feature, and it is the caller's to make because only the
    // caller holds the per-launcher readability signal (see PcScanCache.cpp / HomeView::scanPcLibrary): an
    // EMPTY result alone cannot tell "the user uninstalled everything" from "the store was not readable".
    enum class ScanStatus { Ok, Unreadable };

    struct ScanResult
    {
        ScanStatus         status = ScanStatus::Ok;
        QVector<ScanEntry> entries;   // meaningful only when status == Ok
    };

    // THE LOAD-BEARING, PURE DECISION — no disk, no clock, deterministic; this is what probe_pcscan pins.
    //   fresh.status == Ok         -> fresh.entries, each forced available=true. The caller persists these as
    //                                 the new last-good cache. An empty Ok yields empty (the cache is dropped,
    //                                 not resurrected).
    //   fresh.status == Unreadable -> the CACHED entries, each forced available=false. `fresh.entries` is
    //                                 ignored. The caller does NOT persist — the good cache stands.
    QVector<ScanEntry> merge(const QVector<ScanEntry>& cached, const ScanResult& fresh);

    // The JSON blob a source's cache is persisted as, and its inverse. A faithful round trip of all three
    // fields so the pair is self-contained and testable; storeCached only ever writes a successful (all-
    // available) scan, so a loaded cache is all-available in practice.
    QJsonArray         toJson(const QVector<ScanEntry>& entries);
    QVector<ScanEntry> fromJson(const QJsonArray& arr);

    // The ini key a source's blob lives under: "pcscan/<source>". Carved out as DEVICE-LOCAL in
    // CloudSync::isDeviceLocalKey (installed-scan state is this machine's and must never sync); probe_cloudmerge
    // asserts the carve-out, and this is the one place the prefix is spelled so the two cannot drift.
    QString iniKey(const QString& source);

    // ini-backed layer. loadCached reads the last-good blob ({} when none); storeCached overwrites it.
    QVector<ScanEntry> loadCached(const QString& source);
    void               storeCached(const QString& source, const QVector<ScanEntry>& entries);

    // The one call the folder uses: load the source's last-good cache, merge() the fresh scan onto it, persist
    // the result ONLY when the scan succeeded, and return what to show. The single entry point so "persist on
    // success, fall back on failure" lives in one place and cannot be spelled two different ways at two call
    // sites (the failure mode this codebase keeps being bitten by).
    QVector<ScanEntry> reconcile(const QString& source, const ScanResult& fresh);
}
