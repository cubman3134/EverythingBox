#pragma once
#include <QByteArray>
#include <QString>
#include <optional>

// Reads a PS3 game's Title ID (e.g. "BLUS31156") from a rom path. Handles the two formats EverythingBox
// hands RPCS3 — an extracted/JB game folder (reads PS3_GAME/PARAM.SFO) and a .pkg (reads content_id).
// Any other format returns nullopt so the update step falls through to a normal boot.
namespace Ps3TitleId {
std::optional<QString> titleIdFromPkgHeader(const QByteArray& header);
std::optional<QString> read(const QString& romPath);
}
