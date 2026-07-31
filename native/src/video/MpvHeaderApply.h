// Writing a stream's HTTP headers into a libmpv handle. One inline function, in its own header, so the
// EXACT code the player runs can also be driven by an out-of-tree verification harness against a real
// socket — a harness that re-implements this would only prove its own copy works.
//
// The rules it implements live in StreamHeaders (pure, probe-covered); this is only the libmpv spelling.
#pragma once
#include "../core/StreamHeaders.h"
#include <QByteArray>
#include <QStringList>
#include <QVector>
#include <mpv/client.h>

namespace MpvHeaderApply
{

// Write one property from StreamHeaders::applyTo. An EMPTY value list CLEARS the property — which is how the
// previous stream's headers stop existing, so it must be a real assignment, never a skip.
//
// "Clear" means RESTORE THE OPTION'S DEFAULT, not "assign the empty string". For http-header-fields the two
// coincide (its default is the empty list); for user-agent they emphatically do not — see below.
//
// http-header-fields is a string LIST, and it is set as a node array rather than as mpv's comma-separated
// string form on purpose: header values legitimately contain commas (a multi-value Accept, a Cookie), and
// the string form would split one such value into two malformed fields.
inline void setProperty(mpv_handle* mpv, const QString& property, const QStringList& values)
{
    if (!mpv) return;
    if (property != QLatin1String("http-header-fields"))
    {
        // Scalar (user-agent / referrer). CLEARING means restoring mpv's OWN default for the option — which
        // is NOT "" for both of them:
        //
        //     mpv_create + mpv_initialize, then mpv_get_property(…, MPV_FORMAT_STRING):
        //       user-agent  -> "libmpv"      referrer -> ""      http-header-fields -> ""
        //
        // (Measured against the real libmpv this app links, 2026-07; setting user-agent to "" afterwards
        // leaves it "", it does not spring back.) applyTo runs on EVERY MpvWidget::play, so writing "" here
        // would strip the User-Agent from all playback — local IPTV, debrid, ordinary streams — not just
        // from header-gated ones, and a CDN or WAF that rejects UA-less requests would start refusing
        // content that plays today. That is a regression the header feature has no business causing.
        //
        // Asked of mpv rather than hard-coded so it cannot drift: option-info/<name>/default-value is the
        // option's compiled-in default and stays correct across libmpv versions. The literal below is only
        // the fallback for a libmpv too old to answer, and is the measurement above.
        QByteArray v = values.value(0).toUtf8();
        if (values.isEmpty())
        {
            const QByteArray infoPath = "option-info/" + property.toUtf8() + "/default-value";
            char* def = nullptr;
            if (mpv_get_property(mpv, infoPath.constData(), MPV_FORMAT_STRING, &def) >= 0 && def)
            {
                v = QByteArray(def);
                mpv_free(def);
            }
            else
            {
                v = (property == QLatin1String("user-agent")) ? QByteArray("libmpv") : QByteArray();
            }
        }
        mpv_set_property_string(mpv, property.toUtf8().constData(), v.constData());
        return;
    }
    // The QByteArrays must outlive the node array that points into them; reserve first so no push_back
    // reallocation invalidates a pointer already stored.
    QVector<QByteArray> utf8;
    utf8.reserve(values.size());
    for (const QString& v : values) utf8.push_back(v.toUtf8());
    QVector<mpv_node> nodes(values.size());
    for (int i = 0; i < utf8.size(); ++i)
    {
        nodes[i].format   = MPV_FORMAT_STRING;
        nodes[i].u.string = const_cast<char*>(utf8.at(i).constData());
    }
    mpv_node_list list{};
    list.num    = int(nodes.size());
    list.values = nodes.isEmpty() ? nullptr : nodes.data();
    list.keys   = nullptr;
    mpv_node n{};
    n.format = MPV_FORMAT_NODE_ARRAY;
    n.u.list = &list;
    mpv_set_property(mpv, "http-header-fields", MPV_FORMAT_NODE, &n);
}

// Apply a whole stream's headers. Unconditional by construction: applyTo emits all three properties every
// time, so a stream needing none actively clears whatever the previous one set.
inline void apply(mpv_handle* mpv, const StreamHeaders::Headers& headers)
{
    StreamHeaders::applyTo(headers, [mpv](const QString& property, const QStringList& values) {
        setProperty(mpv, property, values);
    });
}

} // namespace MpvHeaderApply
