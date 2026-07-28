// The settings save/discard transaction (issue #26). Before this, every settings row was immediate-apply —
// all 34 Settings::set* accessors are `store().setValue(k,v); store().sync();` — so there was no pending
// state, nothing to discard, and Back was indistinguishable from Save.
//
// The design is SNAPSHOT AND RESTORE, not buffer and flush. Immediate-apply is kept completely unchanged,
// which is the point: the rows most worth protecting are the ones that MUST apply live (the theme previews
// live; display mode re-lays out the surface you are standing on). A pending map would have to special-case
// exactly those, and Discard would then be a lie for them. Snapshotting instead means live rows need NO
// special case at all — the theme previews because the write genuinely happened — and Discard is a restore.
//
// QtCore only, own file-local store(), so probe_settingstxn links lean.
#pragma once
#include <QString>

namespace SettingsTxn
{
    // Is this key owned by the settings screens? THE LOAD-BEARING PREDICATE. A whole-ini snapshot would be
    // a DATA-LOSS bug: cloud sync, stats accrual, resume positions and the download catalog are all written
    // while a settings panel is open, and rollback would clobber them.
    //
    // Note this is deliberately NOT "exclude everything device-local" — display/mode, roms/folder,
    // library/folder and emulators/root are per-device AND are settings rows a user must be able to
    // discard. CloudSync::isDeviceLocalKey is the precedent for this SHAPE of predicate, not its contents.
    bool inScope(const QString& key);

    // Snapshot every in-scope key. A begin() while already active is a NO-OP, not a reset: hub ->
    // Appearance -> theme picker all call it, and they must share ONE transaction so Discard from any depth
    // reverts the whole visit. Re-snapshotting would silently make earlier changes permanent.
    void begin();
    bool active();

    // How many in-scope keys differ from the snapshot. Compares VALUES, not edits, so changing something
    // and changing it back reads clean and never prompts. The prompt states this count — never the values,
    // which would leak masked credential rows.
    int  dirtyCount();
    bool isDirty();

    void commit();     // drop the snapshot
    void rollback();   // restore every differing in-scope key; remove in-scope keys created during the txn

#ifdef EB_SETTINGSTXN_TEST_SEAM
    // Probe-only: redirect the store to a scratch ini. Gated so production cannot call it — an unguarded
    // call would silently redirect every settings read/write for the process lifetime. The whole seam (the
    // statics, this setter, and the branch in store()) is compiled ONLY for probe_settingstxn, matching the
    // EB_THEMECHOICE_TEST_SEAM precedent in ThemeChoice.h.
    void setIniPathForTesting(const QString& path);
#endif
}
