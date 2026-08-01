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
// The SECOND production fault (EverythingBox.exe.50564.dmp, 2026-07-31) is case 2b, and it is
// here because the answer to that one question was WRONG for it:
//
//     Qt6Qml.dll+0x220ba, READ of 0x30, Rax=0
//
// which is `QQuickRepeater::clear()+0x127` — the d->model->release(item) call rather than the
// at(i) read — reaching QQmlData::get, which dereferences the object's own d_ptr. Same bug,
// same corrupted `deletables`; which site you land on is decided by what the recycled bytes
// happen to be. A formatter that renders only the Rax+4 check prints a bare "MISMATCH" for it
// and tells the next reader to file it elsewhere. So both checks render, the verdict is their
// OR, and the individual checks no longer carry a verdict word at all.
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

static void setNameN(char (&dst)[CrashReport::kFrameModuleCap], const char* src)
{
    std::size_t i = 0;
    for (; src[i] && i + 1 < sizeof(dst); ++i) dst[i] = src[i];
    dst[i] = '\0';
}

// The fault-site block as it reaches the log: formatRecord (everything that needs nothing
// resolved) immediately followed by formatFaultModule (the module + offset and the closing
// rule). They are two functions because a loader-lock hang between them must cost only the
// second — see emitRecord — but a reader sees one block, and so do the assertions below that
// are about the block's CONTENT rather than about the split.
static std::size_t renderFaultBlock(const CrashReport::CrashRecord& r, char* out, std::size_t cap)
{
    const std::size_t a = CrashReport::formatRecord(r, out, cap);
    const std::size_t b = CrashReport::formatFaultModule(r, out + a, cap - a);
    return a + b;
}

// ---------------------------------------------------------------------------------------
// Instrumentation for the emit-order assertions (case 12).
//
// emitRecord takes every enrichment step as a callback precisely so the ORDER it guarantees
// can be observed. These stand-ins record the exact text that had already been handed to the
// sink at the moment each step was entered, which is what makes "the registers were durable
// before the module was resolved" and "the raw addresses were durable before per-frame
// resolution was attempted" assertions rather than claims about a Windows-only handler CI
// never compiles.
// ---------------------------------------------------------------------------------------
struct Trace
{
    char        all[8192];
    std::size_t len       = 0;
    std::size_t maxChunk  = 0;

    // The accumulated log as it stood when each step was entered; -1 means never called.
    char        atModule[8192];
    char        atCapture[8192];
    char        atResolve[8192];
    int         moduleAt  = -1;      // bytes written when the fault-module step ran
    int         captureAt = -1;      // bytes written when capture ran
    int         resolveAt = -1;      // bytes written when resolve ran

    Trace() { all[0] = '\0'; atModule[0] = '\0'; atCapture[0] = '\0'; atResolve[0] = '\0'; }
};

static void traceWrite(void* ctx, const char* data, std::size_t len)
{
    Trace* t = static_cast<Trace*>(ctx);
    if (len > t->maxChunk) t->maxChunk = len;
    if (t->len + len + 1 >= sizeof t->all) { ++failures; return; }
    std::memcpy(t->all + t->len, data, len);
    t->len += len;
    t->all[t->len] = '\0';
}

static void traceResolveModule(void* ctx, CrashReport::CrashRecord& r)
{
    Trace* t = static_cast<Trace*>(ctx);
    t->moduleAt = static_cast<int>(t->len);
    std::memcpy(t->atModule, t->all, t->len + 1);

    setName(r.moduleName, "Qt6Quick.dll");
    r.moduleBase = 0x00007ffcecd40000ull;
}

static void traceCapture(void* ctx, CrashReport::CrashRecord& r)
{
    Trace* t = static_cast<Trace*>(ctx);
    t->captureAt = static_cast<int>(t->len);
    std::memcpy(t->atCapture, t->all, t->len + 1);

    r.frameCount = 3;
    r.frames[0] = 0xaa00ull;
    r.frames[1] = 0xbb00ull;
    r.frames[2] = 0xcc00ull;
}

static void traceResolve(void* ctx, CrashReport::CrashRecord& r)
{
    Trace* t = static_cast<Trace*>(ctx);
    t->resolveAt = static_cast<int>(t->len);
    std::memcpy(t->atResolve, t->all, t->len + 1);

    for (unsigned int i = 0; i < r.frameCount; ++i)
    {
        setNameN(r.frameModule[i], "probe.dll");
        r.frameBase[i] = 0xa000ull;
    }
    r.framesResolved = true;
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
        const std::size_t n = renderFaultBlock(r, buf, sizeof buf);
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
        CHECK_HAS(buf, "Rax+4=0x5 == bad addr  (hit: clear+0x95, at(i) strongref)");
        // The OTHER #28 fault site's check must render too, and must say it did not hit — this
        // dump is the at(i) one. Its absence is what made the 2026-07-31 +0x127 dump render as a
        // bare mismatch (see below); rendering it "no hit" here is what makes that meaningful.
        CHECK_HAS(buf, "null-base check: Rax=0x1 bad addr=0x5  (no hit)");
        // ... and the VERDICT, which is the only line a reader is meant to act on.
        CHECK_HAS(buf, "#28 signature : MATCH");
        // Context: which thread, when, and which of the capped records this is.
        CHECK_HAS(buf, "2026-07-27 08:49:31");
        CHECK_HAS(buf, "thread 37932");
        CHECK_HAS(buf, "record 1/5");
        CHECK_HAS(buf, "exception : 0xc0000005");
        CHECK_HAS(buf, "at        : 0x7ffced04f7a5");
        // The record must never carry memory contents — see CrashReport.h. Nothing here should
        // resemble a dump of bytes; the fields above are the entire vocabulary.
        CHECK_LACKS(buf, "MISMATCH");

        // The SPLIT, which is the write-first rule applied to the fault site itself. The first
        // block is what reaches the disk before the one GetModuleHandleExW call that names the
        // faulting module, so it must be structurally incapable of reading moduleName or
        // moduleBase: a fault taken under a loader lock that never releases has to cost that
        // one field, not the registers and the bad address as well.
        char first[2048];
        const std::size_t nf = CrashReport::formatRecord(r, first, sizeof first);
        CHECK(nf > 0);
        CHECK_HAS(first, "Rax+4=0x5 == bad addr  (hit");      // the evidence survives a hang
        CHECK_HAS(first, "#28 signature : MATCH");            // ... verdict included
        CHECK_HAS(first, "bad addr  : 0x5");
        CHECK_HAS(first, "at        : 0x7ffced04f7a5");        // ... and the raw address, which resolves offline
        CHECK_LACKS(first, "Qt6Quick.dll");                    // ... but nothing that needed the loader lock
        CHECK_LACKS(first, "+0x30f7a5");

        // ... and the module block is the rest of it, ending the record.
        char second[512];
        const std::size_t ns = CrashReport::formatFaultModule(r, second, sizeof second);
        CHECK(ns > 0);
        CHECK(ns == std::strlen(second));
        CHECK_HAS(second, "module    : Qt6Quick.dll+0x30f7a5");
        CHECK_HAS(second, "======================================\n");
        CHECK(nf + ns == n);
    }

    // ---- 2. a fault that is NOT this bug must render as a mismatch --------------------------
    // The check has to be able to say "no". A formatter that always prints MATCH would pass
    // case 1 and would actively mislead the next person reading the log.
    {
        CrashReport::CrashRecord r = productionRecord();
        r.badAddress = 0x9ull;                      // Rax+4 == 0x5 != 0x9, and Rax != 0
        const std::size_t n = CrashReport::formatRecord(r, buf, sizeof buf);
        CHECK(n > 0);
        CHECK_HAS(buf, "Rax+4=0x5 != bad addr  (no hit)");
        CHECK_HAS(buf, "null-base check: Rax=0x1 bad addr=0x9  (no hit)");
        CHECK_HAS(buf, "bad addr  : 0x9");
        CHECK_HAS(buf, "#28 signature : MISMATCH");
        CHECK_LACKS(buf, ": MATCH");                // the verdict is the mismatch one, not both
    }
    {
        // ... and the other direction: same bad address, different Rax.
        CrashReport::CrashRecord r = productionRecord();
        r.rax = 0x2ull;
        CrashReport::formatRecord(r, buf, sizeof buf);
        CHECK_HAS(buf, "Rax+4=0x6 != bad addr  (no hit)");
        CHECK_HAS(buf, "#28 signature : MISMATCH");
        CHECK_LACKS(buf, ": MATCH");
    }

    // ---- 2b. the SECOND production fault site — EverythingBox.exe.50564.dmp, 2026-07-31 ------
    // `clear()+0x127`, the d->model->release(item) call, which reaches QQmlData::get. That
    // dereferences the object's own d_ptr, so Rax == 0 and the read lands at a small fixed
    // offset (0x30, the QObjectData flags word). It is the SAME BUG as the +0x95 dump above —
    // one corrupted read of `deletables`, with the fault site decided by what the recycled
    // bytes happen to be — and the formatter that rendered only the Rax+4 check printed a bare
    // "MISMATCH" for it, i.e. told the next reader to stop assuming it was #28. That is the
    // regression this section exists to prevent, so it asserts the VERDICT, not just the line.
    {
        CrashReport::CrashRecord r = productionRecord();
        setName(r.moduleName, "Qt6Qml.dll");
        r.faultAddress = 0x00007ffcecd620baull;
        r.moduleBase   = 0x00007ffcecd40000ull;     // 0x…620ba - 0x…40000 == 0x220ba
        r.rip          = r.faultAddress;
        r.badAddress   = 0x30ull;                   // QObjectData flags, off a null d_ptr
        r.rax          = 0x0ull;                    // the null d_ptr itself
        r.rdi          = 0x30ull;
        const std::size_t n = renderFaultBlock(r, buf, sizeof buf);
        CHECK(n > 0);
        CHECK_HAS(buf, "Qt6Qml.dll+0x220ba");
        CHECK_HAS(buf, "bad addr  : 0x30");
        CHECK_HAS(buf, "Rax=0x0");
        // The at(i) check legitimately does not hit here (0x0 + 4 == 0x4, not 0x30) …
        CHECK_HAS(buf, "Rax+4=0x4 != bad addr  (no hit)");
        // … and the null-base check is the one that names it.
        CHECK_HAS(buf, "null-base check: Rax=0x0 bad addr=0x30  (hit: clear+0x127");
        CHECK_HAS(buf, "#28 signature : MATCH");
        CHECK_LACKS(buf, "MISMATCH");
    }
    {
        // The null-base check must be able to say no as well, in BOTH of its two terms.
        // Rax == 0 but the read is a long way off the null page: not this bug's shape (the
        // faulting instruction reads a fixed small offset off the pointer), so it must not hit.
        CrashReport::CrashRecord r = productionRecord();
        r.rax        = 0x0ull;
        r.badAddress = 0x8000ull;
        CrashReport::formatRecord(r, buf, sizeof buf);
        CHECK_HAS(buf, "null-base check: Rax=0x0 bad addr=0x8000  (no hit)");
        CHECK_HAS(buf, "#28 signature : MISMATCH");
        CHECK_LACKS(buf, ": MATCH");
    }
    {
        // ... and the boundary itself: 0xfff is inside the null page, 0x1000 is the first byte
        // outside it. Pinned because "< 0x1000" is the whole content of that half of the test.
        CrashReport::CrashRecord r = productionRecord();
        r.rax        = 0x0ull;
        r.badAddress = 0xfffull;
        CrashReport::formatRecord(r, buf, sizeof buf);
        CHECK_HAS(buf, "null-base check: Rax=0x0 bad addr=0xfff  (hit");
        CHECK_HAS(buf, "#28 signature : MATCH");

        r.badAddress = 0x1000ull;
        CrashReport::formatRecord(r, buf, sizeof buf);
        CHECK_HAS(buf, "null-base check: Rax=0x0 bad addr=0x1000  (no hit)");
        CHECK_HAS(buf, "#28 signature : MISMATCH");
    }
    {
        // The other term: a read inside the null page off a NON-null Rax is somebody else's
        // null-deref, not ours. Without this, "badAddress < 0x1000" alone would swallow a large
        // share of every unrelated null-pointer crash in the process and label it #28.
        CrashReport::CrashRecord r = productionRecord();
        r.rax        = 0x18ull;
        r.badAddress = 0x30ull;
        CrashReport::formatRecord(r, buf, sizeof buf);
        CHECK_HAS(buf, "null-base check: Rax=0x18 bad addr=0x30  (no hit)");
        CHECK_HAS(buf, "#28 signature : MISMATCH");
        CHECK_LACKS(buf, ": MATCH");
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
        renderFaultBlock(r, buf, sizeof buf);
        CHECK_HAS(buf, "<unknown>");
        CHECK_HAS(buf, "0x7ffced04f7a5");            // the raw address is still there
        CHECK_LACKS(buf, "Qt6Quick.dll");            // ... and the name it could not resolve is absent
        // The "never fabricate an offset" tripwire is scoped to the MODULE BLOCK rather than
        // swept over the whole record. It used to be a whole-record search for "+0x", which was
        // fine only while nothing else in a record could contain that substring; the two #28
        // signature lines now name their fault sites (clear+0x95 / clear+0x127), so a
        // whole-record sweep would be tripped by the annotations instead of by a fabricated
        // offset. The module line is where an offset off a zero base could actually be invented
        // — formatRecord is structurally forbidden from reading moduleName/moduleBase at all,
        // and case 1 pins that separately.
        char modOnly[512];
        CrashReport::formatFaultModule(r, modOnly, sizeof modOnly);
        CHECK_LACKS(modOnly, "+0x");

        // A name with no base, and a base with no name, are both "unresolved" — neither half
        // alone may produce an offset. (GetModuleHandleExW leaves both untouched on failure,
        // but a future edit that fills one of them from somewhere else must not slip through.)
        r.moduleBase = 0x00007ffcecd40000ull;
        r.moduleName[0] = '\0';
        CrashReport::formatFaultModule(r, buf, sizeof buf);
        CHECK_HAS(buf, "<unknown>");
        CHECK_LACKS(buf, "+0x");

        setName(r.moduleName, "Qt6Quick.dll");
        r.moduleBase = 0;
        CrashReport::formatFaultModule(r, buf, sizeof buf);
        CHECK_HAS(buf, "<unknown>");
        CHECK_LACKS(buf, "+0x");
        CHECK_LACKS(buf, "Qt6Quick.dll");
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

    // ---- 6. no registers (non-x64) omits the register lines AND both checks ------------------
    // Rendering a verdict off a zeroed context would be a fabricated diagnosis, and the
    // null-base check makes that trap sharper rather than softer: an absent register block is
    // ZEROED, so Rax reads as 0 — which is half of that check's hit condition. Suppressing the
    // whole block is what stops "no registers" from silently becoming "MATCH".
    {
        CrashReport::CrashRecord r = productionRecord();
        r.haveRegisters = false;
        renderFaultBlock(r, buf, sizeof buf);
        CHECK_LACKS(buf, "Rax=");
        CHECK_LACKS(buf, "QPointer check");
        CHECK_LACKS(buf, "null-base check");
        CHECK_LACKS(buf, "#28 signature");
        CHECK_HAS(buf, "Qt6Quick.dll+0x30f7a5");     // the rest still renders
        CHECK_HAS(buf, "bad addr  : 0x5");
    }
    {
        // The zeroed-context case explicitly: registers absent, Rax therefore 0, and a bad
        // address inside the null page. Every ingredient of a null-base hit is present except
        // the one thing that would make it evidence — a context that was actually captured.
        CrashReport::CrashRecord r = productionRecord();
        r.haveRegisters = false;
        r.rax        = 0x0ull;
        r.badAddress = 0x30ull;
        renderFaultBlock(r, buf, sizeof buf);
        CHECK_LACKS(buf, "null-base check");
        CHECK_LACKS(buf, "#28 signature");
        CHECK_LACKS(buf, "MATCH");                   // covers MISMATCH too: no verdict at all
        CHECK_HAS(buf, "bad addr  : 0x30");          // the raw fact still renders
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

        // Same contract for the module block. It is written from the same scratch buffer, in
        // the same handler, in the same corrupt process.
        char fullMod[512];
        const std::size_t modLen = CrashReport::formatFaultModule(r, fullMod, sizeof fullMod);
        CHECK(modLen > 20);
        for (std::size_t cap = 1; cap <= modLen + 2; ++cap)
        {
            char canary[600];
            std::memset(canary, '\xAA', sizeof canary);
            const std::size_t n = CrashReport::formatFaultModule(r, canary, cap);
            CHECK(n < cap);
            CHECK(canary[n] == '\0');
            CHECK(n == (cap - 1 < modLen ? cap - 1 : modLen));
            CHECK(std::memcmp(canary, fullMod, n) == 0);
            bool intact = true;
            for (std::size_t i = cap; i < sizeof canary; ++i)
                if (canary[i] != '\xAA') { intact = false; break; }
            CHECK(intact);
            if (!intact) break;
        }
        {
            char guard[8];
            std::memset(guard, '\xAA', sizeof guard);
            CHECK(CrashReport::formatFaultModule(r, guard, 0) == 0);
            for (std::size_t i = 0; i < sizeof guard; ++i) CHECK(guard[i] == '\xAA');
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

    // ---- 9. the cap must never eat the crash this exists to catch ---------------------------
    // The cap is 5 records per process, and benign HANDLED access violations are real — GPU
    // drivers and some libraries raise them as control flow. If five of those land first, the
    // fatal fault arrives at an exhausted cap, and the one-line FATAL marker keeps module,
    // offset and code but loses the bad address and the REGISTERS: precisely the `Rax+4 == bad
    // addr` evidence this whole feature exists to capture. So the fatal path is exempt.
    //
    // Distinguishing "already written" from "still owed" cannot be done on the fault address
    // alone, because a cap is exhausted exactly by a fault that repeats at ONE address. It
    // takes the address AND the sequence counter.
    {
        const unsigned long long addrA = 0x00007ffced04f7a5ull;
        const unsigned long long addrB = 0x00007ffcecd41111ull;

        // Normal case: the first-chance handler wrote record 3 for this address, and this is
        // fault 3. Already on disk — the fatal path must NOT duplicate the block.
        CHECK(CrashReport::fatalNeedsFullRecord(addrA, 3, 3, addrA) == false);

        // The case this fix exists for. Five benign AVs spent the cap, the sixth is fatal, and
        // it is at the SAME address as the fifth — which is what a repeating fault looks like,
        // so the address alone would wrongly report "already recorded" and lose the registers.
        // The sequence counters diverging (5 written, 6 seen) is what catches it.
        CHECK(CrashReport::fatalNeedsFullRecord(addrA, 5, 6, addrA) == true);
        // ... and far past the cap, still owed.
        CHECK(CrashReport::fatalNeedsFullRecord(addrA, 5, 91, addrA) == true);

        // A fatal fault at a different address than the last record, counters aligned: a
        // record is still owed, because the one on disk describes some other fault.
        CHECK(CrashReport::fatalNeedsFullRecord(addrA, 3, 3, addrB) == true);

        // Nothing has ever been written (sequence 0). Always owed — including when the fatal
        // fault address is 0, which must not match the zeroed initial state.
        CHECK(CrashReport::fatalNeedsFullRecord(0, 0, 0, 0) == true);
        CHECK(CrashReport::fatalNeedsFullRecord(0, 0, 4, addrA) == true);

        // The cap still bounds benign noise: the predicate exempts the fatal path only, and
        // says nothing about first-chance records, which is why the note in case 5 is
        // unchanged. Records 1..5 under the cap are still "already recorded" when aligned.
        for (unsigned int i = 1; i <= CrashReport::kMaxRecords; ++i)
            CHECK(CrashReport::fatalNeedsFullRecord(addrA, i, i, addrA) == false);
    }

    // ---- 10. a fatal record renders as a full record, past the cap, WITH registers ----------
    // The failure this guards is silent: a "record 6/5" that also carries the cap note would
    // read as an arithmetic bug, and a fatal record that dropped the register block would be
    // the exact loss the exemption was added to prevent.
    {
        CrashReport::CrashRecord r = productionRecord();
        r.fatal    = true;
        r.sequence = 6;                 // sixth fault seen ...
        r.limit    = 5;                 // ... against a cap of five
        CrashReport::formatRecord(r, buf, sizeof buf);

        CHECK_HAS(buf, "record 6/5");
        CHECK_HAS(buf, "(FATAL - exempt from the cap)");
        // The evidence that must survive the cap. If any of these three go missing the
        // exemption bought nothing.
        CHECK_HAS(buf, "bad addr  : 0x5");
        CHECK_HAS(buf, "Rax=0x1");
        CHECK_HAS(buf, "Rax+4=0x5 == bad addr  (hit");
        CHECK_HAS(buf, "#28 signature : MATCH");
        // "further access violations will NOT be logged this run" is noise on the last fault
        // the process will ever take, and it contradicts the line that was just written.
        CHECK_LACKS(buf, "record cap reached");
    }
    {
        // A non-fatal record at the cap is unchanged: the note stays, the FATAL tag does not
        // appear. Without this the fatal branch could simply suppress the note for everyone.
        CrashReport::CrashRecord r = productionRecord();
        r.sequence = 5; r.limit = 5;
        CrashReport::formatRecord(r, buf, sizeof buf);
        CHECK_HAS(buf, "record cap reached");
        CHECK_LACKS(buf, "exempt from the cap");
    }

    // ---- 11. the call chain: return addresses only, and no fabricated symbols ---------------
    // The #28 root cause came from the STACK, not the fault site. `QQuickItem::setParentItem
    // -> QQuickRepeater::itemChange -> regenerate -> clear` was recovered from return-address
    // candidates; `Qt6Quick.dll+0x30f7a5` alone names one function and says nothing about who
    // was tearing the vector down. A record without the chain leaves the next occurrence
    // exactly where the first one left us.
    {
        CrashReport::CrashRecord r = productionRecord();
        r.frameCount = 4;
        r.frames[0] = 0x00007ffced04f7a5ull;   // QQuickRepeater::clear
        r.frames[1] = 0x00007ffced04f100ull;   // regenerate
        r.frames[2] = 0x00007ffced04ea20ull;   // itemChange
        r.frames[3] = 0x00007ffced012340ull;   // setParentItem

        // The raw block. This is the one that has to be on disk before resolution is
        // attempted, so it must render from frames[] alone.
        const std::size_t n = CrashReport::formatFramesRaw(r, buf, sizeof buf);
        CHECK(n > 0);
        CHECK(n == std::strlen(buf));
        CHECK_HAS(buf, "call chain (raw return addresses, 4 frames");
        CHECK_HAS(buf, "#00 0x7ffced04f7a5");
        CHECK_HAS(buf, "#01 0x7ffced04f100");
        CHECK_HAS(buf, "#02 0x7ffced04ea20");
        CHECK_HAS(buf, "#03 0x7ffced012340");

        // Populate the resolved fields and re-render the RAW block. It must be byte-identical:
        // if the raw block could ever read a resolved field, the write-first ordering would be
        // buying nothing, because the block flushed before resolution would depend on it.
        char rawBefore[2048];
        std::memcpy(rawBefore, buf, n + 1);
        r.framesResolved = true;
        for (unsigned int i = 0; i < 4; ++i)
        {
            setNameN(r.frameModule[i], "Qt6Quick.dll");
            r.frameBase[i] = 0x00007ffcecd40000ull;
        }
        const std::size_t n2 = CrashReport::formatFramesRaw(r, buf, sizeof buf);
        CHECK(n2 == n);
        CHECK(std::memcmp(buf, rawBefore, n) == 0);
        CHECK_LACKS(buf, "Qt6Quick.dll");

        // The enriched block. Offsets computed off each frame's own base — this is what turns
        // an address list back into the call chain, since Qt ships no PDBs and the export
        // table is the only symbol source.
        const std::size_t n3 = CrashReport::formatFramesResolved(r, buf, sizeof buf);
        CHECK(n3 > 0);
        CHECK(n3 == std::strlen(buf));
        CHECK_HAS(buf, "call chain (resolved)");
        CHECK_HAS(buf, "#00 0x7ffced04f7a5  Qt6Quick.dll+0x30f7a5");
        CHECK_HAS(buf, "#01 0x7ffced04f100  Qt6Quick.dll+0x30f100");
        CHECK_HAS(buf, "#03 0x7ffced012340  Qt6Quick.dll+0x2d2340");

        // A frame whose module did not resolve keeps its bare address. Same rule as the fault
        // site: "+0x7ffced04ea20" off a zero base looks like a symbol offset and sends whoever
        // reads it hunting in the wrong binary.
        r.frameBase[2] = 0;
        r.frameModule[2][0] = '\0';
        CrashReport::formatFramesResolved(r, buf, sizeof buf);
        CHECK_HAS(buf, "#02 0x7ffced04ea20\n");
        CHECK_HAS(buf, "#01 0x7ffced04f100  Qt6Quick.dll+0x30f100");   // its neighbours still resolve
    }
    {
        // No frames captured at all: both blocks render nothing rather than an empty heading.
        // "call chain (resolved):" with no frames under it claims the chain resolved to
        // nothing, which is a different and false statement from "no chain was captured".
        CrashReport::CrashRecord r = productionRecord();
        r.frameCount = 0;
        r.framesResolved = true;
        CHECK(CrashReport::formatFramesRaw(r, buf, sizeof buf) == 0);
        CHECK(CrashReport::formatFramesResolved(r, buf, sizeof buf) == 0);

        // Frames captured but resolution never completed — which is what a loader-lock
        // deadlock in the resolve step looks like from the log's side. The raw block still
        // renders; the enriched one does not.
        r.frameCount = 2;
        r.frames[0] = 0x1000ull; r.frames[1] = 0x2000ull;
        r.framesResolved = false;
        CHECK(CrashReport::formatFramesRaw(r, buf, sizeof buf) > 0);
        CHECK(CrashReport::formatFramesResolved(r, buf, sizeof buf) == 0);
    }
    {
        // A full 32-frame chain, and the frame formatters honour the same buffer contract as
        // everything else — they run in the same handler, in the same corrupt process.
        CrashReport::CrashRecord r = productionRecord();
        r.frameCount = CrashReport::kMaxFrames;
        r.framesResolved = true;
        for (unsigned int i = 0; i < CrashReport::kMaxFrames; ++i)
        {
            r.frames[i] = 0x00007ffced040000ull + i * 0x100ull;
            setNameN(r.frameModule[i], "Qt6Quick.dll");
            r.frameBase[i] = 0x00007ffcecd40000ull;
        }
        char fullRaw[4096];
        const std::size_t rawLen = CrashReport::formatFramesRaw(r, fullRaw, sizeof fullRaw);
        char fullRes[4096];
        const std::size_t resLen = CrashReport::formatFramesResolved(r, fullRes, sizeof fullRes);
        CHECK(rawLen > 32 * 10);
        CHECK(resLen > 32 * 20);
        CHECK_HAS(fullRaw, "#31 ");
        CHECK_HAS(fullRes, "#31 ");
        // The handler's scratch buffer must actually hold the biggest of these. If this ever
        // fails, a real crash loses the tail of the chain and nothing says so.
        //
        // Asserted against CrashReport::kScratch, which IS the handler's buffer, not against a
        // copy of the number: with 2560 written out here, shrinking the handler's buffer would
        // leave this green while a real crash silently truncated the chain. The full sweep is
        // over every block the handler emits, since they all share that one buffer.
        CHECK(resLen < CrashReport::kScratch);
        CHECK(rawLen < CrashReport::kScratch);
        {
            char big[CrashReport::kScratch * 2];
            CrashReport::CrashRecord worst = r;
            worst.fatal = false;
            worst.sequence = worst.limit;              // the longest fault-site block: cap note included
            CHECK(CrashReport::formatRecord(worst, big, sizeof big) < CrashReport::kScratch);
            CHECK(CrashReport::formatFaultModule(worst, big, sizeof big) < CrashReport::kScratch);
            CHECK(CrashReport::formatFatalLine(worst, big, sizeof big) < CrashReport::kScratch);
        }

        for (std::size_t cap = 1; cap <= 64; ++cap)
        {
            char canary[128];
            std::memset(canary, '\xAA', sizeof canary);
            const std::size_t k = CrashReport::formatFramesRaw(r, canary, cap);
            CHECK(k < cap);
            CHECK(canary[k] == '\0');
            CHECK(k == (cap - 1 < rawLen ? cap - 1 : rawLen));
            CHECK(std::memcmp(canary, fullRaw, k) == 0);
            bool intact = true;
            for (std::size_t i = cap; i < sizeof canary; ++i)
                if (canary[i] != '\xAA') { intact = false; break; }
            CHECK(intact);

            std::memset(canary, '\xAA', sizeof canary);
            const std::size_t m = CrashReport::formatFramesResolved(r, canary, cap);
            CHECK(m < cap);
            CHECK(canary[m] == '\0');
            CHECK(m == (cap - 1 < resLen ? cap - 1 : resLen));
            CHECK(std::memcmp(canary, fullRes, m) == 0);
            intact = true;
            for (std::size_t i = cap; i < sizeof canary; ++i)
                if (canary[i] != '\xAA') { intact = false; break; }
            CHECK(intact);
            if (!intact) break;
        }

        // cap == 0 writes nothing at all, not even a NUL.
        char guard[8];
        std::memset(guard, '\xAA', sizeof guard);
        CHECK(CrashReport::formatFramesRaw(r, guard, 0) == 0);
        CHECK(CrashReport::formatFramesResolved(r, guard, 0) == 0);
        for (std::size_t i = 0; i < sizeof guard; ++i) CHECK(guard[i] == '\xAA');
    }

    // ---- 12. the emit ORDER — the write-first safeguard ------------------------------------
    // GetModuleHandleExW takes the loader lock. If the process faulted WHILE that lock was
    // held — mid DLL load, in a static initialiser, in a TLS callback — resolution deadlocks
    // and the handler never returns. So nothing may be undelivered when a resolution step
    // starts: the fault site (registers, bad address, raw fault address) must be on disk
    // before the FAULTING MODULE is even looked up, and the raw return addresses must be on
    // disk before per-frame resolution is attempted. Raw addresses resolve offline against a
    // module list, which is how the production dump was read; a deadlock then costs the
    // enriched rendering and nothing else.
    //
    // That order is the contract, so it is asserted here rather than trusted to a comment on a
    // Windows-only handler that CI never compiles.
    {
        Trace t;
        CrashReport::CrashRecord r = productionRecord();
        // Nothing resolved yet — exactly the state the handler builds, since fillCommon no
        // longer touches the module. traceResolveModule is what fills these in.
        r.moduleName[0] = '\0';
        r.moduleBase    = 0;

        char scratch[CrashReport::kScratch];
        CrashReport::emitRecord(r, traceWrite, traceResolveModule, traceCapture, traceResolve,
                                &t, scratch, sizeof scratch);

        // All three steps ran, in order, and none ran before anything was written.
        CHECK(t.moduleAt >= 0);
        CHECK(t.captureAt >= 0);
        CHECK(t.resolveAt >= 0);
        CHECK(t.moduleAt < t.captureAt);
        CHECK(t.captureAt < t.resolveAt);

        // The fault site was durable BEFORE the loader lock was touched at all — this is the
        // step that used to run first, before a single byte had been written, so that a fault
        // under an unreleasable loader lock recorded NOTHING. Not merely "some bytes": the
        // registers, the bad address and the raw fault address specifically, because those are
        // what the hang would have cost.
        CHECK(t.moduleAt > 0);
        CHECK_HAS(t.atModule, "bad addr  : 0x5");
        CHECK_HAS(t.atModule, "Rax+4=0x5 == bad addr  (hit");
        CHECK_HAS(t.atModule, "#28 signature : MATCH");
        CHECK_HAS(t.atModule, "at        : 0x7ffced04f7a5");
        // ... and none of it depended on the module, which had not been resolved yet.
        CHECK_LACKS(t.atModule, "Qt6Quick.dll");
        CHECK_LACKS(t.atModule, "module    :");

        // The fault site was durable BEFORE the stack was walked, module included by then.
        CHECK(t.captureAt > 0);
        CHECK_HAS(t.atCapture, "bad addr  : 0x5");
        CHECK_HAS(t.atCapture, "Rax+4=0x5 == bad addr  (hit");
        CHECK_HAS(t.atCapture, "#28 signature : MATCH");
        CHECK_HAS(t.atCapture, "module    : Qt6Quick.dll+0x30f7a5");

        // Every captured return address was durable BEFORE resolution was attempted, and none
        // of what was written by then depended on a resolved field.
        CHECK_HAS(t.atResolve, "call chain (raw return addresses");
        CHECK_HAS(t.atResolve, "#00 0xaa00");
        CHECK_HAS(t.atResolve, "#01 0xbb00");
        CHECK_HAS(t.atResolve, "#02 0xcc00");
        CHECK_LACKS(t.atResolve, "call chain (resolved)");
        CHECK_LACKS(t.atResolve, "probe.dll");

        // And the finished log has the enriched chain last.
        CHECK_HAS(t.all, "call chain (resolved)");
        CHECK_HAS(t.all, "#00 0xaa00  probe.dll+0xa00");
        // Ordering in the final text, not just in the callbacks.
        CHECK(std::strstr(t.all, "bad addr  : 0x5")
              < std::strstr(t.all, "module    : Qt6Quick.dll+0x30f7a5"));
        CHECK(std::strstr(t.all, "module    : Qt6Quick.dll+0x30f7a5")
              < std::strstr(t.all, "call chain (raw return addresses"));
        CHECK(std::strstr(t.all, "call chain (raw return addresses")
              < std::strstr(t.all, "call chain (resolved)"));

        // Every chunk fitted the scratch buffer: a truncated chunk would mean the on-disk
        // record silently loses its tail in exactly the situation it is written for.
        CHECK(t.maxChunk > 0);
        CHECK(t.maxChunk < sizeof scratch - 1);
    }
    {
        // Null steps are skipped, not crashed on, and a record with no capture step still
        // emits the fault site — the reporter degrades to its previous behaviour rather than
        // to nothing.
        Trace t;
        CrashReport::CrashRecord r = productionRecord();
        char scratch[CrashReport::kScratch];
        CrashReport::emitRecord(r, traceWrite, nullptr, nullptr, nullptr, &t, scratch, sizeof scratch);
        CHECK(t.moduleAt == -1);
        CHECK(t.captureAt == -1);
        CHECK(t.resolveAt == -1);
        CHECK_HAS(t.all, "bad addr  : 0x5");
        CHECK_HAS(t.all, "Rax=0x1");
        CHECK_LACKS(t.all, "call chain");
        // The block still closes even with every step skipped — the record does not end
        // mid-way just because nothing could be enriched.
        CHECK_HAS(t.all, "======================================\n");

        // A null sink, a null buffer and a zero buffer are all no-ops rather than faults. This
        // runs in a process that is already broken; there is no configuration in which it may
        // add a second crash on top of the one it is recording.
        CrashReport::emitRecord(r, nullptr, traceResolveModule, traceCapture, traceResolve, &t, scratch, sizeof scratch);
        CrashReport::emitRecord(r, traceWrite, traceResolveModule, traceCapture, traceResolve, &t, nullptr, 16);
        CrashReport::emitRecord(r, traceWrite, traceResolveModule, traceCapture, traceResolve, &t, scratch, 0);
    }

    // ---- 13. a stack overflow must reach the previous filter, not our record path ------------
    // SetUnhandledExceptionFilter runs for EVERY unhandled exception, and a stack overflow is
    // the one the handler cannot afford. When the guard page goes there is roughly one page of
    // stack left and exception dispatch has already spent part of it; a CrashRecord is ~1.7 KB
    // and the scratch buffer 2.5 KB more, and a prologue commits that frame — via __chkstk,
    // which PROBES it page by page — before any branch is taken. A filter that touched it
    // would fault inside itself, the OS would kill the process on the spot, g_prevFilter (WER)
    // would never be called, and a stack overflow that used to produce a WER dump would
    // produce nothing at all. Adding a crash reporter must not delete the crash report that
    // already worked.
    //
    // The DECISION is pure and lives here. The other half of the fix — every large local moved
    // into a __declspec(noinline) helper, so the filter's own prologue commits nothing — is
    // codegen, which no portable probe can observe; run-headless-probes.sh gates its SHAPE
    // instead ("crashreport handler discipline"), because CI never compiles that half.
    {
        // The one code that skips everything.
        CHECK(CrashReport::faultMustSkipRecording(CrashReport::kExceptionStackOverflow) == true);
        CHECK(CrashReport::faultMustSkipRecording(0xC00000FDul) == true);   // the literal, spelled out

        // And nothing else does. An over-eager predicate is the silent failure here: it would
        // disarm the reporter for whole classes of crash and no test of the FORMATTER would
        // notice, because the formatter is never reached.
        CHECK(CrashReport::faultMustSkipRecording(CrashReport::kExceptionAccessViolation) == false);
        CHECK(CrashReport::faultMustSkipRecording(0xC0000005ul) == false);  // access violation
        CHECK(CrashReport::faultMustSkipRecording(0xC000001Dul) == false);  // illegal instruction
        CHECK(CrashReport::faultMustSkipRecording(0xC0000094ul) == false);  // integer divide by zero
        CHECK(CrashReport::faultMustSkipRecording(0xC0000374ul) == false);  // heap corruption
        CHECK(CrashReport::faultMustSkipRecording(0xE06D7363ul) == false);  // an uncaught C++ throw
        CHECK(CrashReport::faultMustSkipRecording(0x80000003ul) == false);  // breakpoint
        CHECK(CrashReport::faultMustSkipRecording(0ul) == false);

        // The vectored handler filters on the access-violation code alone rather than calling
        // this, so the two must never be able to disagree: if AV were ever skippable, the
        // first-chance path would still walk into the big frame.
        CHECK(CrashReport::faultMustSkipRecording(CrashReport::kExceptionAccessViolation) == false);

        // The portable spellings must be the real NTSTATUS values. CrashReport.cpp
        // static_asserts them against EXCEPTION_ACCESS_VIOLATION / EXCEPTION_STACK_OVERFLOW
        // inside its _WIN32 half; this is the same pin on the platforms where those macros do
        // not exist, so a typo cannot pass CI and then skip the wrong exception on Windows.
        CHECK(CrashReport::kExceptionAccessViolation == 0xC0000005ul);
        CHECK(CrashReport::kExceptionStackOverflow   == 0xC00000FDul);
    }

    // ---- 14. install() is a no-op that cannot throw or crash on a bad path -------------------
    // Called once, early in main(), from a process that has nothing else set up yet.
    CrashReport::install(nullptr);
    CrashReport::install("");

    if (failures == 0) { std::puts("CRASHREPORT-OK"); return 0; }
    std::fprintf(stderr, "CRASHREPORT: %d check(s) failed\n", failures);
    return 1;
}
