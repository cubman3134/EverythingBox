#include "CrashReport.h"

// ---------------------------------------------------------------------------
// Half of this file is the pure formatter; the other half is the Windows
// handler that feeds it. See CrashReport.h for why the split exists and why
// these particular fields are the ones that matter.
//
// Everything below the formatter runs INSIDE a first-chance exception handler,
// in a process whose heap may be exactly what is broken. So, in that half:
// no allocation, no Qt, no C++ exceptions, no locks, no CRT stdio. The
// formatter obeys the same rules anyway (it is called from there), which is
// also what makes it trivially testable.
// ---------------------------------------------------------------------------

namespace
{
    // A bounded character sink. `len` is the count actually written and is kept
    // strictly below `cap` so out[len] = '\0' at the end is always in bounds.
    struct Sink
    {
        char*       buf;
        std::size_t cap;
        std::size_t len;
    };

    // The one place a byte is ever written. `len + 1 < cap` reserves the last byte for the
    // NUL, and covers cap == 0 on its own (1 < 0 is false for size_t), so there is no separate
    // empty-buffer guard here — an extra one would be unreachable code in the single function
    // that must be provably correct, since overrunning here means faulting while reporting a
    // fault. The cap == 0 case is handled once, in finish().
    inline void put(Sink& s, char c)
    {
        if (s.len + 1 < s.cap) s.buf[s.len++] = c;
    }

    inline void putStr(Sink& s, const char* t)
    {
        if (!t) return;
        while (*t) put(s, *t++);
    }

    // Lower-case hex with an 0x prefix, unpadded. Unpadded on purpose: `bad addr : 0x5`
    // reads as "null-ish" at a glance, where 0x0000000000000005 reads as an address.
    inline void putHex(Sink& s, unsigned long long v)
    {
        putStr(s, "0x");
        if (v == 0) { put(s, '0'); return; }
        char tmp[16];
        int n = 0;
        while (v && n < 16) { tmp[n++] = "0123456789abcdef"[v & 0xFull]; v >>= 4; }
        while (n) put(s, tmp[--n]);
    }

    inline void putDec(Sink& s, unsigned long long v)
    {
        if (v == 0) { put(s, '0'); return; }
        char tmp[20];
        int n = 0;
        while (v && n < 20) { tmp[n++] = char('0' + (v % 10ull)); v /= 10ull; }
        while (n) put(s, tmp[--n]);
    }

    inline void putReg(Sink& s, const char* name, unsigned long long v)
    {
        putStr(s, name);
        put(s, '=');
        putHex(s, v);
    }

    const char* opName(int op)
    {
        switch (op)
        {
        case CrashReport::OpRead:    return "READ";
        case CrashReport::OpWrite:   return "WRITE";
        case CrashReport::OpExecute: return "EXECUTE";
        default:                     return "UNKNOWN";
        }
    }

    // "<module>+0x<offset>" when the module resolved, otherwise "<unknown>". Never
    // prints a "+0x…" offset off a zero base: an offset from nothing is not a
    // location, and printing one would look like a symbol that could be looked up.
    void putModuleAndOffset(Sink& s, const CrashReport::CrashRecord& r)
    {
        if (r.moduleBase != 0 && r.moduleName[0] != '\0')
        {
            putStr(s, r.moduleName);
            put(s, '+');
            putHex(s, r.faultAddress - r.moduleBase);
        }
        else
        {
            putStr(s, "<unknown>");
        }
    }

    // "  #03 0x7ffc…" — a fixed two-digit index so the chain reads as a column even
    // when it runs past ten frames.
    void putFrameIndex(Sink& s, unsigned int i)
    {
        putStr(s, "    #");
        put(s, char('0' + ((i / 10u) % 10u)));
        put(s, char('0' + (i % 10u)));
        put(s, ' ');
    }

    std::size_t finish(Sink& s)
    {
        if (s.cap == 0) return 0;
        s.buf[s.len] = '\0';
        return s.len;
    }
}

std::size_t CrashReport::formatRecord(const CrashRecord& r, char* out, std::size_t cap)
{
    Sink s = { out, out ? cap : 0, 0 };

    putStr(s, "\n========== ACCESS VIOLATION ==========\n");

    putStr(s, "  when      : ");
    putStr(s, r.timestamp);
    putStr(s, "  thread ");
    putDec(s, r.threadId);
    putStr(s, "  record ");
    putDec(s, r.sequence);
    put(s, '/');
    putDec(s, r.limit);
    // A fatal record is written even when the cap is spent, so the sequence can legitimately
    // exceed the limit here ("record 6/5"). Say so, or it reads as an arithmetic bug.
    if (r.fatal) putStr(s, "  (FATAL - exempt from the cap)");
    put(s, '\n');

    putStr(s, "  exception : ");
    putHex(s, r.exceptionCode);
    put(s, '\n');

    putStr(s, "  at        : ");
    putHex(s, r.faultAddress);
    putStr(s, "  ");
    putModuleAndOffset(s, r);
    put(s, '\n');

    putStr(s, "  operation : ");
    putStr(s, opName(r.operation));
    put(s, '\n');

    putStr(s, "  bad addr  : ");
    putHex(s, r.badAddress);
    put(s, '\n');

    if (r.haveRegisters)
    {
        putStr(s, "  ");
        putReg(s, "Rax", r.rax); put(s, ' ');
        putReg(s, "Rbx", r.rbx); put(s, ' ');
        putReg(s, "Rcx", r.rcx); put(s, ' ');
        putReg(s, "Rdx", r.rdx);
        put(s, '\n');

        putStr(s, "  ");
        putReg(s, "Rsi", r.rsi); put(s, ' ');
        putReg(s, "Rdi", r.rdi); put(s, ' ');
        putReg(s, "R8",  r.r8);  put(s, ' ');
        putReg(s, "R9",  r.r9);
        put(s, '\n');

        putStr(s, "  ");
        putReg(s, "Rip", r.rip); put(s, ' ');
        putReg(s, "Rsp", r.rsp); put(s, ' ');
        putReg(s, "Rbp", r.rbp);
        put(s, '\n');

        // The whole point of recording registers. QPointer::data() loads
        // ExternalRefCountData::strongref at [d + 4]; at the #28 fault Rax held the
        // garbage `d`, so Rax+4 == bad addr is the signature. A MATCH says "same bug";
        // a MISMATCH says "different fault, stop assuming".
        putStr(s, "  QPointer check: Rax+4=");
        putHex(s, r.rax + 4ull);
        const bool match = (r.rax + 4ull) == r.badAddress;
        putStr(s, match ? " == bad addr  (MATCH)\n" : " != bad addr  (MISMATCH)\n");
    }

    // Suppressed on a fatal record: "further access violations will NOT be logged" is noise
    // on the last fault this process will ever take, and it would contradict the line above.
    if (!r.fatal && r.limit != 0 && r.sequence >= r.limit)
        putStr(s, "  (record cap reached; further access violations will NOT be logged this run)\n");

    putStr(s, "======================================\n");

    return finish(s);
}

std::size_t CrashReport::formatFramesRaw(const CrashRecord& r, char* out, std::size_t cap)
{
    Sink s = { out, out ? cap : 0, 0 };

    if (r.frameCount == 0) return finish(s);

    // Deliberately reads frames[]/frameCount and NOTHING else. This block is the one that
    // has to be on disk before module resolution is attempted (see emitRecord), so it must
    // be structurally incapable of needing a resolved field. Note for whoever reads the log
    // after a resolution deadlock ate the enriched block below: these resolve offline
    // against a module list, which is how the production #28 dump was read in the first place.
    putStr(s, "  call chain (raw return addresses, ");
    putDec(s, r.frameCount);
    putStr(s, " frames; nearest the fault first):\n");

    const unsigned int n = (r.frameCount > kMaxFrames) ? kMaxFrames : r.frameCount;
    for (unsigned int i = 0; i < n; ++i)
    {
        putFrameIndex(s, i);
        putHex(s, r.frames[i]);
        put(s, '\n');
    }

    return finish(s);
}

std::size_t CrashReport::formatFramesResolved(const CrashRecord& r, char* out, std::size_t cap)
{
    Sink s = { out, out ? cap : 0, 0 };

    // Nothing at all unless the resolution pass actually completed. If it deadlocked on the
    // loader lock the process never got here, and if it was skipped the raw block above is
    // the record — emitting an empty "resolved" heading would suggest the chain resolved to
    // nothing, which is a different and wrong claim.
    if (!r.framesResolved || r.frameCount == 0) return finish(s);

    putStr(s, "  call chain (resolved):\n");

    const unsigned int n = (r.frameCount > kMaxFrames) ? kMaxFrames : r.frameCount;
    for (unsigned int i = 0; i < n; ++i)
    {
        putFrameIndex(s, i);
        putHex(s, r.frames[i]);
        // Same rule as the fault site: never an offset off a zero base. A frame whose module
        // did not resolve keeps its bare address rather than acquiring a fake symbol.
        if (r.frameBase[i] != 0 && r.frameModule[i][0] != '\0')
        {
            putStr(s, "  ");
            putStr(s, r.frameModule[i]);
            put(s, '+');
            putHex(s, r.frames[i] - r.frameBase[i]);
        }
        put(s, '\n');
    }

    return finish(s);
}

bool CrashReport::fatalNeedsFullRecord(unsigned long long lastRecordedFault,
                                       unsigned int       lastRecordedSequence,
                                       unsigned int       faultsSeen,
                                       unsigned long long fatalFaultAddress)
{
    // See the contract in CrashReport.h. The address alone is not enough, because a cap gets
    // exhausted precisely by a fault that repeats at one address; the sequence counter is
    // what separates "the handler just wrote this one" from "the handler stopped writing".
    if (lastRecordedSequence == 0)                  return true;   // nothing has ever been written
    if (lastRecordedSequence != faultsSeen)         return true;   // the cap (or a miss) intervened
    if (lastRecordedFault    != fatalFaultAddress)  return true;   // the last record was a different fault
    return false;
}

void CrashReport::emitRecord(CrashRecord& r,
                             WriteFn write,
                             StepFn  capture,
                             StepFn  resolve,
                             void*   ctx,
                             char*   scratch,
                             std::size_t scratchCap)
{
    if (!write || !scratch || scratchCap == 0) return;

    // 1. The fault site — registers, bad address, module+offset — durable before any stack
    //    work happens. Everything after this point can hang without costing the diagnosis.
    {
        const std::size_t n = formatRecord(r, scratch, scratchCap);
        if (n) write(ctx, scratch, n);
    }

    // 2. Capture return addresses. Unwinding is not lock-free either, hence step 1 first.
    if (capture) capture(ctx, r);

    // 3. The raw addresses, durable before resolution is attempted. This is the write-first
    //    safeguard: if step 4 deadlocks on the loader lock, the chain is still on disk and
    //    still resolves offline.
    {
        const std::size_t n = formatFramesRaw(r, scratch, scratchCap);
        if (n) write(ctx, scratch, n);
    }

    // 4. GetModuleHandleExW per frame — the step that can deadlock.
    if (resolve) resolve(ctx, r);

    // 5. The readable form, which is a convenience on top of a record that is already complete.
    {
        const std::size_t n = formatFramesResolved(r, scratch, scratchCap);
        if (n) write(ctx, scratch, n);
    }
}

std::size_t CrashReport::formatFatalLine(const CrashRecord& r, char* out, std::size_t cap)
{
    Sink s = { out, out ? cap : 0, 0 };

    putStr(s, "  ***** FATAL: the fault above was NOT handled - the process is terminating. code=");
    putHex(s, r.exceptionCode);
    putStr(s, " at ");
    putHex(s, r.faultAddress);
    putStr(s, " ");
    putModuleAndOffset(s, r);
    putStr(s, " thread ");
    putDec(s, r.threadId);
    putStr(s, " ");
    putStr(s, r.timestamp);
    put(s, '\n');

    return finish(s);
}

// ===========================================================================
// Windows installer. Everything from here down is #ifdef'd out entirely on
// other platforms, so the formatter above is all that CI's Linux build sees.
// ===========================================================================
#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace
{
    // The handler's formatting buffer, on its stack. Sized for the largest of the three
    // blocks emitRecord emits — the resolved chain, at 32 frames of
    // "    #00 0x7ffced04f7a5  vcruntime140_1.dll+0x30f7a5" — with room to spare, because
    // truncation here would silently drop the tail of a call chain that is the whole point.
    constexpr std::size_t kScratch = 2560;

    // Resolved ONCE at install time. The handler only ever reads it.
    wchar_t       g_logPath[MAX_PATH] = { 0 };
    volatile LONG g_avCount           = 0;   // access violations seen (may exceed kMaxRecords; only the first few are written)
    volatile LONG g_fatalWritten      = 0;   // the fatal marker is written at most once
    LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter = nullptr;
    bool          g_installed         = false;

    // What the last FULL record written was for. The unhandled filter reads these to decide
    // whether this fault is already on disk or still owed a record; see fatalNeedsFullRecord.
    volatile LONG               g_lastRecordedSeq   = 0;
    volatile unsigned long long g_lastRecordedFault = 0;

    // Append `len` bytes to the log. Raw Win32: CreateFileW with FILE_APPEND_DATA gives an
    // atomic append with no seek, and no CRT stdio object to take a lock on. If anything
    // here fails there is nothing sensible to do about it inside an exception handler, so
    // every failure is silent — the alternative is a handler that makes the crash worse.
    //
    // FlushFileBuffers before the close is not belt-and-braces here: the whole ordering
    // discipline in emitRecord is built on "already written" meaning "survives a hang in the
    // next step", and a write sitting in the cache when the handler deadlocks does not.
    // At most a handful of these run per process, so the cost is irrelevant.
    void appendLog(const char* data, std::size_t len)
    {
        if (!g_logPath[0] || len == 0) return;
        HANDLE h = ::CreateFileW(g_logPath,
                                 FILE_APPEND_DATA,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr,
                                 OPEN_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL,
                                 nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        DWORD written = 0;
        ::WriteFile(h, data, static_cast<DWORD>(len), &written, nullptr);
        ::FlushFileBuffers(h);
        ::CloseHandle(h);
    }

    // The sink emitRecord writes through.
    void writeChunk(void*, const char* data, std::size_t len) { appendLog(data, len); }

    void put2(char* p, int v)
    {
        p[0] = char('0' + ((v / 10) % 10));
        p[1] = char('0' + (v % 10));
    }

    // "YYYY-MM-DD HH:MM:SS" without touching the CRT's locale/time machinery.
    void fillTimestamp(char (&ts)[24])
    {
        SYSTEMTIME st = {};
        ::GetLocalTime(&st);
        put2(ts + 0, st.wYear / 100);
        put2(ts + 2, st.wYear % 100);
        ts[4] = '-'; put2(ts + 5, st.wMonth);
        ts[7] = '-'; put2(ts + 8, st.wDay);
        ts[10] = ' ';
        put2(ts + 11, st.wHour);
        ts[13] = ':'; put2(ts + 14, st.wMinute);
        ts[16] = ':'; put2(ts + 17, st.wSecond);
        ts[19] = '\0';
    }

    // Resolve one code address to its module basename + base, into caller-supplied storage.
    // GetModuleHandleEx with UNCHANGED_REFCOUNT takes no reference (nothing to release,
    // nothing to leak) and neither call allocates.
    //
    // It DOES take the loader lock, which is the whole reason emitRecord flushes before any
    // call to this. Both outputs are left untouched on failure, so an unresolved address
    // keeps a zero base and renders as a bare address rather than a fabricated offset.
    void resolveAddress(unsigned long long addr,
                        char* nameOut, std::size_t nameCap,
                        unsigned long long& baseOut)
    {
        HMODULE mod = nullptr;
        if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                      | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  reinterpret_cast<LPCWSTR>(addr), &mod)
            || mod == nullptr)
            return;

        wchar_t full[MAX_PATH] = { 0 };
        const DWORD n = ::GetModuleFileNameW(mod, full, MAX_PATH);
        if (n == 0) return;

        const wchar_t* base = full;
        for (const wchar_t* p = full; *p; ++p)
            if (*p == L'\\' || *p == L'/') base = p + 1;

        // Narrow by hand: no CRT conversion, no locale, no allocation. Module names are
        // ASCII in practice; anything else becomes '?' rather than mojibake.
        std::size_t i = 0;
        for (; base[i] && i + 1 < nameCap; ++i)
            nameOut[i] = (base[i] < 0x80) ? char(base[i]) : '?';
        nameOut[i] = '\0';

        baseOut = reinterpret_cast<unsigned long long>(mod);
    }

    void fillModule(CrashReport::CrashRecord& rec)
    {
        resolveAddress(rec.faultAddress, rec.moduleName, sizeof rec.moduleName, rec.moduleBase);
    }

    // emitRecord step 2. RETURN ADDRESSES ONLY — RtlCaptureStackBackTrace hands back code
    // pointers and copies no stack memory, which is what keeps the no-memory-contents rule
    // intact. FramesToSkip=1 drops this function itself, which is guaranteed noise and
    // guaranteed not part of the fault; nothing below it is skipped, so the exception
    // dispatch frames stay visible rather than being silently guessed at.
    void captureFrames(void*, CrashReport::CrashRecord& rec)
    {
        PVOID raw[CrashReport::kMaxFrames] = { nullptr };
        const USHORT n = ::RtlCaptureStackBackTrace(1, CrashReport::kMaxFrames, raw, nullptr);
        rec.frameCount = (n > CrashReport::kMaxFrames)
                             ? CrashReport::kMaxFrames
                             : static_cast<unsigned int>(n);
        for (unsigned int i = 0; i < rec.frameCount; ++i)
            rec.frames[i] = reinterpret_cast<unsigned long long>(raw[i]);
    }

    // emitRecord step 4 — the one that can deadlock. By the time this runs the fault site
    // AND the raw addresses are already flushed to disk, so a hang here costs the enriched
    // rendering and nothing else.
    void resolveFrames(void*, CrashReport::CrashRecord& rec)
    {
        for (unsigned int i = 0; i < rec.frameCount && i < CrashReport::kMaxFrames; ++i)
            resolveAddress(rec.frames[i],
                           rec.frameModule[i], CrashReport::kFrameModuleCap,
                           rec.frameBase[i]);
        rec.framesResolved = true;
    }

    void fillCommon(CrashReport::CrashRecord& rec, EXCEPTION_POINTERS* ep)
    {
        const EXCEPTION_RECORD* er = ep->ExceptionRecord;
        fillTimestamp(rec.timestamp);
        rec.threadId      = ::GetCurrentThreadId();
        rec.exceptionCode = er->ExceptionCode;
        rec.faultAddress  = reinterpret_cast<unsigned long long>(er->ExceptionAddress);
        fillModule(rec);
        if (er->ExceptionCode == static_cast<DWORD>(EXCEPTION_ACCESS_VIOLATION)
            && er->NumberParameters >= 2)
        {
            rec.operation  = static_cast<int>(er->ExceptionInformation[0]);
            rec.badAddress = static_cast<unsigned long long>(er->ExceptionInformation[1]);
        }
#if defined(_M_X64) || defined(__x86_64__)
        const CONTEXT* c = ep->ContextRecord;
        if (c)
        {
            rec.haveRegisters = true;
            rec.rax = c->Rax; rec.rbx = c->Rbx; rec.rcx = c->Rcx; rec.rdx = c->Rdx;
            rec.rsi = c->Rsi; rec.rdi = c->Rdi; rec.r8  = c->R8;  rec.r9  = c->R9;
            rec.rip = c->Rip; rec.rsp = c->Rsp; rec.rbp = c->Rbp;
        }
#endif
    }

    // First-chance, installed at the FRONT of the vectored chain so it sees the fault
    // before any SEH frame can swallow it. Always returns EXCEPTION_CONTINUE_SEARCH:
    // nothing about the process's behaviour changes, WER still writes its dump, and a
    // legitimately handled AV still gets handled.
    LONG CALLBACK vectoredHandler(EXCEPTION_POINTERS* ep)
    {
        if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
        if (ep->ExceptionRecord->ExceptionCode != static_cast<DWORD>(EXCEPTION_ACCESS_VIOLATION))
            return EXCEPTION_CONTINUE_SEARCH;

        const LONG n = ::InterlockedIncrement(&g_avCount);
        if (n > static_cast<LONG>(CrashReport::kMaxRecords))
            return EXCEPTION_CONTINUE_SEARCH;   // a repeating fault must not fill the disk

        CrashReport::CrashRecord rec = {};
        rec.sequence = static_cast<unsigned int>(n);
        rec.limit    = CrashReport::kMaxRecords;
        fillCommon(rec, ep);

        char buf[kScratch];
        CrashReport::emitRecord(rec, writeChunk, captureFrames, resolveFrames,
                                nullptr, buf, sizeof buf);

        // Publish what was just written, so the unhandled filter can tell whether this same
        // fault already has a full record. The address goes down first and the sequence
        // second: the sequence is what the reader gates on, so it must not become visible
        // while the address it refers to is still stale.
        g_lastRecordedFault = rec.faultAddress;
        ::InterlockedExchange(&g_lastRecordedSeq, n);

        return EXCEPTION_CONTINUE_SEARCH;
    }

    // Runs only when nothing handled the fault, i.e. this really is the crash. Then straight
    // on to whatever filter was installed before us (normally WER's).
    //
    // The full record here is not redundant with the first-chance one. The per-process cap
    // is spendable by benign handled access violations — GPU drivers and some libraries
    // raise them as control flow — and if five of those land first, the crash we built this
    // for arrives at an exhausted cap. The one-line marker below keeps module, offset and
    // code but loses the bad address and the registers, which is exactly the `Rax+4`
    // evidence the feature exists to capture. This handler holds the same EXCEPTION_POINTERS,
    // so it can simply write the record itself. The cap still bounds first-chance noise;
    // only this path is exempt from it.
    LONG WINAPI unhandledFilter(EXCEPTION_POINTERS* ep)
    {
        if (ep && ep->ExceptionRecord && ::InterlockedExchange(&g_fatalWritten, 1) == 0)
        {
            const EXCEPTION_RECORD* er = ep->ExceptionRecord;
            const LONG seen = g_avCount;

            CrashReport::CrashRecord rec = {};
            rec.sequence = static_cast<unsigned int>(seen);
            rec.limit    = CrashReport::kMaxRecords;
            rec.fatal    = true;
            fillCommon(rec, ep);

            char buf[kScratch];

            // Scoped to access violations on purpose. The record block is headed
            // "ACCESS VIOLATION" and its operation/bad-address fields are AV-specific, so
            // rendering it for a stack overflow or an illegal instruction would be a
            // fabricated diagnosis. Those still get the marker line, which carries the code.
            if (er->ExceptionCode == static_cast<DWORD>(EXCEPTION_ACCESS_VIOLATION)
                && CrashReport::fatalNeedsFullRecord(g_lastRecordedFault,
                                                     static_cast<unsigned int>(g_lastRecordedSeq),
                                                     static_cast<unsigned int>(seen),
                                                     rec.faultAddress))
            {
                CrashReport::emitRecord(rec, writeChunk, captureFrames, resolveFrames,
                                        nullptr, buf, sizeof buf);
            }

            // Always. When the first-chance handler already wrote the block, this line is the
            // only thing that marks it fatal — the block itself must not be duplicated.
            const std::size_t len = CrashReport::formatFatalLine(rec, buf, sizeof buf);
            appendLog(buf, len);
        }
        return g_prevFilter ? g_prevFilter(ep) : EXCEPTION_CONTINUE_SEARCH;
    }
}

void CrashReport::install(const char* logPathUtf8)
{
    if (g_installed) return;
    g_installed = true;

    if (logPathUtf8 && *logPathUtf8)
    {
        const int n = ::MultiByteToWideChar(CP_UTF8, 0, logPathUtf8, -1, g_logPath, MAX_PATH);
        if (n <= 0) g_logPath[0] = L'\0';
    }

    ::AddVectoredExceptionHandler(1 /* first in the chain */, vectoredHandler);
    g_prevFilter = ::SetUnhandledExceptionFilter(unhandledFilter);
}

#else  // !_WIN32

void CrashReport::install(const char*) {}

#endif
