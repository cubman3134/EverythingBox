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
// WHAT THIS DELIBERATELY DOES NOT RECORD: any memory contents. No stack bytes,
// no strings, no heap. A plaintext crash log that quotes process memory is a
// credential leak waiting to happen. Code addresses, module names, register
// values and the bad address are all the diagnosis needs and all it gets.
//
// STRUCTURE. The formatter is pure — a plain struct in, characters out, no
// Win32, no Qt, no allocation — so the probe (probe_crashreport) can assert on
// it on any platform, including CI's Linux runner. install() is the thin
// Windows-only half that fills the struct from EXCEPTION_POINTERS and appends
// the rendering to a file. On non-Windows it compiles to nothing.
// ---------------------------------------------------------------------------
namespace CrashReport
{
    // Win32 EXCEPTION_RECORD::ExceptionInformation[0] for an access violation.
    enum Operation { OpRead = 0, OpWrite = 1, OpExecute = 8 };

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
