// Headless test for DISCORD RICH PRESENCE. Prints PRESENCE-OK.
//
// §1 THE MAPPING. Each Kind's activity type, asset key and badge. Asserted per kind rather than in a loop so
//    a wrong arm names itself.
// §2 THE BYTE CLAMP. The arm that catches a naive left(128): Discord's limit is BYTES, and an over-long
//    update is discarded whole, so getting this wrong makes presence stop updating rather than show a
//    shortened title. Driven with multi-byte text, and asserted to land on a codepoint boundary.
// §3 TIMESTAMPS. A countdown only where one is meaningful; pause drops it; resume restores it from the
//    position resumed at; a live channel and a game count UP.
// §4 ART AND BUTTONS. An https poster is used verbatim, anything else falls back to the kind's key; the IMDb
//    button appears only for an IMDB-keyed item and never pushes the card past two buttons.
// §5 EQUALITY, which is the whole basis of the send-only-on-change rule in PresenceController.
// §6 IDLE.
// §7 THE SETTINGS GATE. Off by default, each category answerable on its own, and the master overriding all
//    of them. Every probe_* target is compiled with EB_ISOLATED_DATA_DIR, so the store starts empty on every
//    run and "the default is off" is a real assertion rather than a leftover from the last one.
#include "Presence.h"
#include "Settings.h"

#include <QCoreApplication>
#include <QString>
#include <cstdio>

static int fails = 0;
#define CHECK(cond, name) do { if (cond) printf("PASS %s\n", name); \
    else { printf("FAIL %s\n", name); ++fails; } } while (0)

using Presence::Activity;
using Presence::Item;
using Presence::Kind;

static Item movie()
{
    Item i; i.kind = Kind::Movie; i.title = QStringLiteral("Blade Runner");
    i.subtitle = QStringLiteral("1982"); i.imdbId = QStringLiteral("tt0083658");
    i.artUrl = QStringLiteral("https://img.example/poster.jpg");
    return i;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);   // Settings needs an application object for its store path
    const qint64 kNow = 1700000000;

    // ---- §1 THE MAPPING ----------------------------------------------------------------------------
    CHECK(Presence::typeFor(Kind::Movie)     == Presence::kWatching,  "map/movie is Watching");
    CHECK(Presence::typeFor(Kind::Episode)   == Presence::kWatching,  "map/episode is Watching");
    CHECK(Presence::typeFor(Kind::LiveTv)    == Presence::kWatching,  "map/live TV is Watching");
    CHECK(Presence::typeFor(Kind::Music)     == Presence::kListening, "map/music is Listening");
    CHECK(Presence::typeFor(Kind::Audiobook) == Presence::kListening, "map/audiobook is Listening");
    CHECK(Presence::typeFor(Kind::Game)      == Presence::kPlaying,   "map/game is Playing");
    CHECK(Presence::typeFor(Kind::PcGame)    == Presence::kPlaying,   "map/pc game is Playing");
    // Reading has no honest Discord type; it takes Playing and puts the verb in the body. Pinned so the
    // choice is deliberate rather than a default nobody revisited.
    CHECK(Presence::typeFor(Kind::Reading)   == Presence::kPlaying,
          "map/reading is Playing (no Reading type exists)");

    CHECK(Presence::fallbackAsset(Kind::Movie)     == QStringLiteral("movie"),     "asset/movie");
    CHECK(Presence::fallbackAsset(Kind::Episode)   == QStringLiteral("tv"),        "asset/episode");
    CHECK(Presence::fallbackAsset(Kind::LiveTv)    == QStringLiteral("livetv"),    "asset/live TV");
    CHECK(Presence::fallbackAsset(Kind::Music)     == QStringLiteral("music"),     "asset/music");
    CHECK(Presence::fallbackAsset(Kind::Audiobook) == QStringLiteral("audiobook"), "asset/audiobook");
    CHECK(Presence::fallbackAsset(Kind::Game)      == QStringLiteral("game"),      "asset/game");
    CHECK(Presence::fallbackAsset(Kind::PcGame)    == QStringLiteral("game"),
          "asset/pc game shares the game key");
    CHECK(Presence::fallbackAsset(Kind::Reading)   == QStringLiteral("book"),      "asset/reading");

    CHECK(!Presence::build(Item{}, 0, 0, false, kNow).valid,
          "map/none: an empty item is not a card — the orchestrator clears rather than sending a blank");
    {
        Item titleless; titleless.kind = Kind::Movie;
        CHECK(!Presence::build(titleless, 0, 0, false, kNow).valid,
              "map/untitled: a kind with no title is not a card either");
    }

    // ---- §2 THE BYTE CLAMP -------------------------------------------------------------------------
    {
        CHECK(Presence::clampUtf8(QStringLiteral("short"), 128) == QStringLiteral("short"),
              "clamp/short: a title inside the budget is untouched");

        // 60 copies of a 3-byte character = 180 bytes, over the 128-byte budget.
        const QString cjk(60, QChar(0x6F22));       // 漢
        const QString cut = Presence::clampUtf8(cjk, Presence::kMaxFieldBytes);
        CHECK(cut.toUtf8().size() <= Presence::kMaxFieldBytes,
              "clamp/cjk: the result fits the BYTE budget");
        CHECK(cut.toUtf8().size() == 126 && cut.size() == 42,
              "clamp/cjk: ...by whole characters (42x3=126), not by cutting the 43rd in half");
        CHECK(QString::fromUtf8(cut.toUtf8()) == cut,
              "clamp/cjk: the result round-trips through UTF-8 — no orphaned continuation byte");

        // A 2-byte character straddling the boundary is the case a byte-count-only clamp gets wrong.
        const QString acc = QString(127, QLatin1Char('a')) + QChar(0x00E9);  // 127 + 2 bytes = 129
        const QString acut = Presence::clampUtf8(acc, Presence::kMaxFieldBytes);
        CHECK(acut.toUtf8().size() == 127 && acut.endsWith(QLatin1Char('a')),
              "clamp/straddle: a 2-byte char that will not fit is dropped whole, not halved");

        Item longTitle = movie();
        longTitle.title = QString(300, QLatin1Char('x'));
        const Activity longCard = Presence::build(longTitle, 0, 0, false, kNow);
        CHECK(longCard.details.toUtf8().size() <= Presence::kMaxFieldBytes,
              "clamp/build: build() applies the clamp to details");
    }

    // ---- §3 TIMESTAMPS -----------------------------------------------------------------------------
    {
        // A film 20 minutes into 100 has 80 minutes left, and Discord is told the wall-clock instant it ends.
        const Activity a = Presence::build(movie(), 1200.0, 6000.0, false, kNow);
        CHECK(a.endUnix == kNow + 4800 && a.startUnix == 0,
              "time/countdown: a film reports the instant it ends, and no start");

        const Activity paused = Presence::build(movie(), 1200.0, 6000.0, true, kNow);
        CHECK(paused.endUnix == 0 && paused.startUnix == 0,
              "time/paused: NEITHER timestamp survives a pause — Discord counts down client-side, so an "
              "end left set would run a stopped film to zero");
        CHECK(paused.state == QStringLiteral("Paused"),
              "time/paused: ...and the second line says so instead of the subtitle");

        const Activity resumed = Presence::build(movie(), 1800.0, 6000.0, false, kNow);
        CHECK(resumed.endUnix == kNow + 4200,
              "time/resume: resuming recomputes the end from the position resumed at, not the original one");

        // No duration (a live stream, an unknown length) has no end to count to.
        CHECK(Presence::build(movie(), 10.0, 0.0, false, kNow).endUnix == 0,
              "time/no duration: an unknown length yields no countdown rather than a fabricated one");

        Item chan; chan.kind = Kind::LiveTv; chan.title = QStringLiteral("BBC One");
        const Activity live = Presence::build(chan, 0.0, 0.0, false, kNow);
        CHECK(live.startUnix == kNow && live.endUnix == 0,
              "time/live: a channel counts UP — it has no end");

        Item g; g.kind = Kind::Game; g.title = QStringLiteral("Super Metroid");
        CHECK(Presence::build(g, 0.0, 0.0, false, kNow).startUnix == kNow,
              "time/game: a game counts up from now");

        CHECK(!Presence::hasCountdown(Kind::Game) && !Presence::hasCountdown(Kind::Reading)
              && !Presence::hasCountdown(Kind::LiveTv),
              "time/kinds: games, reading and live TV have no meaningful end");
        CHECK(Presence::hasCountdown(Kind::Movie) && Presence::hasCountdown(Kind::Music),
              "time/kinds: films and tracks do");
    }

    // ---- §4 ART AND BUTTONS ------------------------------------------------------------------------
    {
        const Activity a = Presence::build(movie(), 0, 0, false, kNow);
        CHECK(a.largeImage == QStringLiteral("https://img.example/poster.jpg"),
              "art/https: a public poster URL is used verbatim — no upload needed");
        CHECK(a.smallImage == QStringLiteral("movie"),
              "art/badge: the small badge always carries the kind's icon");

        Item local = movie();
        local.artUrl = QStringLiteral("C:/Users/me/Posters/blade.jpg");
        CHECK(Presence::build(local, 0, 0, false, kNow).largeImage == QStringLiteral("movie"),
              "art/local: a file path falls back to the kind's key — Discord's CDN cannot fetch a local file");

        Item http = movie();
        http.artUrl = QStringLiteral("http://img.example/poster.jpg");
        CHECK(Presence::build(http, 0, 0, false, kNow).largeImage == QStringLiteral("movie"),
              "art/plain http: only https is accepted");

        CHECK(a.buttons.size() == 2, "buttons/movie: IMDb plus the project link");
        CHECK(a.buttons.at(0).first == QStringLiteral("View on IMDb")
              && a.buttons.at(0).second == QStringLiteral("https://www.imdb.com/title/tt0083658/"),
              "buttons/imdb: built from the tt id the app already holds");

        Item ep = movie();
        ep.kind = Kind::Episode; ep.imdbId = QStringLiteral("tt1234567:3:4");
        CHECK(Presence::build(ep, 0, 0, false, kNow).buttons.at(0).second
                  == QStringLiteral("https://www.imdb.com/title/tt1234567/"),
              "buttons/episode: the season:episode suffix is stripped — it is not part of the title URL");

        Item g; g.kind = Kind::Game; g.title = QStringLiteral("Super Metroid");
        const Activity ga = Presence::build(g, 0, 0, false, kNow);
        CHECK(ga.buttons.size() == 1 && ga.buttons.at(0).first == QStringLiteral("Get EverythingBox"),
              "buttons/game: no IMDb id, so only the project link");

        for (Kind k : { Kind::Movie, Kind::Episode, Kind::LiveTv, Kind::Music, Kind::Audiobook,
                        Kind::Game, Kind::PcGame, Kind::Reading }) {
            Item i; i.kind = k; i.title = QStringLiteral("t"); i.imdbId = QStringLiteral("tt1");
            CHECK(Presence::build(i, 0, 0, false, kNow).buttons.size() <= Presence::kMaxButtons,
                  "buttons/cap: no card ever carries more than two");
        }
    }

    // ---- §5 EQUALITY, the basis of send-only-on-change ----------------------------------------------
    {
        CHECK(Presence::build(movie(), 1200.0, 6000.0, false, kNow)
              == Presence::build(movie(), 1200.0, 6000.0, false, kNow),
              "eq/identical: the same inputs build an equal card");
        CHECK(Presence::build(movie(), 1200.0, 6000.0, false, kNow)
              != Presence::build(movie(), 1200.0, 6000.0, true, kNow),
              "eq/pause: pausing changes the card");
        // The one that matters: a tick that does not move the end instant is not a change. At a steady one
        // second per second, position and now advance together and the end lands on the same instant. This
        // is the arm that would break if endUnix were stored as "seconds remaining".
        CHECK(Presence::build(movie(), 1200.0, 6000.0, false, kNow)
              == Presence::build(movie(), 1201.0, 6000.0, false, kNow + 1),
              "eq/steady tick: a second of ordinary playback builds an IDENTICAL card, so nothing is sent");
        CHECK(Presence::build(movie(), 1200.0, 6000.0, false, kNow)
              != Presence::build(movie(), 3000.0, 6000.0, false, kNow),
              "eq/seek: a seek moves the end instant, so a frame IS owed");
    }

    // ---- §6 IDLE ------------------------------------------------------------------------------------
    {
        const Activity i = Presence::idle(kNow - 3600);
        CHECK(i.valid && i.type == Presence::kPlaying, "idle/type");
        CHECK(i.details == QStringLiteral("Browsing") && i.state.isEmpty(), "idle/lines");
        CHECK(i.startUnix == kNow - 3600 && i.endUnix == 0,
              "idle/elapsed: counts up from when the session started");
        CHECK(i.largeImage == QStringLiteral("logo"), "idle/art is the app logo");
    }

    // ---- §7 THE SETTINGS GATE -----------------------------------------------------------------------
    // Off by default, and each category answerable on its own. The defaults matter: this feature broadcasts
    // what somebody is watching to everyone who can see their profile, so it is opted into rather than out of.
    {
        CHECK(!Settings::discordEnabled(),
              "settings/default: presence is OFF until it is asked for");
        CHECK(!Settings::discordShows(Kind::Movie),
              "settings/default: ...so nothing is shown, whatever the categories say");

        Settings::setDiscordEnabled(true);
        CHECK(Settings::discordMovies() && Settings::discordGames() && Settings::discordMusic()
              && Settings::discordReading() && Settings::discordLiveTv() && Settings::discordBrowsing(),
              "settings/categories: once the master is on, every category is on unless silenced");

        CHECK(Settings::discordShows(Kind::Movie) && Settings::discordShows(Kind::Episode),
              "gate/movies covers both films and episodes");
        Settings::setDiscordMovies(false);
        CHECK(!Settings::discordShows(Kind::Movie) && !Settings::discordShows(Kind::Episode),
              "gate/movies: silencing it silences both");
        CHECK(Settings::discordShows(Kind::Game),
              "gate/independent: silencing films leaves games alone");
        Settings::setDiscordMovies(true);

        CHECK(Settings::discordShows(Kind::Game) && Settings::discordShows(Kind::PcGame),
              "gate/games covers emulated and PC alike");
        Settings::setDiscordMusic(false);
        CHECK(!Settings::discordShows(Kind::Audiobook),
              "gate/music: an audiobook follows the MUSIC switch, not the reading one");
        CHECK(Settings::discordShows(Kind::Reading),
              "gate/music: ...and silencing it leaves books alone");
        Settings::setDiscordMusic(true);

        Settings::setDiscordEnabled(false);
        CHECK(!Settings::discordShows(Kind::Movie) && !Settings::discordShows(Kind::Game)
              && !Settings::discordShows(Kind::Music) && !Settings::discordShows(Kind::Reading)
              && !Settings::discordShows(Kind::LiveTv),
              "gate/master: the master switch overrides every category, whatever they say");
        CHECK(!Settings::discordShows(Kind::None),
              "gate/none is never shown");
    }

    if (fails) { printf("PRESENCE-FAIL %d\n", fails); return 1; }
    printf("PRESENCE-OK\n");
    return 0;
}
