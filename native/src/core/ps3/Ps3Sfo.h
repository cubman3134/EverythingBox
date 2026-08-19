#pragma once
#include <QByteArray>
#include <QString>
#include <optional>

// Parses a PS3 PARAM.SFO blob (the small key/value binary Sony stores game metadata in). Pure: takes
// bytes, returns the value or nullopt on any malformation.
namespace Ps3Sfo {
// The string value stored under `key` (TITLE_ID, APP_VER, …). Values are ASCII and null-terminated;
// an empty value reads as absent. nullopt when the blob is malformed or carries no such key.
std::optional<QString> stringValue(const QByteArray& sfo, const QByteArray& key);
std::optional<QString> titleIdFromSfo(const QByteArray& sfo);
}
