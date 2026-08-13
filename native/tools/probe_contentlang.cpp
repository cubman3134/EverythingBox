// Headless probe: canonical language mapping + the content/language setting migration.
#include "LanguageCodes.h"
#include "Settings.h"
#include "AppPaths.h"
#include "AppBrand.h"
#include <QCoreApplication>
#include <QSettings>
#include <QString>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { ++failures; \
    std::fprintf(stderr, "CONTENTLANG-FAIL %s (line %d)\n", #cond, __LINE__); } } while (0)

// MUST run before any Settings:: call, so the store() singleton reads the file with the legacy
// key already present and performs the one-shot migration.
static void testMigrationRunsFirst()
{
    const QString ini = AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile);
    { QSettings s(ini, QSettings::IniFormat); s.setValue(QStringLiteral("subs/language"), QStringLiteral("spa")); s.sync(); }
    CHECK(Settings::preferredLanguage() == QStringLiteral("es"));   // migrated 3->2 on first read
}

static void testRoundTrip()
{
    Settings::setPreferredLanguage(QStringLiteral("fr"));
    CHECK(Settings::preferredLanguage() == QStringLiteral("fr"));
    Settings::setPreferredLanguage(QStringLiteral("spa"));          // canonicalized on write
    CHECK(Settings::preferredLanguage() == QStringLiteral("es"));
    Settings::setPreferredLanguage(QString());                     // empty = no preference
    CHECK(Settings::preferredLanguage().isEmpty());
    // Legacy alias still reflects the unified value.
    Settings::setSubtitleLanguage(QStringLiteral("de"));
    CHECK(Settings::subtitleLanguage() == QStringLiteral("de"));
    CHECK(Settings::preferredLanguage() == QStringLiteral("de"));
}

static void testCanonical()
{
    CHECK(LanguageCodes::toCanonical(QStringLiteral("spa")) == QStringLiteral("es"));
    CHECK(LanguageCodes::toCanonical(QStringLiteral("SPA")) == QStringLiteral("es"));
    CHECK(LanguageCodes::toCanonical(QStringLiteral("fre")) == QStringLiteral("fr"));
    CHECK(LanguageCodes::toCanonical(QStringLiteral("en")) == QStringLiteral("en"));
    CHECK(LanguageCodes::toCanonical(QString()).isEmpty());
    CHECK(LanguageCodes::toCanonical(QStringLiteral("xyz")) == QStringLiteral("xy")); // unknown 3 -> left2
}

static void testMpvList()
{
    CHECK(LanguageCodes::toMpvLangList(QStringLiteral("en")) == QStringLiteral("en,eng"));
    CHECK(LanguageCodes::toMpvLangList(QStringLiteral("es")) == QStringLiteral("es,spa"));
    CHECK(LanguageCodes::toMpvLangList(QString()).isEmpty());
    CHECK(LanguageCodes::toMpvLangList(QStringLiteral("xy")) == QStringLiteral("xy")); // unknown 2 -> as-is
}

static void testReadPreferredNoPreferenceKeepsEmpty()
{
    const QString ini = AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile);
    QSettings s(ini, QSettings::IniFormat);
    s.setValue(QStringLiteral("subs/language"), QStringLiteral("spa")); // a legacy value is present
    s.setValue(QStringLiteral("content/language"), QString());          // user explicitly chose "no preference"
    s.sync();
    CHECK(LanguageCodes::readPreferred(s).isEmpty());                   // must NOT resurface the legacy "es"
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testMigrationRunsFirst();   // first — see comment above
    testRoundTrip();
    testCanonical();
    testMpvList();
    testReadPreferredNoPreferenceKeepsEmpty();
    if (failures == 0) std::printf("CONTENTLANG-OK\n");
    return failures == 0 ? 0 : 1;
}
