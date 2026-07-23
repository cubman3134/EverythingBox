// Persistent user settings (portable INI next to the executable). For now: the chosen core per system.
#pragma once
#include <QString>

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

    // Auto-play the next TV episode when one finishes (default on).
    bool autoplayNextEpisode();
    void setAutoplayNextEpisode(bool on);

    // Parental PIN (stored as a salted hash, never in the clear). Gates leaving a restricted "kids" profile.
    bool hasParentalPin();
    void setParentalPin(const QString& pin); // empty pin clears it
    bool checkParentalPin(const QString& pin);

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

    // Root of the local ROM library, organized RetroBat / ES-DE style as <root>/<system>/<rom files>.
    // Empty => the default (<data>/roms). Settable to anywhere on the system in General settings.
    QString romsFolder();          // resolved path (never empty)
    void setRomsFolder(const QString& path);

    // Root of the local VIDEO library (movies + TV), scanned by LocalLibrary. Empty stored value =>
    // the default (<data>/library). Device-local (never synced): each machine points at its own disk.
    QString libraryFolder();       // resolved path (never empty)
    void setLibraryFolder(const QString& path);

    // Menu background music (RetroBat-style): play tracks dropped in <data>/music while browsing. On by
    // default at a modest volume.
    bool bgmEnabled();
    void setBgmEnabled(bool on);
    int  bgmVolume();                  // 0..100
    void setBgmVolume(int pct);

    // Retro video filter applied over the emulator image: "off" (default) | "scanlines" | "crt" | "lcd".
    QString videoFilter();
    void setVideoFilter(const QString& id);

    QString netplayRelay();                      // "host:port" of the online-netplay relay (empty = not set)
    void setNetplayRelay(const QString& hostPort);

    // Draw bezel / border artwork around the emulator picture (PNG in <data>/bezels). Default off.
    bool bezelEnabled();
    void setBezelEnabled(bool on);

    // Keep scraped game data: persist freshly-scraped metadata + art back into the ROM system's gamelist.xml
    // + ./images ./videos (EmulationStation / RetroBat layout), so it's reused on the folder next time and by
    // other ES-based frontends. Reading an existing gamelist happens regardless; this controls WRITE-back.
    bool keepScrapedData();
    void setKeepScrapedData(bool on);

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

    // Turbo / autofire: which RetroPad buttons auto-fire while held, per player port, plus the toggle
    // speed expressed as the half-cycle length in frames (smaller = faster).
    bool turboButton(int port, int retroId);
    void setTurboButton(int port, int retroId, bool on);
    int  turboHalfPeriod();            // frames the button stays "on" (and "off") each cycle; default 3
    void setTurboHalfPeriod(int frames);
}
