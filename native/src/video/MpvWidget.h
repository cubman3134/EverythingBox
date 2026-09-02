// libmpv embedded in a Qt OpenGL surface (mpv "render API"). Plays everything mpv/ffmpeg can decode
// (MKV/HEVC/AV1/AC3/DTS/...), streams large files, hardware-decoded - in a native window, no engine bridge.
// Structure follows the canonical libmpv `qt_opengl` example.
#pragma once
#include "../media/LoadWatchdog.h"   // #213: which loads are watched, and what a deadline means (pure)
#include <QElapsedTimer>
#include <QString>
#include <QVector>
#include "../core/MediaSegments.h"   // MediaSegments::Chapter — declared in core so it needs no libmpv
#include "../core/StreamHeaders.h"   // per-stream proxyHeaders (QtCore-only, so it costs the probes nothing)
#include "MpvLogThrottle.h"          // #231: the burst counter mpv's own log is written through (pure)
#include <mpv/client.h>
#ifdef Q_OS_IOS
// iOS: OpenGL ES context creation fails in the simulator (and EAGL is deprecated on device), so render
// through mpv's SOFTWARE render API into a QImage instead — same approach as theme2/MpvPreview. The
// public API is identical; only the frame path differs.
#include <QWidget>
#include <QImage>
#include <mpv/render.h>
using MpvWidgetBase = QWidget;
#else
#include <QOpenGLWidget>
#include <mpv/render_gl.h>
using MpvWidgetBase = QOpenGLWidget;
#endif

class QLabel;
class QTimer;

class MpvWidget : public MpvWidgetBase
{
    Q_OBJECT
public:
    explicit MpvWidget(QWidget* parent = nullptr);
    ~MpvWidget() override;

    // local path or http(s)/stream URL. `headers` is this stream's behaviorHints.proxyHeaders.request; it is
    // applied per-load and, being applied unconditionally, CLEARS the previous stream's headers when empty —
    // callers never have to remember to reset anything (and a caller that passes nothing gets a clean load).
    //
    // `displayTitle` (issue #202) is what the CALLER knows this track is called — the queue's own display
    // title. It exists because mpv's `media-title` falls back to the url when a stream carries no metadata,
    // and a Subsonic stream url is a credential, so the audio-only overlay was putting a live token on
    // screen. DEFAULTED, so none of this widget's callers had to change: a caller with nothing to say passes
    // nothing and clears the previous track's title, which is the behaviour a replace-load must have anyway.
    void play(const QString& url, const StreamHeaders::Headers& headers = {},
              const QString& displayTitle = QString());
    // The same title, for the two advances that DO NOT reload — a gapless boundary inside mpv's own playlist
    // and a crossfade promotion. Both change the track with no play() call, so without this the overlay would
    // keep naming the track before it.
    void setNowPlayingTitle(const QString& displayTitle);
    // Gapless one-ahead feed (issue #141): APPEND a track to mpv's own playlist instead of replacing the
    // current file, so mpv's decoder crosses into it without a gap. Used only for the audio queue when gapless
    // is on; the audio queue carries no per-stream headers (local files), so `headers` is normally empty.
    void appendFile(const QString& url, const StreamHeaders::Headers& headers = {});
    // Set mpv's `gapless-audio` for this context (issue #141). Only ever called with `on` = true, when an audio
    // queue starts gapless: "weak" is the honest value — mpv keeps the audio output continuous only when the
    // adjacent tracks share a format, and reinitialises (a normal, correct gap) when they genuinely differ. With
    // gapless off the app never builds an mpv playlist (each track is a replace-load), so this is never set and
    // mpv's default governs — the off path sets no new option.
    void setGaplessAudio(bool on);
    // Issue #193: drop every entry of mpv's OWN playlist strictly after the one playing. The repair for a
    // queue edit that landed on an entry the gapless feed had already handed over — mpv would otherwise flow
    // into a file the app no longer believes comes next, which is a wrong song and not a crash. Touches
    // nothing that has produced a sample, so it is inaudible; the caller re-feeds afterwards.
    void dropQueuedAfterCurrent();
    void stop();
    void setPaused(bool paused);
    bool isPaused() const;   // current mpv "pause" flag (OS-lifecycle pause query)
    void togglePause();
    void seekRelative(double seconds);
    void setPosition(double seconds);
    void cycleSubtitle();                     // step through subtitle tracks (… -> off -> 1 -> 2 -> …)
    void addSubtitle(const QString& path);    // load + select an external subtitle file (.srt/.ass/…)
    void takeScreenshot(const QString& path); // save the current frame (with subtitles) to a PNG file

    // One audio or subtitle track in the current file, for building a picker menu.
    struct Track { int id = 0; QString title; QString lang; bool selected = false; };
    // The current file's chapters, with their titles — the raw material for chapter-derived skip segments.
    // (nextChapter()/prevChapter() below are relative jumps and cannot answer "is chapter 2 called Intro?".)
    QVector<MediaSegments::Chapter> chapters() const;
    // Container frame rate, 0 when unknown. Only needed to convert a Kodi .edl's "#<frame>" time form.
    double fps() const;
    QVector<Track> subtitleTracks() const;    // sub tracks in the current file (empty if none)
    QVector<Track> audioTracks() const;       // audio tracks in the current file (empty if none)
    void setSubtitleTrack(int id);            // select subtitle track id; id < 0 turns subtitles off
    void setAudioTrack(int id);               // select audio track id; id < 0 disables audio
    double subtitleDelay() const;             // current subtitle timing offset, seconds
    void setSubtitleDelay(double seconds);
    double audioDelay() const;                // current audio timing offset, seconds (mpv "audio-delay"); 0.0 when no mpv
    void setAudioDelay(double seconds);
    double subtitleScale() const;             // current subtitle size multiplier (1.0 = default)
    void setSubtitleScale(double factor);
    // Apply the user's subtitle-appearance preferences (font/size/colour/outline/box/position/bold + the
    // ASS-override gate) from Settings to mpv (issue #71). Called once at player creation and again, live,
    // whenever a Subtitles setting changes — every option is runtime-settable, so a change is visible on the
    // currently-playing sub at once. See SubtitleStyle::toMpvOptions for the pure mapping.
    void applySubtitleStyle();
    // Apply the user's audio-output preferences (device / passthrough / exclusive mode) from Settings to mpv
    // (issue #69). Called once at player creation and again, live, whenever an Audio setting changes. The device
    // change takes effect at once; passthrough and exclusive mode reconfigure the AO, so they take full effect on
    // the next audio (re)init. See AudioOutput::toMpvOptions for the pure mapping.
    void applyAudioOutput();
    // Apply ReplayGain (issue #141): set mpv's `replaygain` / `replaygain-preamp` / `replaygain-clip` /
    // `replaygain-fallback` for the file mpv currently has LOADED. Unlike the applies above this one cannot be
    // driven from Settings alone — the answer depends on the item — so the host passes what it already knows:
    // `isMusic` is the same music-vs-audiobook/podcast split issue #140's per-item speed makes (false for video
    // and for a chaptered spoken-word file), and the file's own REPLAYGAIN_* tag PRESENCE is read here off
    // mpv's metadata for the loaded file, so no second tag pass and no extra file read is needed. Called at
    // every file-loaded and again, live, when either ReplayGain setting changes. ReplayGain::toMpvOptions owns
    // every decision — including the audiobook carve-out and the untagged-plays-unmodified guarantee — and all
    // four options are written unconditionally so the previous item's gain can never bleed into this one.
    void applyReplayGain(bool isMusic);

    // ---- Crossfade (issue #141) --------------------------------------------------------------------------
    // Overlap the next track with the one playing, for `seconds`, on a SECOND mpv instance — the shape #141
    // decided on, and the one MediaPane already proves out by running two players side by side for split
    // screen. libmpv exposes no audio graph a single instance could mix two files through, so two decoders is
    // not a workaround here, it is the mechanism.
    //
    // The two instances are DECKS and they ping-pong. The deck created in the constructor keeps the GL render
    // context and is the only one video ever plays on; the second is created on the first crossfade, is
    // audio-only, and lives for the rest of the session. `beginCrossfade` loads `url` on whichever deck is
    // idle and starts an equal-power ramp (Crossfade::outgoingGain/incomingGain) across both; when the ramp
    // finishes — or when the outgoing deck hits EOF, whichever comes first, and in practice it is usually the
    // EOF by a fraction of the audio output's buffer — the incoming deck BECOMES the active one, the outgoing
    // deck is stopped, and crossfadePromoted() is emitted. Everything above this
    // class keeps talking to one MpvWidget and never learns which context is behind it.
    //
    // WHOSE JOB IS WHOSE: this class owns the two decoders, the ramp and the handover. It does not know what a
    // queue is, whether the item is music, or whether the two tracks share an album — the host decides whether
    // a boundary may be crossfaded at all (Crossfade::secondsFor) and only then calls this.
    void beginCrossfade(const QString& url, double seconds, const StreamHeaders::Headers& headers = {});
    bool crossfading() const { return xfIncoming_ != nullptr; }
    // Finish the window NOW, at full volume on the incoming track. This is what a Next press during a
    // crossfade resolves to: the track fading in IS the next track, so a skip lands on it rather than jumping
    // over it, and it lands there with one deck playing and nothing orphaned.
    void endCrossfadeNow();
    // Abandon the window: the incoming deck is stopped and forgotten, the outgoing one returns to full volume
    // and carries on. Used by every path that replaces or stops what is playing (play(), stop(), leaving the
    // page, teardown) so no window can outlive the thing it was a transition out of.
    void cancelCrossfade();
    // Apply refresh-rate matching Tier 1 (issue #70): set mpv's `video-sync` from the "Reduce judder" toggle so
    // video locks to the display clock (display-resync) or falls back to mpv's audio-clock default. Called once
    // at player creation and again, live, whenever the toggle changes. Inert for audio-only playback (mpv keeps
    // the audio clock when there is no video track). See RefreshSync::videoSyncFor for the pure mapping.
    void applyRefreshSync();
    // Apply HDR output handling (issue #68): set mpv's tone-mapping / hdr-compute-peak / target-colorspace-hint
    // from the "HDR video" two-way switch — tone-map HDR to SDR (default, fixes the washed-out case) or signal
    // HDR10 to the swapchain when the display supports it (passthrough, tone-map fallback where it does not).
    // Called once at player creation and again, live, whenever the setting changes. Inert for SDR content (mpv's
    // tone-mapping only engages on an HDR transfer). See HdrOutput::optionsFor for the pure mapping.
    void applyHdrOutput();
    // One selectable audio output, from mpv's `audio-device-list` property. `name` is the id stored in Settings
    // and set as `audio-device`; `description` is the human label shown in the picker.
    struct AudioDevice { QString name; QString description; };
    // The audio outputs mpv can see on this machine, for the Settings device picker. Read live from this
    // player's initialised mpv context (the list is a system property, so any context answers it); empty on any
    // failure — the picker always prepends its own "Auto" entry regardless.
    QVector<AudioDevice> availableAudioDevices() const;
    void nextChapter();                       // jump to the next chapter (M4B audiobooks, chaptered videos)
    void prevChapter();                       // jump to the previous chapter
    void setVolume(int percent);              // 0..200 (boost above 100%); 100 = original level
    int  volume() const { return volumePercent_; }   // the level the host last asked for (before the #141 ramp)
    void setMuted(bool muted);
    void setSpeed(double factor);             // playback rate (1.0 = normal); pitch-corrected by mpv
    double speed() const;                     // current playback rate

signals:
    void durationChanged(double seconds);
    void positionChanged(double seconds);
    void endReached();
    // mpv could not play what it was given — a dead link, a missing file, a container it cannot open. Carries
    // mpv's own reason. Distinct from endReached(): that one means "finished", and treating a failure as a
    // finish advances the playlist past a track that never played. Without this a failed load was SILENT: the
    // player sat on an empty surface with no error and no way for anything above to know.
    void loadFailed(const QString& reason);
    // The load STALLED (issue #213): mpv began the file and then said nothing at all — no FILE_LOADED, no
    // END_FILE — for longer than LoadWatchdog allows. Distinct from loadFailed because the REMEDY differs: a
    // stalled link is usually not expired, the source is dead or throttled, and telling the listener to mint
    // a fresh link sends them to the wrong fix. Carries how long was waited, for the message. The widget does
    // NOT stop mpv itself on a stall: what the screen is owed is the host's decision (PlaybackFailure::plan),
    // and pre-empting it here would override the gapless carve-out.
    void loadStalled(int waitedSeconds);
    // mpv's current playlist index changed (issue #141). Under gapless, mpv advances its OWN playlist across a
    // track boundary without stopping the decoder, so no per-track EOF fires; this is how the host learns a
    // boundary was crossed. Emitted whenever mpv reports `playlist-pos`; the host acts on it only while gapless
    // is armed (off, it is inert — every replace-load leaves a single-entry playlist the host ignores).
    void playlistPositionChanged(int pos);
    // Crossfade (issue #141): the incoming deck just became the active one, so the app's notion of "current
    // track" is exactly one behind. Emitted BEFORE the newly active file's durationChanged / positionChanged /
    // fileLoaded are re-announced, so the host has already advanced its queue (and re-keyed the resume /
    // per-track state) by the time those arrive — the same ordering the gapless playlist-pos boundary has.
    void crossfadePromoted();
    void chapterCountChanged(int count);      // how many chapters the current file has (0 = none)
    // mpv's `pause` flag changed — by this class's own togglePause/setPaused, or by anything else that
    // reaches mpv (a keybinding, the OS lifecycle pause, the sleep timer). Emitted rather than polled
    // because pause is exactly the state in which the position ticks STOP arriving: a transport button that
    // refreshed itself on the next tick would learn it had been paused only when it was played again.
    void pausedChanged(bool paused);
    // Fired once the file's tracks are known. hasUsableSubtitle is true when it already carries a subtitle
    // track in the preferred language (or any, if no preference) — so a listener can auto-fetch one only when
    // it's false. isVideo distinguishes a real video (worth subtitling) from an audio-only file.
    void fileLoaded(bool hasUsableSubtitle, bool isVideo);

protected:
#ifdef Q_OS_IOS
    void paintEvent(QPaintEvent*) override;
#else
    void initializeGL() override;
    void paintGL() override;
#endif
    void resizeEvent(QResizeEvent*) override;

private slots:
    void maybeUpdate();   // a frame is ready -> repaint
    void onMpvEvents();   // drain mpv's event queue on the GUI thread
    void refreshNowPlaying(); // show/hide the audio-only "now playing" overlay

private:
    static void onMpvRedraw(void* ctx);                       // render-update callback (any thread)
    static void onMpvWakeup(void* ctx);                       // event wakeup callback (any thread)
#ifndef Q_OS_IOS
    static void* getProcAddress(void* ctx, const char* name); // GL loader for mpv
#endif
    // `fromActive` is false for events drained off the deck that is NOT currently the player — during a
    // crossfade window that is the deck fading in, and for the rest of the session it is a stopped deck.
    // An inactive deck's events are deliberately almost all discarded: its EOF is not the queue's EOF, its
    // position is not the position anything above is showing, and letting either through was the whole class
    // of bug a second decoder introduces.
    void handleEvent(mpv_event* event, mpv_handle* from, bool fromActive);
    // #231: one MPV_EVENT_LOG_MESSAGE — mpv's and ffmpeg's own words about the stream — scrubbed of any
    // credential and passed through the burst counter on its way into stream_debug.log.
    void handleLogMessage(mpv_event* event, bool fromActive);
    void logVideoInfo(); // append the loaded video's codec/resolution/pixfmt/hwdec to the debug log

    // Crossfade internals (issue #141). See the beginCrossfade note above for the deck model.
    mpv_handle* ensureSecondDeck();                          // create the audio-only deck on first use
    void applyAudioOutputTo(mpv_handle* h);                  // #69's device/passthrough onto ONE deck
    void applyReplayGainTo(mpv_handle* h, bool isMusic);     // #141's levelling onto ONE deck
    void applyDeckVolumes();                                 // push volumePercent_ * this deck's ramp gain
    void crossfadeTick();                                    // one step of the equal-power ramp
    void promoteIncomingDeck();                              // the handover; emits crossfadePromoted()
    void announceActiveDeck();                               // re-emit duration/position/chapters/fileLoaded

    // The deck that owns the render context and is the ONLY one video plays on. `mpv` below is whichever deck
    // is currently the player, so every existing call in this class already routes to the right context; this
    // pointer exists so the render context and the video path never follow the swap.
    mpv_handle* mpvPrimary_ = nullptr;
    mpv_handle* mpvSecond_  = nullptr;  // the crossfade deck; null until the first crossfade
    mpv_handle* xfIncoming_ = nullptr;  // non-null only inside a window: the deck fading IN
    QTimer* xfTimer_ = nullptr;         // the ramp clock
    // ...and the ramp's MEASURE, which is wall time and not a count of ticks. A 25 ms QTimer that is late (a
    // paint, a scan, anything on the GUI thread) still eventually delivers every tick — just later — so
    // counting them stretches a 6 s fade to however long the GUI thread felt like taking. Measuring the gap
    // between ticks makes the ramp the length it was asked for however badly the timer is served, which is
    // the difference between a fade that is 6 s of music and one that is 6 s of good luck.
    QElapsedTimer xfClock_;
    double xfSeconds_ = 0.0;            // the window length asked for
    double xfElapsed_ = 0.0;            // how far into it we are (frozen while paused)
    int volumePercent_ = 100;           // the volume the HOST asked for; the ramp scales this, never replaces it
    bool gaplessArmed_ = false;         // setGaplessAudio() state, so a newly created deck inherits it

    mpv_handle* mpv = nullptr;          // the ACTIVE deck — always mpvPrimary_ or mpvSecond_
    mpv_render_context* mpv_gl = nullptr; // GL render context (desktop) / SW render context (iOS)
#ifdef Q_OS_IOS
    void renderSoftwareFrame(); // drain a ready frame into frame_ and schedule a repaint
    QImage frame_;
#endif

    // "Now playing" overlay shown for audio-only files (no video track) so they aren't a black screen.
    QLabel* nowPlaying_ = nullptr;
    QTimer* npTimer_ = nullptr;   // brief delay after load before deciding audio-vs-video (avoids a flash)
    QString mediaTitle_;          // mpv's own `media-title` — a real title, an ICY tag, or the URL ITSELF
    QString hostTitle_;           // #202: what the host says this track is called; used when mpv's is a url
    QString playedUrl_;           // #202: the last replace-load, so a label can be derived without the query
    bool hasVideo_ = false;

    // #213 load watchdog. Armed on START_FILE of the ACTIVE deck, disarmed by FILE_LOADED / END_FILE / stop().
    QTimer* loadTimer_ = nullptr;
    LoadWatchdog::Phase loadPhase_ = LoadWatchdog::Phase::First;
    bool loadWatched_ = false;    // LoadWatchdog::watches(url) for the file play() last handed mpv
    bool fileLoaded_ = false;     // FILE_LOADED seen for the current file
    void armLoadWatchdog(LoadWatchdog::Phase phase);
    LoadWatchdog::Progress loadProgress() const; // what mpv can say about the file's bytes: three-valued, see .cpp
    void onLoadWatchdog();

    // #231 mpv log capture. The throttle holds the per-shape burst counters; the clock is its only source of
    // time (the class deliberately has none of its own, so a probe can drive four windows in microseconds);
    // the timer exists ONLY to get a summary out of a burst that has stopped, and runs only while one is
    // outstanding. Not started here — see the constructor.
    MpvLogThrottle logThrottle_;
    QElapsedTimer  logClock_;
    QTimer*        logFlushTimer_ = nullptr;
};
