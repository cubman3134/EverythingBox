# Discord shows what you are watching, playing and reading

**Date:** 2026-08-30
**Follows:** f3906c3e (`Merge: the player's transport row steps its own focus ring`)

## The problem

Every other thing on the user's machine that plays media tells Discord about it. Spotify does,
Steam does, RuneLite does, the emulator frontends people compare us against do. EverythingBox —
which is the one app on that machine that plays *all* of it — is invisible. Open a film, start a
SNES game, put on an audiobook, and your Discord profile says nothing at all.

That is a discovery problem as much as a feature gap. A presence card is the only advertising an
app like this gets that people actually welcome: a friend sees "Watching Blade Runner" under the
name EverythingBox and asks what it is.

The facts required to build such a card are already in the app, at seams that already exist. What
is missing is a transport, a rule for turning an item into a card, and a place to switch it off.

## What we are building

A presence card that follows whatever the user is doing, across every media track the app has.

| In the app | Discord type | `details` | `state` | Large image | Timestamp |
|---|---|---|---|---|---|
| Movie | Watching (3) | `Blade Runner` | `1982` | poster | `end` → counts down |
| TV episode | Watching (3) | `The Bear` | `S3E4 · Violet` | poster | `end` |
| Live TV | Watching (3) | `BBC One` | `Live TV` | channel logo | `start` → counts up |
| Music | Listening (2) | track title | `Artist — Album` | album art | `end` |
| Audiobook | Listening (2) | book title | `Ch. 12 · Author` | cover | `end` |
| Emulated game | Playing (0) | `Super Metroid` | `SNES` | box art | `start` |
| PC game | Playing (0) | `Hollow Knight` | `Steam` | capsule art | `start` |
| Comic | Playing (0) | `Watchmen` | `Reading · Ch. 3` | cover | `start` |
| Book / PDF | Playing (0) | `Project Hail Mary` | `Reading · p. 142 of 320` | cover | `start` |
| Nothing open | Playing (0) | `Browsing` | — | app logo | `start` (session) |

The `state` column is whatever the call site already has in hand, not a new lookup: the release year
for a movie when the metadata carries one and nothing when it does not, the chapter for a comic, the
page for a book or PDF, the storefront for a PC game.

Off by default. One master switch and six category switches under it. Up to two buttons per card:
`View on IMDb` for anything IMDB-keyed, and a permanent `Get EverythingBox`.

### Three decisions inside that table

**Reading has no honest Discord type.** The header renders as *Playing*, *Watching* or *Listening
to* and there is no fourth. Reading takes `Playing` and puts the verb in the body, so the card
reads *Playing EverythingBox / Watchmen / Reading · Ch. 3*. Wrong in the header, honest in the two
lines a human actually reads.

**Paused drops the timestamp.** Discord runs the countdown client-side from `timestamps.end`. Leave
that field set through a pause and the card counts a stopped film down to zero and then sits at
"0:00 left" — a lie that costs nothing to avoid. On pause the timestamp is cleared and `state`
becomes `Paused`; resume sends a fresh `end` computed from the position we resumed at.

**Art is an external URL, and only sometimes.** Discord accepts a public HTTPS URL directly in
`assets.large_image`, so streamed content shows its real poster with no asset uploads at all. A
local library's artwork is a file on the user's disk, which Discord's CDN cannot fetch. So the rule
is mechanical: use the row's `thumb` when it starts with `https://`, otherwise fall back to one of
eight uploaded keys (`movie`, `tv`, `livetv`, `music`, `audiobook`, `game`, `book`, `logo`). The
small badge always carries the type icon, so a card is identifiable even when the poster is not.

## Why not the two cheaper designs

**Hanging it off `RecentStore::add`.** One notifier inside that one function would catch all 37
"something was opened" call sites for free, which is genuinely tempting. It is wrong at both edges.
`CloudMerge` calls `add()` too, so a sync from the user's other device would announce *that*
device's item as this one's current activity. And the store only ever learns about opens — it has
no concept of stopping, pausing or finishing, which is three quarters of what a presence card is.

**Extending the `Scrobbler`.** Presence is "now playing" and `ScrobbleProvider` already has a
`nowPlaying` verb, so the shapes rhyme. They do not match. Everything that makes scrobbling correct
— the half-or-four-minutes threshold, the offline retry queue, the batched `submit` — is
meaningless for presence, and everything presence needs — pause, position, idle, clear — is
something scrobbling deliberately refuses to have (see the comment at the top of
`ScrobbleProvider.h`, which says so in as many words). Folding one into the other would cost a
clean seam to save a file.

## Architecture

Four files, in the shape `Scrobble.h` / `ScrobbleProvider.h` / `Scrobbler.cpp` already established
and `ConsumptionStats` already validated across every media track.

### `core/Presence.h` — the rules, and nothing that touches the world

```cpp
namespace Presence
{
    enum class Kind { None, Movie, Episode, LiveTv, Music, Audiobook, Game, PcGame, Reading };

    struct Item          // what the app knows
    {
        Kind    kind = Kind::None;
        QString title;      // "The Bear"
        QString subtitle;   // "S3E4 · Violet"
        QString artUrl;     // https:// only; anything else -> the fallback key
        QString imdbId;     // "tt123" -> the IMDb button; empty otherwise
        QString system;     // console id for games ("snes"), storefront for PC ("Steam")
    };

    struct Activity      // what goes on the wire
    {
        int     type = 0;
        QString details, state, largeImage, largeText, smallImage, smallText;
        qint64  startUnix = 0, endUnix = 0;
        QVector<QPair<QString, QString>> buttons;   // label, url
        bool operator==(const Activity& o) const;   // the throttle's whole basis
    };

    Activity build(const Item&, double positionSec, double durationSec, bool paused, qint64 nowUnix);
    Activity idle(qint64 sessionStartUnix);
    QString  clampUtf8(const QString&, int maxBytes);
}
```

No Qt beyond `QString`/`QVector`, no network, no settings, and **no clock**: `nowUnix` is a
parameter for the same reason `trakt::planMissed` takes its own — a rule that reads the clock can
only be tested by waiting.

`clampUtf8` is not decoration. Discord's limit on `details` and `state` is **128 bytes**, not 128
characters, and an update that exceeds it is discarded whole rather than truncated. A naive
`left(128)` on an accented or CJK title both overruns the limit and can split a codepoint. The
clamp counts UTF-8 bytes and never cuts mid-sequence.

### `core/PresenceTransport.h` — the seam

```cpp
struct PresenceTransport
{
    virtual ~PresenceTransport() = default;
    virtual void setActivity(const Presence::Activity&) = 0;
    virtual void clearActivity() = 0;
    virtual bool connected() const = 0;
};
```

Not a `QObject`, for `ScrobbleProvider.h`'s reason: the orchestrator drives it directly, and a
non-`QObject` seam is one a probe can replace with a two-line recording fake.

### `core/DiscordPresence.cpp` — the only I/O in the feature

A `QLocalSocket` walking `discord-ipc-0` through `discord-ipc-9`. On Windows those are
`\\.\pipe\discord-ipc-N`; elsewhere they are sockets under `$XDG_RUNTIME_DIR` (falling back to
`$TMPDIR`, then `/tmp`), including the `snap.discord/` and `app/com.discordapp.Discord/`
subdirectories that Snap and Flatpak installs use.

The framing is `[opcode u32 LE][length u32 LE][json]`, written as **one** `write()` — a frame split
across two writes breaks the pipe. Opcode 0 carries the handshake `{"v":1,"client_id":...}`; opcode
1 carries `{"cmd":"SET_ACTIVITY","args":{"pid":...,"activity":{...}},"nonce":...}`; opcodes 3 and 4
are ping and pong, which must be answered or Discord drops the connection.

**Discord not running is the normal case, not a failure.** Most users will not have it open, and
the ones who do will start it after us as often as before. So a failed connect is silent and
retried on a 5s → 60s backoff with no notification, no log spam and no error state; when a connect
finally succeeds the current activity is re-sent immediately, so starting Discord mid-film
populates the card without waiting for the next track boundary.

**A named hazard.** This codebase has already lost days to a `QLocalSocket` destroyed inside its own
`readyRead` emission — that was `probe_uitest`'s "flaky" 3% `rc=139`, and it was real heap
corruption, because Qt's frames resume on the socket after the slot returns and a `QPointer` does
not reach that far. The reconnect path here therefore never tears the socket down from inside one
of its own slots; teardown is deferred past the emission the way `deferPastQmlEmission` does it.

### `core/PresenceController.cpp` — the orchestrator

Owns the current `Item`, the position, the duration and the paused flag. On any input change it
rebuilds the `Activity` and **sends only if it differs from the one last sent**.

That single rule is what makes Discord's limit of five updates per twenty seconds a non-problem
rather than a thing to engineer around: a per-second position tick that does not move the countdown
produces a byte-identical `Activity` and sends nothing at all. A 4-second floor coalesces whatever
genuinely does change faster than that, so a scrub through a film is one send rather than a burst.

It also owns the settings gate. A category switched off during playback clears the card
immediately; the master switched off calls `clearActivity()` and disconnects.

## The hook points

Six, all of them seams that already exist. `ConsumptionStats` faced the identical "generalize across
every media track" problem and needed four; this needs those four plus the two game edges.

| # | Seam | Covers |
|---|---|---|
| 1 | `PlaybackSession` — `trackChanged`, plus the position/duration the host already pumps | movies, episodes, music, audiobooks, live TV |
| 2 | the host's mpv pause callback | `setPaused` |
| 3 | `playbackStopped` / `queueCleared` | back to idle |
| 4 | `GameLauncher::aboutToLaunch` and the launch tail | the game item |
| 5 | `GameLauncher::restoreRequested` (the emulator exited) | back to idle |
| 6 | `PdfView` / `ComicView` / `EbookView`, at the same page-turn edges `ConsumptionStats::addPagesRead` uses | reading |

The `Item`'s fields are assembled from the same locals that already build the `RecentItem` at each
of those sites — title, thumb, kind and system are all in hand there — so the feature adds no
lookups, no scrapes and no timers.

## Settings

In **both** settings builders, themed and classic. A user-facing setting added to only one of them
is unreachable from the default surface, and this repo has shipped that mistake before; the ROMs
folder row is the precedent for doing it properly.

```
discord/enabled    bool, default false    the master switch
discord/movies     bool, default true     movies and TV
discord/games      bool, default true     emulated and PC games
discord/music      bool, default true     music and audiobooks
discord/reading    bool, default true     books, comics, PDFs
discord/livetv     bool, default true     IPTV channels
discord/browsing   bool, default true     show "Browsing" when nothing is open
```

The six category rows only matter while the master is on. Alongside them, a status line built the
way `Scrobbler::statusLine()` is: *Connected to Discord* / *Discord is not running* / *Off*.
Without it, "it does not work" is unfalsifiable — the user cannot tell a wrong setting from a
closed Discord from a broken build, and neither can we.

## Testing

`probe_presence`, registered in all three places a probe has to be registered (the CMake target, the
runner script, and the CONTRIBUTING list). It needs no Discord, no socket and no network:

- every `Kind` maps to the right type, both lines, and the right image keys
- the 128-**byte** clamp against a multi-byte title — the arm that catches a naive `left(128)`
- countdown arithmetic; pause drops `end`; resume restores it from the resumed position
- send-only-on-change: an unchanged position tick produces no frame at all
- each category toggle gates its own `Kind`; the master off clears
- idle ↔ active transitions in both directions
- art falls back to the type key for a non-`https` thumb, and uses the URL when it is one
- the IMDb button appears only for an IMDB-keyed item, and its URL is well-formed; the
  `Get EverythingBox` button appears on every card, and no card ever carries more than two
- a frame round-trips through the encoder: opcode, little-endian length, single write

The recording fake transport is the `ScrobbleProvider` fake's pattern exactly.

## Out of scope

Join, spectate and party invites (they exist for multiplayer lobbies, and this app's netplay is
itself unverified on hardware); the Discord GameSDK; per-item privacy overrides; and presence for
individual settings screens beyond the single `Browsing` state.

## Prerequisite on the repository owner

Create the application at <https://discord.com/developers/applications>, named **EverythingBox** —
that name is the bold top line of every card. Upload the app logo as its icon, and upload the eight
fallback art assets listed above. The Application ID is public and gets compiled in as a constant.

Everything in this spec can be built and probed before that exists; only the live check against a
running Discord client needs it.
