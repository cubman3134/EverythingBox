// A STAND-IN FOR A RECOMPILER (issue #248, increment c), built in-tree and used by probe_recompbuild only.
//
// WHY IT EXISTS. The build runner's interesting behaviour is all about what a child process DOES: how long it
// talks for, whether it exits or dies, whether it leaves the file it promised, and whether it can be stopped
// halfway. None of that can be driven against a real recompiler — psxrecomp is somebody else's program under
// a noncommercial licence, it is not in this repository and it never will be, and a probe that downloaded one
// would fail whenever a release was retagged. So the probe drives the runner against this: a program with no
// dependencies whose entire behaviour is on its command line.
//
// DELIBERATELY NOT Qt. It is launched by a QProcess in a probe that already links Qt, and every Qt symbol in
// here would be a Qt DLL that has to be beside it on Windows for the probe to be able to launch it at all.
//
// It also serves as the honest answer to "was the ROM copied": the probe hands it a fixture file as the
// `--disc` argument, and `--echo-args` prints back exactly what it was given.
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#if defined(_WIN32)
#  include <windows.h>
#endif

int main(int argc, char** argv)
{
    long        lines = 0;
    long        sleepMs = 0;
    int         exitCode = 0;
    bool        crash = false;
    bool        forever = false;
    long        longLines = 0;   // lines with no newline discipline: one very long line each
    bool        echoArgs = false;
    std::string makeFile;

    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : std::string(); };
        if (a == "--lines")          lines = std::strtol(next().c_str(), nullptr, 10);
        else if (a == "--sleep-ms")  sleepMs = std::strtol(next().c_str(), nullptr, 10);
        else if (a == "--exit")      exitCode = int(std::strtol(next().c_str(), nullptr, 10));
        else if (a == "--crash")     crash = true;
        else if (a == "--forever")   forever = true;
        else if (a == "--long")      longLines = std::strtol(next().c_str(), nullptr, 10);
        else if (a == "--make")      makeFile = next();
        else if (a == "--echo-args") echoArgs = true;
    }

    if (echoArgs)
        for (int i = 1; i < argc; ++i) std::printf("ARG %s\n", argv[i]);

    // A very long single line, repeated. This is what a linker echoing its own command line looks like, and
    // it is the shape that bounds-checks the log tail's per-line cap rather than its line count.
    for (long i = 0; i < longLines; ++i)
    {
        std::printf("[%3ld%%] ", (longLines > 1) ? (i * 100 / (longLines - 1)) : 100);
        for (int k = 0; k < 4000; ++k) std::fputc('x', stdout);
        std::fputc('\n', stdout);
    }

    for (long i = 0; i < lines; ++i)
    {
        std::printf("[%3ld%%] Building CXX object gen/rec_%ld.cpp.o\n",
                    (lines > 1) ? (i * 100 / (lines - 1)) : 100, i);
        std::fflush(stdout);
        if (sleepMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
    }

    if (!makeFile.empty())
    {
        if (FILE* f = std::fopen(makeFile.c_str(), "wb"))
        {
            std::fputs("not a real program\n", f);
            std::fclose(f);
        }
    }

    if (forever)
    {
        long i = 0;
        for (;;)
        {
            std::printf("still working %ld\n", i++);
            std::fflush(stdout);
            std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs > 0 ? sleepMs : 20));
        }
    }

    std::fflush(stdout);
    // Ends WITHOUT exiting, which is what QProcess reports as CrashExit and what a compiler that runs out of
    // memory or hits an internal error actually does.
    //
    // NOT abort(). On Windows the C runtime's abort() prints a message and can hand the process to Windows
    // Error Reporting, which on a developer machine means a dialog box appearing in the middle of a headless
    // probe run. TerminateProcess with an access-violation status is the same thing to the parent — a process
    // that ended without exiting — and involves nobody else.
    if (crash)
    {
#if defined(_WIN32)
        ::TerminateProcess(::GetCurrentProcess(), 0xC0000005u);
#else
        std::raise(SIGSEGV);
#endif
    }
    return exitCode;
}
