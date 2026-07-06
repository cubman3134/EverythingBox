#include "LibretroCore.h"
#include <cstdio>
#include <cstdlib>
int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: probe <core> [rom]   (set MMV_SYSTEM_DIR for cores that need a BIOS)\n"); return 2; }
    LibretroCore core; std::string err;
    if (const char* sd = getenv("MMV_SYSTEM_DIR")) { core.systemDir = sd; printf("systemDir: %s\n", sd); }
    if (!core.loadCore(argv[1], &err)) { printf("loadCore failed: %s\n", err.c_str()); return 1; }
    printf("core: %s %s [%s]\n", core.systemInfo().library_name, core.systemInfo().library_version, core.systemInfo().valid_extensions);
    printf("options harvested: %zu\n", core.options().size());
    int shown = 0;
    for (const auto& o : core.options()) {
        if (shown++ >= 8) { printf("  ... (%zu more)\n", core.options().size() - 8); break; }
        printf("  [%s] %s = %s  (%zu choices: ", o.key.c_str(), o.desc.c_str(),
               core.optionValue(o.key).c_str(), o.values.size());
        for (size_t i = 0; i < o.values.size() && i < 5; ++i) printf("%s%s", i?"|":"", o.values[i].first.c_str());
        printf("%s)\n", o.values.size() > 5 ? "|..." : "");
    }
    if (argc < 3) { printf("core loaded + retro_init ran OK (no ROM supplied)\n"); return 0; }
    if (!core.loadGame(argv[2], &err)) { printf("loadGame failed: %s\n", err.c_str()); return 1; }
    auto& av = core.avInfo();
    printf("av: %ux%u (max %ux%u) fps=%.3f sr=%.0f\n", av.geometry.base_width, av.geometry.base_height,
           av.geometry.max_width, av.geometry.max_height, av.timing.fps, av.timing.sample_rate);
    size_t audioFrames = 0; core.onAudio = [&](const int16_t*, size_t f){ audioFrames += f; };
    for (int i = 0; i < 200; ++i) core.runFrame();
    const uint8_t* f = core.frameBGRA(); unsigned w = core.frameWidth(), h = core.frameHeight();
    if (!f || !w || !h) { printf("NO FRAME\n"); return 1; }
    long nb = 0; double sum = 0; size_t px = (size_t)w*h;
    for (size_t i = 0; i < px*4; i += 4) { if (f[i]|f[i+1]|f[i+2]) nb++; sum += f[i]+f[i+1]+f[i+2]; }
    printf("frame %ux%u nonblack=%ld/%zu mean=%.1f audioFrames=%zu\n", w, h, nb, px, sum/(px*3), audioFrames);
    printf("%s\n", (nb > (long)px/4) ? "EMULATION RUNS: real frame + audio in native C++" : "frame mostly black");

    // ---- save-state round-trip: save, diverge, restore, confirm the frame matches the saved point ----
    std::vector<uint8_t> state;
    if (!core.saveState(state)) { printf("save states: NOT SUPPORTED by this core/content\n"); return 0; }
    auto checksum = [&]{ const uint8_t* p = core.frameBGRA(); size_t n = (size_t)core.frameWidth()*core.frameHeight()*4;
                         unsigned long h2 = 1469598103934665603UL; for (size_t i=0;i<n;++i){ h2^=p[i]; h2*=1099511628211UL; } return h2; };
    for (int i = 0; i < 120; ++i) core.runFrame();          // advance so state visibly diverges
    const unsigned long afterRun = checksum();
    const bool restored = core.loadState(state.data(), state.size());
    for (int i = 0; i < 120; ++i) core.runFrame();          // replay the same 120 frames from the restored state
    const unsigned long afterRestore = checksum();
    printf("save states: size=%zu restored=%s deterministic_replay=%s\n",
           state.size(), restored ? "yes" : "NO",
           (restored && afterRestore == afterRun) ? "MATCH (state round-trips)" : "differ");
    return 0;
}
