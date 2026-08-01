#!/usr/bin/env python3
"""Bundled-theme / community-registry drift check (issue #57).

Three themes exist TWICE: bundled here under native/themes2, and published in the community registry
(github.com/cubman3134/everythingbox-themes) so the Appearance panel can point users at them. Nothing
noticed when a bundled theme gained a view and its registry twin did not, so both had quietly rotted —
Channels had lost `nowplayingAudio`, the `channels` browse layout and the detail `actionrow`; Triple had
lost EVERYTHING but `home`, which is the exact shape issue #29 was about. Downloading either from the
registry gave you a strictly worse theme than the one already in the app, under the same name.

CI cannot see the registry: it is a different repo, and this suite is deliberately offline (no network, no
keys). So the offline gate compares against a CHECKED-IN RECORD of what the registry is expected to be
serving — native/themes2/REGISTRY-SYNC.json. Change a bundled theme and its hash moves; the gate goes red
and stays red until the record is refreshed and committed with the theme change.

WHAT THE RECORD IS ALLOWED TO CLAIM (issue #151). `--update` recomputes the hashes from the BUNDLED theme;
it has never touched the registry, so for a year it could be run by someone who never published, and the
record would then assert a currency that had never existed. That is not hypothetical — it is what happened.
#57 and #32 ran step 4 without step 3, so the record said the registry was current while it still served
the pre-#29 Triple (`home` and nothing else, 1380 bytes), and the gate was green over it for days. The gate
was not broken; the thing it compared against was a claim nobody had to substantiate.

So the record has to NAME what it was published against, in `publishedAgainst`:

  * `registryCommit` — the sha in the registry that the copies were pushed as. `--update --registry-commit
    <sha>` records it. It is not verifiable here (no network), but it is falsifiable ANYWHERE else, which is
    the whole point: `--verify-registry` fetches that repo and says plainly whether the claim still holds.
  * `unverifiedReason` — the escape hatch, and the reason the escape hatch is not free: `--update
    --assume-published "<why>"` writes the excuse INTO the record, where a reviewer and `git blame` both see
    it, and `--check` prints it on every single run.

A bare `--update` is refused. The offline gate still cannot PROVE the registry is current — nothing here
can — but it no longer silently asserts it either. It reports which of the two the record is.

WHO NORMALLY WRITES THE CLAIM. Not a person, any more. The hand copy into the registry was the step that
failed twice (#57 recorded an intent to republish that nobody performed; #131 was the cleanup), so it is
now a job: on a push to main touching native/themes2/**, .github/workflows/publish-themes.yml checks the
registry out with a deploy key, runs `--publish` into it, pushes, and then reruns `--update
--registry-commit <the sha it just pushed>` and commits THAT back to this repo. So a `registryCommit` in
the record is normally machine-written and substantiated by construction, and `--assume-published` is what
a human writes in a PR to say "the copy has not happened yet" — which is exactly true until the merge.
verify-registry.yml re-runs `--verify-registry` weekly, catching what the publisher structurally cannot
see: a direct edit over there, a revert, or a publish that never ran.

The hash is CANONICAL, not byte-for-byte: parse, re-serialise with sorted keys and no whitespace, hash
that. Channels' bundled theme.json is machine-serialised (one value per line, `\\uXXXX` escapes) while
Triple's and Night's are hand-formatted, and the registry is a contributor-facing repo whose files should
stay readable. Byte equality would weld the two repos' FORMATTING together forever and would report a pure
reindent as drift. Only the meaning is gated. THEME_FORMAT.md is not JSON, so it is hashed as text with the
newline spelling normalised out (see doc_hash — hashing it raw recorded the platform, not the file).

Usage:
  theme-registry-sync.py            # print each bundled theme's canonical hash
  theme-registry-sync.py --check    # gate: compare against REGISTRY-SYNC.json (exit 1 on drift). OFFLINE.
  theme-registry-sync.py --update --registry-commit <sha>       # refresh the record, naming the publish
  theme-registry-sync.py --update --assume-published "<why>"    # refresh it without one, on the record
  theme-registry-sync.py --publish <registry-dir>               # copy the record's targets into a checkout
  theme-registry-sync.py --verify-registry                      # NEEDS NETWORK. Never run by the probe
                                                                # suite; the publish job runs it after its
                                                                # own push, and a weekly workflow re-runs it.
"""
import hashlib
import json
import ntpath          # for the absolute-path refusal in publish_into; see the comment there
import os
import re
import shutil
import sys
import time
import urllib.request   # only --verify-registry reaches the network; --check must never open a socket

HERE = os.path.dirname(os.path.abspath(__file__))
THEMES = os.path.join(HERE, os.pardir, "themes2")
RECORD = os.path.join(THEMES, "REGISTRY-SYNC.json")
DOCS = ["THEME_FORMAT.md"]

# The block that says what the hashes below it were published against (issue #151).
CLAIM = "publishedAgainst"
SHA_RE = re.compile(r"\A[0-9a-f]{7,40}\Z")

# Where --verify-registry looks. Read-only, and only ever from a command a workflow or a maintainer ran on
# purpose: the probe suite must not acquire a network dependency, because a gate that can fail for a reason
# unrelated to the code is a gate people learn to re-run rather than read.
#
# The BRANCH in the raw url is `main`, and that is a coupling worth naming rather than leaving incidental:
# publish-themes.yml pushes to the branch it checked the registry out of, and this reads back from .../main/.
# If those two ever named different branches a publish would push successfully and then every verify target
# would come back "could not read" — a green push followed by a red verify, with nothing pointing at the
# branch as the cause. So the workflow pins `ref: main` on the registry checkout; both halves say `main`
# literally, and moving the registry's default branch is a change that has to be made in both places.
REGISTRY_RAW = "https://raw.githubusercontent.com/cubman3134/everythingbox-themes/main/"
REGISTRY_API = "https://api.github.com/repos/cubman3134/everythingbox-themes/"


def canonical_hash_bytes(data):
    """SHA-256 of a theme.json's MEANING — parsed, re-serialised sorted+compact, hashed. Returns (hash, doc).

    Spelled at the BYTE level so --verify-registry can hash a response body through the SAME code --check
    hashes a file with: it holds bytes off the network and has no local path, --check holds a path and never
    opens a socket. Two spellings of this one rule would let the registry and the record agree on a value
    neither the app's gate nor --update would compute, which is the drift this whole file exists to prevent.
    Raises on bytes that are not JSON; every caller turns that into a reported problem.
    """
    doc = json.loads(data.decode("utf-8"))
    blob = json.dumps(doc, sort_keys=True, separators=(",", ":"), ensure_ascii=True)
    return hashlib.sha256(blob.encode("utf-8")).hexdigest(), doc


def doc_hash_bytes(data):
    """SHA-256 of a document's TEXT with the newline spelling normalised out first. See doc_hash."""
    return hashlib.sha256(data.replace(b"\r\n", b"\n")).hexdigest()


def canonical_hash(path):
    """SHA-256 of a theme.json's MEANING, read from a file. Returns (hash, parsed document).

    check() inspects the parsed document for views declared with no elements, which is why the hash does not
    come back alone.
    """
    with open(path, "rb") as f:
        return canonical_hash_bytes(f.read())


def doc_hash(path):
    """SHA-256 of a document's TEXT, with the newline spelling normalised out first.

    Same principle as canonical_hash one function up: gate the meaning, not the encoding. There the noise
    was indentation; here it is CRLF-vs-LF, which is not something the author chose at all — it is whatever
    the checkout produced, so hashing it raw records the platform that ran --update rather than the file.
    """
    with open(path, "rb") as f:
        return doc_hash_bytes(f.read())


def selftest():
    """The doc hash must not depend on the line endings of whoever's checkout produced it.

    THEME_FORMAT.md is not JSON, so it is hashed as text rather than by meaning — and no .gitattributes
    pins its line endings, so it is CRLF in a Windows working copy and LF in CI's. A raw byte hash
    therefore recorded whichever platform last ran --update and could never match the other. It recorded
    CRLF, so the gate went red on twelve consecutive CI runs over a file nobody had edited, while every
    developer's local run passed — a gate that cannot pass in CI reports nothing, including the real drift
    it exists to catch.

    Gate the property, not that one incident: hash the same text spelled both ways and require one answer.
    """
    import tempfile
    text = "# Heading\n\nA line.\n\n* item\n"
    out = []
    with tempfile.TemporaryDirectory() as tmp:
        for name, data in (("lf", text.encode()), ("crlf", text.replace("\n", "\r\n").encode())):
            p = os.path.join(tmp, name + ".md")
            with open(p, "wb") as f:
                f.write(data)
            out.append(doc_hash(p))
    if out[0] != out[1]:
        return ["the doc hash still depends on line endings, so it records the platform that ran --update\n"
                "      as LF   %s\n"
                "      as CRLF %s\n"
                "    Whichever one is in the record, the other checkout can never match it." % tuple(out)]
    return []


def describe_claim(rec):
    """What does this record say it was published against? Returns (problems, one line to print).

    The hashes below this block are computed from the BUNDLED themes, so on their own they say only "the
    record was refreshed", never "the registry was updated". Whether those two coincided is a fact about a
    push nobody recorded — until now. This reads the record's own statement about it and, crucially, prints
    it whatever the answer is: the failure mode #151 is about is not a wrong claim, it is an INVISIBLE one.
    """
    claim = rec.get(CLAIM)
    if not isinstance(claim, dict):
        return (["REGISTRY-SYNC.json has no '%s' block, so the hashes below it name nothing they were "
                 "published against. Refresh the record with  --update --assume-published \"<why>\"  (the "
                 "publish job rewrites it with --registry-commit <sha> when the merge lands)." % CLAIM], None)

    sha = str(claim.get("registryCommit", "")).strip().lower()
    why = str(claim.get("unverifiedReason", "")).strip()

    if sha and why:
        return (["%s names BOTH a registryCommit and an unverifiedReason. It is one or the other: either "
                 "the copies were pushed as %s, or they were not and the reason stands. Two answers is no "
                 "answer, and the next reader picks whichever one they were hoping for." % (CLAIM, sha)],
                None)
    if sha:
        if not SHA_RE.match(sha):
            return (["%s.registryCommit is %r, which is not a git sha (7-40 hex). A malformed one cannot be "
                     "looked up, so it reads as a citation while being unfollowable." % (CLAIM, sha)], None)
        return ([], "record claims these were published to the registry as commit %s. "
                    "Offline here - run  theme-registry-sync.py --verify-registry  to test that claim." % sha)
    if why:
        # NOT a failure. A record that admits it is an intent is the honest state, and it is the state every
        # PR that edits a theme is in: the copy happens on merge, not before. What was wrong before #151 was
        # that it read identically to a verified one; now it announces itself on every run, and the publish
        # job replaces it with a real sha the moment the merge lands.
        return ([], "record makes NO verified publish claim: %s" % why)

    return (["%s names neither a registryCommit nor an unverifiedReason, so the record asserts the registry "
             "is current and offers nothing to check that against - which is the state issue #151 exists to "
             "end. Rerun --update with --assume-published \"<why>\" or --registry-commit <sha>." % CLAIM],
            None)


def _write(path, text):
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)


def _read(path):
    with open(path, encoding="utf-8", newline="") as f:
        return f.read()


def _make_bundled(root):
    """A themes2/-shaped source dir: one theme with a subdirectory, plus a published doc."""
    os.makedirs(os.path.join(root, "Shared", "sounds"))
    _write(os.path.join(root, "Shared", "theme.json"), '{"name":"Shared","views":{}}')
    _write(os.path.join(root, "Shared", "sounds", "new.wav"), "NEW")
    _write(os.path.join(root, "DOC.md"), "# doc\n")


def _make_registry(root):
    """A registry checkout: an OLDER copy of the published theme (carrying a file the bundled copy no
    longer has), a registry-only theme the record says nothing about, and a stale published doc."""
    os.makedirs(os.path.join(root, "themes2", "Shared", "sounds"))
    _write(os.path.join(root, "themes2", "Shared", "theme.json"), '{"name":"old"}')
    _write(os.path.join(root, "themes2", "Shared", "sounds", "stale.wav"), "OLD")
    os.makedirs(os.path.join(root, "themes2", "RegistryOnly"))
    _write(os.path.join(root, "themes2", "RegistryOnly", "theme.json"), '{"name":"RegistryOnly"}')
    _write(os.path.join(root, "DOC.md"), "# stale\n")


def _theme_rec(name, dest_rel):
    return {"publishedThemes": {name: {"registryPath": dest_rel, "canonicalSha256": ""}}, "publishedDocs": {}}


def _publish_caught(rec, themes_dir, registry_dir, problems, label):
    """publish_into, with a CRASH reported as a problem instead of escaping as a traceback.

    Every refusal in publish_into stands in front of a destructive call. Delete one and the call runs on
    exactly the input it was guarding — rmtree on a path that is not there, copytree from a source that is
    not there — and the gate goes red naming shutil instead of the rule that went missing. The message this
    produces is double-reported alongside the case's own assertion; two lines for one fault is the right
    trade when the alternative is a stack trace with no statement of what broke.
    """
    try:
        return publish_into(rec, themes_dir, registry_dir)
    except Exception as exc:                                            # noqa: BLE001
        problems.append("--publish crashed instead of returning a refusal (%s): %s: %s\n"
                        "    A refusal that guards a destructive call has stopped working."
                        % (label, type(exc).__name__, exc))
        return [], ["(crashed: %s)" % exc]


def _selftest_publish():
    """--publish copies what the record names, replaces wholesale, refuses the rest, and on a refusal
    writes NOTHING.

    Each property below is a real failure mode rather than a hypothetical.

    (1) The registry carries themes this repo does not bundle — Default, Grid, Lumen, Midnight — so a sync
    that walked themes2/ instead of the record would delete four themes on its first run. (2) A theme folder
    must be REPLACED, not merged: the canonical hash covers theme.json alone, so a sound file dropped from
    the bundled theme would survive every check we have while still being served. (3) A recorded target the
    registry does not already carry is a catalog change — index.json entries hold a `description` that
    exists nowhere in theme.json — so publishing it would serve a theme nothing lists; the same refusal
    covers docs, where creating a file the registry does not serve is publishing by accident.

    (4) CONTAINMENT. `dest` is built from the record's registryPath and handed to rmtree, and nothing else
    in this file validates that path: --check compares hashes. A registryPath of "." or "themes2/../.."
    passes the "must already exist" test precisely BECAUSE it exists, so a typo'd record commits green and
    then deletes the checkout, or its parent, on whatever an operator points --publish at.

    (5) ALL OR NOTHING. Themes are processed before docs, so a record with a good theme and a bad doc used
    to replace the theme and only then refuse — reporting "nothing published" over a half-published
    checkout. Validation is now a separate first pass; this asserts that it stayed one.

    (6) The three refusals and the missing-checkout guard each get a case that names them specifically. All
    four were previously unasserted: deleting any one of them left this gate green.
    """
    import tempfile
    problems = []
    # The doubled slash is deliberate: it resolves to the same folder (an empty component is a no-op in
    # os.path.join), the copy below must still land there, and the "copied" list asserted further down is
    # spelled SINGLE-slash — which is what pins the normalisation of the echoed path.
    rec = {
        "publishedThemes": {"Shared": {"registryPath": "themes2//Shared", "canonicalSha256": ""}},
        "publishedDocs": {"DOC.md": {"registryPath": "DOC.md", "sha256": ""}},
    }
    with tempfile.TemporaryDirectory() as tmp:
        src_themes = os.path.join(tmp, "bundled")
        registry = os.path.join(tmp, "registry")
        _make_bundled(src_themes)
        _make_registry(registry)

        copied, bad = publish_into(rec, src_themes, registry)
        if bad:
            problems.append("--publish refused a well-formed registry: %s" % "; ".join(bad))
        elif sorted(copied) != ["DOC.md", "themes2/Shared"]:
            # Only meaningful when the call did not refuse: on a refusal `copied` is empty by design, and
            # reporting it as a wrong copy list is the same fault stated twice.
            problems.append("--publish copied %r, expected the two recorded targets" % sorted(copied))
        if not os.path.isfile(os.path.join(registry, "themes2", "Shared", "sounds", "new.wav")):
            problems.append("--publish did not copy a theme's subdirectory contents.")
        if os.path.exists(os.path.join(registry, "themes2", "Shared", "sounds", "stale.wav")):
            problems.append("--publish MERGED into the theme folder instead of replacing it, so a file the "
                            "bundled theme dropped is still being served.")
        if not os.path.isfile(os.path.join(registry, "themes2", "RegistryOnly", "theme.json")):
            problems.append("--publish deleted a registry-only theme. It must copy the record's targets and "
                            "leave everything else alone.")
        if _read(os.path.join(registry, "DOC.md")) != "# doc\n":
            problems.append("--publish did not overwrite the published doc.")

        # A recorded theme the registry does not carry: refuse, and change nothing.
        os.makedirs(os.path.join(src_themes, "Absent"))
        _write(os.path.join(src_themes, "Absent", "theme.json"), '{"name":"Absent"}')
        copied2, bad2 = _publish_caught(_theme_rec("Absent", "themes2/Absent"), src_themes, registry,
                                        problems, "a theme the registry does not carry")
        if not bad2:
            problems.append("--publish created themes2/Absent in the registry. A theme the registry does "
                            "not already carry needs an index.json entry that cannot be generated.")
        if copied2:
            problems.append("--publish reported copies while refusing: %r" % copied2)
        if os.path.exists(os.path.join(registry, "themes2", "Absent")):
            problems.append("--publish created the folder it claimed to refuse.")

        # Same refusal for a doc. Without it, copy2 CREATES the file — the registry starts serving a
        # document it never agreed to carry, and nothing here would say so.
        _write(os.path.join(src_themes, "NEW.md"), "# new\n")
        rec3 = {"publishedThemes": {},
                "publishedDocs": {"NEW.md": {"registryPath": "NEW.md", "sha256": ""}}}
        copied3, bad3 = _publish_caught(rec3, src_themes, registry, problems,
                                        "a doc the registry does not carry")
        if not bad3 or copied3:
            problems.append("--publish published NEW.md, which the registry does not carry. Creating a "
                            "document there is a catalog change, not a sync.")
        if os.path.exists(os.path.join(registry, "NEW.md")):
            problems.append("--publish created the doc it claimed to refuse.")

        # Recorded but missing from the source dir: refuse, and above all do not delete the registry's copy
        # on the way to failing to replace it.
        reg4 = os.path.join(tmp, "registry4")       # untouched by the case above, so "still there" means it
        _make_registry(reg4)
        copied4, bad4 = _publish_caught(_theme_rec("Gone", "themes2/Shared"), src_themes, reg4,
                                        problems, "a theme missing from the source dir")
        want4 = "missing from %s" % os.path.normpath(src_themes)
        if copied4 or not any(want4 in b for b in bad4):
            problems.append("--publish must refuse a recorded theme that is absent from the source dir and "
                            "name the directory it looked in; it said %r" % (bad4 or copied4))
        if not os.path.isfile(os.path.join(reg4, "themes2", "Shared", "sounds", "stale.wav")):
            problems.append("--publish DELETED the registry's copy of a theme it could not copy. A refusal "
                            "must leave the checkout exactly as it found it.")

        # A registry path that is not a checkout must be reported ONCE, as a missing checkout. Without that
        # guard the operator who typo'd the path gets one refusal per recorded target instead, none of
        # which mentions the thing that is actually wrong.
        copied5, bad5 = _publish_caught(rec, src_themes, os.path.join(tmp, "not-a-checkout"),
                                        problems, "a registry path that does not exist")
        if copied5 or len(bad5) != 1 or "registry checkout not found" not in bad5[0]:
            problems.append("--publish must report a missing registry checkout as exactly that, once; it "
                            "said %r" % (bad5 or copied5))

        # ALL OR NOTHING: a good theme followed by a refused doc. Themes sort first, so this is the exact
        # order in which the interleaved version replaced the theme and only then refused.
        rec6 = {"publishedThemes": {"Shared": {"registryPath": "themes2/Shared", "canonicalSha256": ""}},
                "publishedDocs": {"NEW.md": {"registryPath": "NEW.md", "sha256": ""}}}
        reg6 = os.path.join(tmp, "registry6")
        _make_registry(reg6)
        copied6, bad6 = _publish_caught(rec6, src_themes, reg6, problems, "a good theme and a refused doc")
        if not bad6 or copied6:
            problems.append("--publish accepted a record whose doc target is not in the registry.")
        if _read(os.path.join(reg6, "themes2", "Shared", "theme.json")) != '{"name":"old"}':
            problems.append("--publish replaced an earlier target and THEN refused a later one, leaving the "
                            "checkout half published. A refusal must validate every target before writing "
                            "any of them.")

    # CONTAINMENT: registryPath values that resolve outside the checkout. Each must be refused, and the
    # directory it pointed at must still be there afterwards — this is the one destructive syscall in the
    # file, so the assertion is on the disk, not on the message.
    with tempfile.TemporaryDirectory() as tmp:
        outer = os.path.join(tmp, "outer")
        registry = os.path.join(outer, "registry")
        src_themes = os.path.join(tmp, "bundled")
        _make_bundled(src_themes)
        # Each case names the RULE that must refuse it, not merely "something was refused". That
        # distinction is the whole point here: every absolute spelling below is also caught downstream by
        # must-already-exist (a same-drive "C:/..." is joined onto the checkout by os.path.join, which
        # treats a bare "C:" component as same-drive-relative) or by commonpath (another drive). So an
        # assertion that only counts refusals stays GREEN with the absolute-path branch deleted — measured,
        # not assumed — and the rule would be free to rot behind a rule that happens to shadow it.
        for dest_rel, want in ((".", "escapes the registry checkout"),
                               ("themes2/../..", "escapes the registry checkout"),
                               # Windows spells this "C:/.../outer/neighbour" and POSIX "/tmp/.../neighbour",
                               # so this one case exercises whichever half of the rule the host uses.
                               (os.path.join(outer, "neighbour").replace(os.sep, "/"),
                                "is an absolute path"),
                               ("/etc/everythingbox", "is an absolute path"),      # driveless-rooted
                               ("C:/Windows/System32", "is an absolute path")):    # drive-qualified
            # Rebuilt per case, not once: a case that fails destroys these, and the next case's report must
            # be about the next case rather than about the wreckage the previous one left.
            _make_registry(registry)
            os.makedirs(os.path.join(outer, "neighbour"), exist_ok=True)
            _write(os.path.join(outer, "neighbour", "keep.txt"), "KEEP")
            _write(os.path.join(registry, ".git-marker"), "REPO")
            copied, bad = _publish_caught(_theme_rec("Shared", dest_rel), src_themes, registry,
                                          problems, "registryPath %r" % dest_rel)
            if copied or not any(want in b for b in bad):
                problems.append("--publish must refuse registryPath %r as %r, and did not: %r"
                                % (dest_rel, want, bad or copied))
            for gone, what in ((os.path.join(registry, ".git-marker"), "the registry checkout itself"),
                               (os.path.join(registry, "themes2", "RegistryOnly", "theme.json"),
                                "the registry's other themes"),
                               (os.path.join(outer, "neighbour", "keep.txt"),
                                "a directory beside the registry checkout")):
                if not os.path.isfile(gone):
                    problems.append("--publish with registryPath %r DELETED %s. A recorded path is not a "
                                    "licence to rmtree whatever it resolves to." % (dest_rel, what))
            shutil.rmtree(registry, ignore_errors=True)
    return problems


def _compare_caught(targets, fetched, problems, label):
    """compare_registry, with a CRASH reported as a problem instead of escaping as a traceback.

    compare_registry is handed whatever the registry served, which includes bytes that are not JSON at all.
    Its refusals stand in front of a hash call that raises on exactly that input, so deleting one turns a
    stated refusal into a stack trace. That still reddens the gate, but it names json or hashlib rather than
    the rule that went missing. Reported here it names itself; the case's own assertion states it a second
    time, which is the right trade against a traceback.

    Returns the two problem lists flattened into one: every caller below asks only "was this reported, and
    how". The mismatch/unreadable SPLIT gets its own cases, down in the verdict block.
    """
    try:
        mismatches, unreadable, _lines = compare_registry(targets, fetched)
        return mismatches + unreadable
    except Exception as exc:                                            # noqa: BLE001
        problems.append("--verify-registry crashed instead of reporting a problem (%s): %s: %s\n"
                        "    A refusal that stands in front of a hash call has stopped working."
                        % (label, type(exc).__name__, exc))
        return ["(crashed: %s)" % exc]


def _selftest_verify():
    """The remote comparison agrees with the local one, a refusal is never silent, a match never waits, and
    an unreachable registry is not a verdict.

    --verify-registry exists to prove the registry serves what this repo bundles. If it hashed remote bytes
    even slightly differently from the way --check hashes local ones — a different JSON separator, a
    different newline rule — it would report drift that is not there, or worse, agree when the two differ.
    So the first property under test is that both routes produce the SAME hash for the same content, not
    that either produces a particular constant.

    The socket itself is not covered here and cannot be: this script runs inside a probe suite that is
    deliberately network-free, and --check must never open one. Everything BELOW the socket is: the
    comparison is pure, and the retry loop and the three-way verdict are driven through a fake fetch and a
    fake sleep. That injection seam is the reason run_verify_registry takes its dependencies as arguments
    instead of reaching for urllib and time itself.
    """
    import tempfile
    problems = []
    theme_text = '{\n  "name": "T",\n  "views": {}\n}\n'
    doc_text = "# Doc\n\nBody.\n"
    head_sha = "ef44634ca60ed6c044e9a33c1d41135ed3f4cb41"

    rec = {
        "registry": "https://github.com/cubman3134/everythingbox-themes",
        CLAIM: {"registryCommit": head_sha},
        "publishedThemes": {"T": {"registryPath": "themes2/T", "canonicalSha256": ""}},
        "publishedDocs": {"DOC.md": {"registryPath": "DOC.md", "sha256": ""}},
    }

    reindented = json.dumps(json.loads(theme_text), indent=4).encode()
    crlf_doc = doc_text.replace("\n", "\r\n").encode()
    good = {"themes2/T/theme.json": reindented, "DOC.md": crlf_doc}

    with tempfile.TemporaryDirectory() as tmp:
        os.makedirs(os.path.join(tmp, "T"))
        theme_path = os.path.join(tmp, "T", "theme.json")
        with open(theme_path, "wb") as f:
            f.write(theme_text.encode())
        doc_path = os.path.join(tmp, "DOC.md")
        with open(doc_path, "wb") as f:
            f.write(crlf_doc)                       # CRLF on purpose: the doc hash must not see it

        # ONE implementation of each hash rule. --check hashes a PATH, --verify-registry hashes BYTES off
        # the network, and the moment those two routes stop delegating to the same core the registry and the
        # record can agree on a value neither the app's gate nor --update would compute — the same drift
        # this file exists to prevent, one level up. Asserted as agreement between the two routes rather
        # than against a constant, so it survives any legitimate change to the rule itself.
        got, parsed = canonical_hash(theme_path)
        if got != canonical_hash_bytes(theme_text.encode())[0]:
            problems.append("canonical_hash(path) and canonical_hash_bytes(data) computed different hashes "
                            "for the same theme, so --check and --verify-registry gate different rules.")
        if parsed != json.loads(theme_text):
            problems.append("canonical_hash no longer returns (hash, parsed document); check() inspects that "
                            "document for views declared with no elements.")
        if doc_hash(doc_path) != doc_hash_bytes(doc_text.encode()):
            problems.append("doc_hash(path) and doc_hash_bytes(data) computed different hashes for the same "
                            "document, so --check and --verify-registry gate different rules.")

        # registry_targets is the ONE place that decides what the verifier reads and what it expects, and it
        # is built from the same published_targets --publish copies. Pinned here so the two cannot drift
        # apart: a verifier that reads a different set from the one the publisher writes proves nothing
        # about it. The paths must come from the record's registryPath, and the expectations from the
        # BUNDLED files.
        targets = registry_targets(rec, tmp)
        if [t[0] for t in targets] != ["themes2/T/theme.json", "DOC.md"]:
            problems.append("registry_targets read back %r; it must derive its paths from the record's "
                            "registryPath, the same list --publish writes." % [t[0] for t in targets])
        if [t[3] for t in targets] != [got, doc_hash(doc_path)]:
            problems.append("registry_targets did not take its expected hashes from the BUNDLED files: %r"
                            % [t[3] for t in targets])

        # A recorded theme with nothing bundled under that name. The verifier must say so rather than
        # skipping it, which would silently shrink the set it claims to have proven.
        missing = registry_targets({"publishedThemes": {"Ghost": {"registryPath": "themes2/Ghost"}}}, tmp)
        bad = _compare_caught(missing, {"themes2/Ghost/theme.json": reindented}, problems,
                              "a theme recorded as published but not bundled")
        if not any("not bundled here" in b for b in bad):
            problems.append("--verify-registry did not report a recorded theme that is not bundled here: %r"
                            % bad)

    # Matching content, spelled differently on both axes the hashes are supposed to ignore: the theme
    # reindented, the doc with CRLF line endings.
    bad = _compare_caught(targets, good, problems, "content that matches")
    if bad:
        problems.append("--verify-registry reported drift on content that matches: %s" % "; ".join(bad))

    # Real drift on each axis.
    changed = json.dumps({"name": "T", "views": {"home": {}}}).encode()
    if not _compare_caught(targets, dict(good, **{"themes2/T/theme.json": changed}), problems,
                           "a changed theme"):
        problems.append("--verify-registry missed a changed theme.json.")
    if not _compare_caught(targets, dict(good, **{"DOC.md": b"# Different\n"}), problems, "a changed doc"):
        problems.append("--verify-registry missed a changed doc.")

    # A fetch that failed must be a problem, never a pass — and must be reported AS a failed fetch. This is
    # the case that decides whether an outage reads as "the registry is fine". Without the branch a None body
    # falls through to the hash and is reported as "does not parse", which sends whoever reads it to a
    # registry file that is very probably fine; pinning the wording is what keeps the branch alive.
    for label, fetched in (("a file that could not be read", dict(good, **{"themes2/T/theme.json": None})),
                           ("a file the fetch never returned at all", {"DOC.md": crlf_doc})):
        bad = _compare_caught(targets, fetched, problems, label)
        if not any("could not read" in b for b in bad):
            problems.append("--verify-registry treated %s as agreement, or reported it as something other "
                            "than a failed read: %r" % (label, bad))

    # Unparseable JSON is a problem, not a crash.
    bad = _compare_caught(targets, dict(good, **{"themes2/T/theme.json": b"not json"}), problems,
                          "unparseable")
    if not any("does not parse" in b for b in bad):
        problems.append("--verify-registry did not report an unparseable theme.json as unparseable: %r" % bad)

    # THE RETRY, driven through a fake fetch and a fake sleep: no socket, no wait, --check stays offline.
    # raw.githubusercontent is a CDN and eventually consistent, so a read moments after a push — which is
    # exactly when the publish job reads — can return the PRE-push bytes. Retrying a MISMATCH costs minutes
    # in the rare failing case and must cost nothing in the common one, which is the first assertion below.
    def drive(bodies, attempts=3, tgts=None):
        seen = {"urls": [], "slept": [], "log": []}

        def fetch(url):
            seen["urls"].append(url)
            rel = url[len(REGISTRY_RAW):] if url.startswith(REGISTRY_RAW) else url
            return bodies.get(rel), "fake"

        mismatches, unreadable = _verify_loop(targets if tgts is None else tgts, fetch,
                                              seen["slept"].append, seen["log"].append, attempts, 30)
        seen["problems"] = mismatches + unreadable
        return seen

    seen = drive(good)
    if seen["problems"] or len(seen["urls"]) != 2 or seen["slept"]:
        problems.append("--verify-registry did not accept a matching registry on the FIRST attempt: it "
                        "reported %r, fetched %d url(s) and slept %r. A match must never wait on the retry."
                        % (seen["problems"], len(seen["urls"]), seen["slept"]))
    if seen["urls"] and not seen["urls"][0].startswith(REGISTRY_RAW):
        problems.append("--verify-registry fetched %r, which is not under the registry's raw root."
                        % seen["urls"][0])

    seen = drive(dict(good, **{"DOC.md": b"# Different\n"}))
    if not seen["problems"] or len(seen["urls"]) != 6 or seen["slept"] != [30, 30]:
        problems.append("--verify-registry must re-fetch a mismatch and only then report it, waiting between "
                        "attempts and not after the last: 3 attempts over 2 targets fetched %d url(s) and "
                        "slept %r, reporting %r. A single-shot check moments after a push reads the CDN's "
                        "previous bytes and fails a job that did everything right."
                        % (len(seen["urls"]), seen["slept"], seen["problems"]))

    # An unreadable file is retried on the same terms: it is a problem either way, and never agreement.
    seen = drive(dict(good, **{"DOC.md": None}))
    if not seen["problems"] or len(seen["urls"]) != 6:
        problems.append("--verify-registry did not re-fetch a file it could not read (%d url(s), %r); a "
                        "transient read failure moments after a push is the same CDN window as a mismatch."
                        % (len(seen["urls"]), seen["problems"]))

    # "Did I check anything?" — the same floor check() puts under its own scan. Both of these fall through a
    # loop body that never runs and land on the success return, which reports a PASS having fetched nothing.
    for label, seen in (("a record that names nothing as published", drive(good, tgts=[])),
                        ("a run allowed no attempts", drive(good, attempts=0))):
        if not seen["problems"] or seen["urls"]:
            problems.append("--verify-registry PASSED %s, fetching %d url(s). A verifier that walks an empty "
                            "list reports the registry as proven when it read none of it."
                            % (label, len(seen["urls"])))

    # THE VERDICT IS THREE-WAY, and the third value is the one that carries the weight: a registry nobody
    # could REACH must not read as "not current". Conflating the two files an outage as drift, sends
    # somebody to a registry that is fine, and — the expensive direction — teaches the next reader to
    # discount a red verify. Driven end to end through run_verify_registry, so what is asserted is the exit
    # status the two workflows branch on, not an internal list.
    def verdict(bodies, head=None):
        log = []

        def fetch(url):
            if url.startswith(REGISTRY_API):
                return head, "fake api failure"
            rel = url[len(REGISTRY_RAW):] if url.startswith(REGISTRY_RAW) else url
            return bodies.get(rel), "fake"

        # attempts=1: these cases are about the verdict, not the retry, and retrying here would only make a
        # failing selftest slower to read. `targets` is passed in because the themes dir it was computed
        # from is gone by now — and passing it is also what keeps this case honest about which set was read.
        code = run_verify_registry(rec, None, fetch, lambda _s: None, log.append, 1, 0, targets=targets)
        return code, "\n".join(log)

    ok_head = json.dumps({"sha": head_sha}).encode()
    code, log = verdict(good, head=ok_head)
    if code != 0:
        problems.append("--verify-registry returned %d for a registry that matches; 0 is the only value the "
                        "publish job may see after its own push. It logged:\n%s" % (code, log))
    code, log = verdict(dict(good, **{"DOC.md": b"# Different\n"}), head=ok_head)
    if code != 1:
        problems.append("--verify-registry returned %d for a registry that serves different content; a real "
                        "mismatch must be 1. It logged:\n%s" % (code, log))
    code, log = verdict(good, head=None)
    if code != 2 or "NOT a verdict" not in log:
        problems.append("--verify-registry returned %d when the registry could not be reached at all. An "
                        "unreachable registry is UNKNOWN (2), never a verdict about the record — filing an "
                        "outage as drift is how a red verify stops being believed. It logged:\n%s"
                        % (code, log))
    code, log = verdict(good, head=b"not json")
    if code != 2:
        problems.append("--verify-registry returned %d when the registry's HEAD lookup returned bytes it "
                        "could not parse. Bytes that are not the API's answer are still 'could not find "
                        "out', not a verdict. It logged:\n%s" % (code, log))
    code, log = verdict(dict(good, **{"DOC.md": None}), head=ok_head)
    if code != 2:
        problems.append("--verify-registry returned %d when a published file could not be READ. Nothing "
                        "DISAGREED; something was unreachable, and the two must not report the same. It "
                        "logged:\n%s" % (code, log))
    return problems


def bundled_themes():
    """Every folder under themes2/ that holds a theme.json, sorted."""
    out = []
    for name in sorted(os.listdir(THEMES)):
        if os.path.isfile(os.path.join(THEMES, name, "theme.json")):
            out.append(name)
    return out


def load_record():
    with open(RECORD, encoding="utf-8") as f:
        return json.load(f)


def published_targets(rec):
    """(kind, name, registry-relative destination) for everything the record publishes.

    ONE place decides what "published" means. --publish copies exactly this list and --verify-registry reads
    exactly this list back, so the two cannot disagree about scope — and neither walks themes2/, which
    carries a different set of themes than the registry does.

    No source path is returned: --publish resolves one against whichever themes dir it was handed, and
    registry_targets against whichever it was handed. Returning a value computed from a module global that a
    parameterised caller is trying not to use is how a helper starts lying.
    """
    out = []
    for name in sorted(rec.get("publishedThemes", {})):
        out.append(("theme", name, rec["publishedThemes"][name].get("registryPath") or ("themes2/" + name)))
    for doc_name in sorted(rec.get("publishedDocs", {})):
        out.append(("doc", doc_name, rec["publishedDocs"][doc_name].get("registryPath") or doc_name))
    return out


def publish_into(rec, themes_dir, registry_dir):
    """Copy the record's published targets from themes_dir into registry_dir.

    Parameterised rather than reading the module globals so _selftest_publish can drive it against scratch
    directories — the whole point of putting the copy rules here instead of in the workflow.

    TWO PASSES, and that separation is the design: every target is validated before ANY target is written.
    The first version interleaved them, so a record whose second target was refused had already deleted and
    replaced the first — the caller was handed "nothing was published" while the checkout on disk was half
    republished, which is the one state nobody can reason about. Validating first is what makes the refusal
    mean what it says: a non-empty problems list is a promise that the checkout was not touched at all, so
    the fix is "correct the record and re-run", with no need to work out what landed.

    Returns (copied, problems). They are mutually exclusive: problems means copied is empty, and empty
    problems means every recorded target was written.
    """
    if not os.path.isdir(registry_dir):
        return [], ["registry checkout not found at %s" % registry_dir]

    root = os.path.normcase(os.path.realpath(registry_dir))
    problems = []
    plan = []

    for kind, name, dest_rel in published_targets(rec):
        # `name` is the folder name for a theme and the filename for a doc, and both sit directly under
        # themes_dir — which is why the source can be derived here rather than handed in.
        src = os.path.join(themes_dir, name)
        dest = os.path.join(registry_dir, *dest_rel.split("/"))

        # CONTAINMENT, first and before anything else looks at this target. Below, a theme folder is
        # deleted outright; `dest` comes from the record's registryPath, which nothing else in this file
        # validates (--check compares hashes, not paths). A registryPath of "." or "themes2/../.." resolves
        # to an EXISTING directory, so the "must already exist" refusal further down waves it through and
        # rmtree takes the checkout — .git included — or its parent. realpath, so a symlink inside the
        # checkout cannot be used to step out either; normcase, because the comparison must not hinge on
        # the casing of a Windows path.
        # BOTH spellings of "absolute", on every interpreter this may run on, tested by hand rather than
        # delegated to isabs. The DRIVE half must come from ntpath and not os.path: run this job on POSIX
        # and os.path.splitdrive("C:/Windows") reports no drive, so a Windows-absolute registryPath would
        # read as relative and be joined onto the checkout. The ROOTED half is spelled out because
        # ntpath.isabs no longer covers it: through Python 3.12 it answered True for a driveless "/x" or
        # "\x", but 3.13 redefined absolute as "has a drive AND is rooted" (or is a UNC path), so on 3.13+
        # ntpath.isabs("/etc/everythingbox") is False. Between them these two tests catch everything isabs
        # ever caught (drive-rooted "C:/x", UNC "//srv/share/x" — both rooted anyway) plus drive-RELATIVE
        # "C:x", and they answer identically on 3.12 and 3.14; isabs is therefore not called at all.
        if dest_rel.startswith(("/", "\\")) or ntpath.splitdrive(dest_rel)[0]:
            problems.append(
                "%s: registryPath %r is an absolute path. The record names paths RELATIVE to the registry\n"
                "    checkout, because the checkout lives at a different place on every machine that runs\n"
                "    this job. Fix it in REGISTRY-SYNC.json." % (name, dest_rel))
            continue
        real = os.path.normcase(os.path.realpath(dest))
        try:
            escapes = real == root or os.path.commonpath([root, real]) != root
        except ValueError:
            escapes = True          # different drives: commonpath refuses to compare them, and so do we
        if escapes:
            problems.append(
                "%s: registryPath %r escapes the registry checkout (it resolves to %s).\n"
                "    This job REPLACES a theme folder wholesale, so publishing to that path would delete\n"
                "    whatever is already there. Fix it in REGISTRY-SYNC.json." % (name, dest_rel, real))
            continue

        if not os.path.exists(src):
            problems.append("%s is recorded as published but is missing from %s."
                            % (name, os.path.normpath(themes_dir)))
            continue

        # REFUSAL: never CREATE a path in the registry. A recorded theme the registry does not already
        # carry is a catalog change — index.json entries hold a `description` that exists nowhere in
        # theme.json, so it cannot be generated, and a folder nothing lists is a theme nobody can find.
        if kind == "theme" and not os.path.isdir(dest):
            problems.append(
                "%s is recorded as published to %s, but the registry has no such folder.\n"
                "    What was tested is the FOLDER, so adding only an index.json entry gets you this same\n"
                "    refusal again. The registry needs BOTH: the folder (its contents do not matter — this\n"
                "    job overwrites them wholesale) AND an index.json entry naming it (name, author,\n"
                "    description, dir), because the description exists nowhere in theme.json and a folder\n"
                "    nothing lists is a theme nobody can find. Do that side before merging the theme here,\n"
                "    or the first publish run goes red on main." % (name, dest_rel))
            continue
        if kind == "doc" and not os.path.isfile(dest):
            problems.append("%s is recorded as published to %s, but the registry has no such file."
                            % (name, dest_rel))
            continue

        # Echoed in its normalised spelling: "themes2//Shared" and "themes2/Shared/" both resolve to the
        # same place and are copied correctly, but printed back verbatim a `published` line looks wrong.
        plan.append((kind, src, dest, "/".join(p for p in dest_rel.split("/") if p)))

    if problems:
        # `copied` deliberately does not exist yet: on this path there is nothing to report because nothing
        # below has run. The all-or-nothing promise in the docstring is structural, not a discarded list.
        return [], problems

    copied = []
    for kind, src, dest, dest_rel in plan:
        if kind == "theme":
            # WHOLESALE, not merge. The canonical hash covers theme.json alone, so a sound or font dropped
            # from the bundled theme would otherwise survive here and keep being served, invisible to every
            # check in this file.
            shutil.rmtree(dest)
            shutil.copytree(src, dest)
        else:
            shutil.copy2(src, dest)
        copied.append(dest_rel)
    return copied, []


def publish(registry_dir):
    """--publish entry point. Returns a list of problems; empty means the copy is done."""
    try:
        rec = load_record()
    except Exception as exc:                                            # noqa: BLE001
        return ["cannot read %s: %s" % (RECORD, exc)]
    copied, problems = publish_into(rec, THEMES, registry_dir)
    if problems:
        return problems
    for dest_rel in copied:
        print("  published %s" % dest_rel)
    print("  %d target(s) published into %s; everything else there was left untouched."
          % (len(copied), registry_dir))
    return []


def registry_targets(rec, themes_dir):
    """(registry-relative path, label, hash kind, expected hash or None) for everything the verifier reads.

    Built from published_targets, so --publish and --verify-registry cannot disagree about what "published"
    means: one writes exactly this set and the other reads exactly this set back. The path comes from the
    record's registryPath rather than from a guess at themes2/<name>/ — the field would otherwise be
    decorative, and the first theme published under a different folder name would be checked at a path
    nobody wrote down, producing a 404 that reads like an outage rather than like a disagreement.

    The expected hash is taken from the BUNDLED file, not from the record. --check already gates the record
    against the bundle offline, so the question left for a network call is the one only a network call can
    answer: is the registry serving what this repo ships. A recorded target with nothing bundled under that
    name gets None, which compare_registry reports rather than skips.

    A theme's registryPath names its FOLDER; only the theme.json inside it is hashed, which is the same
    thing --check compares and the same limitation --publish documents (it replaces the folder wholesale
    precisely because the hash cannot see the rest of it).
    """
    out = []
    for kind, name, dest_rel in published_targets(rec):
        rel = "/".join(p for p in dest_rel.split("/") if p)
        if kind == "theme":
            local = os.path.join(themes_dir, name, "theme.json")
            out.append((rel + "/theme.json", name, "canonical",
                        canonical_hash(local)[0] if os.path.isfile(local) else None))
        else:
            local = os.path.join(themes_dir, name)
            out.append((rel, name, "doc", doc_hash(local) if os.path.isfile(local) else None))
    return out


def compare_registry(targets, fetched):
    """Compare fetched registry bytes against the bundled copies. PURE, so the selftest covers it offline.

    `fetched` maps a registry-relative path to bytes, or to None when the fetch failed. Returns
    (mismatches, unreadable, lines) — and the SPLIT is the point. A file that disagrees is a verdict about
    the registry; a file nobody could read is a fact about the network, and the caller turns those into
    different exit statuses. Both are problems, neither is ever a pass: an outage that reads as "the
    registry is fine" is the one failure mode a verifier must not have, and it is the one nobody would
    notice, because it looks exactly like success on the day everything is in fact fine.
    """
    mismatches, unreadable, lines = [], [], []
    for rel, label, kind, want in targets:
        if want is None:
            mismatches.append("%s: recorded as published but not bundled here, so there is nothing to "
                              "compare the registry's copy against." % label)
            continue
        data = fetched.get(rel)
        if data is None:
            unreadable.append("%s: could not read the registry's copy at %s. Until it can be read, this "
                              "check is asserting nothing about %s." % (label, rel, label))
            continue
        try:
            if kind == "canonical":
                got, doc = canonical_hash_bytes(data)
            else:
                got, doc = doc_hash_bytes(data), None
        except Exception as exc:                                        # noqa: BLE001
            # Whatever the registry serves is unvalidated input here, unlike the bundled copy that --check
            # reads. Bytes that are not JSON are a fact about the registry worth reporting, not a crash.
            mismatches.append("%s: the registry's copy at %s does not parse: %s" % (label, rel, exc))
            continue
        views = None if doc is None else (",".join(sorted(doc.get("views", {}))) or "(none)")
        if got == want:
            lines.append("  OK    %-10s %s%s"
                         % (label, got[:16], "" if views is None else "  views: %s" % views))
        else:
            lines.append("  STALE %-10s registry %s  bundled %s" % (label, got[:16], want[:16]))
            if views is not None:
                lines.append("        registry views: %s" % views)
            mismatches.append("%s: the registry serves different content under this name. Either the publish "
                              "job did not run, or the registry was edited directly." % label)
    return mismatches, unreadable, lines


def fetch_raw(url):
    """Fetch one registry url. Returns (bytes, "") or (None, why) — and never raises.

    A failed fetch has to come back as a VALUE so compare_registry can report it as a problem and the caller
    can grade it "unknown" rather than "stale". Raising would abort the run somewhere above the comparison,
    and a verifier that aborts says nothing at all about the files it did read. no-cache because a cached
    copy of the very file we are checking for freshness is the one thing this must not be handed; the CDN is
    free to ignore that, which is what the retry in _verify_loop is for.
    """
    req = urllib.request.Request(url, headers={"User-Agent": "everythingbox-registry-sync",
                                               "Cache-Control": "no-cache"})
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:             # noqa: S310 - fixed https hosts
            return resp.read(), ""
    except Exception as exc:                                            # noqa: BLE001
        # Deliberately broad. urllib raises HTTPError/URLError (both OSError) for the expected failures, but
        # also ValueError for a malformed url and http.client errors for a truncated response. Every one of
        # them means "this file was not read", the caller turns that into a refusal, so nothing is swallowed
        # into a pass — the reason is carried out so the operator is not left guessing at a bare "could not
        # read".
        return None, "%s: %s" % (type(exc).__name__, exc)


def _verify_loop(targets, fetch, sleep, log, attempts, delay):
    """Fetch every target and compare, re-fetching while they disagree. Returns (mismatches, unreadable).

    Retries on MISMATCH rather than on error, and that is deliberate: raw.githubusercontent is a CDN and is
    eventually consistent, so a single-shot check moments after a push reports the PRE-push bytes and fails
    a job that did everything right — and moments after a push is exactly when the publish job reads, since
    it verifies its own work. Retrying costs a few minutes in the rare failing case and nothing in the
    common one: a first-attempt match returns immediately, which _selftest_verify pins. An unreadable file
    is retried on the same terms — it is a problem either way, and it is never converted into agreement.

    fetch, sleep and log are handed in rather than called directly so the loop can be driven with no socket
    and no wait. --check runs this file's selftests, and --check must never touch the network.
    """
    # "Did I check anything?", the same floor check() puts under its own scan. A loop over an empty target
    # list falls straight through to the success return — a PASS that reports a rule as enforced while
    # proving nothing, which is worse than no check at all. Both spellings of empty are refused: a record
    # that names nothing published, and a caller that asked for no attempts.
    if not targets:
        return (["the record names nothing as published, so there is nothing to fetch and a PASS here would "
                 "mean nothing. Check publishedThemes/publishedDocs in %s." % RECORD], [])
    if attempts < 1:
        return (["--verify-registry was asked for %d attempt(s), so it fetched nothing." % attempts], [])

    mismatches, unreadable, lines = [], [], []
    for attempt in range(1, attempts + 1):
        fetched = {}
        for rel, _label, _kind, _want in targets:
            data, why = fetch(REGISTRY_RAW + rel)
            if data is None:
                # Reported as it happens: compare_registry can only say the file could not be read, and on a
                # long retry the cause is the thing that tells an operator whether to look at the registry
                # or at the network.
                log("  could not fetch %s (%s)" % (REGISTRY_RAW + rel, why))
            fetched[rel] = data
        mismatches, unreadable, lines = compare_registry(targets, fetched)
        if not mismatches and not unreadable:
            for line in lines:
                log(line)
            return [], []
        if attempt < attempts:
            log("  attempt %d/%d disagrees; the CDN may still be serving the previous bytes, waiting %ds"
                % (attempt, attempts, delay))
            sleep(delay)
    for line in lines:
        log(line)
    return mismatches, unreadable


def run_verify_registry(rec, themes_dir, fetch, sleep, log, attempts, delay, targets=None):
    """--verify-registry with every dependency handed in. Returns 0 (match), 1 (drift), 2 (unknown).

    Split out from verify_registry so the whole verdict — including "an unreachable registry is NOT a
    verdict" — is assertable with no socket and no wait, the same way the retry loop above it is. --check
    runs this file's selftests and --check must never open a socket.

    THREE-WAY on purpose. 0 and 1 are answers; 2 is "I could not find out", and keeping it distinct is what
    stops an outage being filed as drift — which sends somebody to a registry that is fine and, more
    expensively, teaches the next reader to discount a red verify.

    `targets` is normally derived here from `rec` and `themes_dir`; the selftest passes it directly, which
    is also what lets a caller verify a set it computed earlier.
    """
    claim = rec.get(CLAIM) if isinstance(rec.get(CLAIM), dict) else {}
    sha = str(claim.get("registryCommit", "")).strip().lower()
    why = str(claim.get("unverifiedReason", "")).strip()

    log("registry: %s" % REGISTRY_RAW)
    head_body, head_why = fetch(REGISTRY_API + "commits/main")
    head = ""
    if head_body is not None:
        try:
            head = json.loads(head_body.decode("utf-8"))["sha"]
        except Exception as exc:                                        # noqa: BLE001
            # Bytes came back but they are not the API's answer — a proxy error page, a rate-limit body.
            # That is still "could not find out what HEAD is", never a fact about the record, so it is
            # folded back into the unreachable branch rather than allowed to look like a verdict.
            head_body, head_why = None, "%s: %s" % (type(exc).__name__, exc)
    if head_body is None:
        log("  could not reach the registry: %s" % head_why)
        log("  VERDICT: unknown - this is a network failure, NOT a verdict about the record.")
        return 2
    log("  registry main is at %s" % head)
    if sha:
        log("  record claims publication as %s" % sha)
        if not head.startswith(sha):
            log("  (the registry has moved since; that alone is fine - what matters is the content below)")
    elif why:
        log("  record claims NO publication: %s" % why)

    if targets is None:
        targets = registry_targets(rec, themes_dir)
    mismatches, unreadable = _verify_loop(targets, fetch, sleep, log, attempts, delay)

    log("")
    for problem in mismatches + unreadable:
        log("  " + problem)
    if mismatches:
        log("  VERDICT: the registry is NOT current with this repo.")
        return 1
    if unreadable:
        log("  VERDICT: unknown - %d published file(s) could not be read after %d attempt(s). Nothing "
            "DISAGREED; something was unreachable. NOT a verdict about the record."
            % (len(unreadable), attempts))
        return 2
    log("  VERDICT: the registry serves exactly what is bundled here.")
    if not sha:
        # Everything matched, and the record still says nobody ever published. That is not a failure — the
        # excuse may be older than the publish, or somebody may have pushed the copies without refreshing
        # the record. But it is the one moment the evidence to close it is on screen, and saying nothing
        # here is how a record stays unverified for a year while the thing it doubts has been true all along.
        log("  The record still makes no verified publish claim, so this run is the evidence to close it:")
        log("      native/tools/theme-registry-sync.py --update --registry-commit %s" % head)
    return 0


def verify_registry():
    """--verify-registry entry point. NEEDS NETWORK; never called by the probe suite.

    This is the half of the question the offline gate cannot answer: not "has a bundled theme moved since
    the record was refreshed" but "is the registry actually serving what we say it is". The publish job runs
    it immediately after its own push, and verify-registry.yml re-runs it weekly.
    """
    try:
        rec = load_record()
    except Exception as exc:                                            # noqa: BLE001
        print("cannot read %s: %s" % (RECORD, exc))
        return 2
    return run_verify_registry(rec, THEMES, fetch_raw, time.sleep, print, 6, 30)


def check():
    # Before comparing anything: is the comparison itself sound? A hash that varies by checkout makes every
    # result below meaningless in one direction or the other, so it is checked first and reported as its own
    # problem rather than as drift in a file nobody touched. The publish and verify selftests ride here for
    # the same reason: their rules stand in front of an rmtree and in front of a green verdict, and neither
    # is reachable from any probe. All three are offline; nothing below opens a socket.
    bad = selftest() + _selftest_publish() + _selftest_verify()
    themes = bundled_themes()

    # "Did I scan anything?" — a gate that walks an empty corpus prints PASS, which is worse than no gate
    # at all: it reports a rule as enforced. The floor is deliberately below the real count and above zero,
    # so a themes2/ that moved out from under this script reports itself instead of passing.
    if len(themes) < 3:
        bad.append("themes2/ holds %d theme(s) - expected at least the 3 shipped ones. This gate scanned "
                   "almost nothing; treat a PASS as meaningless until the path is fixed." % len(themes))

    try:
        rec = load_record()
    except Exception as exc:                                            # noqa: BLE001
        print("  cannot read %s: %s" % (RECORD, exc))
        return ["REGISTRY-SYNC.json is missing or unparseable - this gate is now asserting nothing."]

    # What the record says it was published against, before anything is compared to it. Printed on every
    # run, pass or fail: the point of #151 is that a record which had never been published read exactly like
    # one that had.
    claim_bad, claim_line = describe_claim(rec)
    bad.extend(claim_bad)
    if claim_line:
        print("  " + claim_line)

    published = rec.get("publishedThemes", {})
    exempt = rec.get("notPublished", {})

    for name in themes:
        path = os.path.join(THEMES, name, "theme.json")
        try:
            got, doc = canonical_hash(path)
        except Exception as exc:                                        # noqa: BLE001
            bad.append("%s/theme.json does not parse: %s" % (name, exc))
            continue

        # A view declared with an empty `elements` counts as not declared everywhere in the engine (see
        # THEME_FORMAT.md), so a theme that "declares" one is publishing a view that silently falls back.
        for view, body in sorted(doc.get("views", {}).items()):
            if not body.get("elements"):
                bad.append("%s declares view '%s' with no elements - the engine treats that as undeclared, "
                           "so the view falls back to the built-in layout." % (name, view))

        # Every bundled theme is either tracked as published or explicitly exempted WITH A REASON. Without
        # this, the cheapest way to silence a red gate is to delete the theme's entry, and the drift this
        # exists to catch walks straight back in.
        if name in published:
            want = published[name].get("canonicalSha256", "")
            if got != want:
                bad.append(
                    "%s has changed since the record was last refreshed.\n"
                    "      recorded %s\n"
                    "      now      %s\n"
                    "    This record is what the offline gate compares against, and it no longer describes\n"
                    "    the theme in this repo — so nothing here says the registry's '%s' is anything but\n"
                    "    the older theme, served under the same name. Refresh it and commit it with the\n"
                    "    theme change:\n"
                    "      native/tools/theme-registry-sync.py --update --assume-published \"<why>\"\n"
                    "    You do not copy anything into the registry yourself, and \"<why>\" is normally just\n"
                    "    that: on merge to main the publish-themes workflow pushes '%s' to the registry,\n"
                    "    then rewrites this record with --update --registry-commit <the sha it pushed> and\n"
                    "    commits that back here. Use --registry-commit yourself only if you published by\n"
                    "    hand." % (name, want or "(none)", got, name, name))
        elif name not in exempt:
            bad.append("%s is bundled but appears in neither publishedThemes nor notPublished in "
                       "REGISTRY-SYNC.json. Add it to whichever it is - an untracked theme is exactly the "
                       "state this gate exists to prevent." % name)
        elif not str(exempt.get(name, "")).strip():
            bad.append("%s is listed in notPublished with no reason. An unexplained exemption is "
                       "indistinguishable from an oversight." % name)

    # A theme tracked as published that no longer exists here: the registry is now serving something this
    # repo has dropped, which is the same two-copies problem pointing the other way.
    for name in sorted(published):
        if name not in themes:
            bad.append("%s is recorded as published to the registry but is no longer bundled here. Either "
                       "restore it or remove it from the registry and from this record." % name)

    for doc_name in DOCS:
        path = os.path.join(THEMES, doc_name)
        if not os.path.isfile(path):
            bad.append("%s is missing from themes2/ - the registry publishes a copy of it." % doc_name)
            continue
        want = rec.get("publishedDocs", {}).get(doc_name, {}).get("sha256", "")
        got = doc_hash(path)
        if got != want:
            bad.append(
                "%s has changed since the record was last refreshed.\n"
                "      recorded %s\n"
                "      now      %s\n"
                "    That file is the format reference theme authors read, and the registry serves its own\n"
                "    copy. Refresh the record and commit it with the change:\n"
                "      native/tools/theme-registry-sync.py --update --assume-published \"<why>\"\n"
                "    The publish workflow copies it into the registry alongside the themes on merge to main\n"
                "    and rewrites the claim with the sha it pushed."
                % (doc_name, want or "(none)", got))

    return bad


def update(registry_commit=None, assume_reason=None):
    """Refresh the record — and make it say what it was refreshed FOR.

    `registry_commit` and `assume_reason` are mutually exclusive and one is REQUIRED; main() refuses a bare
    --update. That refusal is the fix for #151: the old bare form made "record a publish" and "record an
    intention to publish" the same keystroke, so the two became indistinguishable in the file, and the one
    that had actually happened was the wrong one.

    The publish workflow is the normal caller of the --registry-commit form: it pushes to the registry, reads
    back the sha it created, calls this, and commits the result to this repo. A human writing
    --registry-commit by hand is now the exception (a publish done by hand), and --assume-published is what a
    PR that edits a theme writes, because at that moment the copy genuinely has not happened yet.
    """
    rec = load_record()
    rec.setdefault("publishedThemes", {})
    rec.setdefault("publishedDocs", {})
    claim = {}
    if registry_commit:
        claim["registryCommit"] = registry_commit.strip().lower()
    else:
        claim["unverifiedReason"] = assume_reason.strip()
    # Whatever was there before is REPLACED, never merged: leaving a stale registryCommit next to a fresh
    # unverifiedReason (or the reverse) would make the record claim two things at once, which describe_claim
    # rejects — but it should not be possible to write in the first place.
    rec[CLAIM] = claim
    for name in bundled_themes():
        if name in rec["publishedThemes"]:
            rec["publishedThemes"][name]["canonicalSha256"] = canonical_hash(
                os.path.join(THEMES, name, "theme.json"))[0]
    for doc_name in DOCS:
        path = os.path.join(THEMES, doc_name)
        if os.path.isfile(path):
            rec["publishedDocs"].setdefault(doc_name, {})["sha256"] = doc_hash(path)
    with open(RECORD, "w", encoding="utf-8", newline="\n") as f:
        json.dump(rec, f, indent=2, ensure_ascii=False)
        f.write("\n")
    print("updated %s" % RECORD)
    if registry_commit:
        print("  published against registry commit %s" % registry_commit.strip().lower())
    else:
        print("  RECORDED AS UNVERIFIED: %s" % assume_reason.strip())
        print("  Nothing was checked against the registry. That sentence is now in the record and prints on")
        print("  every --check run until the publish job (or somebody) reruns with --registry-commit.")


UPDATE_REFUSAL = """\
--update on its own is refused (issue #151).

  It recomputes the record from the BUNDLED themes and never looks at the registry, so on its own it
  records "somebody ran this", not "the registry was updated". That is how the record came to assert a
  publish that had not happened: #57 and #32 ran it without step 3, and the registry served the pre-#29
  Triple under a green gate for days.

  Say which one this is:

    --update --assume-published "<why>"    the copy has NOT happened. In a PR that edits a theme this is
                                           the normal answer, because the publish job does the copy on
                                           merge. The reason goes into the record and prints on every
                                           --check run until the job replaces it with a real sha.

    --update --registry-commit <sha>       the copies ARE in the registry, as <sha>. The publish workflow
                                           writes this itself after its own push; write it by hand only if
                                           you published by hand. Falsifiable:  --verify-registry
"""


def main():
    args = sys.argv[1:]
    arg = args[0] if args else ""
    if arg == "--check":
        problems = check()
        for p in problems:
            print("  " + p)
        return 1 if problems else 0
    if arg == "--publish":
        if len(args) < 2:
            print("  --publish needs the path to a registry checkout")
            return 1
        problems = publish(args[1])
        for p in problems:
            print("  " + p)
        return 1 if problems else 0
    if arg == "--verify-registry":
        # The ONLY mode that touches the network. --check runs offline in the probe suite; keeping the fetch
        # behind its own flag is what lets that stay true.
        return verify_registry()
    if arg == "--update":
        commit = reason = None
        rest = args[1:]
        while rest:
            flag = rest.pop(0)
            value = rest.pop(0) if rest else ""
            if flag == "--registry-commit":
                commit = value
            elif flag == "--assume-published":
                reason = value
            else:
                print("unknown option %s" % flag)
                print(UPDATE_REFUSAL)
                return 2
        if bool(commit) == bool(reason):                      # neither, or both
            if commit and reason:
                print("--registry-commit and --assume-published are mutually exclusive: the record holds one "
                      "answer, not two.")
            print(UPDATE_REFUSAL)
            return 2
        if commit and not SHA_RE.match(commit.strip().lower()):
            print("--registry-commit %r is not a git sha (7-40 hex). A sha nobody can look up is not a "
                  "citation." % commit)
            return 2
        if reason is not None and not reason.strip():
            print("--assume-published needs a reason. An empty one is the silent record #151 is about.")
            return 2
        update(registry_commit=commit, assume_reason=reason)
        return 0
    for name in bundled_themes():
        print("%-12s %s" % (name, canonical_hash(os.path.join(THEMES, name, "theme.json"))[0]))
    for doc_name in DOCS:
        path = os.path.join(THEMES, doc_name)
        if os.path.isfile(path):
            print("%-12s %s" % (doc_name, doc_hash(path)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
