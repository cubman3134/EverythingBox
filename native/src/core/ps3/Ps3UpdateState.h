#pragma once
#include <QString>
#include <QJsonObject>

// Per-Title-ID record of the highest update version already installed, persisted as a small JSON file.
// Makes the launch-time update check a no-op once a game is current.
class Ps3UpdateState {
public:
    explicit Ps3UpdateState(QString path);
    bool needsUpdate(const QString& titleId, const QString& latest) const;
    void markInstalled(const QString& titleId, const QString& version);
private:
    QString     path_;
    QJsonObject installed_; // titleId -> version
    void load();
    void save() const;
};
