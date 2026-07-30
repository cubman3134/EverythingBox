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

    if (r.limit != 0 && r.sequence >= r.limit)
        putStr(s, "  (record cap reached; further access violations will NOT be logged this run)\n");

    putStr(s, "======================================\n");

    return finish(s);
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
    // Resolved ONCE at install time. The handler only ever reads it.
    wchar_t       g_logPath[MAX_PATH] = { 0 };
    volatile LONG g_avCount           = 0;   // access violations seen (may exceed kMaxRecords; only the first few are written)
    volatile LONG g_fatalWritten      = 0;   // the fatal marker is written at most once
    LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter = nullptr;
    bool          g_installed         = false;

    // Append `len` bytes to the log. Raw Win32: CreateFileW with FILE_APPEND_DATA gives an
    // atomic append with no seek, and no CRT stdio object to take a lock on. If anything
    // here fails there is nothing sensible to do about it inside an exception handler, so
    // every failure is silent — the alternative is a handler that makes the crash worse.
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
        ::CloseHandle(h);
    }

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

    // Faulting module basename + base. GetModuleHandleEx with UNCHANGED_REFCOUNT takes no
    // reference (nothing to release, nothing to leak) and neither call allocates.
    void fillModule(CrashReport::CrashRecord& rec)
    {
        HMODULE mod = nullptr;
        if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                      | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  reinterpret_cast<LPCWSTR>(rec.faultAddress), &mod)
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
        for (; base[i] && i + 1 < sizeof(rec.moduleName); ++i)
            rec.moduleName[i] = (base[i] < 0x80) ? char(base[i]) : '?';
        rec.moduleName[i] = '\0';

        rec.moduleBase = reinterpret_cast<unsigned long long>(mod);
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

        char buf[1024];
        const std::size_t len = CrashReport::formatRecord(rec, buf, sizeof buf);
        appendLog(buf, len);

        return EXCEPTION_CONTINUE_SEARCH;
    }

    // Runs only when nothing handled the fault, i.e. this really is the crash. One line,
    // once, then straight on to whatever filter was installed before us (normally WER's).
    LONG WINAPI unhandledFilter(EXCEPTION_POINTERS* ep)
    {
        if (ep && ep->ExceptionRecord && ::InterlockedExchange(&g_fatalWritten, 1) == 0)
        {
            CrashReport::CrashRecord rec = {};
            fillCommon(rec, ep);
            char buf[512];
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
