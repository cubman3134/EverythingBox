// symbolize_dump — resolve a Windows minidump's faulting-thread stack to functions + lines, using PDBs on a
// search path. Built because Release now ships a matching MyMediaVault.pdb but there's no cdb/WinDbg on the box;
// this gives a symbolized call stack from the command line without opening Visual Studio.
//
//   symbolize_dump <crash.dmp> [symbol_search_path]
//
// The search path (default ".") should contain MyMediaVault.exe + MyMediaVault.pdb (e.g. C:\MyMediaVault-app) so
// DbgHelp can match the module to its PDB by GUID. Qt/system DLLs have no PDB, so they resolve to module+offset —
// still enough to pair with the WER fault offset. Windows-only, console, links dbghelp.
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include <cstring>
#include <string>

#pragma comment(lib, "dbghelp.lib")

// The mapped dump + its saved memory ranges, for the StackWalk read callback.
static BYTE*                     g_base   = nullptr; // start of the mapped .dmp
static MINIDUMP_MEMORY64_LIST*   g_mem64  = nullptr;
static MINIDUMP_MEMORY_LIST*     g_mem32  = nullptr;

// ReadProcessMemory routine for StackWalk64: serve reads from the dump's saved memory, not a live process.
static BOOL CALLBACK readDumpMemory(HANDLE, DWORD64 addr, PVOID buf, DWORD size, LPDWORD bytesRead)
{
    if (g_mem64)
    {
        ULONG64 rva = g_mem64->BaseRva;
        for (ULONG64 i = 0; i < g_mem64->NumberOfMemoryRanges; ++i)
        {
            const MINIDUMP_MEMORY_DESCRIPTOR64& d = g_mem64->MemoryRanges[i];
            if (addr >= d.StartOfMemoryRange && addr < d.StartOfMemoryRange + d.DataSize)
            {
                const ULONG64 off = addr - d.StartOfMemoryRange;
                const DWORD n = (DWORD)min((ULONG64)size, d.DataSize - off);
                memcpy(buf, g_base + rva + off, n);
                if (bytesRead) *bytesRead = n;
                return TRUE;
            }
            rva += d.DataSize;
        }
    }
    if (g_mem32)
    {
        for (ULONG i = 0; i < g_mem32->NumberOfMemoryRanges; ++i)
        {
            const MINIDUMP_MEMORY_DESCRIPTOR& d = g_mem32->MemoryRanges[i];
            const ULONG64 start = d.StartOfMemoryRange, sz = d.Memory.DataSize;
            if (addr >= start && addr < start + sz)
            {
                const ULONG64 off = addr - start;
                const DWORD n = (DWORD)min((ULONG64)size, sz - off);
                memcpy(buf, g_base + d.Memory.Rva + off, n);
                if (bytesRead) *bytesRead = n;
                return TRUE;
            }
        }
    }
    if (bytesRead) *bytesRead = 0;
    return FALSE;
}

static std::wstring readMiniString(BYTE* base, RVA rva)
{
    if (!rva) return L"";
    auto* s = (MINIDUMP_STRING*)(base + rva);
    return std::wstring(s->Buffer, s->Length / sizeof(WCHAR));
}

int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: symbolize_dump <crash.dmp> [symbol_search_path]\n"); return 2; }
    const char* dumpPath = argv[1];
    const char* symPath  = argc >= 3 ? argv[2] : ".";

    HANDLE hFile = CreateFileA(dumpPath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) { fprintf(stderr, "can't open dump: %s\n", dumpPath); return 1; }
    HANDLE hMap = CreateFileMappingA(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
    g_base = hMap ? (BYTE*)MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0) : nullptr;
    if (!g_base) { fprintf(stderr, "can't map dump\n"); return 1; }

    // Streams we need: the exception (crash context), the module list (to load symbols), and saved memory.
    MINIDUMP_EXCEPTION_STREAM* exc = nullptr; ULONG excSize = 0;
    MINIDUMP_MODULE_LIST*      mods = nullptr; ULONG modSize = 0;
    ULONG sz = 0; void* p = nullptr;
    if (MiniDumpReadDumpStream(g_base, ExceptionStream, nullptr, &p, &sz))    exc  = (MINIDUMP_EXCEPTION_STREAM*)p;
    if (MiniDumpReadDumpStream(g_base, ModuleListStream, nullptr, &p, &sz))   mods = (MINIDUMP_MODULE_LIST*)p;
    if (MiniDumpReadDumpStream(g_base, Memory64ListStream, nullptr, &p, &sz)) g_mem64 = (MINIDUMP_MEMORY64_LIST*)p;
    if (MiniDumpReadDumpStream(g_base, MemoryListStream, nullptr, &p, &sz))   g_mem32 = (MINIDUMP_MEMORY_LIST*)p;

    HANDLE proc = GetCurrentProcess();
    // EXACT_SYMBOLS: only use a PDB whose GUID matches the crashed module, so we never print plausible-but-wrong
    // lines from a PDB of a different build. The deployed MyMediaVault.pdb matches the deployed exe by design.
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_EXACT_SYMBOLS);
    if (!SymInitialize(proc, symPath, FALSE)) { fprintf(stderr, "SymInitialize failed\n"); return 1; }

    if (mods)
        for (ULONG i = 0; i < mods->NumberOfModules; ++i)
        {
            const MINIDUMP_MODULE& m = mods->Modules[i];
            const std::wstring name = readMiniString(g_base, m.ModuleNameRva);
            SymLoadModuleExW(proc, nullptr, name.empty() ? nullptr : name.c_str(), nullptr,
                             m.BaseOfImage, (DWORD)m.SizeOfImage, nullptr, 0);
        }

    if (!exc) { fprintf(stderr, "no exception stream (not a crash dump?)\n"); return 1; }
    const MINIDUMP_EXCEPTION& er = exc->ExceptionRecord;
    printf("Exception 0x%08X at 0x%016llX\n\n", (unsigned)er.ExceptionCode, (unsigned long long)er.ExceptionAddress);

    // The crash context lives in the exception stream. x64 only (this app is 64-bit).
    CONTEXT ctx = *(CONTEXT*)(g_base + exc->ThreadContext.Rva);
    STACKFRAME64 sf = {};
    sf.AddrPC.Offset = ctx.Rip;    sf.AddrPC.Mode = AddrModeFlat;
    sf.AddrFrame.Offset = ctx.Rbp; sf.AddrFrame.Mode = AddrModeFlat;
    sf.AddrStack.Offset = ctx.Rsp; sf.AddrStack.Mode = AddrModeFlat;

    printf("Faulting thread call stack:\n");
    for (int frame = 0; frame < 64; ++frame)
    {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, GetCurrentThread(), &sf, &ctx,
                         readDumpMemory, SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
            break;
        if (sf.AddrPC.Offset == 0) break;

        const DWORD64 addr = sf.AddrPC.Offset;
        // Module name.
        IMAGEHLP_MODULE64 mi = { sizeof(mi) }; char mod[64] = "?";
        if (SymGetModuleInfo64(proc, addr, &mi)) strncpy(mod, mi.ModuleName, sizeof(mod) - 1);

        // Function + displacement.
        char symBuf[sizeof(SYMBOL_INFO) + 512] = {};
        auto* sym = (SYMBOL_INFO*)symBuf; sym->SizeOfStruct = sizeof(SYMBOL_INFO); sym->MaxNameLen = 511;
        DWORD64 disp = 0;
        // File:line.
        IMAGEHLP_LINE64 line = { sizeof(line) }; DWORD lineDisp = 0;
        if (SymFromAddr(proc, addr, &disp, sym))
        {
            if (SymGetLineFromAddr64(proc, addr, &lineDisp, &line))
                printf("  #%02d  %s!%s+0x%llX   (%s:%lu)\n", frame, mod, sym->Name,
                       (unsigned long long)disp, line.FileName, line.LineNumber);
            else
                printf("  #%02d  %s!%s+0x%llX\n", frame, mod, sym->Name, (unsigned long long)disp);
        }
        else
        {
            printf("  #%02d  %s+0x%llX\n", frame, mod, (unsigned long long)(addr - mi.BaseOfImage));
        }
    }

    SymCleanup(proc);
    return 0;
}
