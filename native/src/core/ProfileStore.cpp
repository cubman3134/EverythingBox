#include "ProfileStore.h"
#include "AppPaths.h"

#include <QSettings>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QUuid>

static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/mymediavault.ini"),
                       QSettings::IniFormat);
    return s;
}

QVector<Profile> ProfileStore::list()
{
    QVector<Profile> out;
    const QByteArray json = store().value(QStringLiteral("profiles/list")).toString().toUtf8();
    for (const QJsonValue& v : QJsonDocument::fromJson(json).array())
    {
        if (!v.isObject()) continue;
        const QJsonObject o = v.toObject();
        Profile p;
        p.id   = o.value(QStringLiteral("id")).toString();
        p.name = o.value(QStringLiteral("name")).toString();
        p.icon = o.value(QStringLiteral("icon")).toString();
        if (!p.id.isEmpty()) out.push_back(p);
    }
    return out;
}

static void save(const QVector<Profile>& items)
{
    QJsonArray arr;
    for (const Profile& p : items)
    {
        QJsonObject o;
        o.insert(QStringLiteral("id"), p.id);
        o.insert(QStringLiteral("name"), p.name);
        o.insert(QStringLiteral("icon"), p.icon);
        arr.append(o);
    }
    store().setValue(QStringLiteral("profiles/list"),
                     QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
    store().sync();
}

Profile ProfileStore::add(const QString& name, const QString& icon)
{
    Profile p;
    p.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    p.name = name.trimmed().isEmpty() ? QStringLiteral("User") : name.trimmed();
    p.icon = icon.isEmpty() ? QStringLiteral("🙂") : icon;
    QVector<Profile> items = list();
    items.push_back(p);
    save(items);
    return p;
}

void ProfileStore::update(const QString& id, const QString& name, const QString& icon)
{
    QVector<Profile> items = list();
    for (Profile& p : items)
        if (p.id == id)
        {
            if (!name.trimmed().isEmpty()) p.name = name.trimmed();
            if (!icon.isEmpty())           p.icon = icon;
        }
    save(items);
}

void ProfileStore::remove(const QString& id)
{
    QVector<Profile> items = list();
    for (int i = items.size() - 1; i >= 0; --i)
        if (items[i].id == id) items.remove(i);
    save(items);
    if (currentId() == id) setCurrent(items.isEmpty() ? QString() : items.first().id);
}

QString ProfileStore::currentId()
{
    return store().value(QStringLiteral("profiles/current")).toString();
}

void ProfileStore::setCurrent(const QString& id)
{
    store().setValue(QStringLiteral("profiles/current"), id);
    store().sync();
}

Profile ProfileStore::current()
{
    const QString id = currentId();
    for (const Profile& p : list())
        if (p.id == id) return p;
    return {};
}
