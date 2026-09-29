#pragma once
#include "AppPaths.h"

#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <QString>

// THE RELAY BUFFER for categories this device has switched off (issue #27; the #27 review, findings 2 and 5).
//
// Off means this device neither sends its own values of a category nor applies a peer's. It does not mean the
// category disappears from what this device uploads. Both documents it uploads REPLACE the shared copy, so a
// device that simply left a category out would strip every other device's values of it from the cloud (and,
// in the merge document, their tombstones) until each of them pushed again. It would also make its own
// fingerprint describe a different bundle from the stamp the peer pushed, so no pull could ever reach the
// fixed point PendingPush relies on. Instead the device RELAYS the category: it keeps the peer's copy as the
// last pull delivered it, and uploads that copy verbatim.
//
// Three parts:
//   "settings" — the RELAY for the settings bundle (CloudSync): { <settings key>: <string value> }. It starts as
//                this device's own values at the switch and is replaced by the peer's at every pull.
//   "frozen"   — this device's own values at the switch, and nothing else ever writes them. Turning the category
//                back on adopts a relayed value ONLY where it differs from this snapshot — where a peer really
//                changed the key — and otherwise keeps what this device holds now (the second review, finding 1:
//                a one-device account relays its own snapshot back to itself, so "adopt the relay" rewound every
//                edit made while the category was off).
//   "sections" — the relay for the progress document (CloudMerge): { <section>: <its JSON> }. No snapshot is
//                needed there: sections MERGE on the way back in (newest wins per row), so this device's newer
//                rows win against its own older snapshot by the ordinary rule.
//
// ITS OWN FILE, not the ini: a relayed section can be a whole peer's statistics, and in the ini it would be
// rewritten by every store().sync() of every store in the app. Here it is written only when it changes. It is
// device-local by construction: not in the ini, so neither the bundle nor the fingerprint nor a Discard can see
// it, and the bundle zips only settings.json and themes/. Its RELAYED values belong to ONE account: signing out and
// switching backend forget them (CloudSync::forgetRelay), so account X's relayed values are never uploaded into
// account Y. What belongs to this DEVICE stays (#476): the snapshot is kept, the settings relay goes back to it,
// and the sections go back to this device's own, exactly as at the switch.
namespace synccat
{
inline QString carryPath() { return AppPaths::dataDir() + QStringLiteral("/sync-carry.json"); }
inline QString carrySettingsPart() { return QStringLiteral("settings"); }
inline QString carryFrozenPart() { return QStringLiteral("frozen"); }
inline QString carrySectionsPart() { return QStringLiteral("sections"); }

// One line into stream_debug.log, where every other sync diagnostic lands. No values — they may be anything.
inline void carryLog(const QString& msg)
{
    QFile f(AppPaths::dataDir() + QStringLiteral("/stream_debug.log"));
    if (f.open(QIODevice::Append | QIODevice::Text))
        f.write((QDateTime::currentDateTime().toString(Qt::ISODate) + QStringLiteral("  ") + msg
                 + QStringLiteral("\n")).toUtf8());
}

// The whole relay file. Missing reads as empty (the normal case). Unreadable or corrupt ALSO reads as empty —
// harmless: a switched-off category then relays nothing until the next pull refills it — but it is logged.
inline QJsonObject loadCarryAll()
{
    QFile f(carryPath());
    if (!f.exists()) return QJsonObject();
    if (!f.open(QIODevice::ReadOnly))
    {
        carryLog(QStringLiteral("cloud sync: the category relay file could not be read - treated as empty"));
        return QJsonObject();
    }
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
    {
        carryLog(QStringLiteral("cloud sync: the category relay file is corrupt - treated as empty"));
        return QJsonObject();
    }
    return doc.object();
}

inline QJsonObject loadCarry(const QString& part) { return loadCarryAll().value(part).toObject(); }

// Replace the whole file (empty parts dropped; no file at all once everything is empty). Returns false when
// the write failed — a caller about to switch a category off must then NOT switch it.
inline bool saveCarryAll(QJsonObject all)
{
    for (const QString& k : all.keys())
        if (all.value(k).toObject().isEmpty()) all.remove(k);
    if (all == loadCarryAll()) return true;   // unchanged: no write
    if (all.isEmpty()) return !QFile::exists(carryPath()) || QFile::remove(carryPath());
    QSaveFile f(carryPath());
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(QJsonDocument(all).toJson(QJsonDocument::Compact));
    return f.commit();
}

// This device's OWN merge-document sections for every category switched off here, serialised from its stores now:
// the same thing CloudMerge::switchCategory seeds the relay with at a switch-off. CloudSync::forgetRelay re-seeds
// the relay from it when the account changes (#476). CloudMerge.cpp installs it; a build that links CloudSync
// without CloudMerge has no merge document to upload, and leaves it null.
using OwnSectionsFn = QJsonObject (*)();
inline OwnSectionsFn& ownSwitchedOffSections()
{
    static OwnSectionsFn fn = nullptr;
    return fn;
}

// Replace one part. See saveCarryAll for the return value.
inline bool saveCarry(const QString& part, const QJsonObject& value)
{
    QJsonObject all = loadCarryAll();
    all.insert(part, value);
    return saveCarryAll(all);
}
} // namespace synccat
