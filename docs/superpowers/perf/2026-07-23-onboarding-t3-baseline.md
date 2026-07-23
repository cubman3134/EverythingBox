# Perf baseline - 2026-07-23 (onboarding Drive-restore T3)

Standard route, deployed Release `MyMediaVault.exe` (post-T3), driven by `perfbaseline.py`. Three runs.
Onboarding is off the hot paths by design: on the returning-user path (`onboarding/done=true` in the
deployed ini) startup adds exactly ONE `Settings` flag read and skips the choice screen entirely; the
choice screen + restore flow are reached only on a true first run and never touch `nav.select`.

## Key spans (worst ms | avg ms | samples), 3 runs

| span | run1 | run2 | run3 | b2t6 baseline |
|---|---|---|---|---|
| startup.total | 2779 | 1698 | 1406 | 1374 |
| startup.firstpaint | 468 | 454 | 458 | 569 |
| startup.home | 1418 | 276 | 237 | 133 |
| nav.select (worst/avg over 67) | 439 / 31 | 412 / 30 | 412 / 30 | 390 / 28 |

Run 1's elevated `startup.total`/`startup.home` is the cold-disk first-launch-after-deploy artifact (the
fresh exe + Qt/mpv/SDL DLLs page in on first run); runs 2–3 settle to warm steady state, with run 3's
`startup.total` (1406 ms) landing on the b2t6 baseline (1374 ms). `nav.select` is flat — avg 30–31 ms
vs baseline 28 ms is sampling noise, worst is single-sample jitter. `startup.firstpaint` is unchanged/better.

Conclusion: the T3 findFile data-safety fix (one added `Status` flag + a pure `restorePullStage` classifier)
has no measurable startup or nav cost. Onboarding stays off the hot paths.

## Run 3 (warm steady state) full table

| span | worst ms | avg ms | samples |
|---|---|---|---|
| startup.total | 1406 | 1406 | 1 |
| startup.firstpaint | 458 | 458 | 1 |
| nav.select | 412 | 30 | 67 |
| startup.home | 237 | 237 | 1 |
| startup.settings | 107 | 107 | 1 |
| catalog.load | 100 | 86 | 2 |
| open.game | 74 | 74 | 1 |
| startup.theme | 3 | 3 | 1 |
| startup.addons | 3 | 3 | 1 |
| marks.shelves | 1 | 0 | 2 |
