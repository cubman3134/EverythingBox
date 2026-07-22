// User profiles. Each profile owns its home-screen content (its Recent list is namespaced by profile id).
// At startup the app must have a selected profile: with none, one is created; with several, one is chosen.
// Persisted as a JSON list in mymediavault.ini, with the active profile id under "profiles/current".
#pragma once
#include <QString>
#include <QVector>

struct Profile
{
    QString id;    // stable unique id (used to namespace per-user data)
    QString name;  // display name
    QString icon;  // a "cute" avatar (an emoji glyph picked at creation)
    bool restricted = false; // "kids" profile: leaving it (switch profile / open Settings) needs the parental PIN
};

namespace ProfileStore
{
    QVector<Profile> list();
    Profile add(const QString& name, const QString& icon = QString()); // create with a fresh id
    void update(const QString& id, const QString& name, const QString& icon); // rename / change avatar
    void setRestricted(const QString& id, bool restricted); // mark a profile as a kids profile
    void remove(const QString& id);     // delete; if it was current, current moves to the first remaining
    QString currentId();                // active profile id ("" if none selected yet)
    void setCurrent(const QString& id);
    Profile current();                  // the active profile (empty Profile if none)
    void migrateIcons();                // one-time: repair legacy Windows-1252 mojibake in stored icons
}
