// Headless check of the slang-shader PRESET model (src/core/ShaderPreset.h) and the per-game / per-system
// override store (src/core/ShaderPresetStore) — issue #99, SLICE 1. QtCore-only: the model is PURE (no disk, no
// librashader, nothing renders) and the store is a QSettings wrapper over the isolated everythingbox.ini, so it
// runs under the offscreen QPA in CI. Prints SHADERPRESET-OK on success; any failure prints
// SHADERPRESET-FAIL <cond> (line) and exits non-zero.
//
// WHAT IT PINS:
//   * the curated registry carries the expected ids in order, "off" has an empty slangp, and exactly the
//     Mega-Bezel entry is heavy;
//   * presetIdForLegacyFilter maps each of the four legacy VideoFilter ids (scanlines/crt/lcd/off) — plus the
//     empty and an unknown value — onto the right preset id, so no upgrading user's setting breaks;
//   * resolvePreset layers per-game over per-system over the global default, with "" meaning unset (falls
//     through) and "off" being a real winning choice (distinct from unset);
//   * kindForId / the custom-path wrap+unwrap round-trip;
//   * the store round-trips a preset id by key, an absent key reads empty, a clear is a plain delete (device-
//     local, no husk), and the per-system default is reachable via systemKey / systemDefault / setSystemDefault;
//   * (slice 5) scanUserPresets turns a dir + listing into custom entries for *.slangp only (case-insensitive),
//     ids carrying customPrefix()+path and display = base name, order preserved; pickerEntries lays them out as
//     off + builtins + user; displayNameForId labels every kind (off/builtin/custom/unknown).
//
// The CloudSync device-local classification for the shaderpreset prefix is asserted in probe_cloudmerge (that is
// where CloudSync is linked), both directions, next to the emugfx asserts.
//
// FIXTURES ARE HAND-AUTHORED, INDEPENDENT OF THE CODE UNDER TEST: every expected id / flag is written here by
// hand, and the raw store leaf is addressed by an MD5 taken with QCryptographicHash directly, NOT via
// ShaderPresetStore::hashKey. An assertion therefore cannot pass merely because it re-ran the function it checks.
#include "ShaderPreset.h"
#include "ShaderPresetStore.h"
#include "AppPaths.h"
#include "AppBrand.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QSettings>
#include <cstdio>

using namespace ShaderPreset;

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "SHADERPRESET-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

// Independent oracle for the store's key hashing (md5-hex over UTF-8), so the probe addresses a game's raw ini
// leaf without calling ShaderPresetStore::hashKey.
static QString md5hex(const QString& key)
{
    return QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Md5).toHex());
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QString iniPath = AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile);

    // ==== 1. CURATED REGISTRY — expected ids in order, off's empty slangp, exactly one heavy entry ============
    {
        const QVector<Entry> reg = registry();
        // The exact id set the issue's "handful" ships, in display order. Hand-listed, not derived from reg.
        const QStringList expectIds = { QStringLiteral("off"), QStringLiteral("scanlines"), QStringLiteral("crt"),
                                        QStringLiteral("lcd-grid"), QStringLiteral("sharp"), QStringLiteral("mega-bezel") };
        CHECK(reg.size() == expectIds.size());
        for (int i = 0; i < expectIds.size() && i < reg.size(); ++i) CHECK(reg[i].id == expectIds[i]);

        // Every expected id is a known builtin; a made-up one is not.
        for (const QString& id : expectIds) CHECK(isBuiltinId(id));
        CHECK(!isBuiltinId(QStringLiteral("no-such-preset")));

        // "off" names no shader file; a real shader preset names one.
        CHECK(entryForId(offId()).slangp.isEmpty());
        CHECK(!entryForId(QStringLiteral("crt")).slangp.isEmpty());

        // Exactly the Mega-Bezel entry is heavy; the light presets are not. (Tripwire on the heavy flag.)
        CHECK(isHeavyId(QStringLiteral("mega-bezel")) == true);
        CHECK(isHeavyId(QStringLiteral("crt")) == false);
        CHECK(isHeavyId(QStringLiteral("off")) == false);
        CHECK(isHeavyId(QStringLiteral("sharp")) == false);
        CHECK(isHeavyId(QStringLiteral("no-such-preset")) == false);   // non-registry id is not heavy
        int heavyCount = 0; for (const Entry& e : reg) if (e.heavy) ++heavyCount;
        CHECK(heavyCount == 1);
    }

    // ==== 2. LEGACY MIGRATION — each of the four VideoFilter ids maps onto the right preset (+ empty/unknown) ==
    {
        CHECK(presetIdForLegacyFilter(QStringLiteral("scanlines")) == QStringLiteral("scanlines"));
        CHECK(presetIdForLegacyFilter(QStringLiteral("crt"))       == QStringLiteral("crt"));
        CHECK(presetIdForLegacyFilter(QStringLiteral("lcd"))       == QStringLiteral("lcd-grid"));
        CHECK(presetIdForLegacyFilter(QStringLiteral("off"))       == QStringLiteral("off"));
        CHECK(presetIdForLegacyFilter(QString())                   == QStringLiteral("off"));  // "" -> no shader
        CHECK(presetIdForLegacyFilter(QStringLiteral("bogus"))     == QStringLiteral("off"));  // unknown -> no shader
        // Each legacy mapping lands on a real curated preset (so nobody's setting points at nothing).
        for (const char* leg : { "scanlines", "crt", "lcd", "off" })
            CHECK(isBuiltinId(presetIdForLegacyFilter(QString::fromLatin1(leg))));
    }

    // ==== 3. SCOPE RESOLUTION — per-game > per-system > global; "" = unset (falls through), "off" wins =========
    {
        const QString G = QStringLiteral("crt"), S = QStringLiteral("lcd-grid"), D = QStringLiteral("sharp");
        CHECK(resolvePreset(G, S, D) == G);                                   // per-game wins
        CHECK(resolvePreset(QString(), S, D) == S);                          // no game -> per-system
        CHECK(resolvePreset(QString(), QString(), D) == D);                  // neither -> global default
        CHECK(resolvePreset(QString(), QString(), QString()).isEmpty());     // all unset -> unset
        // "off" is NOT unset: a per-game "off" beats a per-system preset (the point of an explicit off).
        CHECK(resolvePreset(offId(), QStringLiteral("crt"), QStringLiteral("sharp")) == offId());
        // A per-system "off" beats the global default too.
        CHECK(resolvePreset(QString(), offId(), QStringLiteral("crt")) == offId());
    }

    // ==== 4. KIND + CUSTOM PATH — the three kinds, and the disk-path wrap/unwrap round-trip ===================
    {
        CHECK(kindForId(offId())   == Kind::Off);
        CHECK(kindForId(QString()) == Kind::Off);                            // unset reads as nothing-to-load
        CHECK(kindForId(QStringLiteral("crt")) == Kind::Builtin);
        const QString abs = QStringLiteral("C:/shaders/my/cool-crt.slangp");
        const QString cid = customPresetId(abs);
        CHECK(kindForId(cid) == Kind::Custom);
        CHECK(isCustomId(cid));
        CHECK(!isCustomId(QStringLiteral("crt")));
        CHECK(customPath(cid) == abs);                                       // unwrap recovers the exact path
        CHECK(customPath(QStringLiteral("crt")).isEmpty());                  // a builtin id carries no path
    }

    // ==== 5. STORE — round-trip by key; absent reads empty; clear is a plain delete (no husk) =================
    {
        const QString key = QStringLiteral("romlib:C:/roms/snes/Chrono Trigger.sfc");
        ShaderPresetStore::set(key, QStringLiteral("crt"));

        CHECK(ShaderPresetStore::get(key) == QStringLiteral("crt"));
        CHECK(ShaderPresetStore::has(key));
        CHECK(ShaderPresetStore::get(QStringLiteral("romlib:C:/roms/snes/Untouched.sfc")).isEmpty());
        CHECK(!ShaderPresetStore::has(QStringLiteral("romlib:C:/roms/snes/Untouched.sfc")));

        // Raw leaf under shaderpreset/items/<md5(key)> — addressed by the independent md5 oracle, carrying the
        // preset id verbatim (a plain string, not JSON).
        QSettings st(iniPath, QSettings::IniFormat);
        const QString leaf = QStringLiteral("shaderpreset/items/") + md5hex(key);
        CHECK(st.contains(leaf));
        CHECK(st.value(leaf).toString() == QStringLiteral("crt"));

        // A custom path stores verbatim behind the sentinel and reads back whole.
        const QString cid = customPresetId(QStringLiteral("C:/shaders/mine.slangp"));
        ShaderPresetStore::set(key, cid);
        CHECK(ShaderPresetStore::get(key) == cid);

        // Clear: device-local + unsynced -> a PLAIN row delete, no husk.
        ShaderPresetStore::reset(key);
        CHECK(ShaderPresetStore::get(key).isEmpty());
        CHECK(!ShaderPresetStore::has(key));
        QSettings st2(iniPath, QSettings::IniFormat);
        CHECK(!st2.contains(leaf));                                          // the row is gone, not a husk

        // Setting an empty id is a no-op removal, never an empty row.
        ShaderPresetStore::set(key, QString());
        CHECK(!ShaderPresetStore::has(key));
    }

    // ==== 6. PER-SYSTEM DEFAULT reachable via systemKey/systemDefault, independent of a game override =========
    {
        ShaderPresetStore::set(ShaderPresetStore::systemKey(QStringLiteral("snes")), QStringLiteral("lcd-grid"));
        CHECK(ShaderPresetStore::systemDefault(QStringLiteral("snes")) == QStringLiteral("lcd-grid"));
        // A game with no override resolves to the system default (which itself falls back to a global default).
        CHECK(resolvePreset(ShaderPresetStore::get(QStringLiteral("romlib:C:/roms/snes/NoOverride.sfc")),
                            ShaderPresetStore::systemDefault(QStringLiteral("snes")),
                            QStringLiteral("off")) == QStringLiteral("lcd-grid"));
        // A different system has no default -> resolution falls through to the global default.
        CHECK(ShaderPresetStore::systemDefault(QStringLiteral("gba")).isEmpty());
        CHECK(resolvePreset(QString(), ShaderPresetStore::systemDefault(QStringLiteral("gba")),
                            QStringLiteral("sharp")) == QStringLiteral("sharp"));
        // The raw per-system leaf lives under the reserved control-byte spelling, addressed by the md5 oracle.
        QSettings st(iniPath, QSettings::IniFormat);
        const QString sysLeaf = QStringLiteral("shaderpreset/items/")
            + md5hex(QStringLiteral("\x01shaderpreset-system:snes"));
        CHECK(st.value(sysLeaf).toString() == QStringLiteral("lcd-grid"));

        ShaderPresetStore::reset(ShaderPresetStore::systemKey(QStringLiteral("snes")));  // clean up
    }

    // ==== 6b. STORE setSystemDefault — writes/reads the reserved key; empty id is a plain delete (slice 5) ======
    {
        ShaderPresetStore::setSystemDefault(QStringLiteral("gba"), QStringLiteral("sharp"));
        CHECK(ShaderPresetStore::systemDefault(QStringLiteral("gba")) == QStringLiteral("sharp"));
        // Raw leaf under the reserved control-byte spelling, addressed by the independent md5 oracle.
        QSettings st(iniPath, QSettings::IniFormat);
        const QString sysLeaf = QStringLiteral("shaderpreset/items/")
            + md5hex(QStringLiteral("\x01shaderpreset-system:gba"));
        CHECK(st.value(sysLeaf).toString() == QStringLiteral("sharp"));
        // Empty id clears it — a plain delete, no husk (device-local).
        ShaderPresetStore::setSystemDefault(QStringLiteral("gba"), QString());
        CHECK(ShaderPresetStore::systemDefault(QStringLiteral("gba")).isEmpty());
        QSettings st2(iniPath, QSettings::IniFormat);
        CHECK(!st2.contains(sysLeaf));
        // An empty system id is a guarded no-op (never writes a bogus row).
        ShaderPresetStore::setSystemDefault(QString(), QStringLiteral("crt"));
        CHECK(ShaderPresetStore::systemDefault(QString()).isEmpty());
    }

    // ==== 7. USER .slangp DISCOVERY — scanUserPresets: *.slangp only (case-insensitive), id/display/order ========
    // Fixtures are a HAND-AUTHORED dir + listing; every expected id/name is written out by hand (NOT via
    // customPresetId / QFileInfo), so an assertion cannot pass merely by re-running the function it checks.
    {
        const QString dir = QStringLiteral("Z:/eb/shaders/user");
        const QVector<Entry> u = scanUserPresets(dir, QStringList{
            QStringLiteral("A.slangp"), QStringLiteral("B.slangp"),
            QStringLiteral("note.txt"), QStringLiteral("C.SLANGP") });
        CHECK(u.size() == 3);                                             // .txt dropped; the two .slangp + one .SLANGP kept
        CHECK(u[0].id == QStringLiteral("custom:Z:/eb/shaders/user/A.slangp"));   // customPrefix + dir/name, by hand
        CHECK(u[0].displayName == QStringLiteral("A"));                  // base name, extension stripped
        CHECK(u[0].slangp == QStringLiteral("Z:/eb/shaders/user/A.slangp"));
        CHECK(u[0].heavy == false);
        CHECK(kindForId(u[0].id) == Kind::Custom);
        CHECK(u[1].id == QStringLiteral("custom:Z:/eb/shaders/user/B.slangp"));
        CHECK(u[1].displayName == QStringLiteral("B"));
        CHECK(u[2].id == QStringLiteral("custom:Z:/eb/shaders/user/C.SLANGP")); // path case preserved, ext matched anyway
        CHECK(u[2].displayName == QStringLiteral("C"));
        // Empty listing -> empty; a listing with no .slangp (incl. a bare ".slang") -> empty.
        CHECK(scanUserPresets(dir, QStringList{}).isEmpty());
        CHECK(scanUserPresets(dir, QStringList{ QStringLiteral("readme.txt"), QStringLiteral("shader.slang") }).isEmpty());
        // Order follows the listing, not sorted.
        const QVector<Entry> swp = scanUserPresets(dir, QStringList{ QStringLiteral("B.slangp"), QStringLiteral("A.slangp") });
        CHECK(swp.size() == 2);
        CHECK(swp[0].displayName == QStringLiteral("B"));
        CHECK(swp[1].displayName == QStringLiteral("A"));
    }

    // ==== 8. PICKER ORDER + LABELS — pickerEntries (off, builtins, then user) and displayNameForId ==============
    {
        const QVector<Entry> reg = registry();
        const QVector<Entry> user = scanUserPresets(QStringLiteral("Z:/u"),
            QStringList{ QStringLiteral("My CRT.slangp"), QStringLiteral("Retro.slangp") });
        const QVector<Entry> rows = pickerEntries(user);
        CHECK(rows.size() == reg.size() + 2);
        CHECK(rows.first().id == QStringLiteral("off"));                 // Off is always the first row
        for (int i = 0; i < reg.size() && i < rows.size(); ++i) CHECK(rows[i].id == reg[i].id); // builtins keep order
        CHECK(rows[reg.size()].id == QStringLiteral("custom:Z:/u/My CRT.slangp"));  // user presets appended, in order
        CHECK(rows[reg.size()].displayName == QStringLiteral("My CRT"));
        CHECK(rows[reg.size() + 1].id == QStringLiteral("custom:Z:/u/Retro.slangp"));
        CHECK(pickerEntries(QVector<Entry>{}).size() == reg.size());    // no user presets -> exactly the registry

        // displayNameForId across the three kinds + an unknown id (hand-authored expectations).
        CHECK(displayNameForId(QString())                    == QStringLiteral("Off"));   // unset
        CHECK(displayNameForId(QStringLiteral("off"))        == QStringLiteral("Off"));
        CHECK(displayNameForId(QStringLiteral("crt"))        == QStringLiteral("CRT"));
        CHECK(displayNameForId(QStringLiteral("lcd-grid"))   == QStringLiteral("LCD Grid"));
        CHECK(displayNameForId(QStringLiteral("custom:Z:/x/My Shader.slangp")) == QStringLiteral("My Shader"));
        CHECK(displayNameForId(QStringLiteral("no-such-builtin")) == QStringLiteral("Off")); // unknown -> Off
    }

    if (failures == 0) std::printf("SHADERPRESET-OK\n");
    else               std::fprintf(stderr, "SHADERPRESET: %d check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
