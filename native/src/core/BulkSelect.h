// Bulk-edit's testable heart (issue #65). Two pure, QtCore-only pieces that carry the risk in "select fifty
// games and do one thing to all of them", extracted OUT of the UI so a headless probe can pin them and
// mutation testing can prove each assertion discriminates:
//
//   * Selection — a set of browse-item indices with the multi-select verbs (toggle / selectAll / clear /
//     invert / isSelected / count). Trivial individually, but the probe pins the invariants a grid overlay
//     leans on: toggle is its own inverse, selectAll-then-clear is empty, invert twice is identity. `invert`
//     and `selectAll` take the universe (the level's full index list) because "the complement of the
//     selection" is meaningless without knowing the whole set — and taking it explicitly keeps the model from
//     ever holding an index the level does not have.
//
//   * reassignTargetPath — where a bug LOSES a user's ROM. Reassigning a game's system MOVES its file on
//     disk to <root>/<canonical-folder>/. This resolves that destination WITHOUT touching the disk: the "does
//     a file already live there" question is an injected predicate, so the probe drives every branch (fresh
//     name / collision / already-in-place / odd characters) with no filesystem at all. It is collision-safe by
//     construction — it never returns a path a caller would overwrite, and it never returns the source's own
//     candidate name when that name is already taken at the destination.
//
// QtCore only (QString / QFileInfo / QDir string ops, no disk I/O, no Quick/Widgets), so it builds under the
// offscreen QPA and links nothing. MainWindow does the actual QFile::rename + the store loops; this file owns
// the decisions those loops must not get wrong.
#pragma once
#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QString>
#include <QVector>
#include <functional>

namespace BulkSelect
{
    // The multi-select set. Indices are browse-item indices within ONE stable browse level: multi-select mode
    // applies no store change until the batch fires at the end, so the level's model is not rebuilt underneath
    // it and an index means the same item for the whole session in the mode. (When the batch DOES fire, the
    // caller resolves each surviving index to its stable themedLeafKey before mutating any store — the same
    // index->key discipline every single-item verb already follows.)
    class Selection
    {
    public:
        // Add the index if absent, remove it if present. Its own inverse: toggle(i) twice restores the prior
        // state, which is the whole contract of "confirm toggles the focused item".
        void toggle(int index)
        {
            if (!sel_.remove(index)) sel_.insert(index);
        }

        // Select the entire level. Taking the universe explicitly (rather than a count) means a sparse or
        // non-contiguous index list selects exactly those items and nothing spurious.
        void selectAll(const QVector<int>& universe)
        {
            sel_.clear();
            for (int i : universe) sel_.insert(i);
        }

        void clear() { sel_.clear(); }

        // Complement within the universe: an index in the universe flips membership; anything the current set
        // holds that is NOT in the universe is dropped. So invert always leaves the set a subset of the
        // universe, which is what makes "invert twice is identity" hold for any set the universe contains.
        void invert(const QVector<int>& universe)
        {
            QSet<int> next;
            for (int i : universe)
                if (!sel_.contains(i)) next.insert(i);
            sel_ = next;
        }

        bool isSelected(int index) const { return sel_.contains(index); }
        int  count() const { return sel_.size(); }
        bool isEmpty() const { return sel_.isEmpty(); }

        // The selected indices, ascending — a stable iteration order for the batch loop (a QSet iterates in an
        // unspecified order, and "moved 12 games" should process them predictably).
        QVector<int> selected() const
        {
            QVector<int> out(sel_.cbegin(), sel_.cend());
            std::sort(out.begin(), out.end());
            return out;
        }

    private:
        QSet<int> sel_;
    };

    // A collision-safe "base (2).ext" style name derived from `fileName` for occurrence n>=2, preserving the
    // extension so the moved ROM stays loadable. Parentheses and spaces in the base survive (ROM dumps are
    // full of "(USA)", "(Rev 1)", "[!]"), because QFileInfo splits on the LAST dot only. A dotless name gets
    // "base (n)" with no trailing dot.
    inline QString suffixedName(const QString& fileName, int n)
    {
        const QFileInfo fi(fileName);
        const QString base = fi.completeBaseName();      // "Sonic (USA)" from "Sonic (USA).zip"
        const QString ext  = fi.suffix();                // "zip" (empty for a dotless name)
        const QString stem = base + QStringLiteral(" (") + QString::number(n) + QStringLiteral(")");
        return ext.isEmpty() ? stem : stem + QLatin1Char('.') + ext;
    }

    // Resolve where `currentPath` should move to when its system is reassigned to a folder `targetFolder`
    // (the canonical ES-DE/RetroBat name the caller gets from RomLibrary::folderFor(targetSystemId)) under
    // `libraryRoot` (RomLibrary::root()). `destTaken` answers "is there already a file at this absolute path"
    // — injected so this stays pure and the probe drives every branch. Returns a NORMALISED absolute path.
    //
    // Three outcomes the caller must distinguish, and the return value encodes all three so no out-parameter
    // is needed:
    //   * empty string  -> SKIP. currentPath was empty, or the destination is so saturated with collisions
    //                       that no free name was found within the cap. Never move; report it skipped.
    //   * == currentPath -> NO-OP. The game already lives in the target folder; there is nothing to move.
    //                       (Returned as the input path, cleaned, so `ret == QDir::cleanPath(currentPath)`.)
    //   * anything else  -> the destination to QFile::rename the source to. Guaranteed by construction to be a
    //                       path `destTaken` reported FALSE for, so a rename there overwrites nothing.
    //
    // Collision safety is the load-bearing property: if <dest>/<name> is taken, we never return it — we walk
    // "name (2)", "name (3)", … until `destTaken` is false, and return that. We never return the original
    // taken candidate, which is the assertion that stands between a batch reassign and a silently clobbered
    // ROM.
    inline QString reassignTargetPath(const QString& libraryRoot,
                                      const QString& targetFolder,
                                      const QString& currentPath,
                                      const std::function<bool(const QString&)>& destTaken)
    {
        if (currentPath.isEmpty()) return QString();

        const QFileInfo srcInfo(currentPath);
        const QString   fileName = srcInfo.fileName();
        const QString   destDir  = QDir::cleanPath(libraryRoot + QLatin1Char('/') + targetFolder);
        const QString   srcDir   = QDir::cleanPath(srcInfo.path());

        // Already in the right folder: nothing to move. Signalled by returning the (cleaned) source path, so
        // the caller's `target == source` test is a plain string compare.
        if (srcDir == destDir)
            return QDir::cleanPath(currentPath);

        const QString candidate = destDir + QLatin1Char('/') + fileName;
        if (!destTaken(candidate))
            return candidate;

        // The obvious name is taken by a DIFFERENT file (the no-op case above already returned). Derive a
        // non-colliding one rather than overwrite it. The cap is a guard against a pathological injected
        // predicate that never yields; a real library never approaches it, and hitting it returns SKIP rather
        // than looping forever or overwriting.
        for (int n = 2; n < 10000; ++n)
        {
            const QString alt = destDir + QLatin1Char('/') + suffixedName(fileName, n);
            if (!destTaken(alt))
                return alt;
        }
        return QString(); // saturated -> SKIP; never overwrite
    }
}
