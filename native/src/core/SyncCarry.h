#pragma once
#include "AppPaths.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
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
// Two parts, both keyed by what they relay:
//   "settings" — { <settings key>: <string value> } for the settings bundle (CloudSync);
//   "sections" — { <merge-document section>: <its JSON> } for the progress document (CloudMerge).
// Each part starts as this device's OWN copy at the moment of the switch (so the upload does not change when
// the switch does), is replaced by the peer's copy at every pull, and is consumed when the category is turned
// back on.
//
// ITS OWN FILE, not the ini: a relayed section can be a whole peer's statistics, and in the ini it would be
// rewritten by every store().sync() of every store in the app. Here it is written only when it changes. It is
// device-local by construction: it is not in the ini, so neither the bundle nor the fingerprint nor a Discard
// can see it, and the bundle zips only settings.json and themes/.
namespace synccat
{
inline QString carryPath() { return AppPaths::dataDir() + QStringLiteral("/sync-carry.json"); }
inline QString carrySettingsPart() { return QStringLiteral("settings"); }
inline QString carrySectionsPart() { return QStringLiteral("sections"); }

inline QJsonObject loadCarryAll()
{
    QFile f(carryPath());
    if (!f.open(QIODevice::ReadOnly)) return QJsonObject();
    return QJsonDocument::fromJson(f.readAll()).object();
}

inline QJsonObject loadCarry(const QString& part) { return loadCarryAll().value(part).toObject(); }

// Replace one part. No write when nothing changed, and no file at all once both parts are empty — a device on
// the defaults never has one.
inline void saveCarry(const QString& part, const QJsonObject& value)
{
    QJsonObject all = loadCarryAll();
    if (all.value(part).toObject() == value && (all.contains(part) || value.isEmpty())) return;
    if (value.isEmpty()) all.remove(part);
    else                 all.insert(part, value);
    if (all.isEmpty()) { QFile::remove(carryPath()); return; }
    QSaveFile f(carryPath());
    if (f.open(QIODevice::WriteOnly)) { f.write(QJsonDocument(all).toJson(QJsonDocument::Compact)); f.commit(); }
}
} // namespace synccat
