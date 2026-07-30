#pragma once
#include <cstddef>

// ---------------------------------------------------------------------------
// In-process first-chance access-violation reporter (issue #28).
//
// WHY THIS EXISTS. Issue #28 is a ~1-in-6 access violation inside
// QQuickRepeater::clear(). It resisted five reproduction campaigns, and the
// diagnosis that finally identified it came entirely from ONE full WER dump —
// which, along with the four others, was later evicted by WER's LocalDumps
// rotation. There is no copy. So the next occurrence has to record itself,
// in-process, into a file we own and nothing rotates out from under us.
//
// WHAT THE DIAGNOSIS ACTUALLY TURNS ON (this is why these specific fields):
//
//   * faulting module + offset — Qt ships no PDBs, so `Qt6Quick.dll+0x30f7a5`
//     resolved against the export table is what named QQuickRepeater::clear()
//     at all. Without the module BASE the offset cannot be computed, so the
//     base is recorded and the offset is rendered from it.
//   * the bad address — `0x5` is not merely "null-ish"; it is the arithmetic
//     evidence (see below).
//   * the registers — at the real fault `Rax=1`, `Rbx=4`, `Rdi=0x40`.
//     QPointer::data() reads ExternalRefCountData::strongref at [d + 4]
//     (weakref is at +0, strongref at +4). Rax held the garbage `d` read out
//     of QQuickRepeater's `deletables` vector, so `Rax + 4 == bad addr`
//     (1 + 4 = 5) is what identifies the faulting read as a QPointer strongref
//     load rather than any other null-ish dereference. `Rbx`/`Rdi` are the
//     loop index `i` and the byte offset into the vector, i.e. WHERE in
//     `deletables` the garbage came from. That check is rendered explicitly by
//     the formatter, because a human reading a log at 3am should not have to
//     do hex arithmetic to know whether this is the same bug.
//
//   * the CALL CHAIN — and this is the field the #28 diagnosis actually turned
//     on. `QQuickItem::setParentItem -> QQuickRepeater::itemChange ->
//     regenerate -> clear` was recovered from return-address candidates in the
//     dump; the fault SITE alone (`Qt6Quick.dll+0x30f7a5`) names one function
//     and says nothing about who called it or why the vector was being torn
//     down. A record without the chain would leave the next occurrence exactly
//     where the first one left us.
//
// WHAT THIS DELIBERATELY DOES NOT RECORD: any memory contents. No stack bytes,
// no strings, no heap. A plaintext crash log that quotes process memory is a
// credential leak waiting to happen. Code addresses, module names, register
// values and the bad address are all the diagnosis needs and all it gets.
//
// The call chain does not weaken that rule and must not be made to: what is
// captured is RETURN ADDRESSES — code pointers — and nothing else. No stack
// bytes are read out, no arguments, no locals, no strings. The published dump
// analysis on issue #28 is precisely such an address list with no memory
// extracted from it, and this matches that standard exactly.
//
// STRUCTURE. The formatter is pure — a plain struct in, characters out, no
// Win32, no Qt, no allocation — so the probe (probe_crashreport) can assert on
// it on any platform, including CI's Linux runner. install() is the thin
// Windows-only half that fills the struct from EXCEPTION_POINTERS and appends
// the rendering to a file. On non-Windows it compiles to nothing.
//
// The ORDER in which the pieces reach the disk is also pure, and deliberately
// so: emitRecord() below owns it, takes the capture/resolve steps as callbacks,
// and is therefore assertable by the probe on any platform. See its contract
// for why the order is what it is — it is a robustness property, not a
// cosmetic one.
// ---------------------------------------------------------------------------
namespace CrashReport
{
    // Win32 EXCEPTION_RECORD::ExceptionInformation[0] for an access violation.
    enum Operation { OpRead = 0, OpWrite = 1, OpExecute = 8 };

    // How many return addresses the call chain holds. The #28 chain
    // (setParentItem -> itemChange -> regenerate -> clear) is four frames deep
    // below the fault site, and the exception-dispatch frames above it cost a
    // handful more, so 32 is comfortable headroom rather than a guess.
    constexpr unsigned int kMaxFrames = 32;

    // Longest module basename a frame will carry, NUL included. "vcruntime140_1.dll"
    // is 18; anything longer is truncated rather than allowed to grow the record,
    // which lives on the stack of a thread that has just faulted.
    constexpr unsigned int kFrameModuleCap = 32;

    // A plain aggregate — no constructors, no members that own anything. Always
    // initialise with `CrashRecord r = {};`. It is filled on the stack of a
    // process that may already be corrupt, so it must cost nothing to create.
    struct CrashRecord
    {
        char               timestamp[24];   // "YYYY-MM-DD HH:MM:SS" (local), NUL-terminated
        unsigned long      threadId;        // faulting thread
        unsigned int       sequence;        // 1-based: this is record N ...
        unsigned int       limit;           // ... of at most `limit` this process will write

        unsigned long      exceptionCode;   // 0xc0000005 for an access violation
        unsigned long long faultAddress;    // ExceptionAddress (== RIP)
        char               moduleName[64];  // basename of the faulting module, ASCII; empty if unresolved
        unsigned long long moduleBase;      // 0 when the module could not be resolved
        int                operation;       // Operation
        unsigned long long badAddress;      // the address the faulting instruction touched

        bool               haveRegisters;   // false on non-x64: the register lines are then omitted
        unsigned long long rax, rbx, rcx, rdx, rsi, rdi, r8, r9, rip, rsp, rbp;

        // True when this record is being written from the unhandled-exception filter, i.e.
        // this fault is the one that is killing the process. It changes two things: the
        // record is written even though the per-process cap is already spent (see
        // fatalNeedsFullRecord), and the "cap reached" note is suppressed, because a note
        // saying further faults will not be logged is noise on the last fault there will be.
        bool               fatal;

        // The call chain: RETURN ADDRESSES ONLY. Never stack contents — see the header
        // comment. frames[0] is nearest the fault.
        unsigned int       frameCount;
        unsigned long long frames[kMaxFrames];

        // Per-frame resolution, filled by a SEPARATE pass that runs only after the raw
        // addresses above are already on disk. Both stay zero/empty for any frame whose
        // module could not be resolved, and the whole enriched block is skipped when
        // framesResolved is false — an unresolved frame renders as its raw address, never
        // as an offset off a zero base.
        bool               framesResolved;
        char               frameModule[kMaxFrames][kFrameModuleCap];
        unsigned long long frameBase[kMaxFrames];
    };

    // Render `r` into `out`, writing at most `cap` bytes INCLUDING the terminating NUL.
    // Returns the number of characters written, excluding the NUL. `out` is always
    // NUL-terminated when cap > 0; the return is 0 and nothing is written when cap == 0.
    // Never writes at or past out[cap]. Truncation is silent and safe (detectable as
    // return == cap - 1) because the alternative — a handler that faults while reporting
    // a fault — loses the report entirely.
    std::size_t formatRecord(const CrashRecord& r, char* out, std::size_t cap);

    // The one-line "this one was fatal" marker, same buffer contract. First-chance means
    // a handled, harmless AV can appear in the log; this line is how a reader tells the
    // crash from the noise.
    std::size_t formatFatalLine(const CrashRecord& r, char* out, std::size_t cap);

    // The call chain as bare return addresses, one per line, with no module resolution
    // anywhere in it. This is the block that must reach the disk BEFORE resolution is
    // attempted, so it is deliberately incapable of depending on the resolved fields:
    // it reads frames[] and frameCount and nothing else. Renders nothing when
    // frameCount == 0. Same buffer contract as formatRecord.
    std::size_t formatFramesRaw(const CrashRecord& r, char* out, std::size_t cap);

    // The same chain with each frame's module and offset attached — the readable form.
    // Renders nothing unless framesResolved is set, and falls back to the bare address
    // for any individual frame whose module did not resolve. Same buffer contract.
    std::size_t formatFramesResolved(const CrashRecord& r, char* out, std::size_t cap);

    // Has the fault that is now killing the process ALREADY been written as a full record
    // by the first-chance handler? Returns true when the unhandled filter must write one
    // itself.
    //
    // This exists because the per-process record cap can eat the very crash the reporter
    // was written to catch. Benign handled access violations are real — GPU drivers and
    // some libraries use them as control flow — so if five of those land first, the fatal
    // fault hits an exhausted cap and, without this, would be recorded as the one-line
    // FATAL marker alone: module, offset and code survive but the bad address and the
    // REGISTERS are lost, which is exactly the `Rax+4 == bad addr` evidence the whole
    // feature exists to capture. So the fatal path is exempt from the cap. Only the fatal
    // path: benign first-chance noise is still bounded.
    //
    // Distinguishing "already written" from "needs one" cannot be done on the fault
    // address alone, because the common case is a fault that repeats at the SAME address
    // — that is precisely how a cap gets exhausted. So it takes the pair: the address of
    // the last full record written, AND its sequence number compared against the total
    // number of faults seen. If the last record written was for the most recent fault
    // (lastRecordedSequence == faultsSeen) and that fault was at this address, the
    // first-chance handler has already emitted this exact block and the fatal path adds
    // only its marker line. If the counters have diverged — which is what a spent cap
    // looks like, sequence 5 against 6 faults seen — a full record is still owed even
    // when the addresses match.
    //
    // lastRecordedSequence == 0 means no record has ever been written, so a full one is
    // always owed; that also keeps a fatal fault at address 0 from matching the zeroed
    // initial state.
    bool fatalNeedsFullRecord(unsigned long long lastRecordedFault,
                              unsigned int       lastRecordedSequence,
                              unsigned int       faultsSeen,
                              unsigned long long fatalFaultAddress);

    // A chunk of finished text on its way to the log. The sink is expected to make the
    // bytes durable before returning — see emitRecord for why that matters.
    using WriteFn = void (*)(void* ctx, const char* data, std::size_t len);

    // One of the two side-effecting steps emitRecord interleaves with its writes.
    using StepFn = void (*)(void* ctx, CrashRecord& r);

    // Emit one complete record through `write`, in the order that survives the worst case.
    // Either step may be null, in which case it is skipped.
    //
    //   1. write() the fault-site record            (formatRecord)
    //   2. capture()  — fill frames[] / frameCount
    //   3. write() the raw return addresses         (formatFramesRaw)
    //   4. resolve()  — fill frameModule[] / frameBase[] / framesResolved
    //   5. write() the enriched chain               (formatFramesResolved)
    //
    // WHY THIS ORDER, AND WHY IT IS A FUNCTION RATHER THAN A COMMENT. Step 4 calls
    // GetModuleHandleExW once per frame, and that takes the loader lock. If the process
    // faulted while the loader lock was held — during a DLL load, a static initialiser, a
    // TLS callback — step 4 deadlocks and the handler never returns. The reporter must
    // therefore never have anything undelivered when it enters step 4. Step 1 puts the
    // registers and the bad address on disk before any frame work happens at all, and
    // step 3 puts the raw addresses there before resolution is attempted; if step 4 hangs,
    // what is already written is still a complete, usable record. Raw addresses resolve
    // offline against a module list, which is exactly how the production dump was read.
    //
    // Step 2 is inside the same discipline for the same reason: stack unwinding is not
    // free of locks either, so the fault-site record is durable before it runs. The
    // existing code already accepts this class of risk once, for the fault address; the
    // point of the ordering is not to multiply it without the write-first safeguard.
    //
    // The order is the contract, so it is expressed as callbacks and asserted by
    // probe_crashreport on any platform, rather than living as a comment on a Windows-only
    // handler that CI never compiles.
    //
    // `scratch`/`scratchCap` is the formatting buffer; it is reused between steps, so the
    // sink must consume each chunk before returning. Nothing here allocates.
    void emitRecord(CrashRecord& r,
                    WriteFn write,
                    StepFn  capture,
                    StepFn  resolve,
                    void*   ctx,
                    char*   scratch,
                    std::size_t scratchCap);

    // Install the handlers. Windows-only; a no-op elsewhere. Call once, early in main().
    //
    // The log path is taken as UTF-8 and resolved into a static wide buffer HERE, at
    // install time, precisely so the handler never has to touch AppPaths, QString or any
    // allocator. That is also why this takes a parameter instead of calling AppPaths
    // itself: it keeps this translation unit free of Qt, which is what lets the probe
    // link it standalone.
    void install(const char* logPathUtf8);

    // How many access-violation records one process will write before it stops. A fault
    // that repeats (a bad pointer touched every frame) must not be able to fill the disk.
    constexpr unsigned int kMaxRecords = 5;
}
