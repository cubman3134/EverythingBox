// Compute the RetroAchievements ROM hash (rc_hash) for a file, exactly as MMV's in-process RA client does, so
// we can check whether a "unsupported game version" is a bad/unlisted dump vs. a hashing problem on our side.
//   usage: probe_rahash <console_id> <file>
#include "rc_hash.h"
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv)
{
    if (argc < 3) { printf("usage: probe_rahash <console_id> <file>\n"); return 2; }
    const uint32_t console = (uint32_t)strtoul(argv[1], nullptr, 10);
    char hash[33] = { 0 };
    const int ok = rc_hash_generate_from_file(hash, console, argv[2]);
    printf("console=%u ok=%d hash=%s\n", console, ok, ok ? hash : "(failed)");
    return ok ? 0 : 1;
}
