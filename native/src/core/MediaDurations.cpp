#include "MediaDurations.h"
#include "AppBrand.h"
#include "AppPaths.h"
#include "ResumeStore.h"

#include <QSettings>

static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

QString MediaDurations::keyFor(const QString& itemKey)
{
    if (itemKey.isEmpty()) return QString();
    // ResumeStore's hash, reused verbatim — tombKey() IS that hash (ResumeStore.cpp), so the two stores are
    // indexed identically and neither can drift onto its own hashing rule.
    return QStringLiteral("mediadur/") + ResumeStore::tombKey(itemKey);
}

void MediaDurations::noteIn(QSettings& s, const QString& itemKey, int seconds)
{
    if (itemKey.isEmpty() || seconds <= 0) return;   // 0 is "unknown", never a stored length (see the header)
    s.setValue(keyFor(itemKey), seconds);
    s.sync();
}

int MediaDurations::secondsIn(QSettings& s, const QString& itemKey)
{
    if (itemKey.isEmpty()) return 0;
    const int v = s.value(keyFor(itemKey), 0).toInt();
    return v > 0 ? v : 0;   // a negative or corrupt value reads as unknown, not as a length
}

void MediaDurations::note(const QString& itemKey, int seconds) { noteIn(store(), itemKey, seconds); }
int  MediaDurations::seconds(const QString& itemKey)           { return secondsIn(store(), itemKey); }
