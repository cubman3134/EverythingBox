#pragma once
#include <QByteArray>
#include <QString>
#include <optional>

// Parses a PS3 PARAM.SFO blob (the small key/value binary Sony stores game metadata in) and returns
// the TITLE_ID value. Pure: takes bytes, returns the id or nullopt on any malformation.
namespace Ps3Sfo {
std::optional<QString> titleIdFromSfo(const QByteArray& sfo);
}
