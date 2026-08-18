#pragma once
#include <QByteArray>
#include <QString>
#include <QVector>

struct Ps3UpdatePackage {
    QString version;
    qint64  size = 0;
    QString sha1;
    QString url;
    QString ps3SystemVer;
};

// Parses Sony's {TITLEID}-ver.xml feed into the update packages, sorted ascending by version.
// Empty body (Sony's "no updates" signal) or malformed XML both yield an empty vector.
namespace Ps3UpdateFeed {
QVector<Ps3UpdatePackage> parseVerXml(const QByteArray& xml);
}
