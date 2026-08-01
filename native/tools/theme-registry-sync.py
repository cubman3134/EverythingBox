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
red until you republish and refresh the record with `--update`.

WHAT THE RECORD IS ALLOWED TO CLAIM (issue #151). `--update` recomputes the hashes from the BUNDLED theme;
it has never touched the registry, so for a year it could be run by someone who never published, and the
record would then assert a currency that had never existed. That is not hypothetical — it is what happened.
#57 and #32 ran step 4 without step 3, so the record said the registry was current while it still served
the pre-#29 Triple (`home` and nothing else, 1380 bytes), and the gate was green over it for days. The gate
was not broken; the thing it compared against was a claim nobody had to substantiate.

So the record now has to NAME what it was published against, in `publishedAgainst`:

  * `registryCommit` — the sha in the registry that the copies were pushed as. `--update --registry-commit
    <sha>` records it. It is not verifiable here (no network), but it is falsifiable ANYWHERE else, which is
    the whole point: `--verify-registry` fetches that repo and says plainly whether the claim still holds.
  * `unverifiedReason` — the escape hatch, and the reason the escape hatch is not free: `--update
    --assume-published "<why>"` writes the excuse INTO the record, where a reviewer and `git blame` both see
    it, and `--check` prints it on every single run. "I refreshed the record but did not push" stops being
    the frictionless default and becomes a sentence somebody has to write down.

A bare `--update` is refused. The offline gate still cannot PROVE the registry is current — nothing here
can — but it no longer silently asserts it either. It reports which of the two the record is.

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
  theme-registry-sync.py --verify-registry                      # NEEDS NETWORK. Never run by the probe
                                                                # suite; for a release step or a maintainer.
"""
import hashlib
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
THEMES = os.path.join(HERE, os.pardir, "themes2")
RECORD = os.path.join(THEMES, "REGISTRY-SYNC.json")
DOCS = ["THEME_FORMAT.md"]

# The block that says what the hashes below it were published against (issue #151).
CLAIM = "publishedAgainst"
SHA_RE = re.compile(r"\A[0-9a-f]{7,40}\Z")

# Where --verify-registry looks. Read-only, and only ever from a command a human or a release step ran on
# purpose: the probe suite must not acquire a network dependency, because a gate that can fail for a reason
# unrelated to the code is a gate people learn to re-run rather than read.
REGISTRY_RAW = "https://raw.githubusercontent.com/cubman3134/everythingbox-themes/main/"
REGISTRY_API = "https://api.github.com/repos/cubman3134/everythingbox-themes/"


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
                 "published against. Refresh the record with  --update --registry-commit <sha>  (or "
                 "--update --assume-published \"<why>\" if you are recording an intent rather than a push)."
                 % CLAIM], None)

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
        # NOT a failure. A record that admits it is an intent is the honest state, and it is exactly the
        # state this repo has been in since #57. What was wrong before was that it read identically to a
        # verified one; now it announces itself on every run.
        return ([], "record makes NO verified publish claim: %s" % why)

    return (["%s names neither a registryCommit nor an unverifiedReason, so the record asserts the registry "
             "is current and offers nothing to check that against - which is the state issue #151 exists to "
             "end. Rerun --update with --registry-commit <sha> or --assume-published \"<why>\"." % CLAIM],
            None)


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
    # Before comparing anything: is the comparison itself sound? A hash that varies by checkout makes every
    # result below meaningless in one direction or the other, so it is checked first and reported as its own
    # problem rather than as drift in a file nobody touched.
    bad = selftest()
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
                    "%s has changed since it was last synced to the registry.\n"
                    "      recorded %s\n"
                    "      now      %s\n"
                    "    The registry copy is now BEHIND this one: anyone downloading '%s' from the registry\n"
                    "    gets the older theme under the same name. Republish it (see REGISTRY-SYNC.json for the\n"
                    "    procedure), then refresh this record, naming the commit you pushed:\n"
                    "      native/tools/theme-registry-sync.py --update --registry-commit <sha>"
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
                "    copy. Republish it alongside the themes, then:\n"
                "      native/tools/theme-registry-sync.py --update --registry-commit <sha>"
                % (doc_name, want or "(none)", got))

    return bad


def update(registry_commit=None, assume_reason=None):
    """Refresh the record — and make it say what it was refreshed FOR.

    `registry_commit` and `assume_reason` are mutually exclusive and one is REQUIRED; main() refuses a bare
    --update. That refusal is the fix for #151: the old bare form made "record a publish" and "record an
    intention to publish" the same keystroke, so the two became indistinguishable in the file, and the one
    that had actually happened was the wrong one.
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
        print("  every --check run until someone publishes and reruns with --registry-commit.")


def verify_registry():
    """NEEDS NETWORK. Never called by the probe suite — the suite is offline on purpose.

    This is the half of the question the offline gate cannot answer: not "has a bundled theme moved since
    the record was refreshed" but "is the registry actually serving what we say it is". It fetches the
    registry's theme.json files and hashes them the same canonical way, so the comparison is meaning to
    meaning and a reindent over there is not a false alarm.

    Run it from a release step, by hand before a release, or from a scheduled job in any repo that is
    allowed a network call.
    """
    import urllib.request

    def fetch(url):
        req = urllib.request.Request(url, headers={"User-Agent": "everythingbox-registry-sync"})
        with urllib.request.urlopen(req, timeout=30) as r:                # noqa: S310 - fixed https host
            return r.read()

    rec = load_record()
    claim = rec.get(CLAIM) if isinstance(rec.get(CLAIM), dict) else {}
    sha = str(claim.get("registryCommit", "")).strip().lower()
    why = str(claim.get("unverifiedReason", "")).strip()

    print("registry: %s" % REGISTRY_RAW)
    try:
        head = json.loads(fetch(REGISTRY_API + "commits/main").decode("utf-8"))["sha"]
    except Exception as exc:                                              # noqa: BLE001
        print("  could not reach the registry: %s" % exc)
        print("  VERDICT: unknown - this is a network failure, NOT a verdict about the record.")
        return 2                                                          # distinct from a real mismatch
    print("  registry main is at %s" % head)
    if sha:
        print("  record claims publication as %s" % sha)
        if not head.startswith(sha):
            print("  (the registry has moved since; that alone is fine - what matters is the content below)")
    elif why:
        print("  record claims NO publication: %s" % why)

    bad = []
    for name in sorted(rec.get("publishedThemes", {})):
        local = os.path.join(THEMES, name, "theme.json")
        if not os.path.isfile(local):
            bad.append("%s: recorded as published but not bundled here" % name)
            continue
        want = canonical_hash(local)[0]
        try:
            served, doc = canonical_hash_bytes(fetch(REGISTRY_RAW + "themes2/%s/theme.json" % name))
        except Exception as exc:                                          # noqa: BLE001
            bad.append("%s: could not read the registry's copy: %s" % (name, exc))
            continue
        views = ",".join(sorted(doc.get("views", {}))) or "(none)"
        if served == want:
            print("  OK    %-10s %s  views: %s" % (name, served[:16], views))
        else:
            print("  STALE %-10s registry %s  bundled %s" % (name, served[:16], want[:16]))
            print("        registry views: %s" % views)
            bad.append("%s: the registry serves a different theme under this name" % name)

    for doc_name in DOCS:
        local = os.path.join(THEMES, doc_name)
        if not os.path.isfile(local):
            continue
        want = doc_hash(local)
        try:
            served = hashlib.sha256(fetch(REGISTRY_RAW + doc_name).replace(b"\r\n", b"\n")).hexdigest()
        except Exception as exc:                                          # noqa: BLE001
            bad.append("%s: could not read the registry's copy: %s" % (doc_name, exc))
            continue
        print("  %-5s %-10s %s" % ("OK" if served == want else "STALE", doc_name, served[:16]))
        if served != want:
            bad.append("%s: the registry serves a different text" % doc_name)

    print()
    if bad:
        for b in bad:
            print("  " + b)
        print("  VERDICT: the registry is NOT current with this repo.")
        return 1
    print("  VERDICT: the registry serves exactly what is bundled here.")
    return 0


def canonical_hash_bytes(data):
    """canonical_hash for bytes already in hand (the registry's copy, over the wire)."""
    doc = json.loads(data.decode("utf-8"))
    blob = json.dumps(doc, sort_keys=True, separators=(",", ":"), ensure_ascii=True)
    return hashlib.sha256(blob.encode("utf-8")).hexdigest(), doc


UPDATE_REFUSAL = """\
--update on its own is refused (issue #151).

  It recomputes the record from the BUNDLED themes and never looks at the registry, so on its own it
  records "somebody ran this", not "the registry was updated". That is how the record came to assert a
  publish that had not happened: #57 and #32 ran it without step 3, and the registry served the pre-#29
  Triple under a green gate for days.

  Say which one this is:

    --update --registry-commit <sha>       you pushed the copies to the registry; <sha> is that commit.
                                           Falsifiable later:  --verify-registry

    --update --assume-published "<why>"    you did not, or cannot say. The reason goes into the record and
                                           prints on every --check run.
"""


def main():
    args = sys.argv[1:]
    arg = args[0] if args else ""
    if arg == "--check":
        problems = check()
        for p in problems:
            print("  " + p)
        return 1 if problems else 0
    if arg == "--verify-registry":
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
