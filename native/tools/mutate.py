#!/usr/bin/env python3
"""Mutation-testing driver for this repo (issue #175).

An assertion is only proven by breaking the behaviour it guards and watching it go red. Everyone here has
written that loop by hand, and every hand-rolled copy has rediscovered the same trap, so this is the one
copy. Point it at a source file, an anchor, a replacement, a build command and a test command, and it
reports one of THREE outcomes:

    KILLED       the mutation applied, the tree rebuilt, the test went red -> the assertion discriminates
    SURVIVED     the mutation applied, the tree rebuilt, the test stayed green -> audit that assertion
    NOT APPLIED  no verdict was reached at all

The third one is the whole point of this file. **A run containing any NOT APPLIED is a failed run, not a
result.** An unapplied mutation is indistinguishable from a surviving one from the outside: the test passes,
because the code under it never changed. A hand-rolled driver reports that as SURVIVED, which reads as
"this assertion is inert" -- and that is the verdict that gets a WORKING assertion deleted. It happened
three times in one day across three independent agents (issues #123, #151, #164), every time on the same
cause, and every time after reading a document that warned about it. Writing it down did not stop it. This
did not need a rule, it needed a tool.

The five things it does that a hand-rolled script does not:

1. **Line endings do not matter.** This repo's working tree is CRLF (`core.autocrlf=true`, no
   `.gitattributes`), so a multi-line pattern written with "\\n" -- which is what you get from a Python
   string, a heredoc, or anything typed on a Unix-shaped keyboard -- matches NOTHING. Every anchor is
   compiled with each of its line breaks turned into `(?:\\r\\n|\\n)`, so the same anchor applies whichever
   way it was spelled, against a file stored either way. The replacement is re-encoded to the line endings
   the file actually uses, so nothing outside the mutated span is rewritten.

2. **Non-application is reported distinctly from survival.** See above. It is also fatal to the run's exit
   status (2), so a matrix cannot be quoted as a result while some of it never ran.

3. **The edit is verified to have landed before the test runs.** The file is re-read from disk and compared
   byte-for-byte with what it held before; `git diff` is consulted as a cross-check. The byte compare is
   the authority, not git: with `core.autocrlf=true` git compares NORMALISED content, so a change that is
   purely line endings shows up as no diff at all -- the exact blind spot this file exists to cover.

4. **The source is restored with a REFRESHED timestamp.** Restoring a backup with `mv` (or `copy`, or
   `shutil.move`) carries the backup's OLD mtime back onto the source. MSBuild then decides the object file
   is newer than the source, skips the compile, and the next test runs the MUTATED binary against a
   pristine tree -- so the following mutant's verdict is about the previous mutant's code. That is a
   separate documented trap in this repo and it belongs in the same tool as the first one. Restore here is
   a write plus `os.utime(path, None)`, and the restore is verified.

5. **A drifted anchor stops the run.** Zero matches is fatal, and so is more matches than declared. An
   anchor that has drifted must not silently mutate nothing, and must not silently mutate a site you did
   not mean. Say how many times you expect to match (default 1) and it is checked.

Four further "the harness did nothing" checks, because a build or a test that did not run is also not a
result:

* a build command that exits non-zero is NOT a kill -- it is `NOT APPLIED (BUILD FAILED)`. A mutant that
  does not compile tells you nothing about the assertion;
* declare `artifact` (the binary the build produces) and its mtime must ADVANCE across the build, or the
  run stops with `NOT APPLIED (ARTIFACT NOT REBUILT)`. That is trap 4 caught from the other side: if the
  build no-ops, whatever the test then runs is stale;
* **the UNMUTATED test must pass first.** Before any mutant is scored, every test command the matrix will
  judge by runs once against the pristine tree and must exit 0 AND print its sentinel. If it does not, the
  whole run aborts (exit 3) with "environment broken, not a mutation result" and NOTHING is scored. This
  exists because it happened live (2026-08-19): a shell without Qt's bin on PATH ran a matrix to
  "6 KILLED, exit 0" while every probe had died 0xC0000135 (STATUS_DLL_NOT_FOUND) before main() -- every
  "kill" was the loader failing, and the sentinel check never fired because it only inspects exit-0 runs;
* a mutant test run that exits with a loader-death NTSTATUS (0xC0000135 DLL not found, 0xC0000139 entry
  point not found, 0xC0000142 DLL init failed) is `NOT APPLIED (TEST DIED IN THE LOADER)`, never KILLED --
  the binary ran zero assertions, so the environment broke mid-matrix.

Usage
-----

    native/tools/mutate.py --spec my-matrix.json
    native/tools/mutate.py --file native/src/core/Foo.cpp --find "a\\nb" --replace "b" \\
                           --build "<build cmd>" --test "<test cmd>" --artifact build/Release/probe_foo.exe
    native/tools/mutate.py --selftest        # prove the three outcomes still discriminate

Spec format (JSON; paths are relative to --root, which defaults to the repo root):

    {
      "build":  "cmake --build build --config Release --target probe_marks --parallel",
      "test":   "build/Release/probe_marks.exe",
      "artifact": "build/Release/probe_marks.exe",
      "sentinel": "MARKS-OK",
      "shell":  "auto",                     // or "bash" to pin Git Bash (never WSL's)
      "env":    { "QT_QPA_PLATFORM": "offscreen" },
      "mutants": [
        {
          "name":    "husk-guard-dropped",
          "file":    "native/src/core/ItemMarks.cpp",
          "find":    "...two lines...",     // "\\n" is fine; so is "\\r\\n"
          "replace": "...one line...",
          "count":   1,                     // expected occurrences; a mismatch is fatal
          "regex":   false,                 // literal by default
          "expect":  "killed"               // optional; a verdict that differs fails the run
        }
      ]
    }

`build`, `test`, `artifact` and `sentinel` may be overridden per mutant. Exit status: 0 = every mutant
produced a verdict and matched whatever it declared; 1 = every mutant produced a verdict but one differed
from its declared `expect`; 2 = at least one mutant produced NO verdict, i.e. this run is not a result;
3 = the BASELINE failed -- the test does not pass on the unmutated tree, the environment is broken, and
nothing was mutated or scored at all.
"""

import argparse
import contextlib
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

# Every line break in an anchor becomes this. It is the single line of code the three incidents in #123,
# #151 and #164 were missing.
EOL_ALT = r"(?:\r\n|\n)"

KILLED = "KILLED"
SURVIVED = "SURVIVED"
NOT_APPLIED = "NOT APPLIED"
ENV_BROKEN = "ENV BROKEN"          # the baseline failed: nothing was mutated, nothing was scored

# NTSTATUS codes the Windows loader exits with BEFORE main() ever runs. A process that dies like this ran
# ZERO assertions, so its non-zero exit is an environment failure, never a kill. Observed live 2026-08-19:
# a shell without Qt's bin on PATH ran a whole matrix to "6 KILLED 0 SURVIVED 0 NOT APPLIED", exit 0, while
# every single probe run had died 0xC0000135 before reaching main.
LOADER_STATUS = {
    0xC0000135: "STATUS_DLL_NOT_FOUND",
    0xC0000139: "STATUS_ENTRYPOINT_NOT_FOUND",
    0xC0000142: "STATUS_DLL_INIT_FAILED",
}


def loader_failure(rc):
    """The NTSTATUS name if this exit code is a loader death, else None."""
    return LOADER_STATUS.get(rc & 0xFFFFFFFF)


# ---------------------------------------------------------------------------------------------------------
# bytes <-> text, losslessly. surrogateescape round-trips any byte sequence, so a source file with a stray
# non-UTF-8 byte in a comment is restored exactly as it was rather than being silently re-encoded.
# ---------------------------------------------------------------------------------------------------------

def read_bytes(path):
    with open(path, "rb") as fh:
        return fh.read()


def write_bytes(path, data):
    with open(path, "wb") as fh:
        fh.write(data)


def decode(data):
    return data.decode("utf-8", "surrogateescape")


def encode(text):
    return text.encode("utf-8", "surrogateescape")


def dominant_eol(text):
    """Which line ending this file is written in. Used to re-encode the REPLACEMENT only."""
    crlf = text.count("\r\n")
    lf = text.count("\n") - crlf
    return "\r\n" if crlf > lf else "\n"


def anchor_regex(pattern, is_regex=False):
    """Compile an anchor that matches regardless of how either side spells a line break.

    The pattern's own line endings are normalised first (so "\\r\\n" and "\\n" spellings of the same anchor
    compile to the same thing), then each break becomes EOL_ALT.

    Literal mode is the default and is what you want: `re.escape` means an anchor full of `()`, `*`, `[]`
    and `.` -- i.e. C++ -- needs no thought. `re.escape` escapes a newline as backslash+newline, which is
    the two-character sequence replaced below.
    """
    pat = pattern.replace("\r\n", "\n")
    if is_regex:
        # A literal newline typed into a regex pattern becomes the alternation too. Regex mode is for
        # anchors that genuinely need alternation or classes; if yours does not, leave it literal.
        body = pat.replace("\n", EOL_ALT)
    else:
        body = re.escape(pat).replace("\\\n", EOL_ALT)
    return re.compile(body)


def encode_replacement(replacement, eol):
    return replacement.replace("\r\n", "\n").replace("\n", eol)


# ---------------------------------------------------------------------------------------------------------
# Command running
# ---------------------------------------------------------------------------------------------------------

def resolve_bash():
    """Git Bash, explicitly -- never WSL's.

    A mutation driver once reported 6/6 KILLED having built nothing at all, because Python resolved `bash`
    to WSL's, the build could not run there, and every non-zero exit read as a kill. If a spec asks for
    bash, it gets a bash that can see this checkout or it gets an error.
    """
    if os.name != "nt":
        found = shutil.which("bash")
        if not found:
            raise RuntimeError("no bash on PATH")
        return found
    candidates = [
        r"C:\Program Files\Git\bin\bash.exe",
        r"C:\Program Files\Git\usr\bin\bash.exe",
        r"C:\Program Files (x86)\Git\bin\bash.exe",
    ]
    for c in candidates:
        if os.path.isfile(c):
            return c
    found = shutil.which("bash")
    if found and "system32" not in found.lower():
        return found
    raise RuntimeError(
        "could not find Git Bash. `bash` on PATH resolves to %r, which is WSL's -- it cannot build this "
        "checkout, and every failure there would read as a kill. Install Git Bash or drop \"shell\": "
        "\"bash\" from the spec." % (found,))


def run_cmd(cmd, cwd, env, shell_kind, timeout=None):
    """Returns (returncode, combined_output). Never raises on a non-zero exit -- that is data here."""
    full_env = dict(os.environ)
    full_env.update({k: str(v) for k, v in (env or {}).items()})
    if isinstance(cmd, list):
        argv, use_shell = list(cmd), False
        # A spec naturally writes "build/Release/probe_marks.exe". CreateProcess will not find a RELATIVE
        # path spelled with forward slashes, and the error it raises ("cannot find the file specified")
        # reads like a missing binary rather than a path-shape problem -- so resolve argv[0] against cwd
        # when it names a file that is really there.
        if argv and not os.path.isabs(argv[0]) and ("/" in argv[0] or "\\" in argv[0]):
            cand = os.path.normpath(os.path.join(cwd, argv[0]))
            if os.path.isfile(cand):
                argv[0] = cand
    elif shell_kind == "bash":
        argv, use_shell = [resolve_bash(), "-c", cmd], False
    else:
        argv, use_shell = cmd, True
    try:
        proc = subprocess.run(argv, cwd=cwd, env=full_env, shell=use_shell, timeout=timeout,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    except subprocess.TimeoutExpired:
        return 124, "TIMEOUT after %ss" % timeout
    return proc.returncode, proc.stdout.decode("utf-8", "replace")


# ---------------------------------------------------------------------------------------------------------
# git cross-check
# ---------------------------------------------------------------------------------------------------------

def git_tracked(root, rel):
    rc, _ = run_cmd(["git", "ls-files", "--error-unmatch", "--", rel], root, None, "auto")
    return rc == 0


def git_has_diff(root, rel):
    """True if git sees a difference. NOT the authority -- see the module docstring, point 3."""
    rc, _ = run_cmd(["git", "diff", "--quiet", "--", rel], root, None, "auto")
    return rc == 1


# ---------------------------------------------------------------------------------------------------------
# Results
# ---------------------------------------------------------------------------------------------------------

class Result(object):
    def __init__(self, name, verdict, reason="", detail=None):
        self.name = name
        self.verdict = verdict          # KILLED / SURVIVED / NOT_APPLIED
        self.reason = reason            # why, for NOT APPLIED; empty otherwise
        self.detail = detail or {}

    @property
    def applied(self):
        return self.verdict != NOT_APPLIED

    def __repr__(self):
        return "<Result %s %s %s>" % (self.name, self.verdict, self.reason)


def say(msg=""):
    sys.stdout.write(msg + "\n")
    sys.stdout.flush()


# ---------------------------------------------------------------------------------------------------------
# One mutant
# ---------------------------------------------------------------------------------------------------------

def run_mutant(m, spec, root, index, total, verbose):
    name = m.get("name") or ("mutant-%d" % index)
    rel = m["file"]
    path = os.path.join(root, rel)
    find = m["find"]
    replace = m.get("replace", "")
    expected_count = int(m.get("count", 1))
    is_regex = bool(m.get("regex", False))

    build = m.get("build", spec.get("build"))
    test = m.get("test", spec.get("test"))
    artifact = m.get("artifact", spec.get("artifact"))
    sentinel = m.get("sentinel", spec.get("sentinel"))
    shell_kind = m.get("shell", spec.get("shell", "auto"))
    env = dict(spec.get("env") or {})
    env.update(m.get("env") or {})
    timeout = m.get("timeout", spec.get("timeout"))

    say("[%d/%d] %s" % (index, total, name))
    say("      file     %s" % rel)

    if not os.path.isfile(path):
        say("      => %s (NO SUCH FILE)" % NOT_APPLIED)
        say()
        return Result(name, NOT_APPLIED, "NO SUCH FILE: %s" % rel)

    orig_bytes = read_bytes(path)
    orig_mtime = os.path.getmtime(path)
    text = decode(orig_bytes)
    eol = dominant_eol(text)

    tracked = git_tracked(root, rel)
    dirty_before = git_has_diff(root, rel) if tracked else False

    # ---- anchor ------------------------------------------------------------------------------------------
    try:
        rx = anchor_regex(find, is_regex)
    except re.error as exc:
        say("      => %s (BAD ANCHOR REGEX: %s)" % (NOT_APPLIED, exc))
        say()
        return Result(name, NOT_APPLIED, "BAD ANCHOR REGEX: %s" % exc)

    matches = list(rx.finditer(text))
    say("      anchor   matched %d time(s), expected %d [file is %s]"
        % (len(matches), expected_count, "CRLF" if eol == "\r\n" else "LF"))
    if len(matches) != expected_count:
        if len(matches) == 0:
            reason = ("ANCHOR MATCHED 0 TIMES -- it has drifted, or the file is not the one you think. "
                      "Line endings are NOT the cause here: the anchor was compiled EOL-agnostic.")
        else:
            reason = ("ANCHOR MATCHED %d TIMES, EXPECTED %d -- it is ambiguous, so mutating it would hit a "
                      "site you did not choose. Lengthen the anchor, or set \"count\"."
                      % (len(matches), expected_count))
        say("      => %s (%s)" % (NOT_APPLIED, reason.split(" -- ")[0]))
        say("         %s" % reason.split(" -- ", 1)[-1])
        say("      source untouched")
        say()
        return Result(name, NOT_APPLIED, reason)

    # ---- apply -------------------------------------------------------------------------------------------
    repl_text = encode_replacement(replace, eol)
    new_text = text
    for mt in reversed(matches):
        new_text = new_text[:mt.start()] + repl_text + new_text[mt.end():]
    new_bytes = encode(new_text)

    # The only copy of the pristine file is in memory. An exception or a Ctrl-C is covered by the `finally`
    # below; a hard kill is not, and a source left silently mutated is the worst outcome this file has, so
    # say where the undo is BEFORE touching anything.
    say("      mutating -- if this run is killed, restore with: git checkout -- %s" % rel)
    try:
        write_bytes(path, new_bytes)
        # The MUTATED file gets a fresh timestamp too, not just the restore: a mutation written with a
        # timestamp the build system considers old is a mutation the build system skips.
        os.utime(path, None)

        # ---- verify the edit LANDED, before anything is built or run ------------------------------------
        on_disk = read_bytes(path)
        if on_disk != new_bytes:
            return _restore_and(path, orig_bytes, orig_mtime, name, NOT_APPLIED,
                                "WRITE DID NOT PERSIST -- the file on disk is not what was written")
        if on_disk == orig_bytes:
            return _restore_and(path, orig_bytes, orig_mtime, name, NOT_APPLIED,
                                "EDIT DID NOT LAND -- the file is byte-identical to before. The replacement "
                                "is the same text as the anchor, so nothing was mutated.")
        delta = abs(len(on_disk) - len(orig_bytes))
        gitnote = "not tracked by git"
        if tracked:
            if dirty_before:
                gitnote = "git diff non-empty (file was ALREADY modified before this run, so uninformative)"
            elif git_has_diff(root, rel):
                gitnote = "git diff non-empty"
            else:
                return _restore_and(path, orig_bytes, orig_mtime, name, NOT_APPLIED,
                                    "GIT SEES NO CHANGE though the bytes differ -- the mutation is purely "
                                    "line endings (core.autocrlf normalises them away), so it changes no code")
        say("      applied  verified on disk: %d byte(s) of length change, %s" % (delta, gitnote))

        # ---- build ---------------------------------------------------------------------------------------
        if build:
            art_path = os.path.join(root, artifact) if artifact else None
            art_before = os.path.getmtime(art_path) if art_path and os.path.isfile(art_path) else None
            rc, out = run_cmd(build, root, env, shell_kind, timeout)
            if verbose:
                say(_indent(out))
            if rc != 0:
                return _restore_and(path, orig_bytes, orig_mtime, name, NOT_APPLIED,
                                    "BUILD FAILED (exit %d) -- a mutant that does not compile says nothing "
                                    "about the assertion. Last output:\n%s" % (rc, _tail(out)))
            if art_path:
                if not os.path.isfile(art_path):
                    return _restore_and(path, orig_bytes, orig_mtime, name, NOT_APPLIED,
                                        "ARTIFACT MISSING after a successful build: %s" % artifact)
                art_after = os.path.getmtime(art_path)
                if art_before is not None and art_after <= art_before:
                    return _restore_and(
                        path, orig_bytes, orig_mtime, name, NOT_APPLIED,
                        "ARTIFACT NOT REBUILT -- %s did not change mtime across the build, so whatever the "
                        "test is about to run is the PREVIOUS binary. Usually a stale source timestamp."
                        % artifact)
                say("      build    ok, %s rebuilt" % os.path.basename(artifact))
            else:
                say("      build    ok (no artifact declared -- freshness unchecked)")

        # ---- test ----------------------------------------------------------------------------------------
        if not test:
            return _restore_and(path, orig_bytes, orig_mtime, name, NOT_APPLIED,
                                "NO TEST COMMAND -- nothing to reach a verdict with")
        rc, out = run_cmd(test, root, env, shell_kind, timeout)
        if verbose:
            say(_indent(out))
        say("      test     exit %d" % rc)
        loader = loader_failure(rc)
        if loader:
            verdict, reason = NOT_APPLIED, (
                "TEST DIED IN THE LOADER (exit 0x%08X, %s) -- the binary never reached main(), so no "
                "assertion ran. A missing DLL is an environment failure, never a kill."
                % (rc & 0xFFFFFFFF, loader))
        elif rc != 0:
            verdict, reason = KILLED, ""
        elif sentinel and sentinel not in out:
            verdict, reason = NOT_APPLIED, ("TEST EXITED 0 WITHOUT PRINTING %r -- it did not run to "
                                            "completion, so \"green\" is not a verdict" % sentinel)
        else:
            verdict, reason = SURVIVED, ""
        return _restore_and(path, orig_bytes, orig_mtime, name, verdict, reason)
    finally:
        # Belt and braces: if anything above threw, the source still goes back. _restore_and is idempotent.
        if read_bytes(path) != orig_bytes:
            _restore(path, orig_bytes, orig_mtime)


def _indent(text, prefix="      | "):
    return "".join(prefix + ln for ln in text.splitlines(True)) or (prefix + "(no output)")


def _tail(text, lines=15):
    return "\n".join(text.splitlines()[-lines:])


def _restore(path, orig_bytes, orig_mtime):
    """Put the source back AND refresh its timestamp. The refresh is not cosmetic -- see docstring point 4."""
    write_bytes(path, orig_bytes)
    os.utime(path, None)            # NOW, not the backup's mtime
    back = read_bytes(path)
    if back != orig_bytes:
        raise RuntimeError("RESTORE FAILED for %s -- the tree is left MUTATED. Fix by hand before "
                           "trusting anything else." % path)
    if os.path.getmtime(path) < orig_mtime:
        raise RuntimeError("RESTORE left %s with an mtime older than it started with; the next build would "
                           "skip it." % path)


def _restore_and(path, orig_bytes, orig_mtime, name, verdict, reason):
    _restore(path, orig_bytes, orig_mtime)
    if verdict == NOT_APPLIED:
        head = reason.split(" -- ")[0].split("\n")[0]
        say("      => %s (%s)" % (NOT_APPLIED, head))
        rest = reason.split(" -- ", 1)
        if len(rest) > 1:
            say("         %s" % rest[1])
    else:
        say("      => %s" % verdict)
    say("      restored, timestamp refreshed")
    say()
    return Result(name, verdict, reason)


# ---------------------------------------------------------------------------------------------------------
# The baseline gate -- the unmutated test must go green ONCE before anything is scored
# ---------------------------------------------------------------------------------------------------------

def _cmd_str(cmd):
    return cmd if isinstance(cmd, str) else subprocess.list2cmdline(cmd)


def baseline_combos(spec, mutants):
    """Every distinct (test, sentinel, env, shell) a mutant in this run will be judged by."""
    combos, seen = [], set()
    for m in mutants:
        combo = {
            "build": m.get("build", spec.get("build")),
            "test": m.get("test", spec.get("test")),
            "artifact": m.get("artifact", spec.get("artifact")),
            "sentinel": m.get("sentinel", spec.get("sentinel")),
            "shell": m.get("shell", spec.get("shell", "auto")),
            "timeout": m.get("timeout", spec.get("timeout")),
            "env": dict(spec.get("env") or {}, **(m.get("env") or {})),
        }
        key = json.dumps(combo, sort_keys=True, default=str)
        if key not in seen and combo["test"]:
            seen.add(key)
            combos.append(combo)
    return combos


def run_baselines(spec, mutants, root, verbose):
    """Run every test command this matrix will score by, against the UNMUTATED tree, before any mutant.

    Each must exit 0 and print its sentinel. One that does not is not a mutation result waiting to happen --
    it is a broken environment, and every 'kill' downstream of it would have been the environment dying, not
    an assertion going red. That exact run happened live (2026-08-19): no Qt on PATH, every probe exit
    0xC0000135 before main(), a full matrix reported as KILLED across the board. This gate makes that shape
    a loud abort instead.

    Returns [] when every baseline is green, else one ENV_BROKEN Result per failing baseline -- and the
    caller scores NOTHING.
    """
    failures = []
    combos = baseline_combos(spec, mutants)
    if not combos:
        return failures
    say("=== baseline: the UNMUTATED test must pass before any mutant is scored (%d command(s)) ==="
        % len(combos))
    for c in combos:
        # A fresh tree may never have built the probe at all; that is not a broken environment yet. Build
        # once if the declared artifact is missing -- but only then, so a baseline against an existing
        # binary stays cheap and a stale binary is still surfaced by the test below.
        if c["build"] and c["artifact"] and not os.path.isfile(os.path.join(root, c["artifact"])):
            say("baseline: %s is missing, building it first" % c["artifact"])
            rc, out = run_cmd(c["build"], root, c["env"], c["shell"], c["timeout"])
            if verbose:
                say(_indent(out))
            if rc != 0:
                failures.append(Result("<baseline>", ENV_BROKEN,
                                       "the UNMUTATED tree does not build (exit %d). Last output:\n%s"
                                       % (rc, _tail(out))))
                say("baseline FAILED: %s" % _cmd_str(c["build"]))
                continue
        rc, out = run_cmd(c["test"], root, c["env"], c["shell"], c["timeout"])
        if verbose:
            say(_indent(out))
        loader = loader_failure(rc)
        if loader:
            why = ("exit 0x%08X (%s): the test binary died in the loader BEFORE main() -- a missing DLL "
                   "(is Qt's bin on PATH?), not a test failure" % (rc & 0xFFFFFFFF, loader))
        elif rc != 0:
            why = "exit %d on the UNMUTATED tree. Last output:\n%s" % (rc, _tail(out))
        elif c["sentinel"] and c["sentinel"] not in out:
            why = ("exit 0 but never printed %r on the UNMUTATED tree -- whatever this command runs, it is "
                   "not the test the sentinel belongs to" % c["sentinel"])
        else:
            say("baseline ok: %s" % _cmd_str(c["test"]))
            continue
        failures.append(Result("<baseline>", ENV_BROKEN, why))
        say("baseline FAILED: %s" % _cmd_str(c["test"]))
        say(_indent(why.split("\n")[0]))
    if failures:
        say()
        say("!" * 104)
        say("ENVIRONMENT BROKEN, NOT A MUTATION RESULT. The test command above fails on the UNMUTATED")
        say("tree, so every KILLED this matrix could have produced would have been the environment dying,")
        say("not an assertion going red -- the exact shape of the 2026-08-19 fake pass (Qt bin off PATH,")
        say("every probe exit 0xC0000135 before main, 'N KILLED' proving nothing). NOTHING was mutated and")
        say("NO verdicts exist. Fix the environment and run the whole matrix again.")
        say("!" * 104)
        say()
    else:
        say()
    return failures


# ---------------------------------------------------------------------------------------------------------
# The matrix
# ---------------------------------------------------------------------------------------------------------

def run_matrix(spec, root, only=None, verbose=False, final_build=True):
    mutants = spec.get("mutants") or []
    if only:
        mutants = [m for m in mutants if m.get("name") in only]
    baseline_failures = run_baselines(spec, mutants, root, verbose)
    if baseline_failures:
        return baseline_failures
    results = []
    total = len(mutants)
    for i, m in enumerate(mutants, 1):
        results.append(run_mutant(m, spec, root, i, total, verbose))

    # The tree's SOURCE is pristine again, but its BINARIES are the last mutant's. Leaving those behind is
    # how a probe suite comes to run a mutated binary against clean source and report on code that does not
    # exist -- and the suite would then be reporting a PASS or a FAIL about code nobody can read.
    #
    # EVERY distinct build command an applied mutant used is re-run, not just the spec-level one. A matrix
    # that gives each mutant its own build/target (a spec with no top-level `build` at all is the normal
    # shape for a cross-probe matrix) would otherwise skip this silently and leave a mutated probe behind
    # for the next person's suite run to trip over.
    if final_build:
        builds = []
        for m, r in zip(mutants, results):
            if not r.applied:
                continue                       # nothing was built for a mutant that never applied
            b = m.get("build", spec.get("build"))
            if not b:
                continue
            key = (json.dumps(b), m.get("artifact", spec.get("artifact")))
            if key not in [k for k, _ in builds]:
                builds.append((key, m))
        if builds:
            say("=== final rebuild from the restored source (%d build command(s)) ===" % len(builds))
        for (_, art), m in builds:
            b = m.get("build", spec.get("build"))
            env = dict(spec.get("env") or {})
            env.update(m.get("env") or {})
            art_path = os.path.join(root, art) if art else None
            before = os.path.getmtime(art_path) if art_path and os.path.isfile(art_path) else None
            rc, out = run_cmd(b, root, env, m.get("shell", spec.get("shell", "auto")),
                              m.get("timeout", spec.get("timeout")))
            if rc != 0:
                say("FAIL: the restored source does not build (exit %d). The tree is NOT clean." % rc)
                say(_tail(out))
                results.append(Result("<final rebuild>", NOT_APPLIED, "RESTORED SOURCE DOES NOT BUILD"))
            elif art_path and before is not None and os.path.getmtime(art_path) <= before:
                say("FAIL: %s was NOT rebuilt from the restored source. Its timestamp did not advance, so "
                    "the binary left behind is a mutant's." % art)
                results.append(Result("<final rebuild>", NOT_APPLIED, "ARTIFACT NOT REBUILT AFTER RESTORE"))
            else:
                say("ok -- %s matches the restored source again." % (art or "the tree's binaries"))
        if builds:
            say()
    return results


def summarise(results):
    say("=" * 104)
    say("%-40s %-12s %s" % ("MUTANT", "VERDICT", "NOTE"))
    say("-" * 104)
    for r in results:
        note = r.reason.split(" -- ")[0].split("\n")[0] if r.reason else ""
        say("%-40s %-12s %s" % (r.name[:40], r.verdict, note[:50]))
    say("=" * 104)
    killed = [r for r in results if r.verdict == KILLED]
    survived = [r for r in results if r.verdict == SURVIVED]
    notapplied = [r for r in results if r.verdict == NOT_APPLIED]
    broken = [r for r in results if r.verdict == ENV_BROKEN]
    say("%d KILLED   %d SURVIVED   %d NOT APPLIED" % (len(killed), len(survived), len(notapplied)))
    say()
    if broken:
        say("ENVIRONMENT BROKEN: the baseline failed, so no mutant was scored and nothing above is a")
        say("mutation verdict. This is not a mutation result.")
        say()
    if notapplied:
        say("!" * 104)
        say("THIS RUN IS NOT A RESULT. %d of %d mutants never reached a verdict." % (len(notapplied),
                                                                                     len(results)))
        say("A mutant that did not apply is NOT a survivor. The test passed because the code it tests was")
        say("never changed -- reading that as 'the assertion is inert' is how a working assertion gets")
        say("deleted. Fix the anchors listed above and run the whole matrix again.")
        say("!" * 104)
        say()
    if survived:
        say("Survivors need a decision, one each. An assertion no mutation kills is either inert -- fix it")
        say("or delete it -- or a deliberate absence-of-behaviour tripwire, in which case say so in a")
        say("comment where it lives, so the next reader does not have to rediscover it:")
        for r in survived:
            say("  - %s" % r.name)
        say()
    return killed, survived, notapplied


def exit_code_for(results, spec_mutants):
    _, _, notapplied = ([r for r in results if r.verdict == KILLED],
                        [r for r in results if r.verdict == SURVIVED],
                        [r for r in results if r.verdict == NOT_APPLIED])
    if any(r.verdict == ENV_BROKEN for r in results):
        return 3
    if notapplied:
        return 2
    expected = {m.get("name"): m.get("expect") for m in spec_mutants if m.get("expect")}
    mismatched = []
    for r in results:
        want = expected.get(r.name)
        if want and want.strip().upper().replace("_", " ") != r.verdict:
            mismatched.append((r.name, want, r.verdict))
    if mismatched:
        say("Declared expectations not met:")
        for n, w, g in mismatched:
            say("  - %s: expected %s, got %s" % (n, w.upper(), g))
        say()
        return 1
    return 0


# ---------------------------------------------------------------------------------------------------------
# Selftest -- proof the three outcomes still discriminate, run by the probe suite
# ---------------------------------------------------------------------------------------------------------

SELFTEST_SOURCE = (
    "// a stand-in for a source file, deliberately written CRLF\r\n"
    "int limit()\r\n"
    "{\r\n"
    "    return 7;\r\n"
    "}\r\n"
    "\r\n"
    "bool guard(int n)\r\n"
    "{\r\n"
    "    if (n < 0) return false;\r\n"
    "    return n <= limit();\r\n"
    "}\r\n"
    "\r\n"
    "const char* label() { return \"WIDGET\"; }\r\n"
    "int a = 1; // repeated\r\n"
    "int b = 1; // repeated\r\n"
    "int c = 1; // repeated\r\n"
)

# "build" copies the source to an artifact; "test" reads the ARTIFACT, never the source -- so a build that
# does not run means the test looks at stale content, exactly like a probe exe.
SELFTEST_BUILD = (
    "import shutil, sys, os\n"
    "shutil.copyfile('subject.txt', 'artifact.txt')\n"
    "os.utime('artifact.txt', None)\n"
)

# Asserts two properties of the artifact: the guard rejects negatives, and the limit is 7. It says NOTHING
# about label() -- that is the genuine survivor.
SELFTEST_TEST = (
    "import sys\n"
    "src = open('artifact.txt', 'rb').read().decode()\n"
    "ok = True\n"
    "if 'if (n < 0) return false;' not in src: ok = False; print('FAIL: negative guard gone')\n"
    "if 'return 7;' not in src: ok = False; print('FAIL: limit is not 7')\n"
    "print('SUBJECT-OK' if ok else 'SUBJECT-FAIL')\n"
    "sys.exit(0 if ok else 1)\n"
)


def _selftest_spec(tmp, build=None):
    py = sys.executable
    return {
        "build": build if build is not None else '"%s" build.py' % py,
        "test": '"%s" test.py' % py,
        "artifact": "artifact.txt",
        "sentinel": "SUBJECT-OK",
        "mutants": [],
    }


def selftest(verbose=False):
    """Every claim in this file's docstring, checked. Prints MUTATE-SELFTEST-OK and returns 0 on success.

    The matrices it drives are captured rather than echoed -- the probe suite runs this on every push and a
    passing gate should be one line, not eighty. Anything captured is replayed if a check fails, and
    --verbose prints it either way.
    """
    failures = []
    transcripts = []

    def quiet_matrix(spec, root):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            res = run_matrix(spec, root, verbose=False, final_build=False)
        transcripts.append(buf.getvalue())
        if verbose:
            say(_indent(buf.getvalue(), "  | "))
        return res

    def check(cond, what):
        if not cond:
            failures.append(what)
            say("  FAIL: %s" % what)
        else:
            say("  ok:   %s" % what)

    tmp = tempfile.mkdtemp(prefix="mutate-selftest-")
    try:
        subject = os.path.join(tmp, "subject.txt")
        write_bytes(subject, SELFTEST_SOURCE.encode())
        write_bytes(os.path.join(tmp, "build.py"), SELFTEST_BUILD.encode())
        write_bytes(os.path.join(tmp, "test.py"), SELFTEST_TEST.encode())
        pristine = read_bytes(subject)
        # The baseline gate runs the TEST before anything is built, so the artifact must exist up front --
        # exactly like the real workflow, where the probe was built green before anyone mutates it.
        artifact = os.path.join(tmp, "artifact.txt")
        write_bytes(artifact, pristine)

        # An anchor spanning a line break, written the way a Python string or a heredoc writes it: "\n".
        # The subject on disk is CRLF. This is the exact shape that produced three phantom survivors.
        lf_anchor = "    if (n < 0) return false;\n    return n <= limit();"
        crlf_anchor = lf_anchor.replace("\n", "\r\n")
        killing_replacement = "    return n <= limit();"

        say("--- 0. the trap itself is real (a naive driver would report SURVIVED here) ---")
        naive = decode(pristine).replace(lf_anchor, killing_replacement)
        check(naive == decode(pristine),
              "a plain str.replace of the '\\n' anchor changes NOTHING in the CRLF file")
        check(len(list(anchor_regex(lf_anchor).finditer(decode(pristine)))) == 1,
              "the same anchor compiled here matches exactly once")
        check(len(list(anchor_regex(crlf_anchor).finditer(decode(pristine)))) == 1,
              "the '\\r\\n' spelling of the same anchor also matches exactly once")

        say("--- 1..8. the outcome matrix ---")
        spec = _selftest_spec(tmp)
        spec["mutants"] = [
            # 1. multi-line "\n" anchor against a CRLF file, breaking an asserted behaviour.
            {"name": "lf-anchor-kills", "file": "subject.txt", "find": lf_anchor,
             "replace": killing_replacement, "expect": "killed"},
            # 2. the same anchor spelled with "\r\n" -- both spellings must work.
            {"name": "crlf-anchor-kills", "file": "subject.txt", "find": crlf_anchor,
             "replace": killing_replacement, "expect": "killed"},
            # 3. single-line kill, for completeness.
            {"name": "limit-kills", "file": "subject.txt", "find": "return 7;",
             "replace": "return 9;", "expect": "killed"},
            # 4. a genuine survivor: the test says nothing about label().
            {"name": "label-survives", "file": "subject.txt", "find": "return \"WIDGET\";",
             "replace": "return \"GADGET\";", "expect": "survived"},
            # 5. a drifted multi-line anchor: zero matches, fatal.
            {"name": "drifted-anchor", "file": "subject.txt",
             "find": "    if (n < 1) return false;\n    return n <= ceiling();",
             "replace": "    return true;"},
            # 6. an ambiguous anchor: three matches where one was declared, fatal.
            {"name": "ambiguous-anchor", "file": "subject.txt", "find": "int a = 1; // repeated",
             "replace": "int a = 2; // repeated", "count": 1},
            # 7. a replacement identical to the anchor: the write "succeeds" and changes nothing.
            {"name": "noop-edit", "file": "subject.txt", "find": "return 7;", "replace": "return 7;"},
            # 8. a build that does not rebuild the artifact -- whatever the test runs is stale.
            {"name": "stale-artifact", "file": "subject.txt", "find": "return 7;", "replace": "return 9;",
             "build": '"%s" -c "pass"' % sys.executable},
        ]
        # 6's anchor really is ambiguous: make it so.
        spec["mutants"][5]["find"] = "= 1; // repeated"
        spec["mutants"][5]["replace"] = "= 2; // repeated"

        before_mtime = os.path.getmtime(subject)
        time.sleep(0.02)
        results = quiet_matrix(spec, tmp)
        by = {r.name: r for r in results}

        check(by["lf-anchor-kills"].verdict == KILLED,
              "a multi-line '\\n' anchor against a CRLF file APPLIES and reports KILLED (not SURVIVED)")
        check(by["crlf-anchor-kills"].verdict == KILLED,
              "the '\\r\\n' spelling of the same anchor reports KILLED too")
        check(by["limit-kills"].verdict == KILLED, "a single-line breaking mutation reports KILLED")
        check(by["label-survives"].verdict == SURVIVED,
              "a mutation the test does not cover reports SURVIVED")
        check(by["drifted-anchor"].verdict == NOT_APPLIED and "0 TIMES" in by["drifted-anchor"].reason,
              "a drifted multi-line anchor reports NOT APPLIED (matched 0 times), never SURVIVED")
        check(by["ambiguous-anchor"].verdict == NOT_APPLIED and "3 TIMES" in by["ambiguous-anchor"].reason,
              "an anchor matching 3 times where 1 was declared reports NOT APPLIED, never a silent mutation")
        check(by["noop-edit"].verdict == NOT_APPLIED and "EDIT DID NOT LAND" in by["noop-edit"].reason,
              "an edit that leaves the file byte-identical reports NOT APPLIED, never SURVIVED")
        check(by["stale-artifact"].verdict == NOT_APPLIED
              and "ARTIFACT NOT REBUILT" in by["stale-artifact"].reason,
              "a build that does not refresh the artifact reports NOT APPLIED, never a verdict on a stale "
              "binary")
        check(NOT_APPLIED not in (by["label-survives"].verdict,),
              "the genuine survivor is not confused with a non-application")
        check(all(r.verdict != SURVIVED for r in results if r.name in
                  ("drifted-anchor", "ambiguous-anchor", "noop-edit", "stale-artifact")),
              "NO non-application anywhere in the matrix was reported as SURVIVED")

        say("--- 9. restore, and the timestamp refresh ---")
        check(read_bytes(subject) == pristine, "the source is byte-identical to how it started")
        check(os.path.getmtime(subject) > before_mtime,
              "the restored source's mtime ADVANCED (a `mv` of a backup would carry the old one back, and "
              "the next build would skip the file)")

        say("--- 10. a run containing a non-application fails ---")
        check(exit_code_for(results, spec["mutants"]) == 2,
              "exit status 2 -- a matrix with any NOT APPLIED is not a result")
        clean = [r for r in results if r.name in ("lf-anchor-kills", "limit-kills", "label-survives")]
        clean_specs = [m for m in spec["mutants"]
                       if m.get("name") in ("lf-anchor-kills", "limit-kills", "label-survives")]
        check(exit_code_for(clean, clean_specs) == 0,
              "exit status 0 when every mutant reached the verdict it declared")
        wrong = [m for m in clean_specs if m["name"] != "label-survives"] + \
                [dict(m, expect="killed") for m in clean_specs if m["name"] == "label-survives"]
        with contextlib.redirect_stdout(io.StringIO()):   # this one deliberately prints a complaint
            code_wrong = exit_code_for(clean, wrong)
        check(code_wrong == 1, "exit status 1 when a verdict differs from the declared expectation")

        say("--- 11. a build failure is not a kill ---")
        spec2 = _selftest_spec(tmp, build='"%s" -c "import sys; sys.exit(3)"' % sys.executable)
        spec2["mutants"] = [{"name": "wont-build", "file": "subject.txt", "find": "return 7;",
                             "replace": "return 9;"}]
        r2 = quiet_matrix(spec2, tmp)[0]
        check(r2.verdict == NOT_APPLIED and "BUILD FAILED" in r2.reason,
              "a mutant that does not build reports NOT APPLIED (BUILD FAILED), never KILLED")
        check(read_bytes(subject) == pristine, "the source is restored after a build failure too")

        say("--- 12. a test that exits 0 without its sentinel is not a survivor ---")
        # This test goes quiet only under mutation (the pristine artifact still earns its sentinel), so the
        # baseline gate passes and the MUTANT-level sentinel check is the one that has to catch it.
        write_bytes(os.path.join(tmp, "quiet.py"),
                    b"import sys\n"
                    b"src = open('artifact.txt', 'rb').read().decode()\n"
                    b"if 'return 7;' in src: print('SUBJECT-OK')\n"
                    b"sys.exit(0)\n")
        write_bytes(artifact, pristine)
        spec3 = _selftest_spec(tmp)
        spec3["test"] = '"%s" quiet.py' % sys.executable
        spec3["mutants"] = [{"name": "silent-green", "file": "subject.txt", "find": "return 7;",
                             "replace": "return 9;"}]
        r3 = quiet_matrix(spec3, tmp)[0]
        check(r3.verdict == NOT_APPLIED and "WITHOUT PRINTING" in r3.reason,
              "a test that exits 0 without printing its sentinel reports NOT APPLIED, never SURVIVED")

        say("--- 13. the tree is left consistent, even when the build is declared per mutant ---")
        # No spec-level "build" at all: the shape a cross-probe matrix takes. The final rebuild has to pick
        # the mutants' own build commands up, or a mutated binary is left sitting there for the next person's
        # suite run -- source clean, binary mutated, and nothing anywhere saying so.
        write_bytes(artifact, pristine)          # case 12 left the artifact mutated (final_build was off)
        spec4 = {"mutants": [{"name": "per-mutant-build", "file": "subject.txt", "find": "return 7;",
                              "replace": "return 9;",
                              "build": '"%s" build.py' % sys.executable,
                              "test": '"%s" test.py' % sys.executable,
                              "artifact": "artifact.txt", "sentinel": "SUBJECT-OK"}]}
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            r4 = run_matrix(spec4, tmp, verbose=False, final_build=True)
        transcripts.append(buf.getvalue())
        check(r4[0].verdict == KILLED and all(x.verdict != NOT_APPLIED for x in r4[1:]),
              "a matrix with no spec-level build still runs the final rebuild (no <final rebuild> failure)")
        check(read_bytes(artifact) == pristine,
              "the artifact left behind is built from the RESTORED source, not from the last mutant")

        say("--- 14. a broken environment is a loud abort, not a page of kills ---")
        # The live incident (2026-08-19): Qt's bin missing from PATH, every probe run dying 0xC0000135
        # BEFORE main(), every non-zero exit scored as KILLED -- a whole matrix "passing" while proving
        # nothing. The unmutated test must go green ONCE before any mutant is scored.
        write_bytes(artifact, pristine)
        spec5 = _selftest_spec(tmp)
        spec5["test"] = '"%s" -c "import sys; sys.exit(2)"' % sys.executable
        spec5["mutants"] = [{"name": "would-be-fake-kill", "file": "subject.txt", "find": "return 7;",
                             "replace": "return 9;", "expect": "killed"}]
        before_subject = read_bytes(subject)
        r5 = quiet_matrix(spec5, tmp)
        check(len(r5) == 1 and r5[0].verdict == ENV_BROKEN,
              "a test failing on the UNMUTATED tree aborts the whole run: no mutant is scored, nothing "
              "reports KILLED")
        check(all(r.verdict != KILLED for r in r5),
              "the abort produced ZERO kill verdicts (the fake pass is impossible)")
        check(read_bytes(subject) == before_subject, "the source was never touched by the aborted run")
        with contextlib.redirect_stdout(io.StringIO()):
            code5 = exit_code_for(r5, spec5["mutants"])
        check(code5 == 3, "exit status 3 -- environment broken, NOT a mutation result")
        check("environment" in (transcripts[-1] if transcripts else "").lower()
              and "not a mutation result" in (transcripts[-1] if transcripts else "").lower(),
              "the abort says 'environment broken, not a mutation result' in so many words")

        spec6 = _selftest_spec(tmp)
        spec6["test"] = '"%s" -c "print(123)"' % sys.executable
        spec6["mutants"] = [{"name": "would-be-fake-kill-2", "file": "subject.txt", "find": "return 7;",
                             "replace": "return 9;"}]
        r6 = quiet_matrix(spec6, tmp)
        check(len(r6) == 1 and r6[0].verdict == ENV_BROKEN,
              "a baseline that exits 0 WITHOUT the sentinel is equally fatal -- the harness is not running "
              "the test it claims to")

        if os.name == "nt":
            say("--- 15. a loader death mid-matrix is not a kill (Windows) ---")
            # The baseline can go green and the environment STILL break under a mutant (PATH edited, DLL
            # deleted mid-run). 0xC0000135-class exits mean the binary never reached main(), so no
            # assertion went red -- that is NOT APPLIED, never KILLED.
            write_bytes(os.path.join(tmp, "loader.py"),
                        b"import sys\n"
                        b"src = open('artifact.txt', 'rb').read().decode()\n"
                        b"if 'return 7;' in src:\n"
                        b"    print('SUBJECT-OK'); sys.exit(0)\n"
                        b"sys.exit(0xC0000135)\n")
            write_bytes(artifact, pristine)
            spec7 = _selftest_spec(tmp)
            spec7["test"] = '"%s" loader.py' % sys.executable
            spec7["mutants"] = [{"name": "loader-death", "file": "subject.txt", "find": "return 7;",
                                 "replace": "return 9;"}]
            r7 = quiet_matrix(spec7, tmp)[0]
            check(r7.verdict == NOT_APPLIED and "LOADER" in r7.reason,
                  "a test exiting 0xC0000135 (STATUS_DLL_NOT_FOUND) reports NOT APPLIED (DIED IN THE "
                  "LOADER), never KILLED")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    say()
    if failures:
        say("MUTATE-SELFTEST-FAIL: %d check(s) failed" % len(failures))
        for f in failures:
            say("  - %s" % f)
        if not verbose:
            say()
            say("--- captured matrices ---")
            for t in transcripts:
                say(_indent(t, "  | "))
        return 1
    say("MUTATE-SELFTEST-OK")
    return 0


# ---------------------------------------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------------------------------------

def repo_root():
    rc, out = run_cmd(["git", "rev-parse", "--show-toplevel"], os.path.dirname(os.path.abspath(__file__)),
                      None, "auto")
    if rc == 0 and out.strip():
        return os.path.normpath(out.strip())
    return os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))


def main(argv):
    ap = argparse.ArgumentParser(
        description="Mutation-testing driver: KILLED / SURVIVED / NOT APPLIED, with the CRLF and stale-"
                    "timestamp traps handled once instead of per script (issue #175).")
    ap.add_argument("--spec", help="JSON matrix (see the module docstring)")
    ap.add_argument("--selftest", action="store_true",
                    help="prove the three outcomes still discriminate; prints MUTATE-SELFTEST-OK")
    ap.add_argument("--root", help="paths in the spec are relative to this (default: the repo root)")
    ap.add_argument("--only", action="append", help="run only the named mutant (repeatable)")
    ap.add_argument("--verbose", action="store_true", help="echo build and test output")
    ap.add_argument("--no-final-build", action="store_true",
                    help="skip the rebuild-from-restored-source at the end. Only when you are about to "
                         "rebuild anyway: without it the tree's binaries are the LAST mutant's.")
    # single-mutant convenience
    ap.add_argument("--file"); ap.add_argument("--find"); ap.add_argument("--replace", default="")
    ap.add_argument("--count", type=int, default=1); ap.add_argument("--regex", action="store_true")
    ap.add_argument("--name"); ap.add_argument("--expect")
    ap.add_argument("--build"); ap.add_argument("--test"); ap.add_argument("--artifact")
    ap.add_argument("--sentinel"); ap.add_argument("--shell", default="auto", choices=["auto", "bash"])
    args = ap.parse_args(argv)

    if args.selftest:
        return selftest(verbose=args.verbose)

    root = os.path.abspath(args.root) if args.root else repo_root()

    if args.spec:
        with open(args.spec, "rb") as fh:
            spec = json.loads(fh.read().decode("utf-8"))
        for k in ("build", "test", "artifact", "sentinel"):
            if getattr(args, k):
                spec[k] = getattr(args, k)
    elif args.file and args.find is not None:
        spec = {"build": args.build, "test": args.test, "artifact": args.artifact,
                "sentinel": args.sentinel, "shell": args.shell,
                "mutants": [{"name": args.name or "mutant-1", "file": args.file, "find": args.find,
                             "replace": args.replace, "count": args.count, "regex": args.regex,
                             "expect": args.expect}]}
    else:
        ap.error("give --spec, or --file with --find, or --selftest")

    say("root: %s" % root)
    say()
    results = run_matrix(spec, root, only=set(args.only) if args.only else None,
                         verbose=args.verbose, final_build=not args.no_final_build)
    summarise(results)
    return exit_code_for(results, spec.get("mutants") or [])


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
