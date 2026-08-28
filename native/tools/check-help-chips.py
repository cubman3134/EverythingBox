#!/usr/bin/env python3
"""Every helpsystem chip in a BUNDLED theme must be one the app can translate to a controller button.

A theme authors its help bar as keyboard chips. padglyphs::verbForHint owns the set the app can translate;
anything outside it is handed back untranslated, which is right for a third-party theme (it is the author's
own text) and wrong for one we ship (a controller user reads a key they cannot press). This gate holds the
bundled themes and the built-in fallback bar to the translatable set.

Exits 0 and prints nothing on success. On failure prints one line per offending chip and exits 1.

Everything printed is forced to ASCII (`ascii()`, not `!r`): a failing chip is by definition often a
non-ASCII glyph, and this runs under a cp1252 console on Windows where printing one raises
UnicodeEncodeError -- a gate that crashes instead of naming the offender is no gate at all.
"""
import json
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent
NATIVE = HERE.parent

# The hints padglyphs::verbForHint maps, read straight out of the source so the two cannot drift.
PADGLYPHS_SRC = NATIVE / "src" / "input" / "PadGlyphs.cpp"
PADGLYPHS = PADGLYPHS_SRC.read_text(encoding="utf-8")
KNOWN = set(re.findall(r'hintKey == QLatin1String\("([^"]*)"\)', PADGLYPHS))

# Arrow chips are a D-pad direction already and pass through by design.
ARROWS = set("\u2190\u2191\u2192\u2193")

# A floor on what the read above must have found. verbForHint is a chain of `hintKey == QLatin1String("X")`
# today; if it is ever rewritten as a table or a map, the regex quietly returns the empty set and this gate
# passes EVERY chip while looking green. These three have been in the vocabulary since it existed and are
# named by all three bundled themes, so their absence means the reader broke, not that the app changed.
FLOOR = {"Enter", "Esc", "I"}


def chip_ok(button: str) -> bool:
    if button in KNOWN:
        return True
    return bool(button) and all(ch in ARROWS for ch in button)


def unescape_js(s: str) -> str:
    """Resolve \\uXXXX escapes in a JS string literal without mangling literal non-ASCII.

    Theme.js writes its arrows as real UTF-8 characters, not escapes. The naive
    `s.encode("utf-8").decode("unicode_escape")` re-reads those UTF-8 bytes as latin-1 and turns a single
    arrow into three mojibake characters, which this gate would then report as an untranslatable chip --
    a false failure. Encoding to latin-1 with backslashreplace turns anything already non-latin-1 into its
    own \\uXXXX escape first, so the round trip is lossless either way.
    """
    return s.encode("latin-1", "backslashreplace").decode("unicode_escape")


def walk(node, out):
    """Collect every helpsystem entry's `button` from an arbitrarily nested theme document."""
    if isinstance(node, dict):
        if node.get("type") == "helpsystem":
            for e in node.get("entries") or []:
                if isinstance(e, dict) and "button" in e:
                    out.append(str(e["button"]))
        for v in node.values():
            walk(v, out)
    elif isinstance(node, list):
        for v in node:
            walk(v, out)


def main() -> int:
    missing = FLOOR - KNOWN
    if missing:
        print(f"{PADGLYPHS_SRC.name}: read no verb for {ascii(sorted(missing))} -- the hint-vocabulary "
              "reader in check-help-chips.py no longer matches verbForHint's source, so this gate is inert")
        return 1

    bad = []

    for theme in sorted((NATIVE / "themes2").glob("*/theme.json")):
        try:
            doc = json.loads(theme.read_text(encoding="utf-8"))
        except Exception as exc:                       # a parse break is the drift gate's business, not ours
            print(f"{theme}: could not parse ({exc})")
            return 1
        chips = []
        walk(doc, chips)
        for c in chips:
            if not chip_ok(c):
                bad.append(f"{theme.relative_to(NATIVE).as_posix()}: chip {ascii(c)} has no controller equivalent")

    # The built-in fallback bar in Theme.js, which renders for a theme that declares no view of its own.
    themejs = (NATIVE / "src" / "theme2" / "qml" / "Theme.js").read_text(encoding="utf-8")
    for c in re.findall(r'\{\s*"button"\s*:\s*"((?:[^"\\]|\\.)*)"', themejs):
        c = unescape_js(c)
        if not chip_ok(c):
            bad.append(f"src/theme2/qml/Theme.js: fallback chip {ascii(c)} has no controller equivalent")

    for line in bad:
        print(line)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
