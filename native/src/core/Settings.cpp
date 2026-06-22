#include "Settings.h"
#include <QSettings>
#include <QCoreApplication>

static QSettings& store()
{
    static QSettings s(QCoreApplication::applicationDirPath() + QStringLiteral("/goliath.ini"),
                       QSettings::IniFormat);
    return s;
}

QString Settings::coreFor(const QString& systemId)
{
    return store().value(QStringLiteral("cores/") + systemId).toString();
}

void Settings::setCoreFor(const QString& systemId, const QString& core)
{
    store().setValue(QStringLiteral("cores/") + systemId, core);
    store().sync();
}

// Keyed "opt/<core>/<key>". The option key is the core's own (e.g. "mgba_gb_model"); it can't collide
// across cores because <core> namespaces it.
QString Settings::optionValue(const QString& core, const QString& key)
{
    return store().value(QStringLiteral("opt/") + core + QStringLiteral("/") + key).toString();
}

void Settings::setOptionValue(const QString& core, const QString& key, const QString& value)
{
    store().setValue(QStringLiteral("opt/") + core + QStringLiteral("/") + key, value);
    store().sync();
}

int Settings::padBinding(int port, int retroId, int defaultCode)
{
    return store().value(QStringLiteral("pad/%1/%2").arg(port).arg(retroId), defaultCode).toInt();
}

void Settings::setPadBinding(int port, int retroId, int code)
{
    store().setValue(QStringLiteral("pad/%1/%2").arg(port).arg(retroId), code);
    store().sync();
}

int Settings::keyBinding(int port, int retroId, int defaultKey)
{
    return store().value(QStringLiteral("kbd/%1/%2").arg(port).arg(retroId), defaultKey).toInt();
}

void Settings::setKeyBinding(int port, int retroId, int qtKey)
{
    store().setValue(QStringLiteral("kbd/%1/%2").arg(port).arg(retroId), qtKey);
    store().sync();
}

bool Settings::turboButton(int port, int retroId)
{
    return store().value(QStringLiteral("turbo/%1/%2").arg(port).arg(retroId), false).toBool();
}

void Settings::setTurboButton(int port, int retroId, bool on)
{
    store().setValue(QStringLiteral("turbo/%1/%2").arg(port).arg(retroId), on);
    store().sync();
}

int Settings::turboHalfPeriod()
{
    return qBound(1, store().value(QStringLiteral("turbo/halfPeriod"), 3).toInt(), 30);
}

void Settings::setTurboHalfPeriod(int frames)
{
    store().setValue(QStringLiteral("turbo/halfPeriod"), qBound(1, frames, 30));
    store().sync();
}
