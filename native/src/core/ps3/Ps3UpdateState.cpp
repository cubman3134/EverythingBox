#include "core/ps3/Ps3UpdateState.h"
#include "core/ps3/Ps3Version.h"
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <utility>

Ps3UpdateState::Ps3UpdateState(QString path) : path_(std::move(path)) { load(); }

void Ps3UpdateState::load()
{
    QFile f(path_);
    if (!f.open(QIODevice::ReadOnly)) return;
    const auto doc = QJsonDocument::fromJson(f.readAll());
    if (doc.isObject()) installed_ = doc.object();
}

void Ps3UpdateState::save() const
{
    QDir().mkpath(QFileInfo(path_).absolutePath());
    QFile f(path_);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    f.write(QJsonDocument(installed_).toJson(QJsonDocument::Compact));
}

bool Ps3UpdateState::needsUpdate(const QString& titleId, const QString& latest) const
{
    const QString have = installed_.value(titleId).toString();
    if (have.isEmpty()) return true;
    return Ps3Version::less(have, latest); // installed older than latest
}

void Ps3UpdateState::markInstalled(const QString& titleId, const QString& version)
{
    installed_.insert(titleId, version);
    save();
}
