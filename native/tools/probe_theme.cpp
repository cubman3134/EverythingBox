// Headless check of the per-profile theme choice (roadmap #57). ThemeChoice owns the theme setting: the
// per-profile key, whether a profile still owes us a pick, what to actually render, and the one-time
// migration that carries both the global->per-profile move and the XMB->Triple folder rename. Every decision
// below is a pure function over its arguments (no ini, no filesystem), so this pins the tables verbatim and
// the UI layers can never drift from them.
//
// Prints THEME-OK on success; any failure prints THEME-FAIL <cond> and exits non-zero.
#include "ThemeChoice.h"

#include <QHash>
#include <QString>
#include <QStringList>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "THEME-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

int main()
{
    const QStringList kShipped = { QStringLiteral("Channels"), QStringLiteral("Triple") };

    // ---- 1. keyFor: the exact key format, including the empty-profile-id case ------------------------
    CHECK(ThemeChoice::keyFor(QStringLiteral("abc123")) == QStringLiteral("themedHome/theme/abc123"));
    CHECK(ThemeChoice::keyFor(QString()) == QStringLiteral("themedHome/theme/default"));

    // ---- 2. needsPick: unset, set-and-installed, set-but-UNINSTALLED --------------------------------
    // A profile with nothing stored owes a pick.
    CHECK(ThemeChoice::needsPick(QString(), kShipped) == true);
    // A profile whose theme is installed does not.
    CHECK(ThemeChoice::needsPick(QStringLiteral("Triple"), kShipped) == false);
    CHECK(ThemeChoice::needsPick(QStringLiteral("Channels"), kShipped) == false);
    // A profile whose stored theme is no longer on disk DOES owe a pick — this is the case a naive
    // "is it empty" check gets wrong, and it is why `installed` is a parameter at all.
    CHECK(ThemeChoice::needsPick(QStringLiteral("Grid"), kShipped) == true);
    // ...but not if that theme is still installed (the migrated-user case: cut from the shipped set,
    // still on their disk, must NOT be re-asked).
    CHECK(ThemeChoice::needsPick(QStringLiteral("Grid"),
                                 { QStringLiteral("Channels"), QStringLiteral("Grid"),
                                   QStringLiteral("Triple") }) == false);

    // ---- 3. resolve: all four ordering steps --------------------------------------------------------
    // (a) stored, when installed.
    CHECK(ThemeChoice::resolve(QStringLiteral("Channels"), kShipped) == QStringLiteral("Channels"));
    // (b) the fallback, when the stored theme is gone.
    CHECK(ThemeChoice::resolve(QStringLiteral("Grid"), kShipped) == QStringLiteral("Triple"));
    CHECK(ThemeChoice::resolve(QString(), kShipped) == QStringLiteral("Triple"));
    // (c) the FIRST installed theme, when the fallback itself isn't installed. A user who deleted
    //     Triple and kept only a community theme must land on that theme, not on a name that is
    //     nowhere on disk.
    CHECK(ThemeChoice::resolve(QStringLiteral("Grid"), { QStringLiteral("Aurora") })
          == QStringLiteral("Aurora"));
    CHECK(ThemeChoice::resolve(QString(), { QStringLiteral("Aurora"), QStringLiteral("Zed") })
          == QStringLiteral("Aurora"));
    // (d) nothing installed at all -> empty. Callers already handle this (MainWindow.cpp:3951).
    CHECK(ThemeChoice::resolve(QStringLiteral("Grid"), {}).isEmpty());
    CHECK(ThemeChoice::resolve(QString(), {}).isEmpty());
    // resolve NEVER returns a folder that isn't installed — the invariant the whole function exists for.
    CHECK(kShipped.contains(ThemeChoice::resolve(QStringLiteral("Nonexistent"), kShipped)));

    // ---- 4. renameLegacyFolder: the XMB -> Triple move ---------------------------------------------
    CHECK(ThemeChoice::renameLegacyFolder(QStringLiteral("XMB")) == QStringLiteral("Triple"));
    CHECK(ThemeChoice::renameLegacyFolder(QStringLiteral("Channels")) == QStringLiteral("Channels"));
    CHECK(ThemeChoice::renameLegacyFolder(QStringLiteral("Grid")) == QStringLiteral("Grid"));
    CHECK(ThemeChoice::renameLegacyFolder(QString()).isEmpty());

    // ---- 5. planMigration: the table --------------------------------------------------------------
    const QStringList twoProfiles = { QStringLiteral("p1"), QStringLiteral("p2") };

    // (a) A legacy global value fans out to every profile that has none.
    {
        const QHash<QString, QString> out =
            ThemeChoice::planMigration(QStringLiteral("Grid"), twoProfiles, {});
        CHECK(out.size() == 2);
        CHECK(out.value(QStringLiteral("p1")) == QStringLiteral("Grid"));
        CHECK(out.value(QStringLiteral("p2")) == QStringLiteral("Grid"));
    }

    // (b) The rename rides along: a legacy global of XMB lands as Triple.
    {
        const QHash<QString, QString> out =
            ThemeChoice::planMigration(QStringLiteral("XMB"), { QStringLiteral("p1") }, {});
        CHECK(out.value(QStringLiteral("p1")) == QStringLiteral("Triple"));
    }

    // (c) An EXISTING per-profile value is never overwritten by the global.
    {
        QHash<QString, QString> existing;
        existing.insert(QStringLiteral("p1"), QStringLiteral("Channels"));
        const QHash<QString, QString> out =
            ThemeChoice::planMigration(QStringLiteral("Grid"), { QStringLiteral("p1") }, existing);
        CHECK(out.isEmpty());
    }

    // (d) ...but an existing value still gets the folder rename applied.
    {
        QHash<QString, QString> existing;
        existing.insert(QStringLiteral("p1"), QStringLiteral("XMB"));
        const QHash<QString, QString> out =
            ThemeChoice::planMigration(QStringLiteral("Grid"), { QStringLiteral("p1") }, existing);
        CHECK(out.size() == 1);
        CHECK(out.value(QStringLiteral("p1")) == QStringLiteral("Triple"));
    }

    // (e) No legacy global and no stored value -> nothing written, so needsPick stays true and the
    //     profile gets the forced pick. This is the genuinely-fresh-install case.
    {
        const QHash<QString, QString> out =
            ThemeChoice::planMigration(QString(), { QStringLiteral("p1") }, {});
        CHECK(out.isEmpty());
        CHECK(ThemeChoice::needsPick(QString(), kShipped) == true);
    }

    // (f) No profiles at all -> nothing to write, no crash.
    CHECK(ThemeChoice::planMigration(QStringLiteral("Grid"), {}, {}).isEmpty());

    // ---- 6. IDEMPOTENCE: applying the plan and re-running produces nothing -------------------------
    // The migration is flag-guarded in practice, but it must ALSO be naturally idempotent — a flag that
    // fails to persist (a crash between write and sync) must not corrupt anything on the second run.
    {
        QHash<QString, QString> existing;
        const QHash<QString, QString> first =
            ThemeChoice::planMigration(QStringLiteral("XMB"), twoProfiles, existing);
        for (auto it = first.constBegin(); it != first.constEnd(); ++it)
            existing.insert(it.key(), it.value());
        const QHash<QString, QString> second =
            ThemeChoice::planMigration(QStringLiteral("XMB"), twoProfiles, existing);
        CHECK(second.isEmpty());
        // ...and a third run over the SAME state is still empty.
        CHECK(ThemeChoice::planMigration(QStringLiteral("XMB"), twoProfiles, existing).isEmpty());
    }

    if (failures == 0) { std::puts("THEME-OK"); return 0; }
    std::fprintf(stderr, "THEME: %d check(s) failed\n", failures);
    return 1;
}
