// The management switch, and the one object every controller-config write goes through (issue #104 increment 2).
//
// THE PROBLEM THIS SOLVES. Some people have hand-built controller profiles in Dolphin or PCSX2 that took an
// evening to get right. A frontend that silently overwrites one has taken something away that they cannot get
// back, and no amount of "but the default is better" makes that acceptable. So the feature carries a PER-EMULATOR
// choice — manage controllers: EverythingBox / the emulator itself — and when it says the emulator itself,
// NOTHING here writes input config for it: not a seat, not a hotkey, not a correction, not a migration.
//
// WHY IT IS AN OBJECT AND NOT AN `if`. A check at the one call site is true until somebody adds a second call
// site. Here the switch is read ONCE, in the constructor, and every writer is a METHOD that refuses when it is
// off. There is no free function to reach for: to add a write you add a method to this class, and a method that
// does not consult `manages_` is visible in this file rather than three call sites away. probe_seats drives EVERY
// method with the switch off and asserts the file on disk is untouched — the assertion is over the writers, not
// over one path through them.
//
// THE SNAPSHOT RULE. Increment 1 wrote only-when-absent, which cannot destroy anything. Hotkeys are key-level
// upserts and a migration rewrites a section, so this increment CAN change a file that already had content.
// Before the FIRST time this feature modifies a given file, the file is copied to "<file>.eb-orig" beside it —
// the same name and the same discipline #103's graphics writer uses, so one emulator config has one pre-EB
// copy no matter which feature touched it first. It is written once and NEVER overwritten: the copy is the
// file as it was before EverythingBox first changed it, not before the most recent change. A user who dislikes
// what we did renames it back without needing us. Nothing is snapshotted for a file that did not exist — the
// way to undo creating a file is to delete it — and nothing is snapshotted for a file THIS object created a
// moment ago, or seat 0's second config block would leave an ".eb-orig" holding seat 0's first one, which is
// not a user's config and reads like one.
//
// PURE ENOUGH TO TEST: QtCore + the filesystem, no SDL, no emulator, no EmulatorManager. probe_seats drives it
// against a temp directory.
#pragma once
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <utility>

namespace ControllerSeats
{
    // Who owns a standalone emulator's input config.
    enum class Manage
    {
        EverythingBox,  // the default: EverythingBox seats pads and binds hotkeys in the emulator's own config
        EmulatorItself, // hands off entirely — the user's own profiles are never touched by this feature
    };

    inline Manage manageFromBool(bool ebManages)
    {
        return ebManages ? Manage::EverythingBox : Manage::EmulatorItself;
    }

    class Writer
    {
    public:
        Writer(QString emulatorId, Manage mode)
            : id_(std::move(emulatorId)), mode_(mode) {}
        Writer(QString emulatorId, bool ebManages)
            : id_(std::move(emulatorId)), mode_(manageFromBool(ebManages)) {}

        const QString& emulatorId() const { return id_; }
        bool manages() const { return mode_ == Manage::EverythingBox; }
        int  writes() const { return writes_; }              // files actually modified/created this launch
        const QStringList& snapshots() const { return snapshots_; } // the ".eb-orig" copies taken this launch

        // Where a file's pre-EverythingBox copy lives. Beside the file, so it travels with the emulator install
        // and is findable without knowing anything about EverythingBox.
        static QString snapshotPath(const QString& path) { return path + QStringLiteral(".eb-orig"); }

        // Write `body` as the whole file, ONLY if nothing is there yet (the seedFileIfAbsent idiom increment 1
        // used for Cemu's per-controller XML). No snapshot: there was no file to preserve.
        bool seedIfAbsent(const QString& path, const QByteArray& body)
        {
            if (!manages()) return false;
            if (QFileInfo::exists(path)) return false;
            QDir().mkpath(QFileInfo(path).absolutePath());
            QFile f(path);
            if (!f.open(QIODevice::WriteOnly)) return false;
            f.write(body);
            f.close();
            created_ << path;   // ours from nothing: a later write to it must not snapshot it as "the original"
            ++writes_;
            return true;
        }

        // Append `body` to the ini at `path` if `marker` is not already somewhere in the file — increment 1's
        // appendIniSectionIfAbsent, byte-for-byte (including the "add a newline first if the file does not end
        // in one" rule), so a seat-0 write through this object is identical to the one that shipped.
        bool appendSectionIfAbsent(const QString& path, const QByteArray& marker, const QByteArray& body)
        {
            if (!manages()) return false;
            QByteArray existing;
            { QFile r(path); if (r.open(QIODevice::ReadOnly)) { existing = r.readAll(); r.close(); } }
            if (existing.contains(marker)) return false; // already has this block (user's own or a prior seed)
            const bool wasThere = QFileInfo::exists(path);
            snapshotOnce(path);
            QDir().mkpath(QFileInfo(path).absolutePath());
            QFile f(path);
            if (!f.open(QIODevice::WriteOnly | QIODevice::Append)) return false;
            if (!existing.isEmpty() && !existing.endsWith('\n')) f.write("\n");
            f.write(body);
            f.close();
            if (!wasThere) created_ << path;   // this append CREATED the file (seat 0's first block)
            ++writes_;
            return true;
        }

        // Replace the whole file with `body`. The one destructive shape here, used only where the caller has
        // already decided the new bytes preserve what the user wrote (the Dolphin GCPad migration, which only
        // rewrites a section byte-identical to EverythingBox's own prior seed, and the ares settings.bml merge,
        // which carries every other ares setting across). Always snapshots first.
        bool replaceContents(const QString& path, const QByteArray& body)
        {
            if (!manages()) return false;
            snapshotOnce(path);
            QDir().mkpath(QFileInfo(path).absolutePath());
            QFile f(path);
            if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
            f.write(body);
            f.close();
            ++writes_;
            return true;
        }

        // Add "key = value" inside [section] of an INI, ONLY IF that key is not already in that section — the
        // hotkey write. Add-if-absent, never upsert: an existing binding (the emulator's own default, or one the
        // user chose) is left exactly as it is. A missing section is created at the end of the file.
        bool addIniKeyIfAbsent(const QString& path, const QString& section, const QString& key,
                               const QString& value)
        {
            if (!manages()) return false;

            QStringList lines;
            bool existed = false;
            {
                QFile r(path);
                if (r.open(QIODevice::ReadOnly | QIODevice::Text))
                {
                    existed = true;
                    lines = QString::fromUtf8(r.readAll()).split(QLatin1Char('\n'));
                    r.close();
                }
            }

            const QString header = QStringLiteral("[%1]").arg(section);
            const QString newLine = QStringLiteral("%1 = %2").arg(key, value);
            int secStart = -1;
            for (int i = 0; i < lines.size(); ++i)
                if (lines[i].trimmed() == header) { secStart = i; break; }

            if (secStart < 0) // no such section — append it at the end
            {
                if (!lines.isEmpty() && !lines.last().trimmed().isEmpty()) lines << QString();
                lines << header << newLine;
            }
            else
            {
                int secEnd = lines.size();
                for (int i = secStart + 1; i < lines.size(); ++i)
                {
                    const QString t = lines[i].trimmed();
                    if (t.startsWith(QLatin1Char('['))) { secEnd = i; break; }   // next section starts
                    if (t.section(QLatin1Char('='), 0, 0).trimmed().compare(key, Qt::CaseInsensitive) == 0)
                        return false;                                            // already bound — leave it
                }
                lines.insert(secEnd, newLine);
            }

            if (existed) snapshotOnce(path);
            QDir().mkpath(QFileInfo(path).absolutePath());
            QFile f(path);
            if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
            f.write(lines.join(QLatin1Char('\n')).toUtf8());
            f.close();
            ++writes_;
            return true;
        }

    private:
        // Copy `path` to "<path>.eb-orig" the first time this feature is about to change it. Never overwrites an
        // existing copy — across launches as well as within one — so the copy always holds the file as it was
        // before EverythingBox first touched it. A file that does not exist has nothing to preserve.
        bool snapshotOnce(const QString& path)
        {
            if (!QFileInfo::exists(path)) return false;
            if (created_.contains(path)) return false;   // WE made this file this launch — nothing of theirs in it
            const QString orig = snapshotPath(path);
            if (QFileInfo::exists(orig)) return false;   // already have the pre-EB copy — never clobber it
            if (!QFile::copy(path, orig)) return false;  // best-effort: a failed copy must not stop the launch
            snapshots_ << orig;
            return true;
        }

        QString     id_;
        Manage      mode_ = Manage::EverythingBox;
        int         writes_ = 0;
        QStringList snapshots_;
        QStringList created_;   // files THIS Writer brought into existence — never snapshotted as "the original"
    };
}
