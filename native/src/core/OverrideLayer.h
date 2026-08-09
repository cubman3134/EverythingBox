// Per-game / per-layer override arithmetic — the pure, window-less core behind issue #95 (per-game core
// options and input-remap overrides). It knows nothing about ini files, cores or ports: it is just the
// RetroArch-style layering rule expressed as three total functions over string maps, so a probe can pin the
// behaviour (and, critically, the NO-LEAK property) without a running core or a ROM.
//
// THE MODEL
// ---------
// A *baseline* is the full set of effective values a game would see with no per-game override at all — for
// core options that is the per-core option file (opt/<core>/*); for input it is the global (and per-system)
// binding. A *delta* (a "layer") carries ONLY the keys whose value the user changed away from the baseline —
// nothing else. The effective set a session actually runs with is the baseline with the ordered layers
// folded on top, later layers winning (issue #95's precedence: core defaults -> per-directory -> per-game;
// we ship core + game, but the fold is n-ary so a middle tier is a data change, not a code change).
//
// THE NO-LEAK RAIL (issue #95's #1 correctness requirement)
// ---------------------------------------------------------
// A game-scoped delta must never be written into the per-core baseline and must never carry into the next
// game launched on the same core. That property is *structural* here, and it rests on one rule this file
// owns: normalizeDelta() drops every key whose desired value equals the baseline. A key equal to its
// baseline therefore can NEVER enter a stored delta, so:
//   * effective(baseline, {}) == baseline                          (a game with no delta sees the baseline)
//   * a "reset this row to the core default" is the ABSENCE of a key, not a key holding the default value —
//     so it cannot be mistaken for an override and re-applied to the next game.
// The store that persists deltas keeps them in their OWN keyspace (Settings' optgame/* and padgame/*), never
// in the baseline's — this unit guarantees the deltas are minimal; the store guarantees they are separate.
#pragma once
#include <QMap>
#include <QString>
#include <QVector>

namespace OverrideLayer
{
    using Map = QMap<QString, QString>;

    // Fold ordered delta layers over a baseline; later layers win. A key absent from every layer keeps its
    // baseline value; a key present only in a layer is added (a per-game override may set an option the core
    // left at an implicit default that the baseline file does not spell out). Order is precedence: pass the
    // layers low-to-high, e.g. { perDirectoryDelta, perGameDelta }.
    Map effective(const Map& baseline, const QVector<Map>& layers);

    // Convenience for the common single-layer case.
    inline Map effective(const Map& baseline, const Map& layer) { return effective(baseline, QVector<Map>{ layer }); }

    // The no-leak store rule. Reduce `desired` to only the keys whose value DIFFERS from the baseline — the
    // minimal delta. A key equal to its baseline is dropped (it is not an override). A key in `desired` that
    // the baseline does not carry is kept (it IS an override away from an unstated default). Keys the baseline
    // has but `desired` omits are not touched — `desired` is the full intended effective set for the keys it
    // mentions, and callers that edit one key at a time should prefer withKey().
    Map normalizeDelta(const Map& baseline, const Map& desired);

    // Set one key in a delta against the baseline, the way a per-row editor does: if `desiredVal` differs from
    // `baselineVal` the key is stored with that value; if it equals the baseline the key is ERASED (a reset).
    // Returns the updated delta. This is the single-row form of the no-leak rule.
    Map withKey(Map delta, const QString& key, const QString& baselineVal, const QString& desiredVal);
}
