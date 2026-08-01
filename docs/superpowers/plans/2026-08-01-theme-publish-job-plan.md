# Theme registry publish job Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the hand step in `REGISTRY-SYNC.json`'s republish procedure with a workflow that pushes changed bundled themes to the community registry and proves it did.

**Architecture:** Two new modes on `native/tools/theme-registry-sync.py` — `--publish <dir>` (pure filesystem) and `--verify-remote` (network) — driven by two new GitHub Actions workflows. The copy rules stay in the script because it already owns which themes are published and where they land; the YAML contributes credential, checkout and invocation only.

**Tech Stack:** Python 3 (stdlib only — `urllib.request`, no `requests`), GitHub Actions, `actions/checkout@v4` with `ssh-key`, bash.

## Global Constraints

- **No AI attribution in commits or PR bodies.** No `Co-Authored-By: Claude` trailer, no "Generated with Claude Code" line, no tool name in the message body (repo root `CLAUDE.md`).
- **Conventional commit prefixes** (`feat:`, `fix:`, `docs:`, `refactor:`, `test:`).
- **Stdlib only.** `theme-registry-sync.py` runs inside the offline probe suite; it must not grow a third-party dependency. Network code must live behind `--verify-remote` and never execute on `--check`.
- **`--check` stays offline.** The probe suite is deliberately network-free and windowless. `selftest()` runs inside `check()`, so anything added there must not touch the network.
- **One implementation of each hash rule.** `--verify-remote` must hash the registry's bytes with the same code `--check` hashes the bundled bytes with. A second spelling of either rule is the exact drift this whole area exists to prevent.
- **The record is the only list of what is published.** `publishedThemes` / `publishedDocs` decide scope. Never walk `themes2/` to decide what to copy — the registry carries `Default`, `Grid`, `Lumen` and `Midnight`, which this repo does not bundle, and a directory-walk sync would delete them on its first run.
- **Secret name:** `THEMES_REGISTRY_DEPLOY_KEY`. **Registry:** `cubman3134/everythingbox-themes`, branch `main`.
- **Spec:** `docs/superpowers/specs/2026-08-01-theme-publish-job-design.md`.
- **Run the script as `python3`** in workflows (ubuntu runners), and `python` locally on Windows.

---

## File Structure

| File | Responsibility |
| --- | --- |
| `native/tools/theme-registry-sync.py` (modify) | Gains `published_targets`, `publish_into`, `publish`, `remote_targets`, `compare_remote`, `verify_remote`, byte-level hash helpers, and two new `selftest` blocks. |
| `.github/workflows/publish-themes.yml` (create) | Credential, registry checkout, invoke `--publish`, commit, push, post-push verify. |
| `.github/workflows/verify-registry.yml` (create) | Weekly + manual `--verify-remote`. No credential. |
| `native/themes2/REGISTRY-SYNC.json` (modify) | `_readme` currently describes this job as unbuilt; rewrite to describe what exists. |
| `CONTRIBUTING.md` (modify) | The theme-change procedure loses its copy-by-hand step. |

Task order: the script's pure half first (Task 1), its network half second (Task 2), then the workflow that uses both (Task 3), then the weekly verifier and the docs that describe the whole thing (Task 4).

---

### Task 1: `--publish` — copy the record's targets into a registry checkout

**Files:**
- Modify: `native/tools/theme-registry-sync.py`

**Interfaces:**
- Consumes: existing `load_record()`, `THEMES`, `DOCS`, `selftest()`.
- Produces: `published_targets(rec) -> list[(kind, name, src_abs, dest_rel)]` where `kind` is `"theme"` or `"doc"`; `publish_into(rec, themes_dir, registry_dir) -> (copied: list[str], problems: list[str])`; `publish(registry_dir) -> list[str]`. Task 2 calls `published_targets`; Task 3 invokes `--publish`.

- [ ] **Step 1: Write the failing selftest**

In `native/tools/theme-registry-sync.py`, add this function immediately after the existing `selftest()`:

```python
def _selftest_publish():
    """--publish copies what the record names, replaces wholesale, and touches nothing else.

    Three properties, each a real failure mode rather than a hypothetical. (1) The registry carries themes
    this repo does not bundle — Default, Grid, Lumen, Midnight — so a sync that walked themes2/ instead of
    the record would delete four themes on its first run. (2) A theme folder must be REPLACED, not merged:
    the canonical hash covers theme.json alone, so a sound file dropped from the bundled theme would
    survive every check we have while still being served. (3) A recorded theme the registry does not
    already carry is a catalog change, and index.json entries hold a `description` that exists nowhere in
    theme.json — publishing the folder alone would serve a theme nothing lists.
    """
    import tempfile
    problems = []
    rec = {
        "publishedThemes": {"Shared": {"registryPath": "themes2/Shared", "canonicalSha256": ""}},
        "publishedDocs": {"DOC.md": {"registryPath": "DOC.md", "sha256": ""}},
    }
    with tempfile.TemporaryDirectory() as tmp:
        src_themes = os.path.join(tmp, "bundled")
        registry = os.path.join(tmp, "registry")

        os.makedirs(os.path.join(src_themes, "Shared", "sounds"))
        _write(os.path.join(src_themes, "Shared", "theme.json"), '{"name":"Shared","views":{}}')
        _write(os.path.join(src_themes, "Shared", "sounds", "new.wav"), "NEW")
        _write(os.path.join(src_themes, "DOC.md"), "# doc\n")

        # The registry starts with an OLDER Shared (carrying a file the bundled copy no longer has) and a
        # registry-only theme the record says nothing about.
        os.makedirs(os.path.join(registry, "themes2", "Shared", "sounds"))
        _write(os.path.join(registry, "themes2", "Shared", "theme.json"), '{"name":"old"}')
        _write(os.path.join(registry, "themes2", "Shared", "sounds", "stale.wav"), "OLD")
        os.makedirs(os.path.join(registry, "themes2", "RegistryOnly"))
        _write(os.path.join(registry, "themes2", "RegistryOnly", "theme.json"), '{"name":"RegistryOnly"}')
        _write(os.path.join(registry, "DOC.md"), "# stale\n")

        copied, bad = publish_into(rec, src_themes, registry)
        if bad:
            problems.append("--publish refused a well-formed registry: %s" % "; ".join(bad))
        if sorted(copied) != ["DOC.md", "themes2/Shared"]:
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
        rec2 = {"publishedThemes": {"Absent": {"registryPath": "themes2/Absent", "canonicalSha256": ""}},
                "publishedDocs": {}}
        os.makedirs(os.path.join(src_themes, "Absent"))
        _write(os.path.join(src_themes, "Absent", "theme.json"), '{"name":"Absent"}')
        copied2, bad2 = publish_into(rec2, src_themes, registry)
        if not bad2:
            problems.append("--publish created themes2/Absent in the registry. A theme the registry does "
                            "not already carry needs an index.json entry that cannot be generated.")
        if copied2:
            problems.append("--publish reported copies while refusing: %r" % copied2)
        if os.path.exists(os.path.join(registry, "themes2", "Absent")):
            problems.append("--publish created the folder it claimed to refuse.")
    return problems
```

Add these two helpers just above `_selftest_publish` (they keep the selftests readable and are used by Task 2's as well):

```python
def _write(path, text):
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)


def _read(path):
    with open(path, encoding="utf-8", newline="") as f:
        return f.read()
```

And fold it into `check()` — find the line `bad = selftest()` near the top of `check()` and replace it with:

```python
    bad = selftest() + _selftest_publish()
```

- [ ] **Step 2: Run it to verify it fails**

```bash
python native/tools/theme-registry-sync.py --check
```

Expected: FAIL, exit 1, with a `NameError: name 'publish_into' is not defined` traceback — the selftest calls a function that does not exist yet.

- [ ] **Step 3: Write the implementation**

Add `import shutil` to the imports at the top of the file (alphabetical: after `os`, before `sys`).

Add these three functions immediately after `load_record()`:

```python
def published_targets(rec):
    """(kind, name, registry-relative destination) for everything the record publishes.

    ONE place decides what "published" means. --publish copies exactly this list and --verify-remote fetches
    exactly this list, so the two cannot disagree about scope — and neither walks themes2/, which carries a
    different set of themes than the registry does.

    No source path is returned: --publish resolves one against whichever themes dir it was handed, and
    --verify-remote has no local source at all. Returning a value only one caller wants, computed from a
    module global the other caller is trying not to use, is how a helper starts lying.
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

    Returns (copied, problems). A non-empty problems list means NOTHING was copied for that target and the
    caller must not commit: publishing half a record is worse than publishing none of it.
    """
    problems = []
    copied = []
    if not os.path.isdir(registry_dir):
        return [], ["registry checkout not found at %s" % registry_dir]

    for kind, name, dest_rel in published_targets(rec):
        # `name` is the folder name for a theme and the filename for a doc, and both sit directly under
        # themes_dir — which is why the source can be derived here rather than handed in.
        src = os.path.join(themes_dir, name)
        dest = os.path.join(registry_dir, *dest_rel.split("/"))

        if not os.path.exists(src):
            problems.append("%s is recorded as published but is missing from themes2/." % name)
            continue

        # REFUSAL: never CREATE a path in the registry. A recorded theme the registry does not already
        # carry is a catalog change — index.json entries hold a `description` that exists nowhere in
        # theme.json, so it cannot be generated, and a folder nothing lists is a theme nobody can find.
        if kind == "theme" and not os.path.isdir(dest):
            problems.append(
                "%s is recorded as published to %s, but the registry has no such folder.\n"
                "    Add its index.json entry there by hand first (name, author, description, dir): the\n"
                "    description exists nowhere in theme.json, so this job will not invent one." % (name, dest_rel))
            continue
        if kind == "doc" and not os.path.isfile(dest):
            problems.append("%s is recorded as published to %s, but the registry has no such file."
                            % (name, dest_rel))
            continue

        if kind == "theme":
            # WHOLESALE, not merge. The canonical hash covers theme.json alone, so a sound or font dropped
            # from the bundled theme would otherwise survive here and keep being served, invisible to every
            # check in this file.
            shutil.rmtree(dest)
            shutil.copytree(src, dest)
        else:
            shutil.copy2(src, dest)
        copied.append(dest_rel)

    if problems:
        return [], problems
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
```

Wire it into `main()` — add this immediately after the `--update` branch:

```python
    if arg == "--publish":
        if len(sys.argv) < 3:
            print("  --publish needs the path to a registry checkout")
            return 1
        problems = publish(sys.argv[2])
        for p in problems:
            print("  " + p)
        return 1 if problems else 0
```

- [ ] **Step 4: Run it to verify it passes**

```bash
python native/tools/theme-registry-sync.py --check
```

Expected: exit 0, no output. The selftest now exercises `publish_into` against scratch directories.

- [ ] **Step 5: Confirm the assertions bite**

Temporarily change `shutil.rmtree(dest)` to `pass` in `publish_into` and re-run:

```bash
python native/tools/theme-registry-sync.py --check
```

Expected: FAIL naming the merge — "`--publish` MERGED into the theme folder instead of replacing it". Restore the line and confirm `--check` is clean again. An assertion that passes against the broken behaviour tests nothing.

- [ ] **Step 6: Try it against the real registry, read-only**

```bash
git clone https://github.com/cubman3134/everythingbox-themes.git /tmp/ebt-dry
python native/tools/theme-registry-sync.py --publish /tmp/ebt-dry
git -C /tmp/ebt-dry status --porcelain
```

Expected: the `--publish` output lists `THEME_FORMAT.md` and the three `themes2/<Name>` paths, and `git status` is **empty** — the registry is currently in sync (it was republished when #131 closed), so a correct `--publish` is a no-op. Any non-empty status here means the copy is doing something the record does not describe; investigate before continuing. Then `rm -rf /tmp/ebt-dry`.

- [ ] **Step 7: Commit**

```bash
git add native/tools/theme-registry-sync.py
git commit -m "feat: theme-registry-sync --publish copies the record's targets into a registry checkout"
```

---

### Task 2: `--verify-remote` — prove the registry matches the record

**Files:**
- Modify: `native/tools/theme-registry-sync.py`

**Interfaces:**
- Consumes: `published_targets` from Task 1; existing `canonical_hash`, `doc_hash`, `load_record`.
- Produces: `canonical_hash_bytes(data) -> (str, dict)`; `doc_hash_bytes(data) -> str`; `raw_base(rec) -> str`; `remote_targets(rec) -> list[(path_rel, label, want, kind)]`; `compare_remote(rec, fetched) -> list[str]`; `verify_remote() -> list[str]`. Tasks 3 and 4 invoke `--verify-remote`.

- [ ] **Step 1: Write the failing selftest**

Add after `_selftest_publish`:

```python
def _selftest_verify():
    """The remote comparison agrees with the local one, and a refusal is never silent.

    --verify-remote exists to prove the registry matches the record. If it hashed remote bytes even
    slightly differently from the way --check hashes local ones — a different JSON separator, a different
    newline rule — it would report drift that is not there, or worse, agree when the two differ. So the
    property under test is that both routes produce the SAME hash for the same content, not that either
    produces a particular constant.

    The fetch itself is not covered here and cannot be: this script runs inside a probe suite that is
    deliberately network-free. Only the comparison is pinned.
    """
    problems = []
    theme_text = '{\n  "name": "T",\n  "views": {}\n}\n'
    doc_text = "# Doc\n\nBody.\n"

    rec = {
        "registry": "https://github.com/cubman3134/everythingbox-themes",
        "publishedThemes": {"T": {"registryPath": "themes2/T",
                                  "canonicalSha256": canonical_hash_bytes(theme_text.encode())[0]}},
        "publishedDocs": {"DOC.md": {"registryPath": "DOC.md",
                                     "sha256": doc_hash_bytes(doc_text.encode())}},
    }

    # Matching content, spelled differently on both axes the hashes are supposed to ignore: the theme
    # reindented, the doc with CRLF line endings.
    reindented = json.dumps(json.loads(theme_text), indent=4).encode()
    crlf_doc = doc_text.replace("\n", "\r\n").encode()
    bad = compare_remote(rec, {"themes2/T/theme.json": reindented, "DOC.md": crlf_doc})
    if bad:
        problems.append("--verify-remote reported drift on content that matches: %s" % "; ".join(bad))

    # Real drift on each axis.
    changed = json.dumps({"name": "T", "views": {"home": {}}}).encode()
    if not compare_remote(rec, {"themes2/T/theme.json": changed, "DOC.md": crlf_doc}):
        problems.append("--verify-remote missed a changed theme.json.")
    if not compare_remote(rec, {"themes2/T/theme.json": reindented, "DOC.md": b"# Different\n"}):
        problems.append("--verify-remote missed a changed doc.")

    # A fetch that failed must be a problem, never a pass. This is the one that decides whether an outage
    # reads as "registry is fine".
    if not compare_remote(rec, {"themes2/T/theme.json": None, "DOC.md": crlf_doc}):
        problems.append("--verify-remote treated an unreadable file as agreement.")
    if not compare_remote(rec, {"DOC.md": crlf_doc}):
        problems.append("--verify-remote treated a missing file as agreement.")

    # Unparseable JSON is a problem, not a crash.
    if not compare_remote(rec, {"themes2/T/theme.json": b"not json", "DOC.md": crlf_doc}):
        problems.append("--verify-remote did not report an unparseable theme.json.")

    # raw_base derives the fetch root from the record rather than hardcoding it.
    if raw_base(rec) != "https://raw.githubusercontent.com/cubman3134/everythingbox-themes/main/":
        problems.append("raw_base built %r from the recorded registry url." % raw_base(rec))
    if raw_base({"registry": "https://example.com/x"}) != "":
        problems.append("raw_base accepted a non-GitHub registry url.")
    return problems
```

Update the `check()` line again:

```python
    bad = selftest() + _selftest_publish() + _selftest_verify()
```

- [ ] **Step 2: Run it to verify it fails**

```bash
python native/tools/theme-registry-sync.py --check
```

Expected: FAIL with `NameError: name 'canonical_hash_bytes' is not defined`.

- [ ] **Step 3: Split the hash rules so there is one implementation of each**

Replace the existing `canonical_hash` and `doc_hash` with byte-level cores plus path wrappers. This is the load-bearing part of the task: `--verify-remote` holds bytes from the network while `--check` holds a path, and two spellings of the same rule is the drift this file exists to prevent.

```python
def canonical_hash_bytes(data):
    """SHA-256 of a theme.json's MEANING — parsed, re-serialised sorted+compact, hashed.

    Byte-level so --verify-remote can hash a response body through the SAME code --check hashes a file
    with. Two spellings of this rule would let the registry and the record agree on a value neither the
    app nor the gate would compute.
    """
    doc = json.loads(data.decode("utf-8"))
    blob = json.dumps(doc, sort_keys=True, separators=(",", ":"), ensure_ascii=True)
    return hashlib.sha256(blob.encode("utf-8")).hexdigest(), doc


def doc_hash_bytes(data):
    """SHA-256 of a document's TEXT with the newline spelling normalised out first. See doc_hash."""
    return hashlib.sha256(data.replace(b"\r\n", b"\n")).hexdigest()


def canonical_hash(path):
    """SHA-256 of a theme.json's MEANING — parsed, re-serialised sorted+compact, hashed."""
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
```

`canonical_hash` still returns `(hash, doc)`, so `check()`'s empty-`elements` inspection is unchanged.

- [ ] **Step 4: Write the comparison and the fetch**

Add `import time` and `import urllib.error` and `import urllib.request` to the imports.

Add these after `published_targets`:

```python
def raw_base(rec):
    """The raw.githubusercontent root for the registry the record names.

    Derived rather than hardcoded so the record stays the single source of truth for WHICH registry this
    is. Returns "" for anything that is not a github.com url, which the caller reports as a refusal.
    """
    url = (rec.get("registry") or "").rstrip("/")
    prefix = "https://github.com/"
    if not url.startswith(prefix):
        return ""
    return "https://raw.githubusercontent.com/" + url[len(prefix):] + "/main/"


def remote_targets(rec):
    """(registry-relative path, label, expected hash, hash kind) for everything --verify-remote fetches."""
    out = []
    for kind, name, dest_rel in published_targets(rec):
        if kind == "theme":
            meta = rec["publishedThemes"][name]
            out.append((dest_rel + "/theme.json", name, meta.get("canonicalSha256", ""), "canonical"))
        else:
            meta = rec["publishedDocs"][name]
            out.append((dest_rel, name, meta.get("sha256", ""), "doc"))
    return out


def compare_remote(rec, fetched):
    """Compare fetched registry bytes against the record. Pure, so selftest covers it.

    `fetched` maps a registry-relative path to bytes, or to None when the fetch failed. A missing or
    unreadable entry is a PROBLEM, never a pass — an outage that reads as "the registry is fine" is the one
    failure mode a verifier must not have.
    """
    problems = []
    for path, label, want, kind in remote_targets(rec):
        data = fetched.get(path)
        if data is None:
            problems.append("could not read %s from the registry. Until it can be read, this check is "
                            "asserting nothing about %s." % (path, label))
            continue
        try:
            got = canonical_hash_bytes(data)[0] if kind == "canonical" else doc_hash_bytes(data)
        except Exception as exc:                                        # noqa: BLE001
            problems.append("%s in the registry does not parse: %s" % (path, exc))
            continue
        if got != want:
            problems.append(
                "%s in the registry does not match this repo's record.\n"
                "      recorded %s\n"
                "      registry %s\n"
                "    Anyone downloading '%s' is getting content this repo did not publish. Either the\n"
                "    publish job did not run, or the registry was edited directly."
                % (label, want or "(none)", got, label))
    return problems


def verify_remote():
    """Fetch the registry's published files and compare them to the record.

    Retries on mismatch rather than on error, and that is deliberate: raw.githubusercontent is a CDN and is
    eventually consistent, so a single-shot check moments after a push reports the OLD bytes and fails a
    job that did everything right. Retrying a mismatch costs a few minutes in the rare failing case and
    nothing in the common one. A first-attempt match returns immediately.
    """
    try:
        rec = load_record()
    except Exception as exc:                                            # noqa: BLE001
        return ["cannot read %s: %s" % (RECORD, exc)]

    base = raw_base(rec)
    if not base:
        return ["the record's \"registry\" is not a github.com url, so there is nothing to fetch: %r"
                % rec.get("registry")]

    attempts = 6
    delay = 30
    problems = []
    for attempt in range(1, attempts + 1):
        fetched = {}
        for path, _label, _want, _kind in remote_targets(rec):
            req = urllib.request.Request(base + path, headers={"Cache-Control": "no-cache"})
            try:
                with urllib.request.urlopen(req, timeout=30) as resp:
                    fetched[path] = resp.read()
            except (urllib.error.URLError, OSError):
                fetched[path] = None
        problems = compare_remote(rec, fetched)
        if not problems:
            print("  registry matches the record (%d file(s) checked)" % len(fetched))
            return []
        if attempt < attempts:
            print("  attempt %d/%d disagrees; the CDN may still be serving the previous bytes, waiting %ds"
                  % (attempt, attempts, delay))
            time.sleep(delay)
    return problems
```

Wire into `main()` after the `--publish` branch:

```python
    if arg == "--verify-remote":
        problems = verify_remote()
        for p in problems:
            print("  " + p)
        return 1 if problems else 0
```

- [ ] **Step 5: Run it to verify it passes**

```bash
python native/tools/theme-registry-sync.py --check
```

Expected: exit 0, no output.

- [ ] **Step 6: Run the real remote check**

```bash
python native/tools/theme-registry-sync.py --verify-remote
```

Expected: exit 0, prints `registry matches the record (4 file(s) checked)` on the first attempt — the registry was republished when #131 closed, so it is currently in sync. If it retries, something is genuinely out of sync; stop and investigate rather than proceeding.

- [ ] **Step 7: Confirm the mismatch path is real**

Temporarily change one character of `Triple`'s `canonicalSha256` in `native/themes2/REGISTRY-SYNC.json` and run:

```bash
python native/tools/theme-registry-sync.py --verify-remote
```

Expected: reports the mismatch for `Triple` with recorded-vs-registry hashes, retries five times (about 2.5 minutes), then exits 1. This also confirms the retry does not mask a real difference. Restore the file with `git checkout -- native/themes2/REGISTRY-SYNC.json` and verify `git status` is clean.

- [ ] **Step 8: Commit**

```bash
git add native/tools/theme-registry-sync.py
git commit -m "feat: theme-registry-sync --verify-remote proves the registry matches the record"
```

---

### Task 3: The publish workflow

**Files:**
- Create: `.github/workflows/publish-themes.yml`

**Interfaces:**
- Consumes: `--check`, `--publish <dir>` and `--verify-remote` from Tasks 1–2; the secret `THEMES_REGISTRY_DEPLOY_KEY`.
- Produces: nothing later tasks consume.

- [ ] **Step 1: Write the workflow**

```yaml
# Publish the bundled themes the community registry also serves.
#
# Three themes exist twice: here under native/themes2, and in
# github.com/cubman3134/everythingbox-themes, so the app's Appearance panel can offer them for download.
# Keeping the two in step was a hand step, and it failed twice — issue #57 recorded the intent to
# republish and nobody did, so the registry served a `home`-only Triple and a Channels without
# nowplayingAudio for months (#131). This job removes the hand step: the registry copies are now
# generated content.
#
# The copy rules are NOT in this file. native/tools/theme-registry-sync.py already knows which themes are
# published and where they land, and a second copy of that list here would be exactly the kind of drift
# this whole area exists to prevent. This workflow contributes a credential, a checkout and an invocation.
name: publish themes

on:
  push:
    branches: [main]
    paths:
      - 'native/themes2/**'
  workflow_dispatch:

# Two pushes to main in quick succession would otherwise race each other into the registry, and the loser
# gets a rejected push rather than a merge. Serialise instead, and never cancel a run mid-publish: a
# cancelled job can have pushed without verifying.
concurrency:
  group: publish-themes
  cancel-in-progress: false

permissions:
  contents: read

jobs:
  publish:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4

      # BEFORE touching the registry. If the record and the bundled themes disagree, someone merged
      # without running --update, and publishing would push content the record does not describe — the
      # same drift pointing the other way. Refuse while the registry is still untouched.
      - name: The record must describe the bundled themes
        run: python3 native/tools/theme-registry-sync.py --check

      # Named explicitly so a missing secret reads as a configuration error rather than as a mysterious
      # checkout failure three steps later. Compared through env rather than inline so the value is never
      # interpolated into a command line.
      - name: The deploy key must be configured
        env:
          KEY: ${{ secrets.THEMES_REGISTRY_DEPLOY_KEY }}
        run: |
          if [ -z "$KEY" ]; then
            echo "::error::THEMES_REGISTRY_DEPLOY_KEY is not set. Generate an ed25519 key, add the public"
            echo "::error::half to everythingbox-themes as a deploy key WITH WRITE ACCESS, and the private"
            echo "::error::half here as that secret. See docs/superpowers/specs/2026-08-01-theme-publish-job-design.md"
            exit 1
          fi

      - name: Check out the registry
        uses: actions/checkout@v4
        with:
          repository: cubman3134/everythingbox-themes
          ssh-key: ${{ secrets.THEMES_REGISTRY_DEPLOY_KEY }}
          path: registry
          # A shallow checkout is fine: this job only ever adds one commit on top of main.
          fetch-depth: 1

      - name: Copy the published themes over
        run: python3 native/tools/theme-registry-sync.py --publish registry

      - name: Commit and push
        working-directory: registry
        run: |
          if [ -z "$(git status --porcelain)" ]; then
            echo "registry already in sync — nothing to publish"
            exit 0
          fi
          git status --short
          git config user.name "everythingbox-publish"
          git config user.email "publish@everythingbox.invalid"
          git add -A
          git commit -m "Publish themes from EverythingBox@${GITHUB_SHA:0:7}

          Generated by the publish-themes workflow in cubman3134/EverythingBox.
          The bundled copies under native/themes2 are the source; edit them there."
          git push

      # The job proves its own work. A push can succeed and still leave the registry wrong — an incomplete
      # copy, a path the record names that nothing checked. This fetches what the registry actually serves
      # and compares it to the record. It retries internally because raw.githubusercontent is eventually
      # consistent and would otherwise report the pre-push bytes.
      - name: Verify the registry now matches the record
        run: python3 native/tools/theme-registry-sync.py --verify-remote
```

- [ ] **Step 2: Validate the YAML parses**

```bash
python -c "import yaml,sys; yaml.safe_load(open('.github/workflows/publish-themes.yml',encoding='utf-8')); print('YAML OK')"
```

Expected: `YAML OK`. If `yaml` is not installed, run `python -m pip install pyyaml` first — this is a local check only and adds no dependency to the repo.

- [ ] **Step 3: Confirm the copy rules are not duplicated here**

```bash
grep -nE "Channels|Night|Triple|THEME_FORMAT" .github/workflows/publish-themes.yml
```

Expected: **no matches**. Any theme name appearing in this file is a second copy of the record's list — the thing the header comment says it is not doing. If a name appears, move that decision into the script.

- [ ] **Step 4: Commit**

```bash
git add .github/workflows/publish-themes.yml
git commit -m "feat: publish the shared themes to the registry on a push to main"
```

---

### Task 4: The weekly verifier, and the docs that describe the whole thing

**Files:**
- Create: `.github/workflows/verify-registry.yml`
- Modify: `native/themes2/REGISTRY-SYNC.json` (the `_readme` array)
- Modify: `CONTRIBUTING.md`

**Interfaces:**
- Consumes: `--verify-remote` from Task 2.
- Produces: nothing.

- [ ] **Step 1: Write the verifier workflow**

```yaml
# Does the community registry actually serve what this repo's record says it does?
#
# The publish job verifies its own push, which covers the case where a publish RAN. This covers the cases
# it structurally cannot: a registry edited directly, a revert there, a publish that failed and was never
# retried, or a bundled-theme change that never triggered the publisher because it landed outside
# native/themes2/**.
#
# Deliberately NOT triggered on push. That would race the publish job and report a drift the publisher is
# thirty seconds from fixing — a gate that cries wolf gets muted, and a muted gate reports nothing.
#
# No credential: the registry is public and this only reads.
name: verify registry

on:
  schedule:
    # Mondays, offset off the hour so it does not pile up with everything else scheduled at :00.
    - cron: "23 7 * * 1"
  workflow_dispatch:

permissions:
  contents: read

jobs:
  verify:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: The registry must match the record
        run: python3 native/tools/theme-registry-sync.py --verify-remote
```

- [ ] **Step 2: Validate it parses**

```bash
python -c "import yaml; yaml.safe_load(open('.github/workflows/verify-registry.yml',encoding='utf-8')); print('YAML OK')"
```

Expected: `YAML OK`.

- [ ] **Step 3: Rewrite the record's `_readme`**

`native/themes2/REGISTRY-SYNC.json`'s `_readme` currently ends by describing this job as hypothetical ("The guarantee is a publish job… which is why it is not wired up here"). Replace the whole `_readme` array with:

```json
  "_readme": [
    "What the community theme registry is expected to be serving for each theme this app ALSO bundles.",
    "",
    "Three of the themes in this folder exist twice: here, and in the registry at the url below, so the",
    "Appearance panel can send users somewhere to browse and download them. Nothing used to notice when a",
    "bundled theme gained a view and its registry twin did not, and both had quietly rotted (issue #57):",
    "the registry's Channels had no nowplayingAudio view, a grid instead of the channels browse layout and",
    "no detail actionrow, and its Triple had nothing but `home` — downloading either gave you a strictly",
    "worse theme than the one already in the app, under the same name.",
    "",
    "canonicalSha256 is the hash of the theme's MEANING, not its bytes: parse the theme.json, re-serialise",
    "it with sorted keys and no whitespace, hash that. So a reformat is not drift and the two repos are free",
    "to spell the same theme differently. native/tools/theme-registry-sync.py computes it; the headless probe",
    "suite runs that script with --check on every push.",
    "",
    "TO CHANGE A PUBLISHED THEME:",
    "  1. edit it here, as usual;",
    "  2. the gate goes red, naming the theme;",
    "  3. run  native/tools/theme-registry-sync.py --update  and commit the result with the theme change.",
    "",
    "There is no longer a step where you copy anything into the registry by hand. On a push to main that",
    "touches native/themes2/**, the publish-themes workflow checks the registry out with a deploy key,",
    "copies over exactly the targets listed below, pushes, and then re-fetches what the registry serves to",
    "confirm it matches. The verify-registry workflow re-checks the same thing every Monday, which catches",
    "what the publisher cannot see: a direct edit there, a revert, or a publish that never ran.",
    "",
    "So this record is now a FACT rather than an intent — but only for the themes it lists. Adding a NEW",
    "theme here is still a two-repo change: the publish job refuses to create a folder the registry does",
    "not already carry, because index.json needs a `description` that exists nowhere in theme.json and a",
    "theme folder nothing lists is a theme nobody can find."
  ],
```

Then confirm the file still parses and the gate is green:

```bash
python -c "import json; json.load(open('native/themes2/REGISTRY-SYNC.json',encoding='utf-8')); print('JSON OK')"
python native/tools/theme-registry-sync.py --check
```

Expected: `JSON OK`, then exit 0 with no output. The `_readme` is not hashed, so editing it cannot move any recorded value.

- [ ] **Step 4: Update `CONTRIBUTING.md`**

In the section `### A theme that ships here also ships in the registry — change both`, replace these two paragraphs *verbatim* (they are the second and third paragraphs of that section, currently around lines 251–263):

```
`=== bundled-theme / registry drift ===` fails the moment a bundled theme's
*meaning* changes (the hash is of the parsed, re-serialised JSON, so a reformat
is free). When it goes red, republish the changed folder to the registry, then
run `native/tools/theme-registry-sync.py --update` and commit the refreshed
`native/themes2/REGISTRY-SYNC.json` with the theme change. That file documents
the procedure, and is also where a theme gets recorded as deliberately *not*
published.

The gate cannot see the registry — it is a different repo, and this suite is
offline by design — so it checks the record, not the remote. It makes drift
loud, not impossible; making it impossible means a publish job with a
cross-repo write credential.
```

with:

```
`=== bundled-theme / registry drift ===` fails the moment a bundled theme's
*meaning* changes (the hash is of the parsed, re-serialised JSON, so a reformat
is free). When it goes red, run `native/tools/theme-registry-sync.py --update`
and commit the refreshed `native/themes2/REGISTRY-SYNC.json` with the theme
change. That file documents the procedure, and is also where a theme gets
recorded as deliberately *not* published.

You no longer copy anything into the registry yourself. On merge to `main`, the
`publish themes` workflow checks the registry out with a deploy key, copies over
exactly the targets that record lists, pushes, and then re-fetches what the
registry serves to confirm it matches. `verify registry` re-checks the same
thing every Monday, which catches what the publisher cannot see: a direct edit
there, a revert, or a publish that failed and was never retried.

The offline suite still checks the record rather than the remote, because it has
no network by design — so the record is what goes red in your PR, and the
workflows are what make it true afterwards. Adding a *new* theme to the registry
is still a two-repo change: the publish job refuses to create a folder the
registry does not already carry, because `index.json` needs a `description` that
exists nowhere in `theme.json`.
```

- [ ] **Step 5: Run the full probe suite**

```bash
BUILD_DIR=build bash native/tools/run-headless-probes.sh
```

Qt 6.8.3's `bin` must be on `PATH` (the prefix is in `build/CMakeCache.txt`) or every probe exits 127. Expect `PASS: bundled-theme / registry drift` — the drift gate runs `--check`, which now carries all three selftest blocks. One pre-existing unrelated failure is expected: `FAIL: mpv video preview (rc=127)` (the probe binary exists but no mpv runtime DLL is deployed in this worktree). `netplay both:direct` is flaky — roughly 1 run in 3 — and is also unrelated.

- [ ] **Step 6: Commit**

```bash
git add .github/workflows/verify-registry.yml native/themes2/REGISTRY-SYNC.json CONTRIBUTING.md
git commit -m "feat: verify the registry weekly, and drop the copy-by-hand step from the docs"
```

---

## Verification

After Task 4:

1. `python native/tools/theme-registry-sync.py --check` — exit 0. This runs all three selftest blocks.
2. `python native/tools/theme-registry-sync.py --verify-remote` — exit 0, first attempt.
3. `BUILD_DIR=build bash native/tools/run-headless-probes.sh` — `PASS: bundled-theme / registry drift`, no new failures.
4. Both workflow files parse as YAML.

**The workflows themselves cannot be verified until the secret exists.** Neither runs successfully without `THEMES_REGISTRY_DEPLOY_KEY`, and creating it is the repository owner's step. Report that plainly rather than implying the publish path has been exercised. Once the secret is in place, the two runs that would confirm it are:

- a `workflow_dispatch` of **verify registry** — needs no secret, and proves `--verify-remote` works on a runner;
- a `workflow_dispatch` of **publish themes** — with the registry already in sync it should reach "registry already in sync — nothing to publish" and then verify green, exercising every step including the checkout and the credential without changing anything.

## Follow-ups (not in this plan)

- Nothing here prevents a bundled theme changing without touching `native/themes2/**` — a change to how `theme.json` is *interpreted* moves no file. The weekly verifier is the backstop for that, not the publisher.
- The registry's own `theme-assets.yml` validates asset paths after a push rather than before. If a bad theme ever reaches the registry through this job, that gate reports it there and not here.
