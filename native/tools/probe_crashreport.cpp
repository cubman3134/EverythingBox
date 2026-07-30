// Headless check of the crash-report formatter (issue #28).
//
// The crash reporter's whole reason to exist is that the NEXT occurrence of a bug that has
// already resisted five reproduction campaigns must record itself — because the five WER
// dumps that produced the current diagnosis were destroyed by LocalDumps rotation and there
// is no copy. A crash reporter that is silently broken therefore costs us the same thing
// twice, and it fails silently BY CONSTRUCTION: nobody looks at crash_report.log until
// after the crash, at which point it is too late to notice it renders garbage.
//
// So the rendering is asserted here, and the primary case is not a made-up one — it is the
// real production fault, byte for byte, from the dump analysed in issue #28:
//
//     Qt6Quick.dll+0x30f7a5, READ of 0x5, Rax=1 Rbx=4 Rdi=0x40
//
// with `Rax + 4 == bad addr` (1 + 4 = 5) — the arithmetic that identifies the faulting read
// as QPointer::data() loading ExternalRefCountData::strongref at [d+4] off a garbage entry
// in QQuickRepeater's `deletables`. If a future edit breaks that check's rendering, the log
// stops answering the one question it is written to answer ("is this the same bug?"), and
// this probe is the only thing standing between that edit and a lost diagnosis.
//
// The formatter is pure and Qt-free by design (see CrashReport.h), so this links against
// nothing and runs on any platform — including CI's Linux runner, where the Windows half of
// CrashReport.cpp compiles to nothing.
//
// Prints CRASHREPORT-OK on success; any failure prints CRASHREPORT-FAIL <cond> and exits non-zero.
#include "CrashReport.h"

#include <cstdio>
#include <cstring>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "CRASHREPORT-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

// A CHECK that prints the rendering when it fails — a "text does not contain X" failure is
// useless without seeing the text.
#define CHECK_HAS(buf, needle) do { \
    if (std::strstr((buf), (needle)) == nullptr) { \
        std::fprintf(stderr, "CRASHREPORT-FAIL missing \"%s\" (line %d) in:\n%s\n", (needle), __LINE__, (buf)); \
        ++failures; \
    } \
} while (0)

#define CHECK_LACKS(buf, needle) do { \
    if (std::strstr((buf), (needle)) != nullptr) { \
        std::fprintf(stderr, "CRASHREPORT-FAIL unexpected \"%s\" (line %d) in:\n%s\n", (needle), __LINE__, (buf)); \
        ++failures; \
    } \
} while (0)

static void setName(char (&dst)[64], const char* src)
{
    std::size_t i = 0;
    for (; src[i] && i + 1 < sizeof(dst); ++i) dst[i] = src[i];
    dst[i] = '\0';
}

// The real #28 fault. Base chosen so faultAddress - moduleBase == 0x30f7a5 exactly, using the
// literal addresses out of the dump: 0x00007ffced04f7a5 with Qt6Quick.dll at 0x00007ffcecd40000.
static CrashReport::CrashRecord productionRecord()
{
    CrashReport::CrashRecord r = {};
    std::memcpy(r.timestamp, "2026-07-27 08:49:31", sizeof "2026-07-27 08:49:31");
    r.threadId      = 37932;
    r.sequence      = 1;
    r.limit         = CrashReport::kMaxRecords;
    r.exceptionCode = 0xc0000005ul;
    r.faultAddress  = 0x00007ffced04f7a5ull;
    setName(r.moduleName, "Qt6Quick.dll");
    r.moduleBase    = 0x00007ffcecd40000ull;        // 0x7ffced04f7a5 - 0x7ffcecd40000 == 0x30f7a5
    r.operation     = CrashReport::OpRead;
    r.badAddress    = 0x5ull;
    r.haveRegisters = true;
    r.rax = 0x1ull;                                 // the garbage QPointer `d`
    r.rbx = 0x4ull;                                 // the loop index i in QQuickRepeater::clear()
    r.rcx = 0x0000017a84600500ull;
    r.rdx = 0x0000004d8dafb398ull;
    r.rsi = 0x0000017adffdeed0ull;
    r.rdi = 0x40ull;                                // byte offset into deletables
    r.r8  = 0x0000017add7db830ull;
    r.r9  = 0x0ull;
    r.rip = 0x00007ffced04f7a5ull;
    r.rsp = 0x0000004d8dafb360ull;
    r.rbp = 0x0000017add7db830ull;
    return r;
}

int main()
{
    char buf[2048];

    // ---- 1. the real production fault renders every field the diagnosis needs ---------------
    {
        const CrashReport::CrashRecord r = productionRecord();
        const std::size_t n = CrashReport::formatRecord(r, buf, sizeof buf);
        CHECK(n > 0);
        CHECK(n == std::strlen(buf));

        // The module + offset. Without this the fault is an unsymbolisable raw address: Qt ships
        // no PDBs, so "Qt6Quick.dll+0x30f7a5" resolved against the export table is the ONLY thing
        // that named QQuickRepeater::clear().
        CHECK_HAS(buf, "Qt6Quick.dll+0x30f7a5");
        // The operation. READ vs WRITE decides whether the garbage pointer was dereferenced or
        // stored through — a completely different bug shape.
        CHECK_HAS(buf, "operation : READ");
        // The bad address.
        CHECK_HAS(buf, "bad addr  : 0x5");
        // The registers that encode the loop position inside deletables.
        CHECK_HAS(buf, "Rax=0x1");
        CHECK_HAS(buf, "Rbx=0x4");
        CHECK_HAS(buf, "Rdi=0x40");
        // The arithmetic that identifies the QPointer::data() strongref read at [d+4].
        CHECK_HAS(buf, "Rax+4=0x5 == bad addr  (MATCH)");
        // Context: which thread, when, and which of the capped records this is.
        CHECK_HAS(buf, "2026-07-27 08:49:31");
        CHECK_HAS(buf, "thread 37932");
        CHECK_HAS(buf, "record 1/5");
        CHECK_HAS(buf, "exception : 0xc0000005");
        CHECK_HAS(buf, "at        : 0x7ffced04f7a5");
        // The record must never carry memory contents — see CrashReport.h. Nothing here should
        // resemble a dump of bytes; the fields above are the entire vocabulary.
        CHECK_LACKS(buf, "(MISMATCH)");
    }

    // ---- 2. a fault that is NOT this bug must render as a mismatch --------------------------
    // The check has to be able to say "no". A formatter that always prints MATCH would pass
    // case 1 and would actively mislead the next person reading the log.
    {
        CrashReport::CrashRecord r = productionRecord();
        r.badAddress = 0x9ull;                      // Rax+4 == 0x5 != 0x9
        const std::size_t n = CrashReport::formatRecord(r, buf, sizeof buf);
        CHECK(n > 0);
        CHECK_HAS(buf, "Rax+4=0x5 != bad addr  (MISMATCH)");
        CHECK_HAS(buf, "bad addr  : 0x9");
        CHECK_LACKS(buf, "(MATCH)");                // "(MISMATCH)" does not contain "(MATCH)"
    }
    {
        // ... and the other direction: same bad address, different Rax.
        CrashReport::CrashRecord r = productionRecord();
        r.rax = 0x2ull;
        CrashReport::formatRecord(r, buf, sizeof buf);
        CHECK_HAS(buf, "Rax+4=0x6 != bad addr  (MISMATCH)");
        CHECK_LACKS(buf, "(MATCH)");
    }

    // ---- 3. the other access-violation operations ------------------------------------------
    {
        CrashReport::CrashRecord r = productionRecord();
        r.operation = CrashReport::OpWrite;
        CrashReport::formatRecord(r, buf, sizeof buf);
        CHECK_HAS(buf, "operation : WRITE");
        CHECK_LACKS(buf, "READ");

        r.operation = CrashReport::OpExecute;
        CrashReport::formatRecord(r, buf, sizeof buf);
        CHECK_HAS(buf, "operation : EXECUTE");

        r.operation = 7;                            // not a value Win32 documents
        CrashReport::formatRecord(r, buf, sizeof buf);
        CHECK_HAS(buf, "operation : UNKNOWN");
    }

    // ---- 4. an unresolved module must not fabricate an offset -------------------------------
    // moduleBase 0 means GetModuleHandleEx failed. "SomeName+0x7ffced04f7a5" would look like a
    // real symbol offset and send whoever reads it hunting in the wrong binary.
    {
        CrashReport::CrashRecord r = productionRecord();
        r.moduleBase = 0;
        r.moduleName[0] = '\0';
        CrashReport::formatRecord(r, buf, sizeof buf);
        CHECK_HAS(buf, "<unknown>");
        CHECK_LACKS(buf, "+0x");
        CHECK_HAS(buf, "0x7ffced04f7a5");            // the raw address is still there
    }

    // ---- 5. the rate-limit note appears on the LAST permitted record only -------------------
    // The cap is what stops a fault that repeats every frame from filling the disk; the note is
    // what stops a reader concluding "it only happened five times".
    {
        CrashReport::CrashRecord r = productionRecord();
        r.sequence = 4; r.limit = 5;
        CrashReport::formatRecord(r, buf, sizeof buf);
        CHECK_HAS(buf, "record 4/5");
        CHECK_LACKS(buf, "record cap reached");

        r.sequence = 5;
        CrashReport::formatRecord(r, buf, sizeof buf);
        CHECK_HAS(buf, "record 5/5");
        CHECK_HAS(buf, "record cap reached");
    }

    // ---- 6. no registers (non-x64) omits the register lines AND the check -------------------
    // Rendering "Rax=0x0 ... (MATCH)" off a zeroed context would be a fabricated diagnosis.
    {
        CrashReport::CrashRecord r = productionRecord();
        r.haveRegisters = false;
        CrashReport::formatRecord(r, buf, sizeof buf);
        CHECK_LACKS(buf, "Rax=");
        CHECK_LACKS(buf, "QPointer check");
        CHECK_HAS(buf, "Qt6Quick.dll+0x30f7a5");     // the rest still renders
        CHECK_HAS(buf, "bad addr  : 0x5");
    }

    // ---- 7. the fatal marker ----------------------------------------------------------------
    // First-chance means a handled, harmless AV can land in the log. Without this line there is
    // no way to tell "the app survived that" from "that is the crash".
    {
        const CrashReport::CrashRecord r = productionRecord();
        char line[512];
        const std::size_t n = CrashReport::formatFatalLine(r, line, sizeof line);
        CHECK(n > 0);
        CHECK(n == std::strlen(line));
        CHECK_HAS(line, "FATAL");
        CHECK_HAS(line, "NOT handled");
        CHECK_HAS(line, "Qt6Quick.dll+0x30f7a5");
        CHECK_HAS(line, "0xc0000005");
        CHECK_HAS(line, "thread 37932");
        CHECK(line[n - 1] == '\n');                  // it is one line, terminated
        CHECK(std::strchr(line, '\n') == line + n - 1);
    }

    // ---- 8. capacity: truncation must be safe at EVERY size --------------------------------
    // This runs inside a first-chance handler in a process that may already be corrupt. A
    // formatter that writes one byte past the end there does not produce a bad log line, it
    // produces a second, unrelated crash on top of the one we were trying to record — and the
    // report is lost exactly when it mattered.
    {
        const CrashReport::CrashRecord r = productionRecord();

        char full[2048];
        const std::size_t fullLen = CrashReport::formatRecord(r, full, sizeof full);
        CHECK(fullLen > 200);                        // sanity: the full rendering is substantial

        // cap == 0: nothing written at all, not even a NUL.
        {
            char guard[8];
            std::memset(guard, '\xAA', sizeof guard);
            const std::size_t n = CrashReport::formatRecord(r, guard, 0);
            CHECK(n == 0);
            for (std::size_t i = 0; i < sizeof guard; ++i) CHECK(guard[i] == '\xAA');
        }

        // Every cap from 1 up to just past the full length. The buffer is a canary field and the
        // region past `cap` must come back untouched; the prefix must match the full rendering.
        for (std::size_t cap = 1; cap <= fullLen + 2; ++cap)
        {
            char canary[2200];
            std::memset(canary, '\xAA', sizeof canary);
            const std::size_t n = CrashReport::formatRecord(r, canary, cap);

            CHECK(n < cap);                                   // never fills past the NUL slot
            CHECK(canary[n] == '\0');                         // always terminated
            CHECK(n == std::strlen(canary));
            CHECK(n == (cap - 1 < fullLen ? cap - 1 : fullLen));   // truncates to exactly what fits
            CHECK(std::memcmp(canary, full, n) == 0);         // prefix of the untruncated rendering

            bool pastEndIntact = true;
            for (std::size_t i = cap; i < sizeof canary; ++i)
                if (canary[i] != '\xAA') { pastEndIntact = false; break; }
            CHECK(pastEndIntact);                             // nothing written at or past out[cap]
            if (!pastEndIntact) break;                        // one report is enough; don't spam 1000 lines
        }

        // Same contract for the fatal line.
        char fullLine[512];
        const std::size_t lineLen = CrashReport::formatFatalLine(r, fullLine, sizeof fullLine);
        CHECK(lineLen > 40);
        for (std::size_t cap = 1; cap <= lineLen + 2; ++cap)
        {
            char canary[600];
            std::memset(canary, '\xAA', sizeof canary);
            const std::size_t n = CrashReport::formatFatalLine(r, canary, cap);
            CHECK(n < cap);
            CHECK(canary[n] == '\0');
            CHECK(n == (cap - 1 < lineLen ? cap - 1 : lineLen));
            CHECK(std::memcmp(canary, fullLine, n) == 0);
            bool intact = true;
            for (std::size_t i = cap; i < sizeof canary; ++i)
                if (canary[i] != '\xAA') { intact = false; break; }
            CHECK(intact);
            if (!intact) break;
        }
    }

    // ---- 9. install() is a no-op that cannot throw or crash on a bad path -------------------
    // Called once, early in main(), from a process that has nothing else set up yet.
    CrashReport::install(nullptr);
    CrashReport::install("");

    if (failures == 0) { std::puts("CRASHREPORT-OK"); return 0; }
    std::fprintf(stderr, "CRASHREPORT: %d check(s) failed\n", failures);
    return 1;
}
