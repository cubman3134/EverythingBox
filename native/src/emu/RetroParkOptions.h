// Bridges RetroPark's v9 core-options ABI channel into EverythingBox's own CoreOption model.
//
// A RetroPark-backed system (driven shim core, presenting core) exposes its user-tunable settings
// as a JSON array via rp_runtime_core_options_json. This helper parses that JSON into the same
// CoreOption structs the native libretro frontend already produces, so the settings UI and the
// per-core/per-game persistence layer can treat both backends identically. harvest() additionally
// spins up a headless runtime to pull a shim core's option set WITHOUT launching any game.
#pragma once
#include <vector>
#include <QByteArray>
#include <QString>
#include "libretro/LibretroCore.h" // CoreOption

namespace RetroParkOptions
{
    // Parse the rp_runtime_core_options_json array into CoreOption structs (QJsonDocument).
    // Returns an empty vector for an empty array / malformed input.
    std::vector<CoreOption> parse(const QByteArray& json);

    // Harvest options for a shim core dir WITHOUT launching a game: a headless RP_GFX_D3D11 runtime,
    // load_core(coreDir), rp_runtime_core_options_json, parse, destroy. Empty vector on failure.
    std::vector<CoreOption> harvest(const QString& coreDir);
}
