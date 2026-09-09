// Persistent user settings (portable INI next to the executable). For now: the chosen core per system.
#pragma once
#include <QString>
#include <QStringList>
#include <QMap>
#include "../video/SubtitleStyle.h"   // Settings::subtitleStyle() returns the pure Style the player applies (#71)
#include "../video/AudioOutput.h"     // Settings::audioOutput() returns the pure Output the player applies (#69)
#include "../video/HdrOutput.h"       // Settings::hdrOutput() returns the pure HDR Mode the player applies (#68)
#include "../video/ReplayGain.h"      // Settings::replayGainMode() returns the pure Mode the player applies (#141)
#include "../video/Crossfade.h"       // Settings::crossfadeSeconds() is clamped through Crossfade's band (#141)
#include "../ebook/ReaderTypography.h" // Settings::readerTypography() returns the pure typography the reader applies (#135)
#include "EmuBackend.h"                // Settings::backendFor() returns the per-system emulation backend (RetroPark Slice 2a)
#include "Presence.h"                 // Settings::discordShows() answers per Presence::Kind (Discord presence)

namespace Settings
{
    // Stable per-install device identity (multi-device sync). Minted ONCE as a UUID on first read and
    // persisted at key "device/id"; every later call returns the same string. Write-once: a non-empty stored
    // value is never overwritten, so concurrent/repeated reads can never mint a second id. This id is
    // device-LOCAL — the sync carve-out (T4) excludes "device/*" from the synced settings bundle, and it
    // namespaces the per-device accumulators (T3) so two devices never double-count. Never empty on return.
    QString deviceId();                       // key "device/id"; minted once, stable, device-local

    // What this device CALLS ITSELF on the LAN (issue #143): the name a peer's picker shows as
    // "EverythingBox on <name>". Defaults to the machine's host name, which is already the name the user
    // gave this box, so the feature is usable before anyone visits Settings. Device-local like device/id
    // beside it -- syncing it would rename every box on the account to whatever the last one typed, and the
    // whole point of the string is to tell them apart. Never empty on return.
    QString deviceName();                     // key "device/name", default = the machine host name
    void setDeviceName(const QString& name);

    // General playback: auto-show subtitles on every video, and the preferred subtitle language (an ISO
    // 639 code like "eng"; empty = no preference / first available).
    bool subtitlesOnByDefault();
    void setSubtitlesOnByDefault(bool on);
    QString subtitleLanguage();
    void setSubtitleLanguage(const QString& code);

    // The preferred CONTENT language (canonical ISO-639-1 two-letter, e.g. "en"; empty = no
    // preference). Governs subtitle + audio track selection and is sent to our server as
    // Accept-Language. Migrated once from the legacy 3-letter "subs/language".
    QString preferredLanguage();
    void setPreferredLanguage(const QString& code);

    // Subtitle appearance (issue #71): font, size, colour, outline, background box and vertical position, applied
    // to mpv's un-styled (SRT/text) subtitle renderer via SubtitleStyle::toMpvOptions. Normal synced user
    // preferences (the "subs/" group is NOT in CloudSync's device-local carve-out), so a look chosen on one
    // device follows the account to the next — unlike #69's per-device settings. subtitleStyle() gathers the
    // stored values (with mpv-matching defaults) into the pure Style struct the player and probe_substyle share.
    SubtitleStyle::Style subtitleStyle();
    QString subtitleFont();                 // key "subs/font"; "" => mpv's default family
    void    setSubtitleFont(const QString& family);
    int     subtitleSizePercent();          // key "subs/sizePercent"; default 100 (-> sub-scale 1.0)
    void    setSubtitleSizePercent(int pct);
    QString subtitleColor();                // key "subs/color"; default "#FFFFFF"
    void    setSubtitleColor(const QString& hex);
    int     subtitleBorderSize();           // key "subs/borderSize"; default 3
    void    setSubtitleBorderSize(int px);
    QString subtitleBorderColor();          // key "subs/borderColor"; default "#000000"
    void    setSubtitleBorderColor(const QString& hex);
    bool    subtitleBox();                  // key "subs/box"; default false (no background box)
    void    setSubtitleBox(bool on);
    int     subtitleBoxOpacity();           // key "subs/boxOpacity"; default 75 (percent)
    void    setSubtitleBoxOpacity(int pct);
    int     subtitlePosition();             // key "subs/pos"; default 100 (0 = top … 100 = bottom)
    void    setSubtitlePosition(int pos);
    bool    subtitleBold();                 // key "subs/bold"; default false
    void    setSubtitleBold(bool on);
    bool    subtitleOverrideStyled();       // key "subs/assOverride"; default false (leave ASS/SSA alone)
    void    setSubtitleOverrideStyled(bool on);

    // Reader typography (issue #135): the ebook reading font, size, line spacing, page margin, justification and
    // reading theme, applied to EbookView's QTextDocument + page chrome via ReaderTypography::resolve. Normal
    // synced user preferences (the "reader/" group is NOT in CloudSync's device-local carve-out) — reading taste
    // follows the account across devices. readerTypography() gathers the stored values into the pure Settings the
    // reader and probe_readertypography share. Size reuses the EXISTING "ebook/fontSize" key that the in-reader
    // A+/A− stepper already drives, so there is ONE notion of reading size (mirroring #71's single size notion).
    ReaderTypography::Settings readerTypography();
    QString readerFont();                   // key "reader/font"; "" => the reader's own default family
    void    setReaderFont(const QString& family);
    int     readerFontSize();               // key "ebook/fontSize"; default 14 (clamped 8..40)
    void    setReaderFontSize(int pt);
    int     readerLineSpacing();            // key "reader/lineSpacing"; default 100 (clamped 100..250)
    void    setReaderLineSpacing(int pct);
    int     readerMargin();                 // key "reader/margin"; default 6 (clamped 0..25)
    void    setReaderMargin(int pct);
    bool    readerJustify();                // key "reader/justify"; default false
    void    setReaderJustify(bool on);
    ReaderTypography::Theme readerTheme();   // key "reader/theme"; default Light (stored as int 0..3)
    void    setReaderTheme(ReaderTypography::Theme t);

    // In-book lookup (issue #137). Define and Wikipedia are zero-config (Wiktionary's and Wikipedia's REST
    // endpoints need no key), so the only setting the feature has is WHERE to translate: a LibreTranslate-class
    // instance URL, supplied by the user because no such endpoint is free and keyless. EMPTY IS MEANINGFUL —
    // the Translate verb is then ABSENT from the reader's action menu rather than present and failing, which is
    // the difference between a feature you have not set up and one that is broken.
    QString readerTranslateEndpoint();      // key "reader/translateEndpoint"; default "" (no Translate verb)
    void    setReaderTranslateEndpoint(const QString& url);

    // Audio output (issue #69): the output device, passthrough (bitstream to receiver) and exclusive mode,
    // mapped to mpv's audio-device / audio-spdif / audio-exclusive via AudioOutput::toMpvOptions. Unlike the
    // subtitle look above, these are DEVICE-LOCAL: an audio-device id is meaningless on another machine, so the
    // "audio/" group is in CloudSync's device-local carve-out and does NOT sync (probe_audioout + probe_cloudmerge
    // §16 pin that). audioOutput() gathers the stored values (with mpv-matching defaults) into the pure Output
    // struct the player and probe_audioout share — one place gathers the group so a new field is added in one spot.
    AudioOutput::Output audioOutput();
    QString audioDevice();                  // key "audio/device"; "" => Auto (mpv's auto-select)
    void    setAudioDevice(const QString& id);
    bool    audioPassthrough();             // key "audio/passthrough"; default false (decode to PCM)
    void    setAudioPassthrough(bool on);
    bool    audioExclusive();               // key "audio/exclusive"; default false (shared mode)
    void    setAudioExclusive(bool on);

    // Auto-play the next TV episode when one finishes (default on).
    bool autoplayNextEpisode();
    void setAutoplayNextEpisode(bool on);

    // Gapless playback for the audio queue (issue #141). DEFAULT OFF — opt-in. When on, an audio queue feeds
    // mpv's own playlist one-ahead so the decoder never stops across a track boundary (live/concept albums play
    // with no seam); when off, the pre-#141 stop-start-per-track path is used unchanged. Applies to the audio
    // queue only (not video, not a single file). A plain user preference — it syncs with the settings bundle
    // like autoplayNext (not device-local, not per-item).
    bool gaplessAudio();
    void setGaplessAudio(bool on);

    // ReplayGain (issue #141). Off / Track / Album, DEFAULT ALBUM — unlike gapless this is on out of the box,
    // because it only ever acts on files that were deliberately tagged with a gain, and on those files doing
    // nothing is the wrong answer. Applies to MUSIC only; audiobooks and podcasts are carved out inside
    // ReplayGain::effectiveMode, not here. The preamp is a plain dB offset on top of the tagged gain, clamped
    // to the ±15 dB band ReplayGain::clampPreamp defines (house style: clamped on read AND write, cf.
    // defaultPlaybackSpeed). Both are ordinary user preferences — they bundle-sync like autoplayNext, and
    // neither is device-local or per-item. Clipping prevention is NOT a setting: it stays on (ReplayGain.h
    // says why).
    ReplayGain::Mode replayGainMode();
    void setReplayGainMode(ReplayGain::Mode mode);
    double replayGainPreamp();
    void setReplayGainPreamp(double db);

    // Crossfade for the music queue (issue #141), in SECONDS. 0 == off and is the DEFAULT; 1-12 is the band
    // #141 names. Off by default for the opposite reason ReplayGain is on by default: ReplayGain only ever
    // acts on files somebody deliberately tagged, while a crossfade rewrites every boundary it is allowed
    // near — so it waits to be asked for. Where it then applies is NOT decided here: the music-only carve-out,
    // the same-album suppression and the too-short-track cap all live in Crossfade::secondsFor. A plain user
    // preference — it bundle-syncs like autoplayNext, and is neither device-local nor per-item.
    int crossfadeSeconds();
    void setCrossfadeSeconds(int seconds);

    // Look lyrics up online when a track has none of its own (issue #142, source 3). DEFAULT ON, which the
    // issue asks for and which is only defensible because of what it is a toggle over: LRCLIB is free, keyless
    // and accountless, the lookup happens once per track and only when that track is actually PLAYING (never
    // as a library sweep), the answer is cached in the item's MetaCache folder so it is never asked for twice,
    // and it is only reached at all when neither the .lrc sidecar nor the file's own tags had anything — the
    // precedence in LyricSources::needsOnline, not here. Off leaves the two local sources working untouched.
    // A plain user preference: it bundle-syncs like autoplayNext, and is neither device-local nor per-item.
    bool onlineLyrics();
    void setOnlineLyrics(bool on);

    // Is the CLASSIC player page's lyric panel showing (issue #142)? DEFAULT OFF: it is a third pane in the
    // player splitter, and a listener who has not asked for lyrics should not be handed one. Not a
    // general-settings row on either surface, deliberately — it is the remembered state of the player's own
    // gear-menu toggle, the same shape as which subtitle track you last chose, not a preference you go to
    // Settings to find. The THEMED layout needs no twin: its lyric panel is placed by the theme.
    bool lyricsPanel();
    void setLyricsPanel(bool on);

    // The default playback speed applied to a non-music audio item (audiobook/podcast) that has no remembered
    // per-item speed (issue #140). Default 1.0; clamped to the same 0.5–3.5x band the transport allows. Music
    // ignores this and stays 1x unless a per-item speed was explicitly set — the split lives in
    // SpeedStore::speedForItem, not here.
    double defaultPlaybackSpeed();
    void setDefaultPlaybackSpeed(double rate);

    // The jump/skip interval, in seconds, the audio transport's skip-back / skip-forward controls step by
    // (issue #140). Default 30 — interval-jumping ("what did she just say"), not scrubbing, is the audiobook
    // muscle memory. Clamped to a sane 5..120 s band on read and write. A plain playback preference: it
    // bundle-syncs with the settings like autoplayNext (NOT device-local, NOT per-item — playback/* is neither
    // carve-out). Video seeking is unaffected; only the audio transport reads this.
    int  audioJumpSeconds();
    void setAudioJumpSeconds(int seconds);

    // Touch gestures over the video player (issue #162). One switch per gesture FAMILY, all default ON: the
    // recogniser only ever runs on a touch form factor, so an enabled family is invisible on desktop and TV.
    // gestureEdgeInset is the band along each window edge where a touch is ignored so the OS's own back /
    // notification swipes are never fought (0..96 px, default 24). The video skip interval is NOT here on
    // purpose — it is audioJumpSeconds above, shared, exactly as issue #162 asks.
    bool gestureVolume();
    void setGestureVolume(bool on);
    bool gestureBrightness();
    void setGestureBrightness(bool on);
    bool gestureSeek();
    void setGestureSeek(bool on);
    bool gestureDoubleTap();
    void setGestureDoubleTap(bool on);
    bool gestureLongPress();
    void setGestureLongPress(bool on);
    bool gesturePinch();
    void setGesturePinch(bool on);
    int  gestureEdgeInset();
    void setGestureEdgeInset(int px);

    // Touch reading (issue #147), in the SAME gestures/ home the video player's keys use — a reader gesture
    // and a player gesture are one vocabulary, so they are one group of settings. All three are inert off a
    // touch form factor (ReaderGestureConfig applies FormFactor exactly as #162 does), which is what keeps a
    // phone's preset out of the TV profile.
    //
    // readerTapZones is a PRESET, stored as ReaderGestures::TapPreset's int: 0 left-goes-back (the default),
    // 1 its left-thumb mirror, 2 menu-only (paging by swipe alone). Clamped on read AND on write, so a
    // hand-edited ini reads as the default rather than as no zones at all. readerKeepAwake defaults OFF —
    // holding a device awake is a promise about someone's battery and it is theirs to make.
    int  readerTapZones();
    void setReaderTapZones(int preset);
    bool readerSwipePaging();
    void setReaderSwipePaging(bool on);
    bool readerKeepAwake();
    void setReaderKeepAwake(bool on);

    // Dual-page landscape for BOOKS (issue #147): a two-column spread when the viewport is wide. A pagination
    // geometry change in the existing text flow — not a comic's two-up, which is issue #154's and untouched.
    // Default ON, matching the comic spread's own default: two columns on a wide screen is better reading,
    // and the row below turns it off for anyone who disagrees.
    bool readerDualPage();
    void setReaderDualPage(bool on);

    // Skip an episode's intro / end credits when one is known (default on). skipSegmentsAuto seeks silently
    // instead of offering the on-screen chip (default off — a wrong learned range is recoverable when it is
    // a button you ignored, and invisible when it is a seek that already happened).
    bool skipSegments();
    void setSkipSegments(bool on);
    bool skipSegmentsAuto();
    void setSkipSegmentsAuto(bool on);

    // Parental PIN (stored as a salted hash, never in the clear). Gates leaving a restricted "kids" profile.
    bool hasParentalPin();
    void setParentalPin(const QString& pin); // empty pin clears it
    bool checkParentalPin(const QString& pin);

    // "Who's using EverythingBox?" at launch (issue #30). Always-ask is the DEFAULT — this is the opt-out for
    // a single-user install that finds the extra press pointless, and it speaks ONLY for the exactly-one-
    // profile case. The picker-or-not decision itself is ProfilePasscode::mustShowPicker, which also refuses
    // to honour this when that one profile has a passcode; this accessor is just the stored preference.
    bool skipProfilePickerWhenSingle();          // key "profiles/skipPickerWhenSingle", default false
    void setSkipProfilePickerWhenSingle(bool on);

    // Trakt.tv scrobbling. Client id/secret come from a Trakt API app the user registers; the tokens are
    // obtained via the device-code OAuth flow and refreshed automatically. All empty => Trakt is off.
    QString traktClientId();
    void setTraktClientId(const QString& v);
    QString traktClientSecret();
    void setTraktClientSecret(const QString& v);
    QString traktAccessToken();
    QString traktRefreshToken();
    qint64  traktTokenExpiry();          // unix seconds; 0 = not connected
    void setTraktTokens(const QString& access, const QString& refresh, qint64 expiryUnix);
    void clearTraktTokens();

    // MUSIC SCROBBLING (issue #192). PER PROFILE — two people who share the box have two listening histories,
    // so every key below is namespaced by the active profile through Scrobble::profileSlot, exactly as the
    // Trakt backfill cursor is. And DEVICE-LOCAL in both directions: the token is a secret that must never
    // ride the sync bundle to another machine, and an on/off bound to a credential you have to paste on each
    // device anyway is not a preference that should arrive without it. Scrobble::isDeviceLocalKey owns that
    // carve-out for CloudSync, so the exclusion is written in terms of the same prefix these writers use.
    //
    // These four are the half a settings DISCARD may revert (they are typed and toggled by the user); the
    // counter, the offline queue and the last error are the other half and live in ScrobbleQueue, out of the
    // transaction's scope, because playback writes them while a settings panel is open.
    bool scrobbleEnabled();                      // the visible per-profile on/off, default OFF
    void setScrobbleEnabled(bool on);
    bool scrobbleSpokenAudio();                  // include audiobooks + podcasts, default OFF (see Scrobble.h)
    void setScrobbleSpokenAudio(bool on);
    // THE DOUBLE-COUNT COORDINATION (issue #193). ONE setting, which is the issue's exact requirement ("a
    // single clear setting rather than two that silently conflict"), and it answers one question: does the
    // music server forward its own plays to Last.fm / ListenBrainz already?
    //
    //   OFF (default): this app reports a play from a music server to the server AND to the listening
    //                  services you have connected here. Right for a server that keeps its own play counts
    //                  and forwards nothing, which is the ordinary setup.
    //   ON:            this app reports a play from a music server to that server ONLY, and lets the server
    //                  do the forwarding. Right when Navidrome (or Airsonic, or Gonic) is signed in to
    //                  Last.fm or ListenBrainz itself — otherwise every one of those plays is counted twice.
    //
    // Note which half it does NOT touch: local files and addon streams are unaffected either way, because
    // there is no server in the middle of those to forward anything. Scrobble::verdictForDestination is the
    // rule, and it is why the server itself is still told when this is on: it can only forward what it hears.
    bool scrobbleServerForwards();               // default OFF
    void setScrobbleServerForwards(bool on);

    // ---- DISCORD RICH PRESENCE ---------------------------------------------------------------------
    // OFF by default and opted into once, because this broadcasts what somebody is watching to everyone who
    // can see their Discord profile. The five category switches sit UNDER the master: they are meaningless
    // while it is off, which is what discordShows() encodes so that no caller has to remember it.
    //
    // DEVICE-LOCAL (see CloudSync::isDeviceLocalKey). Whether the machine in the living room announces what
    // it is playing is a property of THAT machine, not of the account - turning presence on for a laptop
    // must not silently switch it on for a shared TV.
    bool discordEnabled();               void setDiscordEnabled(bool on);
    bool discordMovies();                void setDiscordMovies(bool on);    // films and episodes
    bool discordGames();                 void setDiscordGames(bool on);     // emulated and PC
    bool discordMusic();                 void setDiscordMusic(bool on);     // music and audiobooks
    bool discordReading();               void setDiscordReading(bool on);   // books, comics, PDFs
    bool discordLiveTv();                void setDiscordLiveTv(bool on);
    bool discordBrowsing();              void setDiscordBrowsing(bool on);  // the "Browsing" idle card

    // The ONE predicate that maps a kind onto its toggle. Both settings builders and the orchestrator ask
    // this rather than re-deriving the mapping, so "an audiobook follows the music switch" is written once.
    bool discordShows(Presence::Kind kind);
    // The ListenBrainz user token — one string, pasted once, no OAuth dance. THE USER'S OWN SECRET: nothing
    // in this app logs it, echoes it, or puts it in an error message (see ListenBrainzClient.cpp).
    QString listenBrainzToken();
    void setListenBrainzToken(const QString& token);
    // A custom API root. Empty => the public ListenBrainz service. Non-empty transparently covers Maloja and
    // the other servers that implement the same submit-listens endpoint, which is why this is one setting
    // rather than a second provider.
    QString listenBrainzApiUrl();
    void setListenBrainzApiUrl(const QString& url);

    // LAST.FM (#192 increment 2). The SESSION KEY, and only the session key: the desktop-auth flow has no
    // password to store, and the request token it is exchanged for is spent the moment it is used. THE
    // USER'S OWN SECRET, on exactly the terms the ListenBrainz token above is held: read by one caller at
    // the moment it signs a request, never logged, never echoed into a message, and carved out of every sync
    // bundle by Scrobble::isDeviceLocalKey (the "scrobble/" prefix already covers it). Per profile.
    QString lastFmSessionKey();
    void setLastFmSessionKey(const QString& key);
    // WHICH account that session belongs to, so the settings row can name it. Not a secret — but it is
    // written and cleared with the key, because a username left behind after a disconnect claims a link that
    // is no longer there.
    QString lastFmAccount();
    void setLastFmAccount(const QString& name);

    // OpenSubtitles.com credentials for auto-downloading subtitles when a video has none in the preferred
    // language. The REST API needs an app API key (register once, free) for search, plus the user's account
    // (login is required to download). All three empty => the feature is dormant. Stored in the local INI.
    QString openSubApiKey();
    void setOpenSubApiKey(const QString& key);
    QString openSubUsername();
    void setOpenSubUsername(const QString& user);
    QString openSubPassword();
    void setOpenSubPassword(const QString& pass);

    // Steam Web API credentials (user-supplied; NEVER embedded). The API key (also used by PC-game achievements)
    // plus the 64-bit SteamID unlock the owned-but-not-installed library on the Steam console. Either empty =>
    // installed-only, no network. Stored in the local INI at steam/apikey + steam/steamid.
    QString steamWebApiKey();
    void setSteamWebApiKey(const QString& key);
    QString steamId();
    void setSteamId(const QString& id);

    // Open the app maximized to full screen on launch (default off — a normal resizable window).
    bool startFullscreen();
    void setStartFullscreen(bool on);

    // Form-factor / adaptivity (subsystem D). The chosen display mode: "auto" (default — platform detection)
    // or an explicit "desktop"|"tv"|"mobile" override. FormFactor resolves this into its token table; a
    // caller that writes it must then call FormFactor::instance().refresh() to re-resolve + notify.
    QString displayMode();                    // "auto"|"desktop"|"tv"|"mobile"; default "auto"; key "display/mode"
    void    setDisplayMode(const QString& mode);
    // Whether the one-time "we detected a TV — switch to the TV layout?" prompt has already been shown.
    bool    tvPromptDone();                   // key "display/tvPromptDone", default false
    void    setTvPromptDone(bool done);
    // Whether the one-time first-run onboarding choice (Restore-from-Drive vs. a new library) has been resolved.
    // Device-local (carved out at CloudSync::isDeviceLocalKey) so a restored/synced peer never re-triggers it.
    bool    onboardingDone();                 // key "onboarding/done", default false
    void    setOnboardingDone(bool done);

    // On-screen virtual gamepad (touch form factors). Tri-state override stored as "auto"|"on"|"off":
    // "auto" (default) shows it only in the Mobile form factor, "on" always, "off" never. Opacity is 0..100
    // (default 45). virtualPadEnabled() is the ONE visibility resolver the emulator uses (RetroView::
    // virtualPadShouldShow() delegates to it); "auto" resolves against the FormFactor authority, not the raw
    // display/mode string.
    QString virtualPad();                     // key "emu/virtualPad", default "auto"
    void    setVirtualPad(const QString& mode);
    bool    virtualPadEnabled();              // "on" || ("auto" && FormFactor::mode()==Mobile)
    int     virtualPadOpacity();              // key "emu/virtualPadOpacity", 0..100, default 45
    void    setVirtualPadOpacity(int pct);

    // Check GitHub for a newer app release on startup (default on). The check is silent unless one is found.
    bool checkUpdatesOnStartup();
    void setCheckUpdatesOnStartup(bool on);

    // FOLLOWING A SERIES (issue #155). How often the background pass asks each followed series' source what
    // children it has now, in HOURS — one of follow::intervalChoicesHours() (6 / 12 / 24 / 168), or 0 for
    // MANUAL, where nothing runs until "Check now" is pressed. Default daily.
    //
    // Both keys live under "following/" and are ORDINARY SYNCED PREFERENCES that ride the settings bundle: how
    // often to check and whether to check on a metered link are choices about the user, and someone who set
    // "weekly" on the TV means it on the phone too. The prefix is deliberately NOT "follow/", which
    // CloudSync::isPerItemStoreKey claims for the per-item follow marks — "following/" does not start with
    // "follow/" (the slash is load-bearing), and probe_cloudmerge pins that the two are classified apart.
    int  followIntervalHours();
    void setFollowIntervalHours(int hours);

    // Whether the scheduled pass may run on a METERED connection. Off by default, per the issue: a background
    // refresh over somebody's phone tethering is exactly the thing a polite feature does not do. "Check now"
    // is a deliberate press and is never gated on it.
    bool followOnMetered();
    void setFollowOnMetered(bool on);

    // The local UI-test/automation channel (Settings ▸ Debug): lets a test agent drive navigation and take
    // screenshots without the window needing focus (see core/UiTestServer). Default off.
    bool uiTestChannel();
    void setUiTestChannel(bool on);

    // The local remote-control HTTP server (Settings ▸ General ▸ Remote control, issue #76): an off-by-default
    // tiny control surface (play/pause/seek/navigate) so a phone browser on the same LAN can drive the app.
    // Bound to all interfaces ONLY when this is on, and CONTROL-only (no filesystem, no eval). Default OFF.
    bool remoteControlEnabled();                 // key "remote/enabled", default false
    void setRemoteControlEnabled(bool on);
    int  remoteControlPort();                    // key "remote/port", default 8090 (clamped 1..65535 on write)
    void setRemoteControlPort(int port);

    // Root of the local ROM library, organized RetroBat / ES-DE style as <root>/<system>/<rom files>.
    // Empty => the default (<data>/roms). Settable to anywhere on the system in General settings.
    QString romsFolder();          // resolved path (never empty)
    void setRomsFolder(const QString& path);

    // Root of the local VIDEO library (movies + TV), scanned by LocalLibrary. Empty stored value =>
    // the default (<data>/library). Device-local (never synced): each machine points at its own disk.
    QString libraryFolder();       // resolved path (never empty)
    void setLibraryFolder(const QString& path);

    // Root of the local MUSIC library (issue #74), scanned by MusicLibrary into Artists -> Albums -> Tracks.
    // Empty stored value => the default (<data>/musiclibrary). Device-local (never synced), like the two below.
    //
    // The default is NOT <data>/music, which is already taken: BackgroundMusic::musicDir() puts the UI's
    // ambient loops there. Sharing it would make a fresh install index the chrome's own background tracks as
    // the user's record collection the first time the music library scanned — a library nobody put there,
    // full of files they cannot account for.
    QString musicFolder();         // resolved path (never empty)
    void setMusicFolder(const QString& path);

    // Root of the local AUDIOBOOK library (issue #139), scanned by AudiobookLibrary into
    // Authors / Narrators / Series -> books -> their files. Empty stored value => the default
    // (<data>/audiobooks), which does not exist on a fresh install and is what keeps the Audiobooks
    // category off until somebody points this somewhere real (AudiobookLibrary::hasLibrary).
    //
    // SEPARATE FROM musicFolder() ON PURPOSE, and this is the whole classification story. An .mp3 under
    // this root is an audiobook; the SAME FILE under the music root is music. #139 says so in as many
    // words, and it is the right call: every heuristic anybody has proposed — long files are books, files
    // with chapters are books, files with a COMPOSER tag are classical — is confidently wrong about
    // somebody's library and wrong SILENTLY, and the person it is wrong about has no way to say so. A
    // folder the user chose is a statement they made and can change.
    //
    // NESTING IS THE USER'S BUSINESS TOO. Nothing stops this from being inside the music root or the other
    // way round; a file under both is scanned by both, and shows up in both categories, which is the
    // literal reading of what the person configured. Refusing the overlap would be a rule they did not ask
    // for, applied to a folder they deliberately chose.
    QString audiobookFolder();     // resolved path (never empty)
    void setAudiobookFolder(const QString& path);

    // Root of the local READING library (issue #134) — books AND comics — scanned by BookLibrary into
    // Authors and Series -> books. Empty stored value => the default (<data>/books), which does not exist on
    // a fresh install and is what keeps the Books category off until somebody points this somewhere real
    // (BookLibrary::hasLibrary).
    //
    // ONE ROOT FOR BOTH, and the reason is that the ambiguity the other libraries have does not exist here.
    // An .mp3 is genuinely either music or an audiobook and only its owner can say which, which is why those
    // two need separate folders. A .cbz is an archive of page images with no text, an .epub is a spine of
    // XHTML: within a reading collection the FORMAT already answers "book or comic", so a second root would
    // ask the user a question the file has already answered. The classification that DOES need a person —
    // "is this pile of PDFs a library or just my tax returns" — is exactly the one this setting makes, and
    // it is made once.
    //
    // NESTING IS THE USER'S BUSINESS, as it is for the audiobook root: a folder under both this and the
    // music root is scanned by both and appears in both categories, which is the literal reading of what
    // was configured.
    QString readingFolder();       // resolved path (never empty)
    void setReadingFolder(const QString& path);

    // FILL IN A BOOK'S BLANKS FROM ONLINE METADATA (issue #134 increment 2). Key "reading/enrichOnline",
    // DEFAULT OFF — a reading library is somebody's own files and this is the one part of the feature that
    // leaves the machine, so it is asked for rather than assumed. On, a book with no author or no cover may
    // be looked up; a book that has both is never asked about, and with this off nothing is asked at all.
    // What it can do with an answer is bounded in code, not by intent: BookLibrary::acceptedFill drops every
    // field the file already carries, so local metadata always wins and a lookup can only fill a blank.
    bool booksEnrichOnline();      // key "reading/enrichOnline", default false
    void setBooksEnrichOnline(bool on);

    // PER-SERIES COMIC READING DIRECTION (issue #152). A comic archive's ComicInfo.xml states its direction
    // in <Manga> and that is the DEFAULT; this is the user's answer for a series, and it beats the document
    // outright — the same "user edits are above all" rule the whole reading library follows.
    //
    // The value is ComicInfo::Direction's persisted spelling, and it is an int here ON PURPOSE: Settings.h is
    // included by most of the app, and it should not drag the comic layer's headers in behind it for three
    // numbers. 0 = no override (the document decides), 1 = left to right, 2 = right to left. Setting 0
    // FORGETS the override rather than storing a third state, so a series the user has never had an opinion
    // about and one they changed their mind back on are the same thing.
    //
    // `seriesKey` is BookLibrary::seriesKeyFor / ComicName::seriesKey — the same folded key the shelf groups
    // a series by, so an override survives a re-spelling of the series' display name.
    int  comicDirectionOverride(const QString& seriesKey);
    void setComicDirectionOverride(const QString& seriesKey, int direction);

    // PER-SERIES COMIC DISPLAY OPTIONS (issue #154) — the reading mode (paged L2R / paged R2L / webtoon), the
    // page-split override, border crop, the colour filter and the webtoon thumbnail rail. One generic pair
    // rather than five typed ones, for the same reason the value above is an int: Settings.h is included by
    // most of the app and must not drag the comic layer's headers in behind it. The option NAMES are declared
    // once in comic/ReadingModes.h (ComicRead::Opt), which also reserves increment 2's — a name spelled
    // twice is a setting that is silently forgotten.
    //
    // 0 ALWAYS MEANS "the default" and setting it FORGETS the option, so "never had an opinion" and "changed
    // my mind back" are the same stored state and an untouched library stores nothing at all. `seriesKey` is
    // ComicRead::seriesKeyFor() — the document's series when it has one, the filename's otherwise.
    int  comicDisplayOption(const QString& seriesKey, const QString& option);
    void setComicDisplayOption(const QString& seriesKey, const QString& option, int value);

    // AD-HOC MULTI-VALUE SEPARATORS for artist and genre tags (issue #196), as a whitespace-separated list of
    // the separators themselves — the stored default is ";" and "; / feat." would be three of them. They are
    // used ONLY when the container gave one string; a repeated Vorbis field or a NUL-separated ID3v2.4 frame
    // is already structured and is never re-split (AudioTags.h has the rule). Album artist is never split at
    // all, whatever is in here.
    //
    // WHY THE DEFAULT IS ONE CHARACTER. A separator that is wrong is not merely a missed split: it SHREDS a
    // band name into two artists that both look real, and nobody notices until they go looking for a band
    // that is no longer there. A semicolon is the one candidate no act is named with, and it is what Mp3tag,
    // Picard, foobar2000 and Windows all write when they flatten a list into an ID3v2.3 string. "/" is left
    // out because of AC/DC — and Hall/Oates and He/rmit and every "Sunshine/Moonlight" title people paste
    // into an artist field. "feat." is left out because it is prose inside ONE credit, and splitting it
    // renames the primary artist's own record. Both are one edit away for a library that needs them, which
    // is the point of this being a setting at all.
    //
    // AN EXPLICITLY EMPTY VALUE MEANS NO AD-HOC SPLITTING (structured values only), which is why the default
    // applies to a key that was never set rather than to any empty string — "I cleared this on purpose" and
    // "I have never touched this" want opposite answers.
    QString musicTagSeparators();                        // key "music/tagSeparators"; unset => ";"
    void setMusicTagSeparators(const QString& list);
    QStringList musicTagSeparatorList();                 // the same value, tokenised on whitespace

    // WHICH COPY PLAYS when the same album is on this disk AND on a music server (issue #194). One stored
    // string, because "prefer a specific server" would otherwise need a second setting and a migration to
    // keep the two in step:
    //     "local"          the copy on this disk, when there is one          (the default, and what an
    //                      unset value means — it is the answer that works with no network)
    //     "server"         any music server, in the order they were added
    //     "<a server id>"  that server specifically
    // Anything unrecognised — including a server id from another device, since this key syncs and the servers
    // themselves are device-local — reads as "local" inside MusicId::pickAutoSource rather than as an error.
    QString musicPreferredSource();                      // key "music/preferredSource"; unset => "local"
    void setMusicPreferredSource(const QString& v);

    // Root of the local PHOTO library (issue #102), scanned by PhotoLibrary. Empty stored value =>
    // the default (<data>/photos). Device-local (never synced): each machine points at its own disk.
    QString photosFolder();        // resolved path (never empty)
    void setPhotosFolder(const QString& path);

    // The GLOBAL folder personal-TV-channel bumpers/idents come from (issue #179, increment 2). EMPTY by
    // default and empty means NO INTERSTITIALS — never an error, never a folder invented under the data
    // directory: a channel with no bumpers is the ordinary case and has to stay the quiet one. A channel may
    // name its own folder instead (channels::Channel::interstitialDir), which beats this one.
    // Device-local: it is a path on THIS machine's disk, like the ROMs and library folders.
    QString interstitialFolder();  // key "interstitials/folder"; unset => "" (off)
    void setInterstitialFolder(const QString& path);

    // Resolve local-library movie ids online: search installed movie-catalog addons per movie and record the
    // matched catalog ids (CatalogResolver). Off => the library is indexed by NFO ids only. Default on.
    bool resolveOnline();          // key "library/resolveOnline", default true
    void setResolveOnline(bool on);

    // 1G1R-style region collapsing (issue #50): when on, RomLibrary::scan() groups same-title ROM variants
    // that differ only by region/revision tag and surfaces ONE entry per group (best by region preference,
    // then highest revision); the losers are hidden from the grid but stay reachable from the game's detail
    // view ("Other versions"). OFF by default so a curated one-file-per-game set is untouched.
    bool collapseRegionalDuplicates();          // key "library/collapseRegions", default false
    void setCollapseRegionalDuplicates(bool on);

    // Menu background music (RetroBat-style): play tracks dropped in <data>/music while browsing. On by
    // default at a modest volume.
    bool bgmEnabled();
    void setBgmEnabled(bool on);
    int  bgmVolume();                  // 0..100
    void setBgmVolume(int pct);

    // Video hover previews (issue #55): the theme `video` element plays a game's scraped/gamelist snap on
    // hover-dwell instead of only Ken-Burns-panning a still. ON by default (the previews ARE the intended
    // experience) but MUTED by default — the snap volume defaults to 0, so a fresh install plays previews
    // silently and never fights (ducks) the background music. When enabled is false the element shows exactly
    // today's Ken-Burns still and never starts the clip. Keys "video/previewsEnabled" + "video/snapVolume".
    bool videoPreviewsEnabled();       // key "video/previewsEnabled", default true
    void setVideoPreviewsEnabled(bool on);
    int  videoSnapVolume();            // key "video/snapVolume", default 0 (muted); clamped 0..100 on write
    void setVideoSnapVolume(int pct);

    // Attract mode (idle screensaver, issue #54): after this many idle minutes on a menu screen, fade into a
    // full-screen slideshow of library art. OFF by default (a screensaver a user did not ask for is a
    // surprise); default timeout 10 minutes. The minutes are clamped to a sane 1..120 on write.
    bool attractEnabled();             // key "attract/enabled", default false
    void setAttractEnabled(bool on);
    int  attractTimeoutMinutes();      // key "attract/timeoutMin", default 10 (clamped 1..120)
    void setAttractTimeoutMinutes(int minutes);

    // Retro video filter applied over the emulator image: "off" (default) | "scanlines" | "crt" | "lcd".
    QString videoFilter();
    void setVideoFilter(const QString& id);

    // GLOBAL DEFAULT slang-shader preset (issue #99): a ShaderPreset id ("off" | "crt" | "lcd-grid" | … | a
    // "custom:<path>"). On FIRST read (before the user ever picks one) this SEEDS from the legacy videoFilter()
    // via ShaderPreset::presetIdForLegacyFilter, so an upgrading user's Scanlines/CRT/LCD choice carries over.
    // The per-system / per-game overrides live in ShaderPresetStore; this is the fall-through default they layer
    // onto (ShaderPreset::resolvePreset). Nothing renders from it yet — the render slice is later.
    QString shaderPreset();
    void setShaderPreset(const QString& id);

    // Hardware video decoding for the built-in mpv player (issue #67): "off" | "auto" (default) | "on".
    // Read once at player creation and mapped to mpv's hwdec option via HwDecode::mpvOption. Auto is a list of
    // COPY-BACK decoders (HwDecode::autoCopyBackList) with a software fallback: no decoder it can pick hands
    // the render context an interop texture. It used to say "auto-safe" here and call that copy-back, which
    // measurement disproved — on NVIDIA "auto-safe" is nvdec DIRECT (issue #229). Key "video/hwdec".
    QString hwDecode();
    void setHwDecode(const QString& mode);

    // Seek previews (issue #85): how much disk the thumbnail sprite sheets may occupy, in MEGABYTES, under
    // "previews/cacheMb". This one number is the whole feature's switch as well as its bound — ZERO means
    // previews are off and nothing is ever generated, which is why the setting is a size and not a size plus
    // a toggle: "how much may this cost me" is the question a user actually has about a background job that
    // writes to their disk, and two controls that can disagree ("on, 0 MB") would be two ways to say no.
    // Default 512 MB, which at the shipped 10 s / 320x180 sheet is roughly a hundred feature films.
    // The bound is enforced by LRU eviction on the generator's own thread (Trickplay::planEviction).
    // DEVICE-LOCAL (issue #301): the whole "previews/" prefix is in CloudSync's carve-out, because the thing
    // this number bounds is device-local by nature - sheets generated here, in this machine's cache
    // directory, never shared. A handheld and a desktop want different answers, so it must not sync.
    int  previewCacheMb();
    void setPreviewCacheMb(int mb);

    // Seek previews, second trigger (issue #302): may the app also sweep the LOCAL LIBRARY on genuine idle,
    // so a film has its strip the FIRST time it is watched rather than the second? Stored under
    // "previews/idleScan", and device-local by inheritance — the whole "previews/" prefix is in CloudSync's
    // carve-out (#301) — which is the right shape here too: whether a sweep is polite is a fact about this
    // machine, not about the account. A handheld and a desktop want different answers.
    //
    // DEFAULT OFF, and that is a decision rather than caution. #85's trigger is a CONSEQUENCE of something
    // the user did — they opened that file — so spending a few minutes of CPU on it afterwards is work they
    // implicitly asked for. A library sweep is not: it is the app deciding, on its own, to spend hours of
    // decode and up to the whole cache bound in writes on films nobody has opened. Three things we cannot
    // detect make that unsafe to assume for somebody:
    //
    //   * whether the MACHINE is busy. The idle input is app-scoped (the same input funnel attract mode uses),
    //     so EverythingBox minimised behind a game, a compile or a video call looks perfectly idle to us;
    //   * thermal and fan headroom. On a passively-cooled handheld an hour of continuous decode is audible
    //     and warm even when it is plugged in;
    //   * whether the disk wants it. The bound is a size, not a write budget, and an SSD in a cheap TV box is
    //     not the drive the number was chosen against.
    //
    // Where we CAN tell — power — the rule is absolute: never on battery, and never when we cannot tell
    // (TrickplayIdle::evaluate). Given all that, on by default would be the app quietly deciding on the
    // user's behalf; off by default with a plainly-worded switch is the same feature, chosen.
    bool previewIdleScan();
    void setPreviewIdleScan(bool on);

    // Refresh-rate matching, Tier 1 (issue #70): reduce judder by locking video to the display clock via mpv's
    // video-sync=display-resync (RefreshSync::videoSyncFor). Read at player creation and re-applied live on
    // change. The stored value is a plain bool under "video/refreshSync"; when ABSENT the default is form-factor
    // dependent (ON for desktop/TV, OFF for a mobile handheld), resolved against the FormFactor authority the
    // same way virtualPadEnabled() resolves its "auto".
    bool videoRefreshSync();
    void setVideoRefreshSync(bool on);

    // HDR output handling for the built-in mpv player (issue #68): a two-way switch mapped to mpv's
    // tone-mapping / hdr-compute-peak / target-colorspace-hint options via HdrOutput::optionsFor. Read at player
    // creation and re-applied live on change. Stored as the id string under "video/hdr" ("tonemap" default,
    // "passthrough"); hdrOutput() returns the resolved HdrOutput::Mode (a hand-edited/absent value degrades to
    // the ToneMapSdr default). A normal synced setting like the refresh-sync toggle above (video/* is not in
    // CloudSync's device-local carve-out), so a chosen mode follows the account — passthrough simply falls back
    // to tone-mapping on a device whose display cannot show HDR, so syncing it is safe.
    HdrOutput::Mode hdrOutput();
    void setHdrOutput(const QString& modeId);

    QString netplayRelay();                      // "host:port" of the online-netplay relay (empty = not set)
    void setNetplayRelay(const QString& hostPort);

    // Watch together (issue #86): what a HOST's room does when a guest stalls. "wait" (the default) pauses
    // everyone until they catch up; "keepgoing" carries on and just shows who is behind. Stored as the id
    // WatchTogether::policyId spells, under the "watchtogether/" group named in WatchTogether.h; a hand-edited
    // or absent value degrades to "wait". Deliberately a SETTING and not a rule: which of the two a watch
    // party wants is a social answer, not a technical one. Watch-together rooms use netplayRelay() above --
    // one relay, one address to set, because a user reading a code off a TV cannot be asked to know which
    // kind of room it belongs to.
    QString watchTogetherPolicy();
    void setWatchTogetherPolicy(const QString& policyId);

    // One-shot: this install has already swept core-written save files that were left loose in the app
    // directory into saves/ (SaveMeta::sweepStrays). Stored under "device/" so it does NOT sync — the flag
    // describes THIS machine's directory, and another device's strays still need their own pass.
    bool savesStraysSwept();
    void setSavesStraysSwept(bool done);

    // Draw bezel / border artwork around the emulator picture (PNG in <data>/bezels). Default off.
    bool bezelEnabled();
    void setBezelEnabled(bool on);

    // Save states (#93). Auto-increment: quick-save (F2) writes to the NEXT FREE user slot instead of
    // overwriting the current one, turning quick-saves into a history. Default off — quick-save stays a
    // single fast slot unless the user opts into the history. Key "emu/stateAutoIncrement".
    bool stateAutoIncrement();
    void setStateAutoIncrement(bool on);

    // Save-on-exit / resume mode. On emulator close an automatic state is written to a RESERVED slot (distinct
    // from every numbered user slot), and on relaunch this decides what happens:
    //   ResumePrompt (default) — offer "Resume where you left off?" over the game;
    //   ResumeSilent           — restore it silently;
    //   ResumeOff              — never write the auto-state and never resume.
    // Default Prompt: the least-surprising behaviour is to ask before silently rewinding the player's session.
    // Key "emu/resumeMode".
    enum ResumeMode { ResumeOff = 0, ResumePrompt = 1, ResumeSilent = 2 };
    int  resumeMode();
    void setResumeMode(int mode);

    // Hardcore RetroAchievements (issue #94). Opt-in, DEFAULT OFF — softcore stays first-class. When on, a
    // hardcore achievement session disables save states, rewind, fast-forward and cheats (the ONE policy lives
    // in core/Hardcore.h); enabling it resets the current achievement session per the site rule, and the UI
    // asks for consent first. A plain synced user preference: the RA account is one login, and "I play
    // hardcore" is a property of that account, so it follows the user across devices — the "ra/" group is NOT
    // in CloudSync's device-local or per-item carve-outs, so it rides the normal settings bundle like any
    // preference (probe_cloudmerge pins that classification). Key "ra/hardcore".
    bool hardcoreAchievements();
    void setHardcoreAchievements(bool on);

    // Keep scraped game data: persist freshly-scraped metadata + art back into the ROM system's gamelist.xml
    // + ./images ./videos (EmulationStation / RetroBat layout), so it's reused on the folder next time and by
    // other ES-based frontends. Reading an existing gamelist happens regardless; this controls WRITE-back.
    bool keepScrapedData();
    void setKeepScrapedData(bool on);

    // When on (default), a downloaded online game is saved into the ROMs folder as a local ROM, so the next
    // play finds it locally and does not re-download (the transient url-hash cache misses every time because
    // the debrid/source url rotates per play).
    bool keepDownloadsInRoms();
    void setKeepDownloadsInRoms(bool on);

    // Auto-apply a sidecar ROM patch (Game.ips / .bps / .ups beside Game.sfc) at launch, RetroArch's
    // convention. The patch is applied to a derived cache file and that is launched; the original ROM on
    // disk is never modified (see RomPatch). Default true — a patch sitting beside a ROM is a deliberate
    // act, so honouring it is the least-surprising default; the toggle lets a user boot the original
    // without moving the patch file away. Key "roms/autoApplyPatches".
    bool autoApplyRomPatches();
    void setAutoApplyRomPatches(bool on);

    // Auto-install official Sony game updates for a PS3 title before launching it in RPCS3. When on (default),
    // the launch flow fetches the title's update feed, downloads any newer PKG(s) and has RPCS3 install them
    // into its portable dev_hdd0 before boot; a failure never blocks the launch (the unpatched game still runs).
    // Key "ps3/autoUpdate".
    bool ps3AutoUpdate();
    void setPs3AutoUpdate(bool on);

    // Install a game's own update and DLC packages into the target emulator before launching it (issue #189):
    // whatever sits in the `updates/` and `dlc/` folders BESIDE the game, installed per that emulator's recipe
    // (ContentRecipe.h). Default on — a game whose DLC is silently missing looks broken, and nothing here can
    // block a launch: every failure is reported and the base game still boots. Off is the master switch for
    // someone who drives their emulators' content stores entirely by hand. Key "content/autoInstall".
    bool installGameContent();
    void setInstallGameContent(bool on);

    // Verify ROMs against user-supplied No-Intro / Redump DAT files dropped into <data>/dats/ (issue #97). When
    // on, the detail view lazily hashes a game's PAYLOAD and stamps it Verified / Bad / Unknown. Passive — never
    // a nag, Unknown is neutral — so default true; the toggle is for users who never intend to add DATs.
    // Key "roms/verifyDats".
    bool verifyRoms();
    void setVerifyRoms(bool on);

    // Per-system input profile: the system id whose scoped bindings the remap dialog is editing ("" = global
    // default). Not a user-facing "setting" so much as the remap dialog's current scope, persisted for reuse.
    QString inputScope();
    void setInputScope(const QString& systemId);

    // External-player handoff (Stremio-style): which player takes over from the built-in libmpv one, as a
    // stable id string — "builtin" (default) | "vlc" | "mpc" | "custom" | "android". Any unknown/empty value
    // is treated as "builtin" by ExternalPlayer::configuredKind(). externalPlayerPath is the user-picked exe
    // for the "custom" kind (ignored otherwise).
    QString externalPlayer();                       // key "player/external", default "builtin"
    void    setExternalPlayer(const QString& id);
    QString externalPlayerPath();                   // key "player/externalPath"; custom-kind exe
    void    setExternalPlayerPath(const QString& path);

    QString coreFor(const QString& systemId);                       // "" if the user hasn't chosen one
    void setCoreFor(const QString& systemId, const QString& core);

    // Per-system STANDALONE-emulator default (Unified Emulation Picker Task 2), mirroring coreFor exactly: the
    // emulator id a standalone system launches on when a game carries no per-game override. Keyed
    // "emulators/<systemId>". Empty return = inherit the system's built-in default (sys->externalEmulator) —
    // the same empty-is-default posture coreFor has, so a system with no choice is byte-identical to today.
    // A per-system PREFERENCE (a property of the user, not the machine), so it rides the synced settings bundle
    // like cores/<id> and backends/<id> — NOT device-local. NOTE the leaf differs from the two EXACT device-local
    // "emulators/root" / "emulators/fullscreen" keys, which are matched as leaves (never as an "emulators/"
    // prefix), so a real SystemCatalog id can never collide with the machine-local install-root/display keys.
    QString emulatorFor(const QString& systemId);                   // "" if the user hasn't chosen one
    void setEmulatorFor(const QString& systemId, const QString& emulatorId);

    // Per-system emulation backend (RetroPark Slice 2a), mirroring coreFor. A system with no explicit choice
    // inherits the global default (defaultBackend()), which is itself Libretro until the user changes it — so
    // until a game/system is opted into RetroPark every launch resolves to Libretro, byte-identical to today.
    // Keyed "backends/<systemId>"; the global default at "backends/_default". An unknown stored spelling reads
    // back as Libretro (backendFromString's collapse), so a stale value can never break a launch.
    EmuBackend backendFor(const QString& systemId);
    void setBackendFor(const QString& systemId, EmuBackend backend);
    EmuBackend defaultBackend();                                    // global default; Libretro when unset
    void setDefaultBackend(EmuBackend backend);

    // Global host graphics API for RetroPark DRIVEN cores (the CPU-frame refcore / FCEUmm-shim path). Stored as a
    // stable id string — "d3d11" (default) | "opengl". D3D11 is the proven path Slice 2a/2b ship, so an unset or
    // unknown value reads back as "d3d11" and every driven launch is byte-identical to today; "opengl" opts the
    // driven host runtime onto RetroPark's additive OpenGL compositor (RP_GFX_OPENGL). Does NOT affect presenting
    // cores (Dolphin/GC), which always run on Vulkan. Key "retropark/driven_backend".
    QString retroParkDrivenBackend();
    void setRetroParkDrivenBackend(const QString& id);

    // MS-DOS MIDI device (issue #191). The stored value is a device ID declared by the msdos launch recipe's
    // `midi` block — "gm" (a General MIDI soundfont) or "mt32" (Roland MT-32 ROMs) — and "" means "let the
    // core decide", which is the default and is byte-for-byte the pre-#191 launch. Deliberately NOT validated
    // against a fixed list here: which devices exist is DATA (a recipe file), and the launcher simply finds no
    // device for an id the recipe does not declare and seeds nothing. The ASSETS the chosen device needs are
    // the user's to supply and are never downloaded; a missing one leaves the game on its default audio with a
    // message naming the file and the folder. Key "dos/midi_device".
    QString dosMidiDevice();
    void setDosMidiDevice(const QString& id);

    // Per-core option overrides (resolution, BIOS, region, ...). "" means "use the core's default".
    QString optionValue(const QString& core, const QString& key);
    void setOptionValue(const QString& core, const QString& key, const QString& value);

    // Cached raw core-options JSON for a core, keyed "optdesc/<core>" (namespaced by core like opt/<core>/*).
    // Written by RetroParkView after a game's first successful load_content, because late-declaring cores
    // (fceumm-class) only expose their option DESCRIPTORS once content is loaded. The global options editor
    // (which runs pre-launch) reads this cache to show a core's options after the first play. "" when none
    // has been cached yet.
    QString coreOptionDescriptors(const QString& core);
    void setCoreOptionDescriptors(const QString& core, const QString& json);

    // Gamepad button remapping, per player port: (port, RetroPad button id) -> binding code (see Gamepad).
    // Returns the supplied default if the user hasn't bound it.
    int padBinding(int port, int retroId, int defaultCode);
    void setPadBinding(int port, int retroId, int code);

    // Keyboard remapping, per player port: (port, RetroPad button id) -> Qt key code (see Keymap).
    int keyBinding(int port, int retroId, int defaultKey);
    void setKeyBinding(int port, int retroId, int qtKey);

    // ---- Per-game overrides (issue #95). See Settings.cpp for the layering and the no-leak rails. --------
    // Hash a game's stable identity (PlayStats::identity) to a compact, ini-safe token; "" for an empty id.
    QString gameToken(const QString& gameIdentity);

    // The active per-game INPUT layer (highest binding-precedence). Set at launch, cleared at teardown.
    QString inputGameScope();                  // key "input/gameScope"; "" = no game layer active
    void    setInputGameScope(const QString& gameToken);

    // Explicit per-game binding writes (the remap dialog's "This game" scope), targeting the game keyspace
    // only — never the global or per-system one. `token` is a gameToken(). Reset REMOVES the row.
    bool gamePadHasBinding(const QString& token, int port, int retroId);
    void setGamePadBinding(const QString& token, int port, int retroId, int code);
    void clearGamePadBinding(const QString& token, int port, int retroId);
    bool gameKeyHasBinding(const QString& token, int port, int retroId);
    void setGameKeyBinding(const QString& token, int port, int retroId, int qtKey);
    void clearGameKeyBinding(const QString& token, int port, int retroId);

    // Per-game CORE-OPTION deltas, a keyspace separate from the per-core baseline (optionValue). Presence is
    // the override; absence inherits the baseline; a reset REMOVES the key. gameOptionDelta returns every
    // override for (token, core) as key -> value.
    bool    gameHasOption(const QString& token, const QString& core, const QString& key);
    QString gameOptionValue(const QString& token, const QString& core, const QString& key);
    void    setGameOptionValue(const QString& token, const QString& core, const QString& key, const QString& value);
    void    clearGameOptionValue(const QString& token, const QString& core, const QString& key);
    QMap<QString, QString> gameOptionDelta(const QString& token, const QString& core);

    // Runahead frames (issue #100). N core runs are speculated per displayed frame so the response to a button
    // appears the frame it was pressed; 0 = off, 3 = the cap. The GLOBAL value is only a starting point — the
    // right N *is* a property of the game (its own internal input lag), so the per-game override below is the
    // one that matters, and it lives in its own keyspace ("runaheadgame/<token>") exactly like the #95 deltas:
    // presence is the override, absence inherits the global default, and a reset REMOVES the key so it can
    // never be mistaken for a deliberate 0.
    int  runaheadFrames();                    // key "emu/runahead"; default 0 (off), clamped 0..3
    void setRunaheadFrames(int frames);
    bool gameHasRunaheadFrames(const QString& token);
    int  gameRunaheadFrames(const QString& token);   // the game's own N (0 when it has none)
    void setGameRunaheadFrames(const QString& token, int frames);
    void clearGameRunaheadFrames(const QString& token);

    // Turbo / autofire: which RetroPad buttons auto-fire while held, per player port, plus the toggle
    // speed expressed as the half-cycle length in frames (smaller = faster).
    bool turboButton(int port, int retroId);
    void setTurboButton(int port, int retroId, bool on);
    int  turboHalfPeriod();            // frames the button stays "on" (and "off") each cycle; default 3
    void setTurboHalfPeriod(int frames);
}
