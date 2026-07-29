# EverythingBox Discord — community server design

**Date:** 2026-07-29
**Status:** approved design, not yet implemented

## Purpose

EverythingBox has a public repo, five platform builds, an addon protocol and a
theme protocol, but no synchronous place for users or contributors. GitHub
issues serve bug reports well and serve "how do I sideload this on my Shield"
badly.

This design covers **one Discord server** carrying three audiences at once:

1. **Users needing support** — install problems, emulator/core setup, addon and
   theme problems, and the platform-specific friction the README already
   documents (unsigned macOS builds, Android TV sideloading, iOS re-signing).
2. **A community** — people sharing library screenshots, TV setups, themes, and
   feature ideas.
3. **Contributors** — the codebase, the addon and theme protocols, and
   release-candidate testing across platforms we cannot test in CI.

The public front is open; the contributor section is behind a self-assigned
role, gating for noise reduction rather than secrecy.

### Success criteria

- A user with a broken Android TV install finds an answer without opening an
  issue, and the *next* user with that problem finds it without asking.
- The maintainer answers each recurring question once, not repeatedly.
- Contributors have a lower-latency channel than issue comments for protocol and
  design discussion.
- The server survives contact with the piracy requests that an emulation-adjacent
  project attracts.

### Non-goals

- Replacing GitHub issues. Bugs still belong in the tracker; the server routes
  people *to* it.
- Private support tickets. Support is public and searchable by design.
- Moderator staffing. A Moderator role is defined so it exists when needed, but
  the server is designed to be run by one person.

## Content policy

Emulation-adjacent communities attract ROM, ISO and stream-link requests within
days. Discord's Terms of Service prohibit sharing pirated content, and servers
that tolerate it get reported and removed — which would take the project's only
community channel with it.

**The rule: no links to, requests for, or sourcing of copyrighted ROMs, ISOs,
or media streams.** Emulation itself, core configuration, dumping media you own,
and legal sources are all fully on-topic and explicitly welcomed. The rules text
states the reason alongside the rule, because a prohibition with a rationale is
followed and a bare prohibition is argued with.

Enforcement is deliberately asymmetric — see AutoMod below. Domains are blocked
outright; *phrases* only raise an alert. A rule that auto-deletes "I dumped my
own ROM" or "which core loads this ISO" makes the server hostile to its own
subject matter, so a human judges those.

## Server structure

Requires **Community mode enabled** (Forum channels, Onboarding, Announcement
channels, and the full AutoMod ruleset all depend on it).

### Categories and channels

| Category | Channel | Type | Notes |
|---|---|---|---|
| 📌 Start here | `#welcome-and-rules` | Text, read-only | Rules, content policy, links to repo/releases |
| | `#announcements` | Announcement | Releases, breaking changes. Announcement type lets other servers follow it |
| | `#releases` | Text, read-only | Native GitHub webhook, **release events only** |
| 💬 Community | `#general` | Text | |
| | `#showcase` | Text | Library screenshots, TV setups, themes, addons |
| | `#off-topic` | Text | |
| | `Voice Chat` | Voice | One, until demand proves otherwise |
| 🛠 Support | `#support` | **Forum** | The primary support surface. Tags below |
| | `#faq` | Text, read-only | Mirrors the bot's tag macros |
| 🧩 Addons & themes | `#addon-development` | Text | The sandboxed JS addon protocol |
| | `#theme-development` | Text | The theme/QML surface |
| 🔧 Dev *(Contributor)* | `#dev-general` | Text | |
| | `#github` | Text, read-only | Webhook: issues, PRs, commits |
| | `#testing` | Text | RC testing; pings platform roles |
| 🔒 Staff *(Maintainer/Moderator)* | `#mod-log` | Text | Bot + AutoMod logging |
| | `#mod-chat` | Text | |

Fifteen text channels and one voice. Each has a job that no other channel does; anything that
failed that test was folded into a sibling (addon showcase into `#showcase`,
CI/build chatter into `#github`).

### `#support` forum tags

Platform: `Windows` · `macOS` · `Linux` · `Android TV` · `iOS`
Area: `Video` · `Audio` · `Emulation` · `Addons` · `Themes` · `Readers`
State: `Solved`

Tags rather than per-platform channels. This gets precise routing — the
maintainer can filter to every open Android TV thread — without spreading a
small community across twenty-five near-empty rooms. Each problem becomes a
searchable thread, so the answer outlives the conversation.

### Roles

| Role | Assignment | Purpose |
|---|---|---|
| **Maintainer** | Manual | Administrator. The project owner |
| **Moderator** | Manual | Defined now, unused until moderators are recruited. Manage messages/members, no server-settings access |
| **Contributor** | Self, at onboarding | Unlocks the 🔧 Dev category |
| **Addon/Theme Dev** | Self, at onboarding | Pingable audience for protocol changes |
| **Tester** | Self, at onboarding | Pingable for release-candidate testing |
| **Platform roles** | Self, at onboarding | `Windows` `macOS` `Linux` `Android TV` `iOS`. Lets the maintainer ask "anyone on Android TV, try this RC" |
| **Bots** | Automatic | Holds bot permissions as a group |

Contributor is self-assigned because the Dev category is not secret — the gate
exists so that a user asking about video playback is not reading CI chatter.

### Onboarding

Discord native Onboarding: rules screening, then two questions — a platform
picker granting the platform role(s), and "what brings you here?" whose answers
map to Tester, Addon/Theme Dev, and Contributor. Default channels for a new
member are `#welcome-and-rules`, `#general`, `#support`, `#announcements`.

## Moderation stack

### Bots

**One** third-party bot, not two. The GitHub feeds are Discord's native webhook,
not a bot, so they cost no third-party permissions in the server.

**Carl-bot** (Dyno is an equivalent substitute) earns its place primarily for
**tag macros**, not moderation — native AutoMod covers moderation. The recurring
questions are known in advance from the README's own caveats, and each becomes a
one-word command:

| Tag | Content |
|---|---|
| `!logs` | Where `stream_debug.log` lives; Settings ▸ Debug shows its tail and opens the folder |
| `!macos` | Unsigned build — first launch is right-click ▸ Open |
| `!androidtv` | Sideloading the APK; what is and is not available on Android (standalone emulators are desktop-only) |
| `!ios` | AltStore / Sideloadly re-signing; emulation is unavailable on iOS |
| `!build` | The `-DEVERYTHINGBOX_BUILD_APP=ON` configure line and the named-target rule |
| `!bug` | How to file a good issue, with the template link |

Mod-log and reaction roles come along with it.

**Deliberately excluded:** a ticket bot. Public forum threads beat private
tickets for open-source support because the answer stays searchable. Statbot
(growth metrics) and Wick (raid protection) are the sensible second-bot
candidates, deferred until a demonstrated need rather than installed into an
empty server.

Bot feature sets change; the exact capabilities above are verified at
configuration time rather than trusted from memory.

### AutoMod rules

| # | Rule | Action |
|---|---|---|
| 1 | Piracy **domain** blocklist | Block message + alert `#mod-log` |
| 2 | Piracy **phrase** list | **Alert `#mod-log` only — do not block** |
| 3 | Discord invite links, non-staff | Block message |
| 4 | Mention spam (native) | Block |
| 5 | Suspected spam content (native) | Block |

Rule 2's alert-only posture is the load-bearing decision. False positives on
legitimate emulation discussion cost more than the delay of human review.

## Implementation

### Where the work lives

The work splits across two repositories, and the boundary is *discoverability
versus operations*.

**`cubman3134/everythingbox-discord` — new, private.** The setup script, the
structure config, the rules copy and the bot tag-macro content. This follows the
satellite pattern already used for `everythingbox-addons` and
`everythingbox-themes`: separate repos under the same account, cloned locally
but untracked by the app repo.

It is **private**, not public, for one specific reason: `server.json` carries the
AutoMod piracy **domain blocklist**, and publishing that list is a precise
roadmap for evading it. Nothing else in the repo benefits from being public
either — nobody but the maintainer will ever run the script. If the tooling is
ever worth open-sourcing, the blocklist splits into a gitignored sidecar at that
point, not before.

**`cubman3134/EverythingBox` — the existing app repo.** Only the discoverability
surface, because those files only work from the app repo. Four text edits and
one code change; no Node script, no server config.

### The setup script *(everythingbox-discord)*

Zero-dependency **Node** using built-in `fetch` against the Discord REST API: no
`npm install`, no lockfile, no `node_modules`. Keeping it out of the app repo
also means no foreign-language build artefact sitting in a C++/Qt tree for every
contributor to mentally skip past.

- **`server.json`** — the declarative structure: categories, channels, types,
  topics, forum tags, roles, permission overwrites, AutoMod rules. This file,
  not the running server, is the source of truth.
- **`apply.js`** — reads the config and reconciles the server. **Idempotent**:
  match by name, create what is missing, update what has drifted, and **never
  delete**. Re-runnable after any config edit.
- **`--dry-run`** prints the full plan without mutating anything. Always run
  first.

**Token handling.** The script reads `DISCORD_BOT_TOKEN` from the environment,
falling back to a gitignored `.token` file beside the script. The token lives
with the script rather than in the app repo's `native/secrets/` — that directory
is for *build-time* credentials embedded into the binary, which this is not. It
is never committed, logged, or echoed by the script.

### Repository wiring *(EverythingBox)*

| File | Change |
|---|---|
| `README.md` | Community section plus a Discord badge near the download table |
| `.github/ISSUE_TEMPLATE/config.yml` | **New.** A `contact_links` entry putting the invite on GitHub's New Issue chooser — catching people at the moment they need help, before they file a support question as a bug |
| `.github/SUPPORT.md` | **New.** Where to get help: server for questions, issues for bugs |
| `CONTRIBUTING.md` | Pointer to `#dev-general` and `#addon-development` |

### In-app link *(EverythingBox)*

A **Community** entry opening the invite via `QDesktopServices::openUrl`.

Per the standing convention in `CONTRIBUTING.md`,
`MainWindow::openGeneralSettings()` is written twice — a themed `PanelRow`
builder and a classic `QWidget` builder — and the themed surface is the
default-reachable one. **The entry must be added to both halves**, or it ships
invisible to most users. The exact insertion point is determined by reading
`native/src/ui/MainWindow.cpp` during planning.

This is a separate commit from the repo wiring, and it must pass:

```
BUILD_DIR=build bash native/tools/run-headless-probes.sh
```

ending in `ALL HEADLESS PROBES PASSED`.

### Invite

A permanent, non-expiring invite code. A vanity `discord.gg/everythingbox` URL
requires Level 3 boosting and is out of scope.

## Order of operations

Manual prerequisites first — these are account actions that cannot and should
not be scripted:

1. **User** creates an empty server, enables **Community mode**, and confirms
   **2FA** is enabled on the account (Discord requires it for admin actions on
   community servers).
2. **User** creates a bot application in the Discord developer portal, invites
   it with Administrator, and exports the token.

Then:

3. Create the private `everythingbox-discord` repo.
4. Write `server.json` and `apply.js`; run `--dry-run` and review the plan.
5. Apply. Verify the structure in the client.
6. Configure Onboarding, the GitHub webhooks, and Carl-bot tags.
7. Generate the permanent invite.
8. Land the EverythingBox wiring and the in-app link — **last**, because both
   embed the invite URL, which does not exist until step 7.

## Risks

| Risk | Mitigation |
|---|---|
| Piracy requests get the server reported | Explicit rule with stated reasoning, domain-level AutoMod blocking, alert-only phrase rule to keep enforcement humane |
| An empty server reads as abandoned | Fifteen channels, not forty. Onboarding defaults surface only four |
| Solo support does not scale | Forum threads make answers searchable; tag macros make repeat answers one word |
| Script misconfigures a live server | Idempotent, never-deletes, `--dry-run` reviewed before every apply |
| Bot token leak | Env var or a gitignored `.token` beside the script; never committed or logged |
| Blocklist published, enabling evasion | The satellite repo holding `server.json` is private |
| In-app link ships invisible | Explicitly added to both `openGeneralSettings()` builders; probe suite gates the commit |
