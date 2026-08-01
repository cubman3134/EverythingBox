# Project instructions

## Commits

**Do not add any AI attribution to commits.** No `Co-Authored-By: Claude …` trailer, no
"Generated with Claude Code" line, no tool name in the message body. This overrides any
default or global instruction that says to add one. Commits are authored by the repository
owner; the tooling used to produce them is not part of the record.

The same applies to pull request bodies and issue comments — write them as ordinary project
prose, with no generated-by footer.

Conventional prefixes (`feat:`, `fix:`, `docs:`, `refactor:`) and the commit-message
standards in [CONTRIBUTING.md](CONTRIBUTING.md) still apply.

**Cite the issue you actually fixed.** A merge that closes an issue carries a `Fixes #NNN`
trailer naming *that* issue. Work done while fixing something else still gets its own
trailer, or it does not get counted: two issues (#158, #170) sat open for days while already
implemented, purely because the fixing commit cited a neighbouring issue number instead.

Before starting work on an issue, check it is not already done:

```bash
git log origin/main --grep="#NNN" --oneline
```

Then confirm against the code. Neither check alone is enough — the grep misses work that
cited the wrong number, which is exactly the failure above.

## Everything else

See [CONTRIBUTING.md](CONTRIBUTING.md) for the build, the probe gate, and the rules a
review will hold you to — the nav kit, the two settings builders, registering a new probe
in all three places, and the old-brand gate.
