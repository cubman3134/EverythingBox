#!/usr/bin/env python3
"""Assert that a built release archive actually contains what the app needs (issue #319).

WHY THIS EXISTS. `release.yml` packages the Windows zip and the Linux AppImage, and until this script
nothing anywhere looked inside either one. Issue #317 was that bug one layer in: `gamecontrollerdb.txt`
existed in the tree, was correct, and simply never travelled into the archive. Its fix added two
packaging lines that are themselves REVIEWED, NOT TESTED, because the release workflow only runs on a
tag and no step reads back what it produced. The feedback loop for a hole in the archive is a user
downloading a release.

So: after the archive is built, extract it and assert the manifest. A missing file fails the release
instead of shipping.

WHERE THE MANIFEST COMES FROM. A hand-maintained list of shipped files is the same class of bug this
script exists to catch -- it drifts the moment somebody adds a file. So the manifest is DERIVED wherever
the repository already knows the answer, and literal only where nothing in the tree can be asked:

  DERIVED, from the repo tree (`--repo`), which is the same checkout the packaging step copied from:
    * every file under native/addons/**  -> must be in the archive at <bindir>/addons/<same rel path>
    * every file under native/themes2/** -> must be in the archive at <bindir>/themes2/<same rel path>
      (both trees are copied WHOLESALE by the packaging step, so the tree is the list; adding a theme
      or an addon file needs no edit here, and forgetting to package one is caught. <bindir> is the
      executable's own directory -- the zip root on Windows, usr/bin inside the AppDir on Linux --
      because that is what AppPaths::dataDir() resolves to, and it is the ONLY directory ThemeEngine
      and AddonManager read from. See issue #339.)
    * native/gamecontrollerdb.txt        -> beside the executable, BYTE-IDENTICAL
    * native/resources/Uninstall.cmd     -> beside the executable, BYTE-IDENTICAL (Windows only)
  DERIVED, from the built binary itself:
    * Windows: every DLL in EverythingBox.exe's PE import table -- the libraries the loader resolves
      before main() runs, i.e. exactly "what it will not start without" -- must sit in the executable's
      own directory in the zip, or be on PLATFORM_DLLS below.
    * Linux: every DT_NEEDED of usr/bin/EverythingBox that this project SUPPLIES (see BUNDLED_SONAMES)
      must be present inside the AppDir.
  LITERAL, and kept only here:
    * the executable's name, the Qt platform plugin, PLATFORM_DLLS / BUNDLED_SONAMES, and the AppImage's
      AppRun / .desktop / icon. Nothing in the tree names these; they are properties of the packaging
      tools (windeployqt, linuxdeploy) and of the OS.

WINDOWS AND LINUX ASK DIFFERENT QUESTIONS. For the zip, "is it in there" is the whole question: the app
reads its data dir from applicationDirPath() (AppPaths.h), so a file beside EverythingBox.exe is a file
the app finds. For the AppImage, "is it in there" and "can the app find it" are separate -- a file inside
the image at a path the binary never looks at is still missing -- so the AppImage checks also assert that
gamecontrollerdb.txt is in the SAME directory as the binary SDL_GetBasePath() resolves to, that themes2/
and addons/ are in that same directory (dataDir() is applicationDirPath() on Linux too, so an AppImage
that stashed them under usr/share would ship two trees nothing ever opens), and that the Qt platform
plugin is reachable by the search path qt.conf actually establishes.

Usage:
  verify-release-archive.py --kind windows  --archive EverythingBox-windows-x64.zip [--repo .]
  verify-release-archive.py --kind appimage --archive EverythingBox-linux-x86_64.AppImage [--repo .]
  verify-release-archive.py --kind appimage --appdir AppDir [--repo .]     (skip the extract step)
  verify-release-archive.py --selftest [--verbose]

Exit status: 0 = the archive holds everything asserted; 1 = something is missing; 2 = the check could
not be run at all (which is also a failure -- a verifier that silently skips is the defect it guards).
"""

import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile

# --------------------------------------------------------------------------------------------------
# The literal half of the manifest. ONE place, and it is here rather than scattered through release.yml
# so that a reader can see the whole of what is asserted-by-name in one screen.

WINDOWS_EXE = "EverythingBox.exe"
LINUX_BINARY = "EverythingBox"

# Qt finds its platform plugin by directory, not by import: with no platform plugin the process aborts
# with "This application failed to start because no Qt platform plugin could be initialized". Nothing in
# the repo names it, so it is literal.
WINDOWS_PLATFORM_PLUGIN = "platforms/qwindows.dll"
LINUX_PLATFORM_PLUGIN = "platforms/libqxcb.so"

# DLLs that come from Windows itself (or from a driver / the VC++ runtime that Windows machines carry),
# which the packaging step is NOT responsible for shipping. Anything imported and NOT here has to be in
# the zip. Prefix entries end in '*'. Lowercase; the PE loader is case-insensitive.
PLATFORM_DLLS = [
    "ntdll.dll", "kernel32.dll", "kernelbase.dll", "user32.dll", "gdi32.dll", "gdi32full.dll",
    "advapi32.dll", "shell32.dll", "shlwapi.dll", "shcore.dll", "ole32.dll", "oleaut32.dll",
    "oleacc.dll", "comdlg32.dll", "comctl32.dll", "rpcrt4.dll", "sechost.dll", "psapi.dll",
    "version.dll", "winmm.dll", "imm32.dll", "uxtheme.dll", "dwmapi.dll", "userenv.dll", "netapi32.dll",
    "mpr.dll", "wtsapi32.dll", "winspool.drv", "setupapi.dll", "cfgmgr32.dll", "hid.dll", "powrprof.dll",
    "propsys.dll", "avrt.dll", "dbghelp.dll", "dbgcore.dll", "authz.dll", "crypt32.dll", "cryptbase.dll",
    "bcrypt.dll", "bcryptprimitives.dll", "ncrypt.dll", "secur32.dll", "wintrust.dll", "ws2_32.dll",
    "wsock32.dll", "mswsock.dll", "iphlpapi.dll", "dnsapi.dll", "wldap32.dll", "normaliz.dll",
    "usp10.dll", "dwrite.dll", "windowscodecs.dll", "opengl32.dll", "glu32.dll", "d3d9.dll", "d3d11.dll",
    "d3d12.dll", "dxgi.dll", "dxva2.dll", "msvcrt.dll", "ucrtbase.dll",
    # The HLSL compiler the Qt/Quick D3D path import-links. It ships in System32 on Windows 10 and 11,
    # and release.yml passes windeployqt --no-system-d3d-compiler precisely so the machine's copy is NOT
    # redistributed -- so it is the platform's, deliberately, and its absence from the zip is not a hole.
    "d3dcompiler_47.dll",
    # The Vulkan loader ships with the GPU driver / the Vulkan runtime, not with this app. The Windows
    # build import-links it through RetroPark's PUBLIC link (see the Vulkan SDK step in release.yml).
    "vulkan-1.dll",
    # The VC++ runtime. windeployqt copies these when it can find the redist directory; when it cannot,
    # they are the machine's (every Windows box with the VC++ 2015-2022 redistributable has them).
    "vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll",
    "concrt140.dll",
    "api-ms-win-*", "ext-ms-*",
]

# Shared objects this project SUPPLIES on Linux: if the app binary needs one of these it has to be inside
# the AppImage, because no host is expected to have it. Everything else a Linux binary needs (libc,
# libstdc++, libGL, the X libraries...) is the host's, and which of those an AppImage may rely on is
# decided by the AppImage excludelist that linuxdeploy already applies -- restating that list here would
# be exactly the hand-maintained-and-drifting list this script exists to avoid, so it is not restated.
BUNDLED_SONAMES = ("libQt6", "libmpv", "libSDL2")


# --------------------------------------------------------------------------------------------------
# Binary readers. Both are deliberately dependency-free and defensive: this runs on a release runner and
# a parser crash must read as "could not check", never as "checked, fine".

def pe_imported_dlls(data):
    """DLL names in a PE's import directory, i.e. what the loader resolves before the process starts.

    Returns a list of names, or raises ValueError if the bytes are not a PE we can read.
    """
    def u16(o):
        return struct.unpack_from("<H", data, o)[0]

    def u32(o):
        return struct.unpack_from("<I", data, o)[0]

    if len(data) < 0x40 or data[:2] != b"MZ":
        raise ValueError("not a PE image (no MZ header)")
    pe = u32(0x3C)
    if pe + 24 > len(data) or data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("not a PE image (no PE signature)")
    n_sections = u16(pe + 6)
    size_opt = u16(pe + 20)
    opt = pe + 24
    if opt + size_opt > len(data):
        raise ValueError("truncated optional header")
    magic = u16(opt)
    if magic == 0x20B:      # PE32+
        n_dirs_off, dirs_off = opt + 108, opt + 112
    elif magic == 0x10B:    # PE32
        n_dirs_off, dirs_off = opt + 92, opt + 96
    else:
        raise ValueError("unknown optional-header magic 0x%X" % magic)
    n_dirs = u32(n_dirs_off)
    if n_dirs < 2:
        return []           # no import directory at all
    import_rva, import_size = struct.unpack_from("<II", data, dirs_off + 8)
    if import_rva == 0 or import_size == 0:
        return []

    sections = []
    sec_off = opt + size_opt
    for i in range(n_sections):
        o = sec_off + i * 40
        if o + 40 > len(data):
            break
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", data, o + 8)
        sections.append((vaddr, max(vsize, rawsize), rawptr))

    def to_offset(rva):
        for vaddr, span, rawptr in sections:
            if vaddr <= rva < vaddr + span:
                return rawptr + (rva - vaddr)
        return None

    def cstring(off):
        end = data.find(b"\0", off)
        if end < 0:
            raise ValueError("unterminated import name")
        return data[off:end].decode("ascii", "replace")

    base = to_offset(import_rva)
    if base is None:
        raise ValueError("import directory RVA 0x%X is in no section" % import_rva)
    names = []
    for i in range(4096):                        # bounded: a real image has tens, not thousands
        d = base + i * 20
        if d + 20 > len(data):
            break
        fields = struct.unpack_from("<IIIII", data, d)
        if fields == (0, 0, 0, 0, 0):
            break
        off = to_offset(fields[3])
        if off is None:
            raise ValueError("import name RVA 0x%X is in no section" % fields[3])
        names.append(cstring(off))
    return names


def elf_needed(data):
    """DT_NEEDED entries of an ELF64 shared object / executable, plus its e_machine.

    Returns (needed, machine). Raises ValueError if the bytes are not an ELF64 we can read.
    """
    if len(data) < 64 or data[:4] != b"\x7fELF":
        raise ValueError("not an ELF image")
    if data[4] != 2:
        raise ValueError("not a 64-bit ELF (EI_CLASS=%d)" % data[4])
    if data[5] != 1:
        raise ValueError("not a little-endian ELF")
    machine = struct.unpack_from("<H", data, 18)[0]
    e_shoff = struct.unpack_from("<Q", data, 0x28)[0]
    e_shentsize, e_shnum = struct.unpack_from("<HH", data, 0x3A)
    if e_shoff == 0 or e_shnum == 0:
        raise ValueError("no section headers (stripped past what this reader can follow)")
    sections = []
    for i in range(e_shnum):
        o = e_shoff + i * e_shentsize
        if o + 64 > len(data):
            raise ValueError("truncated section header table")
        sh_type = struct.unpack_from("<I", data, o + 4)[0]
        sh_offset, sh_size = struct.unpack_from("<QQ", data, o + 0x18)
        sh_link = struct.unpack_from("<I", data, o + 0x28)[0]
        sections.append((sh_type, sh_offset, sh_size, sh_link))
    dyn = next((s for s in sections if s[0] == 6), None)   # SHT_DYNAMIC
    if dyn is None:
        return [], machine
    if dyn[3] >= len(sections):
        raise ValueError(".dynamic links to section %d, which does not exist" % dyn[3])
    strtab_off, strtab_size = sections[dyn[3]][1], sections[dyn[3]][2]
    needed = []
    for i in range(dyn[2] // 16):
        tag, val = struct.unpack_from("<Qq", data, dyn[1] + i * 16)
        if tag == 0:                                       # DT_NULL
            break
        if tag == 1:                                       # DT_NEEDED
            if val < 0 or val >= strtab_size:
                raise ValueError("DT_NEEDED string offset %d is outside .dynstr" % val)
            off = strtab_off + val
            end = data.find(b"\0", off)
            needed.append(data[off:end].decode("ascii", "replace"))
    return needed, machine


# --------------------------------------------------------------------------------------------------
# The derived half of the manifest.

def repo_tree_files(root, rel):
    """Every file under <root>/<rel>, as (archive-relative path, absolute source path)."""
    src = os.path.join(root, *rel.split("/"))
    out = []
    for dirpath, _dirs, files in os.walk(src):
        for name in sorted(files):
            full = os.path.join(dirpath, name)
            arc = os.path.relpath(full, src).replace(os.sep, "/")
            out.append((arc, full))
    return sorted(out)


class Report(object):
    """Collects findings so the whole picture is printed, not just the first missing file."""

    def __init__(self):
        self.problems = []
        self.checked = 0
        self.lines = []

    def ok(self, msg):
        self.checked += 1
        self.lines.append("  ok    %s" % msg)

    def note(self, msg):
        self.lines.append("  note  %s" % msg)

    def bad(self, msg):
        self.problems.append(msg)
        self.lines.append("  MISSING %s" % msg)

    def dump(self, out=None):
        out = out or sys.stdout          # resolved at call time: --selftest redirects sys.stdout
        for line in self.lines:
            out.write(line + "\n")


# --------------------------------------------------------------------------------------------------
# Windows zip

def check_windows_zip(archive, repo, rep):
    try:
        zf = zipfile.ZipFile(archive)
    except Exception as exc:                                     # noqa: BLE001 - reported, not raised
        rep.bad("the archive could not be opened as a zip: %s" % exc)
        return
    with zf:
        # PowerShell's Compress-Archive has shipped both separators over its life; normalise so the
        # manifest is asserted against the file, not against the packer's spelling of the path.
        entries = {}
        for info in zf.infolist():
            if info.is_dir():
                continue
            entries[info.filename.replace("\\", "/")] = info
        by_lower = dict((k.lower(), k) for k in entries)

        def read(path):
            key = by_lower.get(path.lower())
            return zf.read(entries[key]) if key else None

        # --- the executable
        exe_bytes = read(WINDOWS_EXE)
        if exe_bytes is None:
            rep.bad("%s (the application itself is not in the archive)" % WINDOWS_EXE)
        else:
            rep.ok("%s (%d bytes)" % (WINDOWS_EXE, len(exe_bytes)))

        # --- bundled data, derived from the repo tree the packaging step copied from
        check_bundled_data(repo, rep, read, prefix="")
        expect_identical(repo, rep, read, "native/resources/Uninstall.cmd", "Uninstall.cmd")

        # --- the Qt platform plugin: no import names it, and without it the process aborts on startup
        if read(WINDOWS_PLATFORM_PLUGIN) is None:
            rep.bad("%s (Qt aborts at startup with no platform plugin)" % WINDOWS_PLATFORM_PLUGIN)
        else:
            rep.ok(WINDOWS_PLATFORM_PLUGIN)

        # --- the runtime libraries, derived from the executable's own import table
        if exe_bytes is not None:
            check_windows_imports(exe_bytes, entries, rep)


def check_windows_imports(exe_bytes, entries, rep):
    try:
        imports = pe_imported_dlls(exe_bytes)
    except ValueError as exc:
        rep.bad("the executable's import table could not be read (%s), so nothing can be said about "
                "the runtime libraries it needs" % exc)
        return
    if not imports:
        rep.bad("the executable imports no DLLs at all, which cannot be right for this build -- the "
                "import table was read but came back empty")
        return
    # Only the executable's OWN directory is on the loader's default search path for these, and the
    # executable sits at the root of the zip, so a needed DLL has to be a root-level entry.
    root_level = set(k.lower() for k in entries if "/" not in k)
    # An import table can name the same DLL twice in different casing (real builds do: KERNEL32.dll and
    # kernel32.dll both appear). One entry per library, since the loader is case-insensitive.
    for low in sorted(set(name.lower() for name in imports)):
        dll = next(name for name in imports if name.lower() == low)
        if low in root_level:
            rep.ok("%s (imported by %s, present)" % (dll, WINDOWS_EXE))
        elif platform_provided(low):
            rep.note("%s (imported by %s, provided by the platform: Windows itself, the VC++ "
                     "redistributable, or the GPU driver)" % (dll, WINDOWS_EXE))
        else:
            rep.bad("%s -- %s import-links it, so the process cannot start without it, and it is not "
                    "beside the executable in the archive" % (dll, WINDOWS_EXE))


def platform_provided(name_lower):
    for pat in PLATFORM_DLLS:
        if pat.endswith("*"):
            if name_lower.startswith(pat[:-1]):
                return True
        elif name_lower == pat:
            return True
    return False


# --------------------------------------------------------------------------------------------------
# Shared derived-data assertions

def check_bundled_data(repo, rep, read, prefix):
    """The controller database and the two bundled trees, asserted against the repo tree itself."""
    expect_identical(repo, rep, read, "native/gamecontrollerdb.txt", prefix + "gamecontrollerdb.txt")
    check_bundled_trees(repo, rep, read, prefix)


BUNDLED_TREES = (("native/addons", "addons"), ("native/themes2", "themes2"))


def check_bundled_trees(repo, rep, read, prefix, trees=BUNDLED_TREES):
    """themes2/ and addons/, file by file, under `prefix` (the executable's own directory).

    Split out of check_bundled_data because the AppImage answers the controller database's question
    differently (it has to say WHERE a stray copy is), but asks exactly this one about the two trees.
    `trees` narrows it: a tree the AppImage put in the wrong directory has already been reported as
    such, and listing its several hundred files again as "never packaged" would bury that finding.
    """
    for rel, arcdir in trees:
        files = repo_tree_files(repo, rel)
        if not files:
            rep.bad("%s in the repo checkout holds no files, so this check asserted NOTHING about the "
                    "bundled %s -- point --repo at the checkout the archive was built from"
                    % (rel, arcdir))
            continue
        missing = []
        differing = []
        for arc, full in files:
            got = read("%s%s/%s" % (prefix, arcdir, arc))
            if got is None:
                missing.append("%s%s/%s" % (prefix, arcdir, arc))
            else:
                with open(full, "rb") as f:
                    if f.read() != got:
                        differing.append("%s%s/%s" % (prefix, arcdir, arc))
        if missing:
            for m in missing[:12]:
                rep.bad("%s (in %s, never packaged)" % (m, rel))
            if len(missing) > 12:
                rep.bad("... and %d more file(s) under %s%s" % (len(missing) - 12, prefix, arcdir))
        for d in differing:
            rep.bad("%s (packaged, but its bytes differ from the repo's copy)" % d)
        if not missing and not differing:
            rep.ok("%s%s/ -- all %d file(s) from %s, byte-identical"
                   % (prefix, arcdir, len(files), rel))


def expect_identical(repo, rep, read, repo_rel, arc_path):
    src = os.path.join(repo, *repo_rel.split("/"))
    if not os.path.isfile(src):
        rep.bad("%s is not in the checkout at --repo, so this check asserted NOTHING about %s"
                % (repo_rel, arc_path))
        return
    with open(src, "rb") as f:
        want = f.read()
    got = read(arc_path)
    if got is None:
        rep.bad("%s (%s exists in the tree and was never packaged -- this is issue #317's shape)"
                % (arc_path, repo_rel))
    elif got != want:
        rep.bad("%s (packaged, but its bytes differ from %s)" % (arc_path, repo_rel))
    else:
        rep.ok("%s (byte-identical to %s, %d bytes)" % (arc_path, repo_rel, len(want)))


# --------------------------------------------------------------------------------------------------
# AppImage

def extract_appimage(archive, workdir):
    """--appimage-extract into workdir and return the AppDir (squashfs-root)."""
    path = os.path.abspath(archive)
    os.chmod(path, 0o755)
    proc = subprocess.Popen([path, "--appimage-extract"], cwd=workdir,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    out = proc.communicate()[0]
    root = os.path.join(workdir, "squashfs-root")
    if proc.returncode != 0 or not os.path.isdir(root):
        raise RuntimeError("--appimage-extract failed (rc=%d): %s"
                           % (proc.returncode, out.decode("utf-8", "replace")[-800:]))
    return root


def check_appdir(appdir, repo, rep):
    def read(rel):
        p = os.path.join(appdir, *rel.split("/"))
        if not os.path.isfile(p):
            return None
        with open(p, "rb") as f:
            return f.read()

    # --- the binary. linuxdeploy installs it under usr/bin; find it rather than assume, because the
    # directory it lands in is the directory everything else has to be found RELATIVE TO.
    binrel = None
    for cand in ("usr/bin/" + LINUX_BINARY, LINUX_BINARY):
        if read(cand) is not None:
            binrel = cand
            break
    if binrel is None:
        rep.bad("usr/bin/%s (the application itself is not in the image)" % LINUX_BINARY)
        bindir = "usr/bin"
    else:
        bindir = os.path.dirname(binrel)
        rep.ok("%s (%d bytes)" % (binrel, len(read(binrel))))

    # --- AppRun and the desktop entry: an AppImage with no AppRun does not launch at all.
    if not os.path.exists(os.path.join(appdir, "AppRun")):
        rep.bad("AppRun (an AppImage without it cannot launch)")
    else:
        rep.ok("AppRun")
    desktops = [f for f in sorted(os.listdir(appdir)) if f.endswith(".desktop")]
    if not desktops:
        rep.bad("*.desktop (the image has no desktop entry, so it has no name, icon or Exec)")
    else:
        rep.ok(desktops[0])
        check_desktop_exec(appdir, desktops[0], bindir, rep)
    icons = [f for f in sorted(os.listdir(appdir)) if f.endswith((".png", ".svg"))]
    if not icons:
        rep.bad("the application icon (no .png/.svg at the image root)")
    else:
        rep.ok(icons[0])

    # --- the controller database, and the FINDABILITY half of the question: SDL reads it from
    # SDL_GetBasePath(), the binary's own directory, so being anywhere else in the image is being
    # missing (Gamepad.cpp / EmulatorManager.cpp both spell it that way).
    padrel = bindir + "/gamecontrollerdb.txt"
    stray = None if read(padrel) is not None else find_in_appdir(appdir, "gamecontrollerdb.txt")
    if stray:
        rep.bad("gamecontrollerdb.txt IS in the image (at %s) but not in %s, which is the only "
                "directory SDL_GetBasePath() looks in -- in the image is not the same as findable"
                % (stray, bindir))
    else:
        expect_identical(repo, rep, read, "native/gamecontrollerdb.txt", padrel)

    # --- the Qt platform plugin: in the image AND on the search path qt.conf establishes.
    check_qt_plugin_findable(appdir, bindir, rep)

    # --- the runtime libraries this project supplies, derived from the binary's own DT_NEEDED.
    if binrel is not None:
        check_elf_deps(appdir, read(binrel), binrel, rep)

    # --- the two bundled trees (issue #339), with the same findability half as the pad database.
    check_bundled_trees_findable(appdir, repo, rep, read, bindir)


def check_desktop_exec(appdir, desktop, bindir, rep):
    """The Exec= line has to name something that is actually in the image."""
    with open(os.path.join(appdir, desktop), "rb") as f:
        text = f.read().decode("utf-8", "replace")
    execline = next((l for l in text.splitlines() if l.startswith("Exec=")), None)
    if not execline:
        rep.bad("%s has no Exec= line, so the desktop entry launches nothing" % desktop)
        return
    prog = execline[len("Exec="):].split()[0] if execline[len("Exec="):].split() else ""
    if not prog:
        rep.bad("%s has an empty Exec= line" % desktop)
    elif os.path.isfile(os.path.join(appdir, bindir.replace("/", os.sep), prog)):
        rep.ok("%s Exec=%s resolves to %s/%s" % (desktop, prog, bindir, prog))
    else:
        rep.bad("%s says Exec=%s, but there is no %s/%s in the image" % (desktop, prog, bindir, prog))


def find_in_appdir(appdir, name):
    for dirpath, _dirs, files in os.walk(appdir):
        if name in files:
            return os.path.relpath(os.path.join(dirpath, name), appdir).replace(os.sep, "/")
    return None


def find_dir_in_appdir(appdir, name):
    for dirpath, dirs, _files in os.walk(appdir):
        if name in dirs:
            return os.path.relpath(os.path.join(dirpath, name), appdir).replace(os.sep, "/")
    return None


def check_bundled_trees_findable(appdir, repo, rep, read, bindir):
    """themes2/ and addons/: in the image, AND in the one directory the app reads them from.

    ISSUE #339, and the reason it is a separate function from the plain per-file check. On every
    desktop platform AppPaths::dataDir() is QCoreApplication::applicationDirPath(), so ThemeEngine
    resolves <bindir>/themes2 (ThemeRegistry::themesRoot) and AddonManager resolves <bindir>/addons.
    Neither searches anywhere else and neither falls back. A tree parked at the AppDir root, or under
    usr/share where a distro package would put it, is therefore hundreds of files that ship and are
    never opened -- and the symptom is exactly what #339 reports: no bundled theme, so the themed home
    falls back to classic, and no first-party add-on at all.

    A tree that is simply absent is left to the per-file check below, which names the files; a tree in
    the WRONG place is reported here and then skipped, because relisting its files as "never packaged"
    would bury the one line that says what actually went wrong.
    """
    listable = []
    for rel, name in BUNDLED_TREES:
        if os.path.isdir(os.path.join(appdir, *(bindir.split("/") + [name]))):
            listable.append((rel, name))
            continue
        stray = find_dir_in_appdir(appdir, name)
        if stray:
            rep.bad("%s/ IS in the image (at %s) but not in %s, which is the only directory the app "
                    "reads it from (dataDir() is the executable's own directory) -- in the image is "
                    "not the same as findable" % (name, stray, bindir))
        else:
            listable.append((rel, name))
    check_bundled_trees(repo, rep, read, bindir + "/", listable)


def check_qt_plugin_findable(appdir, bindir, rep):
    """Is the platform plugin in the image, and does the binary's plugin search path reach it?

    Two questions, deliberately. linuxdeploy-plugin-qt writes a qt.conf beside the binary whose Prefix /
    Plugins pair is what Qt resolves plugin paths against; a plugin bundled at a path that qt.conf does
    not point at is a plugin the process will not load, and the symptom is the same abort as not
    bundling it at all.
    """
    where = find_in_appdir(appdir, os.path.basename(LINUX_PLATFORM_PLUGIN))
    if where is None:
        rep.bad("%s (Qt aborts at startup with no platform plugin)" % LINUX_PLATFORM_PLUGIN)
        return
    rep.ok("%s (present at %s)" % (os.path.basename(LINUX_PLATFORM_PLUGIN), where))

    roots = []
    qtconf = os.path.join(appdir, bindir.replace("/", os.sep), "qt.conf")
    if os.path.isfile(qtconf):
        prefix, plugins = "", "plugins"
        with open(qtconf, "rb") as f:
            for line in f.read().decode("utf-8", "replace").splitlines():
                if "=" not in line:
                    continue
                k, v = [p.strip() for p in line.split("=", 1)]
                if k.lower() == "prefix":
                    prefix = v
                elif k.lower() == "plugins":
                    plugins = v
        roots.append(("qt.conf (Prefix=%s, Plugins=%s)" % (prefix or ".", plugins),
                      os.path.normpath(os.path.join(bindir, prefix, plugins))))
    # Qt also looks under the application directory itself, with or without a qt.conf.
    roots.append(("the application directory", os.path.normpath(os.path.join(bindir, "plugins"))))
    roots.append(("the application directory", bindir))

    for how, root in roots:
        cand = os.path.join(appdir, root.replace("/", os.sep), *LINUX_PLATFORM_PLUGIN.split("/"))
        if os.path.isfile(cand):
            rep.ok("the platform plugin is reachable via %s -> %s/%s"
                   % (how, root.replace(os.sep, "/"), LINUX_PLATFORM_PLUGIN))
            return
    rep.bad("%s is inside the image (at %s) but on none of the paths the binary searches (%s) -- being "
            "in the image is not the same as being findable"
            % (os.path.basename(LINUX_PLATFORM_PLUGIN), where,
               ", ".join(r.replace(os.sep, "/") for _h, r in roots)))


def check_elf_deps(appdir, binary_bytes, binrel, rep):
    try:
        needed, machine = elf_needed(binary_bytes)
    except ValueError as exc:
        rep.bad("%s could not be read as an ELF64 image (%s), so nothing can be said about the "
                "libraries it needs" % (binrel, exc))
        return
    if machine != 0x3E:
        rep.bad("%s is not x86-64 (e_machine=0x%X), but the artifact is named x86_64" % (binrel, machine))
    if not needed:
        rep.bad("%s lists no DT_NEEDED libraries at all, which cannot be right for this build" % binrel)
        return
    present = {}
    for dirpath, _dirs, files in os.walk(appdir):
        for f in files:
            present.setdefault(f, os.path.relpath(os.path.join(dirpath, f), appdir).replace(os.sep, "/"))
    for soname in sorted(set(needed)):
        supplied = soname.startswith(BUNDLED_SONAMES)
        if soname in present:
            rep.ok("%s (needed by %s, bundled at %s)" % (soname, os.path.basename(binrel),
                                                         present[soname]))
        elif supplied:
            rep.bad("%s -- %s needs it and no host provides it, so it has to be inside the image and "
                    "is not" % (soname, os.path.basename(binrel)))
        else:
            rep.note("%s (needed by %s, expected from the host)" % (soname, os.path.basename(binrel)))


# --------------------------------------------------------------------------------------------------
# Driver

def run_check(kind, archive, appdir, repo):
    rep = Report()
    print("verifying %s: %s" % (kind, archive or appdir))
    print("manifest derived from the checkout at: %s" % os.path.abspath(repo))
    if kind == "windows":
        check_windows_zip(archive, repo, rep)
        rep.dump()
    else:
        tmp = None
        try:
            if appdir is None:
                tmp = tempfile.mkdtemp(prefix="eb-appimage-")
                appdir = extract_appimage(archive, tmp)
            check_appdir(appdir, repo, rep)
            rep.dump()
        except RuntimeError as exc:
            print("  MISSING the AppImage could not be extracted: %s" % exc)
            rep.bad("the AppImage could not be extracted")
        finally:
            if tmp:
                shutil.rmtree(tmp, ignore_errors=True)
    if rep.problems:
        print("")
        print("RELEASE ARCHIVE INCOMPLETE: %d thing(s) the app needs are not in it:" % len(rep.problems))
        for p in rep.problems:
            print("  - %s" % p)
        print("Fix the packaging step in .github/workflows/release.yml -- do NOT relax this check to "
              "make a release go out; that is the hole issue #319 is about.")
        return 1
    print("")
    print("RELEASE ARCHIVE VERIFIED: %d required item(s) present" % rep.checked)
    return 0


# --------------------------------------------------------------------------------------------------
# Self-test. The point of this script is to FAIL when something did not travel, and a check nobody has
# seen fail is a check nobody can trust -- so the suite builds a synthetic archive of each kind, proves
# the complete one passes, then removes exactly one thing at a time and requires the matching complaint.
# It also carries negative controls, because a check that fires on everything is as useless as one that
# fires on nothing. No compiler, no network, no real archive: the executables are synthesised here.

def synth_pe(imports):
    """A minimal PE32+ image whose import directory names `imports`. Not runnable; parseable."""
    sec_rva, sec_off = 0x1000, 0x200
    descs = b""
    names = b""
    name_base = len(imports) * 20 + 20
    for name in imports:
        descs += struct.pack("<IIIII", 0, 0, 0, sec_rva + name_base + len(names), 0)
        names += name.encode("ascii") + b"\0"
    body = descs + b"\0" * 20 + names
    opt = bytearray(0xF0)
    struct.pack_into("<H", opt, 0, 0x20B)                 # PE32+
    struct.pack_into("<I", opt, 108, 16)                  # NumberOfRvaAndSizes
    struct.pack_into("<II", opt, 112 + 8, sec_rva, len(descs) + 20)   # DataDirectory[1] = imports
    coff = struct.pack("<HHIIIHH", 0x8664, 1, 0, 0, 0, len(opt), 0x22)
    section = struct.pack("<8sIIII", b".idata", len(body), sec_rva, len(body), sec_off) + \
        struct.pack("<IIHHI", 0, 0, 0, 0, 0x40000040)
    head = bytearray(sec_off)
    head[0:2] = b"MZ"
    struct.pack_into("<I", head, 0x3C, 0x80)
    head[0x80:0x84] = b"PE\0\0"
    head[0x84:0x84 + len(coff)] = coff
    head[0x84 + len(coff):0x84 + len(coff) + len(opt)] = opt
    o = 0x84 + len(coff) + len(opt)
    head[o:o + len(section)] = section
    return bytes(head) + body


def synth_elf(needed):
    """A minimal ELF64 whose .dynamic lists `needed` as DT_NEEDED. Not runnable; parseable."""
    strtab = b"\0"
    offsets = []
    for n in needed:
        offsets.append(len(strtab))
        strtab += n.encode("ascii") + b"\0"
    dyn = b"".join(struct.pack("<Qq", 1, off) for off in offsets) + struct.pack("<Qq", 0, 0)
    shoff = 64
    dyn_off = shoff + 3 * 64
    str_off = dyn_off + len(dyn)
    eh = bytearray(64)
    eh[0:4] = b"\x7fELF"
    eh[4], eh[5], eh[6] = 2, 1, 1
    struct.pack_into("<HHI", eh, 16, 3, 0x3E, 1)          # ET_DYN, EM_X86_64
    struct.pack_into("<Q", eh, 0x28, shoff)
    struct.pack_into("<HHHHHH", eh, 0x34, 64, 56, 0, 64, 3, 0)

    def sh(sh_type, off, size, link, entsize):
        s = bytearray(64)
        struct.pack_into("<I", s, 4, sh_type)
        struct.pack_into("<QQ", s, 0x18, off, size)
        struct.pack_into("<I", s, 0x28, link)
        struct.pack_into("<Q", s, 0x38, entsize)
        return bytes(s)

    return bytes(eh) + sh(0, 0, 0, 0, 0) + sh(6, dyn_off, len(dyn), 2, 16) + \
        sh(3, str_off, len(strtab), 0, 0) + dyn + strtab


def _write(path, data):
    d = os.path.dirname(path)
    if d and not os.path.isdir(d):
        os.makedirs(d)
    with open(path, "wb") as f:
        f.write(data if isinstance(data, bytes) else data.encode("utf-8"))


def _synth_repo(root):
    _write(os.path.join(root, "native", "gamecontrollerdb.txt"), "03000000deadbeef,Pad,a:b0,\n")
    _write(os.path.join(root, "native", "resources", "Uninstall.cmd"), "@echo off\r\n")
    _write(os.path.join(root, "native", "addons", "demo", "main.js"), "// addon\n")
    _write(os.path.join(root, "native", "addons", "demo", "manifest.json"), "{}\n")
    _write(os.path.join(root, "native", "themes2", "Night", "theme.json"), "{}\n")
    _write(os.path.join(root, "native", "themes2", "Night", "qml", "Home.qml"), "// qml\n")


def _synth_windows_dist(root, repo):
    _write(os.path.join(root, "EverythingBox.exe"),
           synth_pe(["Qt6Core.dll", "libmpv-2.dll", "SDL2.dll", "KERNEL32.dll", "VCRUNTIME140.dll"]))
    for dll in ("Qt6Core.dll", "libmpv-2.dll", "SDL2.dll"):
        _write(os.path.join(root, dll), b"stub")
    _write(os.path.join(root, "platforms", "qwindows.dll"), b"stub")
    for rel, arcdir in (("addons", "addons"), ("themes2", "themes2")):
        for arc, full in repo_tree_files(repo, "native/" + rel):
            with open(full, "rb") as f:
                _write(os.path.join(root, arcdir, *arc.split("/")), f.read())
    for src, dst in (("native/gamecontrollerdb.txt", "gamecontrollerdb.txt"),
                     ("native/resources/Uninstall.cmd", "Uninstall.cmd")):
        with open(os.path.join(repo, *src.split("/")), "rb") as f:
            _write(os.path.join(root, dst), f.read())


def _zip_dir(src, dest):
    with zipfile.ZipFile(dest, "w", zipfile.ZIP_DEFLATED) as zf:
        for dirpath, _dirs, files in os.walk(src):
            for name in files:
                full = os.path.join(dirpath, name)
                zf.write(full, os.path.relpath(full, src).replace(os.sep, "/"))


def _synth_appdir(root, repo):
    _write(os.path.join(root, "usr", "bin", "EverythingBox"),
           synth_elf(["libQt6Core.so.6", "libmpv.so.2", "libSDL2-2.0.so.0", "libc.so.6"]))
    for so in ("libQt6Core.so.6", "libmpv.so.2", "libSDL2-2.0.so.0"):
        _write(os.path.join(root, "usr", "lib", so), b"stub")
    _write(os.path.join(root, "usr", "plugins", "platforms", "libqxcb.so"), b"stub")
    _write(os.path.join(root, "usr", "bin", "qt.conf"), "[Paths]\nPrefix = ../\nPlugins = plugins\n")
    _write(os.path.join(root, "AppRun"), "#!/bin/sh\nexec usr/bin/EverythingBox\n")
    _write(os.path.join(root, "EverythingBox.desktop"),
           "[Desktop Entry]\nType=Application\nName=EverythingBox\nExec=EverythingBox\n"
           "Icon=EverythingBox\n")
    _write(os.path.join(root, "EverythingBox.png"), b"\x89PNG\r\n\x1a\n")
    with open(os.path.join(repo, "native", "gamecontrollerdb.txt"), "rb") as f:
        _write(os.path.join(root, "usr", "bin", "gamecontrollerdb.txt"), f.read())
    # Beside the binary, not at the AppDir root: usr/bin IS the app's data dir on Linux (issue #339).
    for rel, arcdir in (("addons", "addons"), ("themes2", "themes2")):
        for arc, full in repo_tree_files(repo, "native/" + rel):
            with open(full, "rb") as f:
                _write(os.path.join(root, "usr", "bin", arcdir, *arc.split("/")), f.read())


def _verdict(kind, path, repo, verbose):
    """Run one check with its output captured; return (rc, text)."""
    import io
    buf = io.StringIO()
    saved, sys.stdout = sys.stdout, buf
    try:
        if kind == "windows":
            rc = run_check("windows", path, None, repo)
        else:
            rc = run_check("appimage", None, path, repo)
    finally:
        sys.stdout = saved
    text = buf.getvalue()
    if verbose:
        print(text)
    return rc, text


def selftest(verbose=False):
    tmp = tempfile.mkdtemp(prefix="eb-verify-selftest-")
    failures = []

    def case(kind, mutate, want_rc, want_text, label, want_absent=None):
        """Rebuild a pristine archive, apply `mutate`, and require the stated verdict.

        `want_absent` is the other half a couple of cases need: not just that the right complaint is
        made, but that a misleading one is not made beside it.
        """
        work = os.path.join(tmp, "case%d" % case.n)
        case.n += 1
        repo = os.path.join(work, "repo")
        _synth_repo(repo)
        if kind == "windows":
            dist = os.path.join(work, "dist")
            _synth_windows_dist(dist, repo)
            if mutate:
                mutate(dist, repo)
            target = os.path.join(work, "EverythingBox-windows-x64.zip")
            _zip_dir(dist, target)
        else:
            target = os.path.join(work, "AppDir")
            _synth_appdir(target, repo)
            if mutate:
                mutate(target, repo)
        rc, text = _verdict(kind, target, repo, verbose)
        if rc != want_rc:
            failures.append("%s: expected rc=%d, got rc=%d\n%s" % (label, want_rc, rc, text))
        elif want_text and want_text not in text:
            failures.append("%s: verdict did not mention %r\n%s" % (label, want_text, text))
        elif want_absent and want_absent in text:
            failures.append("%s: verdict should not have mentioned %r\n%s"
                            % (label, want_absent, text))
    case.n = 0

    def rm(rel):
        def go(root, _repo):
            os.remove(os.path.join(root, *rel.split("/")))
        return go

    def put(rel, data):
        def go(root, _repo):
            _write(os.path.join(root, *rel.split("/")), data)
        return go

    def move(src, dst):
        def go(root, _repo):
            target = os.path.join(root, *dst.split("/"))
            if not os.path.isdir(os.path.dirname(target)):
                os.makedirs(os.path.dirname(target))   # e.g. usr/share, which nothing else creates
            os.rename(os.path.join(root, *src.split("/")), target)
        return go

    # ---- Windows: the complete archive passes, and each removal is named.
    case("windows", None, 0, "RELEASE ARCHIVE VERIFIED", "windows/complete")
    case("windows", rm("EverythingBox.exe"), 1, "the application itself", "windows/no exe")
    case("windows", rm("gamecontrollerdb.txt"), 1, "gamecontrollerdb.txt", "windows/no pad db")
    case("windows", put("gamecontrollerdb.txt", "03000000deadbeef,Pad,a:b1,\n"), 1,
         "bytes differ", "windows/wrong pad db")
    case("windows", rm("Uninstall.cmd"), 1, "Uninstall.cmd", "windows/no uninstaller")
    case("windows", rm("addons/demo/main.js"), 1, "addons/demo/main.js", "windows/no addon file")
    case("windows", rm("themes2/Night/qml/Home.qml"), 1, "themes2/Night/qml/Home.qml",
         "windows/no theme file")
    case("windows", rm("platforms/qwindows.dll"), 1, "platforms/qwindows.dll", "windows/no platform")
    case("windows", rm("Qt6Core.dll"), 1, "Qt6Core.dll", "windows/no Qt")
    case("windows", rm("libmpv-2.dll"), 1, "libmpv-2.dll", "windows/no libmpv")
    case("windows", rm("SDL2.dll"), 1, "SDL2.dll", "windows/no SDL2")
    case("windows", put("EverythingBox.exe", synth_pe(["Qt6Core.dll", "libmpv-2.dll", "SDL2.dll",
                                                       "Qt6Freshlylinked.dll"])), 1,
         "Qt6Freshlylinked.dll", "windows/new import nobody packaged")
    # A truncated executable must read as "could not check", never as a pass.
    case("windows", put("EverythingBox.exe", b"MZ" + b"\0" * 200), 1, "import table could not be read",
         "windows/unreadable exe")
    # Negative controls: things that must NOT fail.
    case("windows", put("extra/whatever.txt", "not in the manifest\n"), 0, "VERIFIED",
         "windows/extra file is fine")
    case("windows", put("EverythingBox.exe", synth_pe(["QT6CORE.DLL", "libmpv-2.dll", "SDL2.dll"])), 0,
         "VERIFIED", "windows/import casing is fine")
    case("windows", put("EverythingBox.exe", synth_pe(["Qt6Core.dll", "libmpv-2.dll", "SDL2.dll",
                                                       "api-ms-win-crt-runtime-l1-1-0.dll"])), 0,
         "VERIFIED", "windows/os dll is fine")

    # ---- AppImage: same shape, plus the questions that are only asked here.
    case("appimage", None, 0, "RELEASE ARCHIVE VERIFIED", "appimage/complete")
    case("appimage", rm("usr/bin/EverythingBox"), 1, "the application itself", "appimage/no binary")
    case("appimage", rm("usr/bin/gamecontrollerdb.txt"), 1, "gamecontrollerdb.txt",
         "appimage/no pad db")
    # THE distinction: the file IS in the image, at a path the binary never looks at.
    case("appimage", move("usr/bin/gamecontrollerdb.txt", "gamecontrollerdb.txt"), 1,
         "in the image is not the same as findable", "appimage/pad db in the wrong directory")
    case("appimage", rm("usr/lib/libQt6Core.so.6"), 1, "libQt6Core.so.6", "appimage/no Qt")
    case("appimage", rm("usr/lib/libmpv.so.2"), 1, "libmpv.so.2", "appimage/no libmpv")
    case("appimage", rm("usr/plugins/platforms/libqxcb.so"), 1, "libqxcb.so", "appimage/no platform")
    # Bundled, but qt.conf points somewhere else: present and unloadable.
    case("appimage", lambda r, _p: (os.rename(os.path.join(r, "usr", "plugins"),
                                              os.path.join(r, "usr", "qtplugins")),
                                    _write(os.path.join(r, "usr", "bin", "qt.conf"),
                                           "[Paths]\nPrefix = ../\nPlugins = plugins\n")), 1,
         "on none of the paths the binary searches", "appimage/plugin off the search path")
    case("appimage", rm("AppRun"), 1, "AppRun", "appimage/no AppRun")
    case("appimage", rm("EverythingBox.desktop"), 1, "desktop entry", "appimage/no desktop entry")
    case("appimage", put("EverythingBox.desktop",
                         "[Desktop Entry]\nType=Application\nName=EverythingBox\nExec=Nope\n"), 1,
         "there is no usr/bin/Nope", "appimage/desktop Exec names nothing")
    case("appimage", put("usr/bin/EverythingBox", b"\x7fELF" + b"\0" * 200), 1,
         "could not be read as an ELF64", "appimage/unreadable binary")
    case("appimage", rm("usr/lib/libSDL2-2.0.so.0"), 1, "libSDL2-2.0.so.0", "appimage/no SDL2")
    # ---- issue #339: the two bundled trees, and the AppImage-only "in the image, but not where the
    # app looks" half of the question. A theme that ships and is never read is the same defect as one
    # that did not travel at all, so both spellings of the mistake have to be named.
    case("appimage", rm("usr/bin/addons/demo/main.js"), 1, "usr/bin/addons/demo/main.js",
         "appimage/no addon file")
    case("appimage", rm("usr/bin/themes2/Night/qml/Home.qml"), 1,
         "usr/bin/themes2/Night/qml/Home.qml", "appimage/no theme file")
    case("appimage", move("usr/bin/themes2", "themes2"), 1,
         "in the image is not the same as findable", "appimage/themes at the AppDir root")
    case("appimage", move("usr/bin/addons", "usr/share/addons"), 1,
         "in the image is not the same as findable", "appimage/addons under usr/share")
    # ...and a misplaced tree must not ALSO be relisted file by file, or the one line saying what
    # actually went wrong is buried under everything that followed from it.
    case("appimage", move("usr/bin/themes2", "themes2"), 1, None,
         "appimage/misplaced tree is reported once", want_absent="never packaged")
    # Negative control: what the HOST supplies is not this project's to bundle, and an AppImage that
    # relies on libstdc++/libGL being present is doing what every AppImage does.
    case("appimage", put("usr/bin/EverythingBox",
                         synth_elf(["libQt6Core.so.6", "libmpv.so.2", "libSDL2-2.0.so.0",
                                    "libstdc++.so.6", "libGL.so.1"])), 0, "VERIFIED",
         "appimage/host libs are fine")

    shutil.rmtree(tmp, ignore_errors=True)
    if failures:
        print("verify-release-archive.py --selftest: %d case(s) went wrong" % len(failures))
        for f in failures:
            print("----")
            print(f)
        return 1
    print("verify-release-archive.py --selftest: %d cases -- a complete archive passes, and every "
          "missing item is named" % case.n)
    return 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--kind", choices=("windows", "appimage"))
    ap.add_argument("--archive")
    ap.add_argument("--appdir", help="an already-extracted AppDir, instead of --archive")
    ap.add_argument("--repo", default=".", help="the checkout the archive was built from")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args(argv)

    if args.selftest:
        return selftest(args.verbose)
    if not args.kind:
        ap.error("--kind is required (or --selftest)")
    if args.kind == "windows" and not args.archive:
        ap.error("--archive is required for --kind windows")
    if args.kind == "appimage" and not (args.archive or args.appdir):
        ap.error("--archive or --appdir is required for --kind appimage")
    for p in (args.archive, args.appdir):
        if p and not os.path.exists(p):
            print("cannot verify: %s does not exist" % p)
            return 2
    if not os.path.isdir(os.path.join(args.repo, "native")):
        print("cannot verify: --repo %s is not an EverythingBox checkout (no native/ under it), so the "
              "manifest could not be derived" % args.repo)
        return 2
    return run_check(args.kind, args.archive, args.appdir, args.repo)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
