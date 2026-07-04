// Persistent user settings (portable INI next to the executable). For now: the chosen core per system.
#pragma once
#include <QString>

namespace Settings
{
    // General playback: auto-show subtitles on every video, and the preferred subtitle language (an ISO
    // 639 code like "eng"; empty = no preference / first available).
    bool subtitlesOnByDefault();
    void setSubtitlesOnByDefault(bool on);
    QString subtitleLanguage();
    void setSubtitleLanguage(const QString& code);

    // OpenSubtitles.com credentials for auto-downloading subtitles when a video has none in the preferred
    // language. The REST API needs an app API key (register once, free) for search, plus the user's account
    // (login is required to download). All three empty => the feature is dormant. Stored in the local INI.
    QString openSubApiKey();
    void setOpenSubApiKey(const QString& key);
    QString openSubUsername();
    void setOpenSubUsername(const QString& user);
    QString openSubPassword();
    void setOpenSubPassword(const QString& pass);

    // Open the app maximized to full screen on launch (default off — a normal resizable window).
    bool startFullscreen();
    void setStartFullscreen(bool on);

    // Check GitHub for a newer app release on startup (default on). The check is silent unless one is found.
    bool checkUpdatesOnStartup();
    void setCheckUpdatesOnStartup(bool on);

    // Root of the local ROM library, organized RetroBat / ES-DE style as <root>/<system>/<rom files>.
    // Empty => the default (<data>/roms). Settable to anywhere on the system in General settings.
    QString romsFolder();          // resolved path (never empty)
    void setRomsFolder(const QString& path);

    // Menu background music (RetroBat-style): play tracks dropped in <data>/music while browsing. On by
    // default at a modest volume.
    bool bgmEnabled();
    void setBgmEnabled(bool on);
    int  bgmVolume();                  // 0..100
    void setBgmVolume(int pct);

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
