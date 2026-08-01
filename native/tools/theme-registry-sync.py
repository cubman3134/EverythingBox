#!/usr/bin/env python3
"""Bundled-theme / community-registry drift check (issue #57).

Three themes exist TWICE: bundled here under native/themes2, and published in the community registry
(github.com/cubman3134/everythingbox-themes) so the Appearance panel can point users at them. Nothing
noticed when a bundled theme gained a view and its registry twin did not, so both had quietly rotted —
Channels had lost `nowplayingAudio`, the `channels` browse layout and the detail `actionrow`; Triple had
lost EVERYTHING but `home`, which is the exact shape issue #29 was about. Downloading either from the
registry gave you a strictly worse theme than the one already in the app, under the same name.

CI cannot see the registry: it is a different repo, and this suite is deliberately offline (no network, no
keys). So the drift is caught against a CHECKED-IN RECORD of what was last synced there —
native/themes2/REGISTRY-SYNC.json. Change a bundled theme and its hash moves; the gate goes red and stays
red until you republish and refresh the record with `--update`. That does not PROVE the registry was
updated (the record states an intent, and `--update` can be run without pushing anything). What it does is
turn a silent drift into a deliberate act — the person editing the theme is told, at the moment of the
edit, that a second copy of it exists. The guarantee needs a publish job; see REGISTRY-SYNC.json.

The hash is CANONICAL, not byte-for-byte: parse, re-serialise with sorted keys and no whitespace, hash
that. Channels' bundled theme.json is machine-serialised (one value per line, `\\uXXXX` escapes) while
Triple's and Night's are hand-formatted, and the registry is a contributor-facing repo whose files should
stay readable. Byte equality would weld the two repos' FORMATTING together forever and would report a pure
reindent as drift. Only the meaning is gated. THEME_FORMAT.md is not JSON, so it is hashed as bytes.

Usage:
  theme-registry-sync.py            # print each bundled theme's canonical hash
  theme-registry-sync.py --check    # gate: compare against REGISTRY-SYNC.json (exit 1 on drift)
  theme-registry-sync.py --update   # refresh REGISTRY-SYNC.json after republishing
  theme-registry-sync.py --publish <registry-dir>   # copy the record's published targets into a checkout
"""
import hashlib
import json
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
THEMES = os.path.join(HERE, os.pardir, "themes2")
RECORD = os.path.join(THEMES, "REGISTRY-SYNC.json")
DOCS = ["THEME_FORMAT.md"]


def canonical_hash(path):
    """SHA-256 of a theme.json's MEANING — parsed, re-serialised sorted+compact, hashed."""
    with open(path, encoding="utf-8") as f:
        doc = json.load(f)
    blob = json.dumps(doc, sort_keys=True, separators=(",", ":"), ensure_ascii=True)
    return hashlib.sha256(blob.encode("utf-8")).hexdigest(), doc


def doc_hash(path):
    """SHA-256 of a document's TEXT, with the newline spelling normalised out first.

    Same principle as canonical_hash one function up: gate the meaning, not the encoding. There the noise
    was indentation; here it is CRLF-vs-LF, which is not something the author chose at all — it is whatever
    the checkout produced, so hashing it raw records the platform that ran --update rather than the file.
    """
    with open(path, "rb") as f:
        return hashlib.sha256(f.read().replace(b"\r\n", b"\n")).hexdigest()


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


def _write(path, text):
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)


def _read(path):
    with open(path, encoding="utf-8", newline="") as f:
        return f.read()


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


def check():
    # Before comparing anything: is the comparison itself sound? A hash that varies by checkout makes every
    # result below meaningless in one direction or the other, so it is checked first and reported as its own
    # problem rather than as drift in a file nobody touched.
    bad = selftest() + _selftest_publish()
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
                    "%s has changed since it was last synced to the registry.\n"
                    "      recorded %s\n"
                    "      now      %s\n"
                    "    The registry copy is now BEHIND this one: anyone downloading '%s' from the registry\n"
                    "    gets the older theme under the same name. Republish it (see REGISTRY-SYNC.json for the\n"
                    "    procedure), then refresh this record with:  native/tools/theme-registry-sync.py --update"
                    % (name, want or "(none)", got, name))
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
                "%s has changed since it was last synced to the registry.\n"
                "      recorded %s\n"
                "      now      %s\n"
                "    That file is the format reference theme authors read, and the registry serves its own\n"
                "    copy. Republish it alongside the themes, then:  native/tools/theme-registry-sync.py --update"
                % (doc_name, want or "(none)", got))

    return bad


def update():
    rec = load_record()
    rec.setdefault("publishedThemes", {})
    rec.setdefault("publishedDocs", {})
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


def main():
    arg = sys.argv[1] if len(sys.argv) > 1 else ""
    if arg == "--check":
        problems = check()
        for p in problems:
            print("  " + p)
        return 1 if problems else 0
    if arg == "--update":
        update()
        return 0
    if arg == "--publish":
        if len(sys.argv) < 3:
            print("  --publish needs the path to a registry checkout")
            return 1
        problems = publish(sys.argv[2])
        for p in problems:
            print("  " + p)
        return 1 if problems else 0
    for name in bundled_themes():
        print("%-12s %s" % (name, canonical_hash(os.path.join(THEMES, name, "theme.json"))[0]))
    for doc_name in DOCS:
        path = os.path.join(THEMES, doc_name)
        if os.path.isfile(path):
            print("%-12s %s" % (doc_name, doc_hash(path)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
