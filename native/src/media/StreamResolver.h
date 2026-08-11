#pragma once
#include "../core/StreamHeaders.h"
#include <QObject>
#include <QVector>

class QNetworkAccessManager;

// A single playlist entry. `title`/`url` are the original pair every caller relied on; the four IPTV
// fields (#75) carry the standard M3U extended attributes off the `#EXTINF` line — empty for a plain
// media playlist or a bare-path disc list, populated for an IPTV channel list. Additive: existing
// `{title, url}` aggregate-init sites still compile (the extra members value-initialise to empty).
struct M3uEntry
{
    QString title;
    QString url;
    QString logo;     // tvg-logo   — remote channel-logo URL (tile/row art)
    QString group;    // group-title — the section this channel belongs to
    QString tvgId;    // tvg-id     — EPG channel id (retained for a future XMLTV increment)
    QString tvgName;  // tvg-name   — canonical channel name (retained for a future XMLTV increment)
};

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

    // The EPG URL a playlist declares on its own `#EXTM3U` header line — `url-tvg="…"` (a.k.a. `x-tvg-url="…"`)
    // (#75 inc 3). Empty when the header carries neither. A source's manual `epgUrl` (IptvSource) takes
    // precedence over this; this is the fallback so a playlist that names its own guide is honoured without the
    // user re-typing it. Pure (probe_xmltv).
    static QString m3uHeaderTvgUrl(const QString& text);

    // Which of a playlist's own headers each of its entries is entitled to, parallel to `entries` (#59).
    // A playlist names whatever hosts it likes; the headers it was fetched with belong to `src`'s origin
    // alone, so an entry served from that origin inherits them and an entry pointing elsewhere gets an
    // EMPTY set — which is not the same as being skipped: it is what makes the player clear the previous
    // channel's headers when this one needs none.
    //
    // Pure, and public, because it is the rule worth pinning and classify() (which emits signals from a
    // network callback) is not a seam a probe can reach.
    static QVector<StreamHeaders::Headers> entryHeaders(const QVector<M3uEntry>& entries, const QString& src,
                                                        const StreamHeaders::Headers& headers);

    // local file or http(s) URL. `headers` is the source's behaviorHints.proxyHeaders.request: this is a
    // NON-MPV fetch of the very stream URL, so without them a header-gated playlist/HLS manifest 403s here
    // and the user sees "couldn't load" on a stream the player itself could have played.
    void resolve(const QString& src, const QString& title, const StreamHeaders::Headers& headers = {});

signals:
    // HLS / unparseable / fetch failed. Carries the headers back out so the player gets the same ones this
    // fetch used — the manifest and its segments come from the same host and are gated the same way.
    void playDirect(const QString& url, const QString& title, const StreamHeaders::Headers& headers);
    // IPTV/media list. `entryHeaders` is parallel to `urls`: each entry's own answer from forPlayUrl, because
    // a playlist names whatever hosts it likes and the list's headers belong to the LIST's origin. An entry
    // served from that origin inherits them (which is the common shape — a gated provider's channels sit
    // beside its playlist); an entry pointing at another host gets none, and gets them removed by being
    // handed an empty set rather than by anyone remembering to clear (#59).
    // `groups` and `logos` are parallel to `urls` too (#75): the entry's group-title (for the sectioned
    // channel list) and tvg-logo (channel art), each empty when the playlist did not carry that attribute.
    void playQueue(const QStringList& urls, const QStringList& titles,
                   const QStringList& groups, const QStringList& logos,
                   const QString& recentSrc, const QString& title,
                   const QVector<StreamHeaders::Headers>& entryHeaders);
    void openDisc(const QString& src, const QString& title);     // PlayStation multi-disc set
    void status(const QString& message);                          // transient progress ("Loading playlist…")

private:
    void classify(const QString& src, const QString& text, const QString& title,
                  const StreamHeaders::Headers& headers); // was handleM3u
    QNetworkAccessManager* nam_ = nullptr; // lazily created for remote playlists
};
