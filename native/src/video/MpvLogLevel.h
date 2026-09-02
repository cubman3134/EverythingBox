// WHICH OF mpv's MESSAGES ARE WORTH KEEPING (issue #231). Pure, header-only, QtCore-only.
//
// This looks like it should be one line — ask libmpv for `warn` and write what arrives — and it is not, for
// a reason that was MEASURED on the acceptance run rather than reasoned about.
//
// THE MESSAGE #231 IS ABOUT IS NOT A WARNING. ffmpeg emits its concealment report at AV_LOG_INFO:
//
//   [ffmpeg/video] h264: concealing 5776 DC, 5776 AC, 5776 MV errors in B frame
//
// and mpv DEMOTES everything libav logs at INFO to its own `v`. So `mpv_request_log_messages(ctx, "warn")` —
// the obvious reading of "capture mpv's warnings" — silently discards the single line the issue was filed to
// capture, along with `Could not find ref with POC`, the reconnect notices and most of what the demuxer says
// about a short read. A capture that misses those is the same blind spot with a log line in front of it.
//
// AND `v` FOR EVERYTHING IS NOT AFFORDABLE. Measured on this tree, one HEALTHY file open at `v` costs 111
// lines — 101 of them mpv's own verbose (`vd: Requesting 16 threads`, `libmpv_render: Testing FBO format`,
// `autoconvert: dropping request due to pin disconnect`), which say nothing about the stream. main.cpp caps
// stream_debug.log at 1 MB and DELETES it above that, so ~110 lines per open would rotate the file away in
// a few dozen plays and take the rest of the session's evidence with it.
//
// THE RULE, and the same measurement is what makes it cheap: ask libmpv for `v`, then keep
//
//   * everything at warn / error / fatal — mpv's own judgement that something is wrong, from any module;
//   * everything from `ffmpeg…`, whatever level it carries — because that level is not ffmpeg's opinion of
//     the message's importance, it is mpv's blanket demotion of libav's INFO. This is the whole of the
//     decoder's, the demuxer's and the protocol's own account of the stream.
//
// On the SAME healthy open that costs 111 lines at full `v`, that rule costs 2 — there are ZERO `[v] ffmpeg`
// lines in a clean play. The verbose budget is spent only when libav has something to say, which is exactly
// when #231 wants it.
//
// EB_PERF=1 (the tree's existing diagnostics switch) drops the prefix half and keeps everything at `v` — the
// render-context and filter-graph sequence, for a fault that is in mpv rather than in the stream. EB_MPV_LOG
// overrides the level libmpv is asked for outright (`debug`, `trace`) and implies the same.
#pragma once
#include <QByteArray>
#include <QLatin1String>

namespace MpvLogLevel
{
// The level asked of libmpv. `v` by default — see the header note: `warn` does not include the message this
// exists for. EB_MPV_LOG overrides it verbatim for a targeted run; libmpv validates it, and MpvWidget falls
// back if it is refused, so a typo cannot silence the capture.
inline QByteArray requested(const QByteArray& envOverride)
{
    const QByteArray o = envOverride.trimmed();
    return o.isEmpty() ? QByteArrayLiteral("v") : o;
}

// Whether one delivered message is written down. `verboseWanted` is EB_PERF=1 or an explicit EB_MPV_LOG:
// both mean "show me everything at the level I asked for", so the prefix half is dropped.
inline bool keep(const QByteArray& level, const QByteArray& prefix, bool verboseWanted)
{
    if (verboseWanted) return true;
    if (level == "fatal" || level == "error" || level == "warn") return true;
    // libav's own words, at whatever level mpv's demotion left them. `ffmpeg`, `ffmpeg/video`,
    // `ffmpeg/audio`, `ffmpeg/demuxer`, `ffmpeg/protocol` — mpv builds every one of them by prefixing the
    // libav class name with "ffmpeg", which is why this is a prefix test and not a list of module names.
    return prefix.startsWith("ffmpeg");
}
} // namespace MpvLogLevel
