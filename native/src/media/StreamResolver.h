#pragma once
#include "../core/StreamHeaders.h"
#include <QObject>
#include <QVector>

class QNetworkAccessManager;

struct M3uEntry { QString title; QString url; };

// .m3u / .m3u8 playlist + stream-link dispatch. Three flavours share the extension: an HLS manifest
// (one adaptive stream libmpv chews directly), an IPTV/media list (becomes a channel queue), and a
// PlayStation multi-disc list (the emulator swaps discs itself). resolve() reads/fetches the source,
// classifies it, and emits exactly one outcome signal; the host decides what "play" means.
class StreamResolver : public QObject
{
    Q_OBJECT
public:
    explicit StreamResolver(QObject* parent = nullptr);

    // Pure classification helpers (probe-tested; see tools/probe_m3u.cpp).
    static bool isM3uRef(const QString& urlOrPath);
    static bool isHlsManifest(const QString& text);
    static QVector<M3uEntry> parseM3u(const QString& text, const QString& src);
    static bool looksLikeDiscPlaylist(const QVector<M3uEntry>& entries);

    // local file or http(s) URL. `headers` is the source's behaviorHints.proxyHeaders.request: this is a
    // NON-MPV fetch of the very stream URL, so without them a header-gated playlist/HLS manifest 403s here
    // and the user sees "couldn't load" on a stream the player itself could have played.
    void resolve(const QString& src, const QString& title, const StreamHeaders::Headers& headers = {});

signals:
    // HLS / unparseable / fetch failed. Carries the headers back out so the player gets the same ones this
    // fetch used — the manifest and its segments come from the same host and are gated the same way.
    void playDirect(const QString& url, const QString& title, const StreamHeaders::Headers& headers);
    void playQueue(const QStringList& urls, const QStringList& titles,
                   const QString& recentSrc, const QString& title); // IPTV/media list
    void openDisc(const QString& src, const QString& title);     // PlayStation multi-disc set
    void status(const QString& message);                          // transient progress ("Loading playlist…")

private:
    void classify(const QString& src, const QString& text, const QString& title,
                  const StreamHeaders::Headers& headers); // was handleM3u
    QNetworkAccessManager* nam_ = nullptr; // lazily created for remote playlists
};
