// User profiles. Each profile owns its home-screen content (its Recent list is namespaced by profile id).
// At startup the app must have a selected profile: with none, one is created; with several, one is chosen.
// Persisted as a JSON list in everythingbox.ini, with the active profile id under "profiles/current".
#pragma once
#include <QString>
#include <QVector>

struct Profile
{
    QString id;    // stable unique id (used to namespace per-user data)
    QString name;  // display name
    QString icon;  // a "cute" avatar (an emoji glyph picked at creation)
    bool restricted = false; // "kids" profile: leaving it (switch profile / open Settings) needs the parental PIN
    // Per-profile 4-digit passcode (issue #30), as ProfilePasscode::hash(id, code); empty = no passcode.
    // It lives IN the profile record, not in a sibling key, because profiles/list SYNCS: set the passcode on
    // one device and every device that pulls the bundle already has it. Absent from an older device's JSON
    // simply reads back empty, which is exactly "no passcode" — so the field is backward-compatible in both
    // directions and needs no migration. It is a soft lock; see ProfilePasscode.h for what that does and
    // does not mean.
    QString passHash;
};

namespace ProfileStore
{
    QVector<Profile> list();
    Profile add(const QString& name, const QString& icon = QString()); // create with a fresh id
    void update(const QString& id, const QString& name, const QString& icon); // rename / change avatar
    void setRestricted(const QString& id, bool restricted); // mark a profile as a kids profile
    // Set (non-empty hash) or remove (empty) the per-profile passcode. Takes the HASH, never the code: this
    // store has no business seeing a plaintext passcode, and keeping the hashing in ProfilePasscode is what
    // stops a second call site from inventing its own salt. Callers gate this on the CURRENT code themselves
    // — the store cannot, since it does not know what the user typed.
    void setPasscodeHash(const QString& id, const QString& passHash);
    bool hasPasscode(const QString& id);                    // convenience: a non-empty stored hash
    void remove(const QString& id);     // delete; if it was current, current moves to the first remaining
    QString currentId();                // active profile id ("" if none selected yet)
    void setCurrent(const QString& id);
    Profile current();                  // the active profile (empty Profile if none)
    void migrateIcons();                // one-time: repair legacy Windows-1252 mojibake in stored icons
}
