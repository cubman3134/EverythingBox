# Theme registry publish job — design

`native/themes2/REGISTRY-SYNC.json` has described this job since #57 and explains why it did not exist:

> The guarantee is a publish job: a workflow in this repo that, on a push to main touching
> `native/themes2/**`, checks the registry out with a deploy credential, copies the shared themes over,
> and pushes. That removes the hand step entirely and makes the registry copies generated content. Its
> cost is a cross-repo write credential held as a secret in this repo, which is why it is not wired up
> here.

This builds it. The credential cost is now accepted: a deploy key scoped to the registry repo alone.

## Why now

The hand step failed twice. #57 fixed the bundled themes and recorded the intent to republish; the
republish never happened, so the registry served a `home`-only Triple and a Channels without
`nowplayingAudio` for months (#131). Closing #131 required doing the copy by hand a second time, and
nothing prevents a third.

The existing gate turns a silent drift into a deliberate one — it cannot do more, because `--update`
refreshes the record from the bundled theme without proving anything was pushed. The record states an
intent. This job makes it a fact.

## Architecture

Two workflows in this repo, and two new modes on the script that already owns the rules.

**The copy rules stay in `native/tools/theme-registry-sync.py`.** That script already knows which themes
are published, which paths they occupy in the registry, and how each is hashed. Encoding the copy list in
YAML would make a fourth copy of that truth, which is precisely the class of bug this whole area exists to
prevent. The workflows contribute credential, checkout and invocation — nothing that could disagree with
the record.

### `--publish <registry-dir>` — new mode, pure filesystem

Reads `publishedThemes` and `publishedDocs` from the record and copies each into a registry checkout.

- **Replaces each theme folder wholesale** rather than merging, so a file dropped from a bundled theme is
  dropped in the registry too. A merge would leave orphans that the canonical hash cannot see — it hashes
  `theme.json` alone.
- **Touches nothing the record does not name.** `Default`, `Grid`, `Lumen` and `Midnight` exist only in
  the registry; a sync that walked `themes2/` instead of the record would delete four themes on its first
  run.
- **Refuses to create a path that is not already there.** A `publishedThemes` entry whose `registryPath`
  is absent from the registry means a catalog change, and `index.json` entries carry a `description` that
  exists nowhere in `theme.json` — it is not derivable. The job stops and names the manual step rather
  than publishing a theme that nothing lists.
- Prints what it copied and what it left alone. Exit 1 on either refusal.

### `--verify-remote` — new mode, network

Fetches each published theme's raw URL and each published doc, hashes them the same way `--check` hashes
the bundled copies, and compares against the record. Exit 1 on drift, reporting the same
recorded-vs-actual shape the existing messages use.

The comparison is factored into a pure function so `selftest()` covers it offline; only the fetch itself
is beyond the probe suite, which is deliberately network-free.

### `.github/workflows/publish-themes.yml`

`on:` push to `main` with `paths: native/themes2/**`, plus `workflow_dispatch`.

1. **`--check` before anything else.** If the record and the bundled themes disagree, someone merged
   without `--update`, and publishing would push content the record does not describe — which is the
   drift in the opposite direction. Refuse before touching the registry.
2. Check the registry out over SSH using `THEMES_REGISTRY_DEPLOY_KEY`.
3. `--publish` into that checkout.
4. If the registry working tree is unchanged, log "already in sync" and exit 0. A push touching a theme's
   `sounds/` or an unrelated file under `themes2/` reaches this job legitimately and must not produce an
   empty commit.
5. Commit and push.
6. **`--verify-remote`.** The job proves its own work instead of assuming the push took. A push can succeed
   and still leave the registry wrong if the copy was incomplete.

### `.github/workflows/verify-registry.yml`

`schedule` (weekly) and `workflow_dispatch` only. Runs `--verify-remote`. No credential — the registry is
public.

Deliberately **not** triggered on push: that would race the publisher and report a drift the publisher is
three seconds from fixing. The publisher verifies itself synchronously; this catches what it structurally
cannot — a manual edit to the registry, a revert, or a bundled-theme change that never triggered the
publisher because it landed outside `native/themes2/**`.

## What stays manual, and why

- **`--update` at PR time.** The gate goes red the moment a bundled theme changes, and stays red until the
  record is refreshed. That is still the right place for a human: it is where the author learns a second
  copy exists. The job's contribution is that the record is now true rather than merely asserted.
- **Adding a new shared theme to the catalog** — needs an `index.json` entry with a description.
- **The deploy key**, once.

## Failure behaviour

| Failure | Behaviour |
| --- | --- |
| record ≠ bundled themes | refuse before touching the registry, naming the theme and `--update` |
| `registryPath` absent from registry | refuse, naming the `index.json` step |
| nothing changed | no-op, exit 0 |
| push rejected (concurrent write) | fail loudly; registry pushes are rare and a retry loop would hide a real conflict |
| post-push verify fails | fail loudly with the registry already changed — the state that most needs a human |
| deploy key missing or lapsed | checkout fails with the secret named; the weekly verifier goes red independently, so a silently-dead publisher surfaces within a week |

## Credential

A **deploy key**, not a PAT: scoped to `everythingbox-themes` alone, no user-account association, and no
expiry to manage. A PAT tied to an account stops working when it lapses or the account changes, and grants
more than one repo.

Generated by the repository owner, not by tooling:

```
ssh-keygen -t ed25519 -C "everythingbox theme publish" -f themes-publish-key -N ""
```

- `themes-publish-key.pub` → registry repo → Settings → Deploy keys → Add, **Allow write access**
- `themes-publish-key` → this repo → Settings → Secrets → `THEMES_REGISTRY_DEPLOY_KEY`

The workflow reads that exact secret name and fails with a message naming it when absent, so a
misconfigured secret reads as a configuration error rather than a mysterious checkout failure.

## Testing

- `--publish` is pure filesystem and is covered by `selftest()`: build a scratch "registry" in a temp
  directory, publish into it, and assert the shared themes were replaced, the registry-only themes were
  untouched, a dropped file disappeared, and a missing `registryPath` refuses.
- `--verify-remote`'s comparison is covered the same way, against fixture bytes rather than the network.
- The fetch itself is not covered — the probe suite has no network by design. Said plainly in the code
  rather than implied to be tested.
- `selftest()` already runs inside `--check`, so the probe suite picks all of this up through the existing
  `=== bundled-theme / registry drift ===` gate with no new registration.
- The workflows themselves are verified by `workflow_dispatch` runs against the real registry once the
  secret exists: one no-op run (nothing changed) and one real run (after a deliberate whitespace-only
  change to a published theme, which moves no canonical hash and so is safe to publish and revert).

## Files

- `native/tools/theme-registry-sync.py` — `--publish`, `--verify-remote`, extended `selftest()`
- `.github/workflows/publish-themes.yml` — new
- `.github/workflows/verify-registry.yml` — new
- `native/themes2/REGISTRY-SYNC.json` — the `_readme` describes this job as unbuilt; rewrite that
  paragraph to describe what now exists and what remains manual
- `CONTRIBUTING.md` — the theme-change procedure loses its copy-by-hand step

## Out of scope

- Publishing in the other direction (registry → app). The app's bundled copies are the source.
- Managing `index.json`. Catalog membership is editorial.
- Branch protection or PR review on the registry. `main` there is unprotected today; the registry's own
  `theme-assets.yml` validates asset paths on push, which is the check that matters for content.
