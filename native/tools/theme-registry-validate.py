#!/usr/bin/env python3
"""Registry gate: index.json and each theme's manifest must agree, and every theme must be usable.

WHY THIS LIVES IN THE APP'S REPO AND RUNS IN THE REGISTRY'S CI (issue #151).

The community registry (github.com/cubman3134/everythingbox-themes) serves SEVEN themes. Three of them —
Channels, Night, Triple — are also bundled in this repo, so `theme-registry-sync.py` gates their meaning
against a checked-in record. The other four — Default, Grid, Lumen, Midnight — exist ONLY over there, and
until this file nothing checked them at all: not that they parse, not that they declare a usable view, not
that the gallery card the user reads agrees with the theme they end up installing.

That gap was not theoretical. The registry's index.json credited **Triple** to `cubman3134` while its own
theme.json has always said `EverythingBox` — the card and the installed theme disagreed about the author,
and had for as long as both existed, because no reader ever held the two side by side.

The rule lives HERE because this repo is the thing that consumes index.json and defines what its fields
mean (`formFactors` semantics are in native/src/core/ThemeFormFactors.h; the field list is in
themes2/THEME_FORMAT.md). A second hand-maintained copy in the registry would be a gate that silently stops
matching what it gates. So the registry's workflow downloads this file and runs it, exactly as its
theme-assets.yml already downloads the app's Theme.js rather than reimplementing it.

WHAT THAT MEANS FOR THIS REPO'S OWN PROBE SUITE. The probe suite is offline and has no registry checkout,
so it cannot run the rule against the real data. What it CAN do — and does, via --selftest — is prove the
rule discriminates: build a synthetic registry, confirm it passes, then break one thing at a time and
require the matching complaint. A validator that nothing ever proves can fail is the same defect as a drift
record nobody ever has to substantiate, one level up.

Usage:
  theme-registry-validate.py --registry DIR   # gate a registry checkout (DIR holds index.json + themes2/)
  theme-registry-validate.py --selftest       # no checkout needed: prove each check fires. Run by CI here.

Exit codes:  0 clean   1 a theme or index entry is wrong   2 the gate could not run, or --selftest showed a
check that cannot fire. Two is never "a theme is bad" — it is "do not read anything below as a verdict".
"""
import json
import os
import shutil
import sys
import tempfile

# index.json's array key and each entry's folder key, as the registry actually serves them.
ENTRIES_KEY = "themes2"
DIR_KEY = "dir"

# "Did I scan anything?" A validator that walks an empty list prints PASS, which is worse than no validator:
# it reports a rule as enforced. Deliberately below the real count (7) and above zero, so an index.json that
# lost its entries — or a checkout this was pointed at by mistake — reports itself instead of passing.
MIN_ENTRIES = 4

# The fields the gallery card shows that the installed theme also declares. Any field on BOTH sides is a
# field the user can be shown one answer for and given another.
MIRRORED = ("name", "author", "formFactors")


def load_json(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def norm_form_factors(value):
    """A formFactors declaration reduced to what it MEANS, or None for 'undeclared'.

    Order is not meaning — ["tv","desktop"] and ["desktop","tv"] are the same claim — so the index and the
    manifest are allowed to spell it differently. A bare string is read as undeclared, matching
    ThemeFormFactors.h, which refuses to guess: inferring "desktop" from "desktop" would mean the app
    manufacturing a claim the author did not make.
    """
    if not isinstance(value, list):
        return None
    return sorted(str(v) for v in value)


def validate(root):
    """Returns (problems, fatal). `fatal` means the gate could not run, which is not a verdict on a theme."""
    bad = []
    index_path = os.path.join(root, "index.json")
    if not os.path.isfile(index_path):
        return (["no index.json at %s - this is not a registry checkout, and nothing below was checked."
                 % os.path.abspath(root)], True)
    try:
        index = load_json(index_path)
    except Exception as exc:                                              # noqa: BLE001
        return (["index.json does not parse: %s. The app reads this file over the network and would get "
                 "an empty gallery; nothing below was checked." % exc], True)

    entries = index.get(ENTRIES_KEY)
    if not isinstance(entries, list):
        return (["index.json has no '%s' array. Every theme in the gallery comes from that array, so an "
                 "index without one publishes nothing while looking fine." % ENTRIES_KEY], True)
    if len(entries) < MIN_ENTRIES:
        bad.append("index.json lists %d theme(s) - expected at least %d. This gate scanned almost nothing; "
                   "treat a PASS as meaningless until that is explained." % (len(entries), MIN_ENTRIES))

    listed_dirs = set()
    for i, entry in enumerate(entries):
        if not isinstance(entry, dict):
            bad.append("index.json entry %d is not an object." % i)
            continue
        label = str(entry.get("name") or "entry %d" % i)

        rel = str(entry.get(DIR_KEY, "")).strip()
        if not rel:
            bad.append("%s has no '%s', so the gallery has nowhere to install it from." % (label, DIR_KEY))
            continue
        # An entry may only point INSIDE the registry. `../` here would make a submission reach outside the
        # repo it is submitted to; the sibling asset gate refuses the same shape one level down.
        folder = os.path.normpath(os.path.join(root, rel))
        if os.path.relpath(folder, root).startswith(os.pardir) or os.path.isabs(rel):
            bad.append("%s points its '%s' at %r, which leaves the registry." % (label, DIR_KEY, rel))
            continue
        listed_dirs.add(os.path.normcase(os.path.abspath(folder)))

        manifest_path = os.path.join(folder, "theme.json")
        if not os.path.isfile(manifest_path):
            bad.append("%s is listed at '%s' but there is no theme.json there. Installing it gives the user "
                       "a folder the engine cannot read." % (label, rel))
            continue
        try:
            manifest = load_json(manifest_path)
        except Exception as exc:                                          # noqa: BLE001
            bad.append("%s/theme.json does not parse: %s. The engine would fall back to the built-in layout "
                       "with nothing on screen to say why." % (rel, exc))
            continue

        # The card and the theme must agree. This is the Triple defect: index said cubman3134, the manifest
        # said EverythingBox, and the user was shown one and given the other.
        for field in MIRRORED:
            shown = entry.get(field)
            real = manifest.get(field)
            if field == "formFactors":
                shown, real = norm_form_factors(shown), norm_form_factors(real)
            if shown is None and real is None:
                continue
            if shown != real:
                bad.append("%s: index.json says %s=%r, its theme.json says %r. The gallery card and the "
                           "installed theme disagree, and the user sees the card." % (label, field, shown, real))

        views = manifest.get("views")
        if not isinstance(views, dict) or not views:
            bad.append("%s declares no views at all, so installing it changes nothing the user can see."
                       % label)
            continue
        # A view declared with empty `elements` counts as NOT declared everywhere in the engine
        # (themeDeclaresView tests the element list, not the key), so it is indistinguishable from absent —
        # except that it looks, to a reader of the file, like the view is covered.
        for view in sorted(views):
            body = views[view] if isinstance(views[view], dict) else {}
            if not body.get("elements"):
                bad.append("%s declares view '%s' with no elements - the engine treats that as undeclared, "
                           "so the view silently falls back to the built-in layout." % (label, view))

    # A theme folder nobody listed is invisible: it is published, reviewed and dead. The sync record has the
    # mirror of this check for bundled themes; here it catches a submission whose index entry was dropped in
    # a merge.
    themes_root = os.path.join(root, ENTRIES_KEY)
    if os.path.isdir(themes_root):
        for name in sorted(os.listdir(themes_root)):
            folder = os.path.join(themes_root, name)
            if not os.path.isfile(os.path.join(folder, "theme.json")):
                continue
            if os.path.normcase(os.path.abspath(folder)) not in listed_dirs:
                bad.append("%s/%s holds a theme.json but no index.json entry points at it, so it is in the "
                           "repository and out of the gallery." % (ENTRIES_KEY, name))

    return (bad, False)


# ---------------------------------------------------------------------------------------------------------
# Selftest: the rule has to be shown to discriminate, not merely to run.
# ---------------------------------------------------------------------------------------------------------

def _theme(name, author="EverythingBox", form_factors=None, views=None):
    doc = {"name": name, "author": author}
    if form_factors is not None:
        doc["formFactors"] = form_factors
    doc["views"] = views if views is not None else {
        "home": {"elements": [{"type": "text", "text": name}]},
        "browse": {"elements": [{"type": "grid"}]},
    }
    return doc


def _write_fixture(root, themes, index_entries):
    os.makedirs(os.path.join(root, ENTRIES_KEY))
    for name, doc in themes.items():
        folder = os.path.join(root, ENTRIES_KEY, name)
        os.makedirs(folder)
        if doc is None:                                   # a theme.json that does not parse
            with open(os.path.join(folder, "theme.json"), "w", encoding="utf-8") as f:
                f.write("{ not json")
            continue
        with open(os.path.join(folder, "theme.json"), "w", encoding="utf-8") as f:
            json.dump(doc, f, indent=2)
    with open(os.path.join(root, "index.json"), "w", encoding="utf-8") as f:
        json.dump({ENTRIES_KEY: index_entries}, f, indent=2)


def _baseline():
    """A synthetic registry that is CORRECT, in the shape the real one has: some themes declare
    formFactors, most do not; one spells its declaration in a different order than the index does."""
    themes = {
        "Alpha": _theme("Alpha"),
        "Beta": _theme("Beta", author="someone-else"),
        "Gamma": _theme("Gamma", form_factors=["desktop", "tv"]),
        "Delta": _theme("Delta"),
        "Epsilon": _theme("Epsilon", form_factors=["handheld"]),
    }
    entries = [
        {"name": "Alpha", "author": "EverythingBox", "dir": "themes2/Alpha"},
        {"name": "Beta", "author": "someone-else", "dir": "themes2/Beta"},
        # Deliberately the other order: order is not meaning, and a gate that called this drift would push
        # authors to hand-sort two files in lockstep forever.
        {"name": "Gamma", "author": "EverythingBox", "dir": "themes2/Gamma",
         "formFactors": ["tv", "desktop"]},
        {"name": "Delta", "author": "EverythingBox", "dir": "themes2/Delta"},
        {"name": "Epsilon", "author": "EverythingBox", "dir": "themes2/Epsilon",
         "formFactors": ["handheld"]},
    ]
    return themes, entries


def _run_case(mutate):
    root = tempfile.mkdtemp(prefix="regvalidate-")
    try:
        themes, entries = _baseline()
        if mutate:
            mutate(themes, entries)
        _write_fixture(root, themes, entries)
        return validate(root)
    finally:
        shutil.rmtree(root, ignore_errors=True)


def _drop_field(entries, name, field):
    for e in entries:
        if e.get("name") == name:
            e.pop(field, None)


# Each entry: a label, a mutation, and the text the complaint must contain. The point of the third column
# is that a mutation must be killed by the check it is aimed at — a fixture that goes red for some unrelated
# reason proves nothing about the rule under test.
MUTATIONS = [
    ("index credits the wrong author (the real Triple defect)",
     lambda t, e: [x.update(author="cubman3134") for x in e if x["name"] == "Alpha"],
     "index.json says author='cubman3134'"),
    ("index shows a different name than the manifest",
     lambda t, e: [x.update(name="Alfa") for x in e if x["name"] == "Alpha"],
     "says name='Alfa'"),
    ("index drops a formFactors the manifest declares",
     lambda t, e: _drop_field(e, "Gamma", "formFactors"),
     "formFactors=None"),
    ("index declares a formFactors the manifest does not",
     lambda t, e: [x.update(formFactors=["tv"]) for x in e if x["name"] == "Alpha"],
     "formFactors=['tv']"),
    ("index and manifest declare different form factors",
     lambda t, e: [x.update(formFactors=["mobile"]) for x in e if x["name"] == "Epsilon"],
     "formFactors=['mobile']"),
    ("a view is declared with no elements",
     lambda t, e: t["Beta"]["views"].__setitem__("detail", {"elements": []}),
     "no elements"),
    ("a theme declares no views at all",
     lambda t, e: t["Delta"].__setitem__("views", {}),
     "declares no views at all"),
    ("a theme.json does not parse",
     lambda t, e: t.__setitem__("Alpha", None),
     "does not parse"),
    ("an index entry points at a folder that is not there",
     lambda t, e: [x.update(dir="themes2/Nowhere") for x in e if x["name"] == "Beta"],
     "no theme.json there"),
    ("an index entry points outside the registry",
     lambda t, e: [x.update(dir="../elsewhere") for x in e if x["name"] == "Beta"],
     "leaves the registry"),
    ("an index entry names no folder",
     lambda t, e: _drop_field(e, "Beta", "dir"),
     "has no 'dir'"),
    ("a published theme folder is in no index entry",
     lambda t, e: e.remove([x for x in e if x["name"] == "Delta"][0]),
     "out of the gallery"),
    ("the index lists almost nothing",
     lambda t, e: (t.clear(), e.clear(), t.update({"Alpha": _theme("Alpha")}),
                   e.append({"name": "Alpha", "author": "EverythingBox", "dir": "themes2/Alpha"})),
     "scanned almost nothing"),
]


def selftest():
    problems = []

    clean, fatal = _run_case(None)
    if fatal or clean:
        problems.append("the CORRECT synthetic registry did not pass, so every result below is suspect:\n"
                        + "\n".join("        " + c for c in clean))

    for label, mutate, expect in MUTATIONS:
        found, fatal = _run_case(mutate)
        if fatal:
            problems.append("[%s] the gate went fatal instead of reporting the defect" % label)
        elif not any(expect in f for f in found):
            problems.append("[%s] survived: nothing complained about %r.\n"
                            "        what was reported instead: %s"
                            % (label, expect, found or "(nothing at all)"))

    # A gate that fires on everything is as useless as one that fires on nothing, so the baseline above is
    # not the only negative control: re-spelling a declaration must stay silent.
    def reorder(t, e):
        for x in e:
            if x["name"] == "Gamma":
                x["formFactors"] = ["desktop", "tv"]
        t["Gamma"]["formFactors"] = ["tv", "desktop"]
    noise, _ = _run_case(reorder)
    if noise:
        problems.append("reordering a formFactors declaration was reported as a disagreement: %s" % noise)

    return problems


def main():
    args = sys.argv[1:]
    if args and args[0] == "--selftest":
        problems = selftest()
        for p in problems:
            print("  " + p)
        if problems:
            print("  %d of the checks in this file cannot be shown to fire." % len(problems))
            # 2, not 1: this is "the gate is not trustworthy", never "a theme is wrong". The registry's CI
            # runs this before judging any submission for exactly that reason — a rule that has been made
            # permissive must not read as a clean bill of health for somebody's PR.
            return 2
        print("  %d checks, each proven to fire on the defect it names, plus 2 negative controls."
              % len(MUTATIONS))
        return 0

    root = "."
    if args and args[0] == "--registry":
        if len(args) < 2:
            print("--registry needs a path")
            return 2
        root = args[1]
    elif args:
        print(__doc__)
        return 2

    bad, fatal = validate(root)
    for b in bad:
        print("  " + b)
    if fatal:
        return 2
    if bad:
        print("  %d problem(s). The gallery card a user reads and the theme they install must say the same "
              "thing." % len(bad))
        return 1
    print("  index.json and every theme's manifest agree, and every theme declares a usable view.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
