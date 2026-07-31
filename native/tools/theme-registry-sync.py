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
"""
import hashlib
import json
import os
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


def byte_hash(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


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


def check():
    bad = []
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
        got = byte_hash(path)
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
            rec["publishedDocs"].setdefault(doc_name, {})["sha256"] = byte_hash(path)
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
    for name in bundled_themes():
        print("%-12s %s" % (name, canonical_hash(os.path.join(THEMES, name, "theme.json"))[0]))
    for doc_name in DOCS:
        path = os.path.join(THEMES, doc_name)
        if os.path.isfile(path):
            print("%-12s %s" % (doc_name, byte_hash(path)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
