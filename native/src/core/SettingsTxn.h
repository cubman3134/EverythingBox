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
    // THREAD AFFINITY: every function here is UI-THREAD ONLY. The transaction state (the active flag and the
    // snapshot map) is plain unguarded process-wide state — there is no mutex — so calling any of these off
    // the UI thread is a data race on THAT state.
    //
    // This is deliberately NOT a claim that the ini is UI-thread only. It is not: stats accrual, play-time
    // accrual and download progress write the SAME file from background threads while a settings panel is
    // open. That stays safe for two separate reasons — QSettings guards its own QConfFile internally, and
    // those families are out of scope (see inScope) so the transaction never reads or restores them. What
    // must stay on the UI thread is this module's state, not the file it talks to.
    //
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
    //
    // COST CONTRACT — CALL ON NAVIGATION EVENTS ONLY. dirtyCount() is O(ALL KEYS IN THE INI), not O(settings
    // keys), and isDirty() is dirtyCount() so it costs exactly the same. Noticing a key CREATED since begin()
    // can only be done by scanning, and QSettings::allKeys() materialises a fresh QString for every key in
    // the file. The scan is load-bearing and must stay — but note the families it walks are precisely the
    // ones the transaction IGNORES (resume/, recent/, marks/, stats/, playstats/, pcgames/, downloads/), and
    // those are the UNBOUNDED ones: the cost grows with the size of the user's library, not with the size of
    // the settings screen.
    //
    // So call these from a discrete navigation event — a Back / Save / Discard handler, a panel close. NEVER
    // bind them to a QML property, a paint or layout path, a focus-changed handler, or anything else that
    // runs per keypress. On the 32-bit armv7 Android TV box with a multi-thousand-key ini, that shape costs a
    // full-file scan plus thousands of QString allocations on EVERY arrow press. This is the contract
    // Tasks 2-3 must honour.
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
