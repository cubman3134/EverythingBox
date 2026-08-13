// Persistent user settings (portable INI next to the executable). For now: the chosen core per system.
#pragma once
#include <QString>
#include <QMap>
#include "../video/SubtitleStyle.h"   // Settings::subtitleStyle() returns the pure Style the player applies (#71)
#include "../video/AudioOutput.h"     // Settings::audioOutput() returns the pure Output the player applies (#69)
#include "../video/HdrOutput.h"       // Settings::hdrOutput() returns the pure HDR Mode the player applies (#68)
#include "../ebook/ReaderTypography.h" // Settings::readerTypography() returns the pure typography the reader applies (#135)
#include "EmuBackend.h"                // Settings::backendFor() returns the per-system emulation backend (RetroPark Slice 2a)

namespace Settings
{
    // Stable per-install device identity (multi-device sync). Minted ONCE as a UUID on first read and
    // persisted at key "device/id"; every later call returns the same string. Write-once: a non-empty stored
    // value is never overwritten, so concurrent/repeated reads can never mint a second id. This id is
    // device-LOCAL — the sync carve-out (T4) excludes "device/*" from the synced settings bundle, and it
    // namespaces the per-device accumulators (T3) so two devices never double-count. Never empty on return.
    QString deviceId();                       // key "device/id"; minted once, stable, device-local

    // General playback: auto-show subtitles on every video, and the preferred subtitle language (an ISO
    // 639 code like "eng"; empty = no preference / first available).
    bool subtitlesOnByDefault();
    void setSubtitlesOnByDefault(bool on);
    QString subtitleLanguage();
    void setSubtitleLanguage(const QString& code);

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

    // Root of the local PHOTO library (issue #102), scanned by PhotoLibrary. Empty stored value =>
    // the default (<data>/photos). Device-local (never synced): each machine points at its own disk.
    QString photosFolder();        // resolved path (never empty)
    void setPhotosFolder(const QString& path);

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
    // Read once at player creation and mapped to mpv's hwdec option via HwDecode::mpvOption. Auto =
    // "auto-safe" (copy-back preferred, software fallback) is deliberately the default — it sidesteps the
    // D3D11VA 10-bit-HEVC corruption that made a blanket "no" the original hard-coded choice. Key "video/hwdec".
    QString hwDecode();
    void setHwDecode(const QString& mode);

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

    // Auto-apply a sidecar ROM patch (Game.ips / .bps / .ups beside Game.sfc) at launch, RetroArch's
    // convention. The patch is applied to a derived cache file and that is launched; the original ROM on
    // disk is never modified (see RomPatch). Default true — a patch sitting beside a ROM is a deliberate
    // act, so honouring it is the least-surprising default; the toggle lets a user boot the original
    // without moving the patch file away. Key "roms/autoApplyPatches".
    bool autoApplyRomPatches();
    void setAutoApplyRomPatches(bool on);

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

    // Per-system emulation backend (RetroPark Slice 2a), mirroring coreFor. A system with no explicit choice
    // inherits the global default (defaultBackend()), which is itself Libretro until the user changes it — so
    // until a game/system is opted into RetroPark every launch resolves to Libretro, byte-identical to today.
    // Keyed "backends/<systemId>"; the global default at "backends/_default". An unknown stored spelling reads
    // back as Libretro (backendFromString's collapse), so a stale value can never break a launch.
    EmuBackend backendFor(const QString& systemId);
    void setBackendFor(const QString& systemId, EmuBackend backend);
    EmuBackend defaultBackend();                                    // global default; Libretro when unset
    void setDefaultBackend(EmuBackend backend);

    // Per-core option overrides (resolution, BIOS, region, ...). "" means "use the core's default".
    QString optionValue(const QString& core, const QString& key);
    void setOptionValue(const QString& core, const QString& key, const QString& value);

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

    // Turbo / autofire: which RetroPad buttons auto-fire while held, per player port, plus the toggle
    // speed expressed as the half-cycle length in frames (smaller = faster).
    bool turboButton(int port, int retroId);
    void setTurboButton(int port, int retroId, bool on);
    int  turboHalfPeriod();            // frames the button stays "on" (and "off") each cycle; default 3
    void setTurboHalfPeriod(int frames);
}
