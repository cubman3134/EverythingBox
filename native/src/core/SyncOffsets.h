// Persistent audio + subtitle sync offsets (seconds), the store the player's sync controls read/write. Backed
// by the same portable mymediavault.ini as Settings (QtCore only — no Quick/Widgets). Two axes (Audio, Sub),
// two scopes: a global default per axis, plus optional per-file overrides keyed by an opaque file key. resolve()
// picks the per-file value when present, else the global. Every writer clamps to ±10.0 seconds; a corrupt or
// absent value reads back as 0.0. An empty key means "the globals": resolve() returns them and savePerFile()
// is a no-op (so it never writes a `sync/files//` junk entry).
#pragma once
#include <QString>

namespace SyncOffsets
{
    enum class Which { Audio, Sub };
    struct Pair { double audio = 0.0; double sub = 0.0; };

    Pair   resolve(const QString& key);                            // per-file if present else global; key empty => globals
    void   savePerFile(const QString& key, Which w, double secs);  // clamped ±10.0; no-op on empty key
    void   clearPerFile(const QString& key, Which w);              // removes the per-file entry
    double globalDefault(Which w);                                 // default 0.0; corrupt => 0.0
    void   setGlobalDefault(Which w, double secs);                 // clamped ±10.0
}
