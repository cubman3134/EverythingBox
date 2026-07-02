#include "PcGameStore.h"
#include "AppPaths.h"

#include <QSettings>
#include <QCryptographicHash>

static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/mymediavault.ini"), QSettings::IniFormat);
    return s;
}

// A stable, ini-safe subkey from the (base64/punctuation-laden) game id: '/' and '+' in the raw id would
// otherwise create bogus QSettings subgroups.
static QString keyFor(const QString& id)
{
    const QByteArray h = QCryptographicHash::hash(id.toUtf8(), QCryptographicHash::Sha1).toHex();
    return QStringLiteral("pcgames/") + QString::fromLatin1(h);
}

PcGameStore::Entry PcGameStore::get(const QString& id)
{
    Entry e;
    if (id.isEmpty()) return e;
    const QString k = keyFor(id);
    e.dir = store().value(k + QStringLiteral("/dir")).toString();
    e.exe = store().value(k + QStringLiteral("/exe")).toString();
    e.installerRan = store().value(k + QStringLiteral("/installerRan"), false).toBool();
    return e;
}

void PcGameStore::setDir(const QString& id, const QString& dir)
{
    if (id.isEmpty()) return;
    store().setValue(keyFor(id) + QStringLiteral("/dir"), dir);
    store().sync();
}

void PcGameStore::setExe(const QString& id, const QString& exe)
{
    if (id.isEmpty()) return;
    store().setValue(keyFor(id) + QStringLiteral("/exe"), exe);
    store().sync();
}

void PcGameStore::setInstallerRan(const QString& id, bool ran)
{
    if (id.isEmpty()) return;
    store().setValue(keyFor(id) + QStringLiteral("/installerRan"), ran);
    store().sync();
}
