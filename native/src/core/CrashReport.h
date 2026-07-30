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
// so: emitRecord() below owns it, takes every enrichment step as a callback,
// and is therefore assertable by the probe on any platform. See its contract
// for why the order is what it is — it is a robustness property, not a
// cosmetic one.
//
// WHAT THE HANDLER MAY NOT DO, restated because two of these were violated once
// already and neither violation was visible to CI (the Windows half of
// CrashReport.cpp is never compiled there):
//
//   * NO ALLOCATION AND NO HEAP LOCK, not even transitively. This is why the
//     log file handle is opened ONCE at install() time and held for the
//     lifetime of the process: CreateFileW converts its DOS path to an NT path
//     through RtlDosPathNameToNtPathName_U, which allocates from the process
//     default heap and takes the heap lock. A fault rooted in heap corruption —
//     a near neighbour of the #28 garbage-vector-entry shape — or a fault taken
//     inside malloc/free with the lock held would then re-enter the allocator
//     from the handler and hang or fault a second time, losing the record for
//     the real crash. appendLog is therefore WriteFile + FlushFileBuffers and
//     nothing else. (The file I/O itself is an accepted trade: writing at
//     first chance is the point. The per-append path conversion was not.)
//
//     A consequence worth knowing: crash_report.log now exists, zero bytes
//     long, from startup. An empty one means the reporter was armed and the run
//     was clean.
//
//   * NO LARGE STACK FRAME IN A HANDLER ITSELF. SetUnhandledExceptionFilter and
//     AddVectoredExceptionHandler both run for EXCEPTION_STACK_OVERFLOW, where
//     roughly one page of stack is left and exception dispatch has already
//     eaten part of it. A CrashRecord is ~1.7 KB and the scratch buffer is
//     another 2.5 KB, and a prologue commits that frame (via __chkstk)
//     regardless of which branch the function then takes — so merely declaring
//     them in the filter is enough to fault the filter itself on a stack
//     overflow, kill the process inside the handler, and lose the WER dump that
//     would otherwise have been written. Both handlers are therefore thin:
//     every one of those locals lives in a __declspec(noinline) helper that a
//     stack overflow never reaches, and the unhandled filter additionally tests
//     faultMustSkipRecording() before anything else. Both halves are needed —
//     the early test alone still pays the prologue.
//
// Neither rule is compiled by CI, so both are gated by run-headless-probes.sh
// ("crashreport handler discipline"), which reads the source shape directly:
// no CreateFileW inside appendLog, no CrashRecord/scratch local inside either
// handler, __declspec(noinline) on the helpers that own them, and the
// stack-overflow test present in the unhandled filter. The DECISION in that
// last one is pure (faultMustSkipRecording) and asserted by probe_crashreport
// like everything else.
//
// CONCURRENCY, so whoever reads a real log is not misled: one record is 3–4
// separate appends (fault site, module + footer, raw chain, resolved chain).
// Each append is individually atomic — FILE_APPEND_DATA guarantees that much,
// so no line is ever spliced into another line — but the record as a WHOLE is
// not. Two threads faulting at the same moment can therefore interleave their
// blocks: an "ACCESS VIOLATION" header followed by the other thread's call
// chain is possible. The `thread <id>` field on the header line is what ties a
// block back to its record. This is deliberate: serialising the blocks would
// mean taking a lock in a handler, which is exactly what must not happen here.
// ---------------------------------------------------------------------------
namespace CrashReport
{
    // Win32 EXCEPTION_RECORD::ExceptionInformation[0] for an access violation.
    enum Operation { OpRead = 0, OpWrite = 1, OpExecute = 8 };

    // The two NTSTATUS values this file reasons about, spelled out rather than taken from
    // windows.h so the pure half — and therefore the probe — stays portable. CrashReport.cpp
    // static_asserts each against the real EXCEPTION_* macro inside its _WIN32 half, so a
    // divergence is a compile error on the only platform where it could matter, not a silent
    // mismatch between what the probe asserts and what the handler tests.
    constexpr unsigned long kExceptionAccessViolation = 0xC0000005ul;
    constexpr unsigned long kExceptionStackOverflow   = 0xC00000FDul;

    // How many return addresses the call chain holds. The #28 chain
    // (setParentItem -> itemChange -> regenerate -> clear) is four frames deep
    // below the fault site, and the exception-dispatch frames above it cost a
    // handful more, so 32 is comfortable headroom rather than a guess.
    constexpr unsigned int kMaxFrames = 32;

    // Longest module basename a frame will carry, NUL included. "vcruntime140_1.dll"
    // is 18; anything longer is truncated rather than allowed to grow the record,
    // which lives on the stack of a thread that has just faulted.
    constexpr unsigned int kFrameModuleCap = 32;

    // The handler's formatting buffer, sized for the largest single block emitRecord emits —
    // the resolved chain, 32 frames of "    #00 0x7ffced04f7a5  vcruntime140_1.dll+0x30f7a5".
    //
    // It lives HERE rather than in CrashReport.cpp because probe_crashreport asserts that the
    // biggest rendering actually fits it, and that assertion is worth nothing if the probe
    // holds its own copy of the number: shrink the handler's buffer and the probe stays green
    // while a real crash silently truncates the tail of the call chain — the field the #28
    // diagnosis turned on. One constant, asserted against itself.
    constexpr std::size_t kScratch = 2560;

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

    // Render the fault site into `out`, writing at most `cap` bytes INCLUDING the terminating
    // NUL. Returns the number of characters written, excluding the NUL. `out` is always
    // NUL-terminated when cap > 0; the return is 0 and nothing is written when cap == 0.
    // Never writes at or past out[cap]. Truncation is silent and safe (detectable as
    // return == cap - 1) because the alternative — a handler that faults while reporting
    // a fault — loses the report entirely.
    //
    // This is the block that needs NOTHING resolved: the exception code, the raw fault
    // address, the operation, the bad address and the registers, all of which come straight
    // out of EXCEPTION_POINTERS. It is deliberately incapable of reading moduleName/moduleBase,
    // for the same reason formatFramesRaw cannot read the resolved frame fields — it is the
    // first thing written, before the one GetModuleHandleExW call that names the faulting
    // module, so it must not depend on that call having returned. See emitRecord.
    std::size_t formatRecord(const CrashRecord& r, char* out, std::size_t cap);

    // The faulting module and offset, plus the closing rule — the tail of the fault-site
    // block, written after the module has been resolved. Renders "<unknown>" rather than an
    // offset off a zero base when it did not resolve. Same buffer contract as formatRecord.
    //
    // Split out from formatRecord so that a fault taken under a loader lock that never
    // releases costs this one field instead of the whole record: GetModuleHandleExW takes
    // that lock, and before this split it ran before the first byte of the record reached
    // the disk. Losing "Qt6Quick.dll+0x30f7a5" hurts — it is what named QQuickRepeater::clear()
    // — but the raw fault address in the block above resolves offline against a module list,
    // which is how the production dump was read in the first place, and losing the registers
    // as well would have left nothing.
    std::size_t formatFaultModule(const CrashRecord& r, char* out, std::size_t cap);

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

    // Must this fault be handed straight to the previous filter, with NO recording work at
    // all? True for EXCEPTION_STACK_OVERFLOW and nothing else.
    //
    // SetUnhandledExceptionFilter runs for EVERY unhandled exception, and a stack overflow is
    // the one where the handler cannot afford itself. When the guard page goes, roughly one
    // page of stack is left, and exception dispatch has already spent part of it. A
    // CrashRecord is ~1.7 KB (frameModule alone is 32x32) and the scratch buffer is 2.5 KB
    // more, so the record-writing path cannot run — but worse, its FRAME is committed by the
    // prologue before any branch is taken, so on a stack overflow the filter faults inside
    // itself, the OS terminates the process on the spot, the previous filter is never called
    // and WER writes no dump. That is strictly worse than not having a crash reporter at all:
    // before this reporter existed, a stack overflow produced a WER dump.
    //
    // So the filter tests this FIRST and returns straight to the chain, and separately keeps
    // every large local out of its own frame (see the header comment). Both are needed; the
    // test alone still pays the prologue.
    //
    // Scoped to stack overflow deliberately. Every other exception code — access violation,
    // illegal instruction, divide by zero, a C++ throw that nothing caught — arrives with a
    // normal stack, and the record is exactly what we want for it. Widening this predicate
    // would silently disarm the reporter for whole classes of crash.
    bool faultMustSkipRecording(unsigned long exceptionCode);

    // A chunk of finished text on its way to the log. The sink is expected to make the
    // bytes durable before returning — see emitRecord for why that matters.
    using WriteFn = void (*)(void* ctx, const char* data, std::size_t len);

    // One of the two side-effecting steps emitRecord interleaves with its writes.
    using StepFn = void (*)(void* ctx, CrashRecord& r);

    // Emit one complete record through `write`, in the order that survives the worst case.
    // Any step may be null, in which case it is skipped.
    //
    //   1. write() the fault site — registers, bad address, raw fault address (formatRecord)
    //   2. resolveModule() — fill moduleName / moduleBase
    //   3. write() the faulting module + offset, and the closing rule  (formatFaultModule)
    //   4. capture()  — fill frames[] / frameCount
    //   5. write() the raw return addresses         (formatFramesRaw)
    //   6. resolve()  — fill frameModule[] / frameBase[] / framesResolved
    //   7. write() the enriched chain               (formatFramesResolved)
    //
    // WHY THIS ORDER, AND WHY IT IS A FUNCTION RATHER THAN A COMMENT. Steps 2 and 6 call
    // GetModuleHandleExW, and that takes the loader lock. If the process faulted while the
    // loader lock was held — during a DLL load, a static initialiser, a TLS callback — those
    // steps deadlock and the handler never returns. The reporter must therefore never have
    // anything undelivered when it enters one. So every enrichment step is preceded by a
    // write of everything that does not depend on it: step 1 puts the registers and the bad
    // address on disk before ANY module or stack work happens at all, and step 5 puts the raw
    // return addresses there before per-frame resolution is attempted. A hang in step 2 costs
    // the module name; a hang in step 6 costs the readable chain. Neither costs the record.
    // Raw addresses resolve offline against a module list, which is exactly how the production
    // dump was read.
    //
    // Step 4 is inside the same discipline for the same reason: stack unwinding is not free of
    // locks either, so the fault-site record is durable before it runs.
    //
    // The order is the contract, so it is expressed as callbacks and asserted by
    // probe_crashreport on any platform, rather than living as a comment on a Windows-only
    // handler that CI never compiles.
    //
    // `scratch`/`scratchCap` is the formatting buffer; it is reused between steps, so the
    // sink must consume each chunk before returning. Nothing here allocates.
    void emitRecord(CrashRecord& r,
                    WriteFn write,
                    StepFn  resolveModule,
                    StepFn  capture,
                    StepFn  resolve,
                    void*   ctx,
                    char*   scratch,
                    std::size_t scratchCap);

    // Install the handlers. Windows-only; a no-op elsewhere. Call once, early in main().
    //
    // The log path is taken as UTF-8, converted HERE, and the file is OPENED here — the
    // handle is then held for the lifetime of the process and the handler only ever
    // WriteFile()s to it. That is not tidiness: CreateFileW converts its DOS path to an NT
    // path through RtlDosPathNameToNtPathName_U, which allocates from the default heap and
    // takes the heap lock, so calling it per append would put an allocation on the handler's
    // path — in a process whose heap may be exactly what is broken. See the header comment.
    //
    // Taking the path as a parameter rather than calling AppPaths keeps this translation
    // unit free of Qt, which is what lets the probe link it standalone.
    void install(const char* logPathUtf8);

    // How many access-violation records one process will write before it stops. A fault
    // that repeats (a bad pointer touched every frame) must not be able to fill the disk.
    constexpr unsigned int kMaxRecords = 5;
}
