# RetroAchievements

EverythingBox talks to RetroAchievements through **rcheevos' `rc_client`**, vendored at
`native/third_party/rcheevos`. One client, one active game at a time, attached to the full-screen emulator
(`RetroView`) only — split-screen panes do not participate. Sign-in is optional and nothing below happens
without it.

Three things ride that one client, and none of them costs a network call beyond the game load: achievements,
**leaderboards**, and **rich presence** all come out of the game data `rc_client` fetches when it identifies
the ROM by hash.

Code map:

| Piece | Where |
|---|---|
| The `rc_client` wrapper: login, game load, `doFrame()`, the C trampolines | `src/core/Achievements.{h,cpp}` |
| The hardcore-affordance policy (pure, header-only) | `src/core/Hardcore.h` |
| The leaderboard rules: event map, tracker state, submission rule, copy (pure) | `src/core/Leaderboards.{h,cpp}` |
| The unlock toast, the tracker overlay, the pause-menu pages | `src/emu/RetroView.cpp` |
| Headless checks | `probe_hardcore` (HARDCORE-OK), `probe_ralead` (RALEAD-OK) |

## Softcore and hardcore

**Softcore is the default and stays first-class.** The point is parity of *choice* with RetroArch, not pushing
anyone into permadeath mode. Hardcore is a per-profile opt-in behind a consent screen (Settings → the
`emu.hardcore` row on the themed surface, its twin checkbox on the classic one).

While a hardcore session is live, `hardcore::forbidsInHardcore()` is the ONE rule that disables save states,
loading a state, rewind, fast-forward, the cheat editor and cheat search. Screenshots stay allowed. The pause
menu greys those entries and drops them from the controller nav list, so a player sees *why* rather than
meeting a silent failure. Enabling hardcore mid-session resets the achievement session — the site's rule, not
ours.

The RetroAchievements login token is a **device-local session credential**. It is kept out of cloud sync
deliberately and is never logged.

## Leaderboards

`rc_client` arms every leaderboard that ships with the game's achievement set and raises an event at each
stage of an attempt. EverythingBox does not poll, and does not open a second HTTP client for this.

**Four attempt events, four signals on `Achievements`:**

| rcheevos event | Signal | What the player sees |
|---|---|---|
| `LEADERBOARD_STARTED` | `leaderboardAttemptStarted(id, title, description, willSubmit)` | a corner notice: *"<board> — attempt started"*, or *"(not submitting)"* when the run cannot count |
| `LEADERBOARD_FAILED` | `leaderboardAttemptFailed(id, title)` | *"<board> — attempt failed"* |
| `LEADERBOARD_SUBMITTED` | `leaderboardAttemptSubmitted(id, title, value, willSubmit)` | *"<board> — submitted <value>"*, or *"<value> (not submitted: hardcore only)"* |
| `LEADERBOARD_SCOREBOARD` | `leaderboardSubmitResult(id, submitted, best, newRank, numEntries)` | *"Submitted <value> — rank N of M (best …)"* |

The mapping from a raw `RC_CLIENT_EVENT_*` id to the kind we route on lives in `ra::kindForRcEvent`, and
`ra::dispatch` is the single place an event becomes a UI-visible fact. `Leaderboards.h` transcribes the event
ids by hand so the pure layer needs no rcheevos header; `Achievements.cpp` `static_assert`s each one against
the real enum, so a rcheevos bump that renumbers them fails the **build** rather than quietly delivering a
submit into the failure arm.

### The tracker overlay

`LEADERBOARD_TRACKER_SHOW` / `_UPDATE` / `_HIDE` collapse into one signal —
`leaderboardTrackerChanged(visible, display)` — because the overlay only ever needs *is anything showing, and
what does it read*. `ra::TrackerModel` holds the visible set (several attempts can run at once, each with its
own tracker id), so a **superseded** attempt behaves correctly: a second tracker appearing while the first is
still running leaves both on screen, and the first ending leaves the survivor up rather than blanking the
overlay.

It is drawn as a small rounded card in the **top-right** corner — the unlock toast's visual system, the unlock
toast's palette, the opposite corner so the two never collide. It is **not a widget**: it is two members and a
`paintLeaderboardTracker()` call on the game surface. There is nothing with a focus policy, nothing in the tab
order and nothing that receives an event, so it structurally cannot swallow a button press during a run. An
overlay that ate an input mid-attempt would be worse than no overlay at all.

The tracker **never outlives its game**. `rc_client` raises no hide event when a game goes away, so
`Achievements` clears it explicitly on game load, on game unload and on the hardcore-enable reset.

### Leaderboards only submit in hardcore

That is RetroAchievements' rule. `rc_client` still raises `LEADERBOARD_STARTED` and `LEADERBOARD_SUBMITTED`
in softcore — it simply sends nothing — so an honest UI has to say which happened.

`ra::willSubmit(hardcoreActive, loggedIn)` is the verdict, read once per event and carried into the signals so
a start and its submit cannot disagree. `ra::submissionNote()` is the copy, and it is never empty:

* signed out → *"Not submitting — sign in to RetroAchievements to compete"*
* softcore → *"Not submitting — leaderboards only accept hardcore runs"*
* hardcore → *"Submitting — hardcore run"*

**A softcore leaderboard is visible, readable and plainly marked — never hidden.** Hiding it teaches nobody
why their run is not on the board.

### The per-game list

Pause menu → **Leaderboards** (the entry appears only when the loaded game actually has some, in softcore
exactly as in hardcore). The page is read-only: the rows are labels, so controller focus never lands on a
leaderboard and the only navigable thing on the page is Back. Everything on it comes from
`rc_client_create_leaderboard_list` over already-fetched game data.

## Rich presence

`rc_client` evaluates the game's rich-presence script; `Achievements::richPresence()` reads the current
message and `hasRichPresence()` says whether the game ships a script at all.

* It is a **pull, not a stored value**, and it is **not free** — re-running the script touches core RAM. Call
  it when something is about to show it (the pause menu; the `#64`/`#66` hook surface if a Discord payload
  ever lands there). **Never once per frame on the GUI thread.**
* A game with **no presence script** reads **empty**. `rc_client` would otherwise synthesise
  `"Playing <title>"`, which is not what the site shows and only echoes a title the UI already displays.
* It is **never logged**. The string names what the user is playing and where they are in it.

This increment exposes the string; it does not build a Discord integration. That belongs on #64/#66's hook
surface.

## What is never written to a log

The RA login token, any leaderboard title or running value, and the rich-presence message. `rc_client`'s own
verbose logging (`rc_client_enable_logging`) is deliberately **not** enabled — it would print leaderboard
titles at every attempt. `probe_ralead` §6 installs a Qt message handler and fails if any distinctive fixture
string reaches the transcript.
