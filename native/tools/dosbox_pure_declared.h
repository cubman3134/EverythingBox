// The dosbox-pure core options that EverythingBox's msdos recipe maps a dosbox.conf onto (issue #288), as the
// core itself declares them — a regression fixture shared by probe_dosconf and probe_recipes.
//
// VERIFIED AGAINST THE CORE'S OWN SOURCE, not written from memory: upstream schellingb/dosbox-pure at release
// tag 1.0-preview6 (commit a4a0bab7f8931433588f2fcad9045c85b277373d), core_options.h, the `option_defs[]`
// retro_core_option_v2_definition table the core registers through RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2
// from retro_init (dosbox_pure_libretro.cpp: set_variables). Each entry below cites its line at that commit.
// The value strings are copied exactly; the labels are shortened, because nothing matches on a label.
//
// Build-conditional values, resolved for the builds EverythingBox downloads:
//   * dosbox_pure_cpu_core's "auto" and "dynamic" exist only under C_DYNAMIC_X86 or C_DYNREC
//     (core_options.h:924-930). Both desktop x86-64 and ARM builds define one of them, so both are listed.
//     A build with neither would not declare them, and the declared-options check (DosConf::checkAgainstCore)
//     is what catches that at launch — this table does not have to.
//   * dosbox_pure_cpu_type's "pentium_mmx" exists only #if C_MMX (core_options.h:912), and include/config.h:46
//     defines C_MMX 0 at this commit, so it is NOT declared and NOT listed.
//
// dosbox_pure_cycles is a FIXED list (core_options.h:507-521): auto, max and eleven named cycle counts. The core
// reads a numeric value with atoi (dosbox_pure_libretro.cpp:2407-2414), but it DECLARES only these, and the
// declaration is what a frontend is entitled to set — so a conf's `cycles=fixed 3000` is not an accepted value.
#pragma once
#include "libretro/LibretroCore.h"   // CoreOption — the exact shape LibretroCore::options() hands out
#include <vector>

inline std::vector<CoreOption> dosboxPureDeclaredOptions()
{
    auto opt = [](const char* key, std::vector<const char*> values, const char* def) {
        CoreOption o;
        o.key = key;
        for (const char* v : values) o.values.push_back({ v, v });
        o.defaultValue = def;
        return o;
    };
    return {
        // core_options.h:297-307 — not a conf mapping, but the msdos recipe SEEDS it ("inside"), so it is checked too
        opt("dosbox_pure_conf", { "false", "inside", "outside" }, "false"),
        // core_options.h:503-523
        opt("dosbox_pure_cycles",
            { "auto", "max", "315", "1320", "2750", "4720", "7800", "13400", "26800", "77000", "200000", "500000",
              "1000000" }, "auto"),
        // core_options.h:596-610
        opt("dosbox_pure_machine", { "svga", "vga", "ega", "cga", "tandy", "hercules", "pcjr" }, "svga"),
        // core_options.h:862-884
        opt("dosbox_pure_memory_size",
            { "none", "4", "8", "16", "24", "32", "48", "64", "96", "128", "224", "256", "512", "1024" }, "16"),
        // core_options.h:897-917 (pentium_mmx compiled out: C_MMX 0)
        opt("dosbox_pure_cpu_type", { "auto", "386", "386_slow", "386_prefetch", "486_slow", "pentium_slow" }, "auto"),
        // core_options.h:919-939 (auto/dynamic under C_DYNAMIC_X86 || C_DYNREC)
        opt("dosbox_pure_cpu_core", { "auto", "dynamic", "normal", "simple" }, "auto"),
        // core_options.h:1108-1122
        opt("dosbox_pure_sblaster_type", { "sb16", "sbpro2", "sbpro1", "sb2", "sb1", "gb", "none" }, "sb16"),
        // core_options.h:1124-1138
        opt("dosbox_pure_sblaster_adlib_mode", { "auto", "cms", "opl2", "dualopl2", "opl3", "opl3gold", "none" },
            "auto"),
    };
}
