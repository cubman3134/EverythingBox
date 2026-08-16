#include "Achievements.h"
#include "AppBrand.h"
#include "AppPaths.h"
#include "Settings.h"   // hardcoreAchievements(): the persisted opt-in the client is initialised from (#94)
#include "../libretro/LibretroCore.h"

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QSettings>
#include <QCoreApplication>
#include <QUrl>
#include <QUrlQuery>
#include <QByteArray>
#include <QSet>
#include <cstring>

#include "rc_client.h"
#include "rc_libretro.h"
#include "rc_consoles.h"
#include "rc_api_request.h"
#include "rc_error.h"

namespace {

QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

// All rcheevos state lives here (file-local so the C callbacks below can reach it without exposing types).
struct RAState
{
    rc_client_t* client = nullptr;
    QNetworkAccessManager* nam = nullptr;
    LibretroCore* core = nullptr;
    rc_libretro_memory_regions_t regions{};
    bool memReady = false;
    bool loggedIn = false;
    QString user;
    // Every RA HTTP reply still in flight. serverCallCb inserts, the finished lambda removes, and a game
    // teardown neutralises the survivors (abortPendingReplies) so no stale response is delivered into rc_client
    // after the game it targeted has been freed (the crash: a memcpy out of the freed per-game rc_buffer).
    QSet<QNetworkReply*> pendingReplies;
};

Achievements* g_ach = nullptr;   // the single instance, for the C trampolines
RAState* g_st = nullptr;

// Neutralise every RA reply still in flight on a game teardown. Disconnecting first guarantees each reply's
// `finished` lambda can NEVER run, so the rcheevos response callback is not invoked for a game that is about to
// be (or already has been) freed — this is what definitively prevents the use-after-free. abort() then cancels
// the transfer and deleteLater() reclaims the reply. rcheevos would normally free each request's small
// callback_data when its response is delivered (even on abort, via rc_client_end_async); because we never
// deliver, those structs leak — bounded by the number of requests outstanding at the switch, a one-time
// trade we accept against the crash. Runs on the GUI thread, same as serverCallCb and the lambda, so no lock.
void abortPendingReplies(RAState* st)
{
    if (!st) return;
    const QSet<QNetworkReply*> inflight = st->pendingReplies;
    st->pendingReplies.clear();
    for (QNetworkReply* reply : inflight)
    {
        if (!reply) continue;
        QObject::disconnect(reply, nullptr, nullptr, nullptr);
        reply->abort();
        reply->deleteLater();
    }
}

// rc_client memory read -> the active core's RAM, mapped by rc_libretro.
uint32_t readMemoryCb(uint32_t address, uint8_t* buffer, uint32_t num_bytes, rc_client_t*)
{
    if (!g_st || !g_st->memReady) return 0;
    return rc_libretro_memory_read(&g_st->regions, address, buffer, num_bytes);
}

void coreMemInfoCb(uint32_t id, rc_libretro_core_memory_info_t* info)
{
    if (g_st && g_st->core) { info->data = (uint8_t*)g_st->core->memoryData(id); info->size = g_st->core->memorySize(id); }
    else                    { info->data = nullptr; info->size = 0; }
}

// The User-Agent RetroAchievements identifies the client by. It must be "<client>/<version> <rcheevos clause>"
// (the clause carries the rcheevos version + the console/hash support) — a bare name registers on RA as an
// "unknown emulator". Built once from the app version + rc_client_get_user_agent_clause().
QByteArray raUserAgent(rc_client_t* client)
{
    static QByteArray cached;
    if (cached.isEmpty() && client)
    {
        cached = QByteArray(AppBrand::kUserAgent) + '/' + QCoreApplication::applicationVersion().toUtf8();
        char clause[128] = { 0 };
        if (rc_client_get_user_agent_clause(client, clause, sizeof(clause)) > 0 && clause[0])
        { cached += ' '; cached += clause; }
    }
    return cached.isEmpty() ? QByteArray(AppBrand::kUserAgent) : cached;
}

// rc_client server call -> HTTP via Qt. The response is handed back through the rcheevos callback.
void serverCallCb(const rc_api_request_t* request, rc_client_server_callback_t callback, void* callback_data, rc_client_t* client)
{
    if (!g_st || !g_st->nam) { callback(nullptr, callback_data); return; }
    QNetworkRequest rq{ QUrl(QString::fromUtf8(request->url)) };
    rq.setHeader(QNetworkRequest::UserAgentHeader, QString::fromUtf8(raUserAgent(client ? client : g_st->client)));
    // The RA request verb (login2 / gameid / startsession / patch / awardachievement) for diagnostics only —
    // the rest of the URL and post_data carry the token, so they are never logged.
    const QString raVerb = QUrlQuery(QUrl(QString::fromUtf8(request->url))).queryItemValue(QStringLiteral("r"));
    rq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply;
    if (request->post_data && request->post_data[0])
    {
        rq.setHeader(QNetworkRequest::ContentTypeHeader,
                     QString::fromUtf8(request->content_type ? request->content_type : "application/x-www-form-urlencoded"));
        reply = g_st->nam->post(rq, QByteArray(request->post_data));
    }
    else
    {
        reply = g_st->nam->get(rq);
    }
    g_st->pendingReplies.insert(reply);
    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, callback, callback_data, raVerb] {
        // Drop this reply from the in-flight set FIRST, before any guard or callback: a completed reply must not
        // leave a dangling pointer in the set for a later teardown to abort()/delete a second time, and the set
        // must only ever hold requests that are genuinely still outstanding.
        if (g_st) g_st->pendingReplies.remove(reply);
        // The rc_client owns `callback_data` (a load_state that points back at the client). If the client has been
        // torn down (app shutdown destroys our QNetworkAccessManager, which aborts this in-flight request and fires
        // finished), calling the rcheevos callback would dereference freed state — an access violation inside
        // rc_client_load_error. Skip it once the client is gone; the process is exiting so the leaked load_state is moot.
        if (!g_st || !g_st->client) { reply->deleteLater(); return; }
        const QByteArray body = reply->readAll();
        const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        // Surface RA rejections (e.g. http 403 "unsupported_client") — the response body has no token, so log
        // just the verb, status, and RA's Code field.
        if (http >= 400 || body.contains("\"Success\":false"))
        {
            QString code; const int ci = body.indexOf("\"Code\":\"");
            if (ci >= 0) { const int s = ci + 8; const int e = body.indexOf('"', s); if (e > s) code = QString::fromUtf8(body.mid(s, e - s)); }
            qWarning("RA: %s -> http %d %s", qUtf8Printable(raVerb.isEmpty() ? QStringLiteral("?") : raVerb),
                     http, qUtf8Printable(code));
        }
        rc_api_server_response_t resp;
        resp.body = body.constData();
        resp.body_length = (size_t)body.size();
        resp.http_status_code = (reply->error() == QNetworkReply::NoError) ? (http ? http : 200) : (http ? http : 0);
        callback(&resp, callback_data); // body stays alive for the synchronous duration of this call
        reply->deleteLater();
    });
}

void eventHandlerCb(const rc_client_event_t* event, rc_client_t*)
{
    if (!g_ach) return;
    if (event->type == RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED && event->achievement)
    {
        char badge[256] = { 0 };
        rc_client_achievement_get_image_url(event->achievement, RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED, badge, sizeof(badge));
        emit g_ach->achievementUnlocked(QString::fromUtf8(event->achievement->title),
                                        QString::fromUtf8(event->achievement->description),
                                        (int)event->achievement->points,
                                        QString::fromUtf8(badge));
    }
}

void loginCb(int result, const char* error_message, rc_client_t* client, void*)
{
    if (!g_ach || !g_st) return;
    if (result == RC_OK)
    {
        g_st->loggedIn = true;
        if (const rc_client_user_t* u = rc_client_get_user_info(client))
        {
            g_st->user = QString::fromUtf8(u->username ? u->username : (u->display_name ? u->display_name : ""));
            store().setValue(QStringLiteral("ra/user"), g_st->user);
            store().setValue(QStringLiteral("ra/token"), QString::fromUtf8(u->token ? u->token : ""));
            store().sync();
        }
        emit g_ach->loginResult(true, g_st->user);
    }
    else
    {
        g_st->loggedIn = false;
        emit g_ach->loginResult(false, QString::fromUtf8(error_message ? error_message : "Login failed."));
    }
}

void gameLoadCb(int result, const char* error_message, rc_client_t* client, void*)
{
    if (!g_ach) return;
    if (result == RC_OK)
    {
        const rc_client_game_t* g = rc_client_get_game_info(client);
        rc_client_user_game_summary_t sum; std::memset(&sum, 0, sizeof(sum));
        rc_client_get_user_game_summary(client, &sum);
        qInfo("RA: loaded '%s' — %u/%u achievements", g && g->title ? g->title : "?",
              sum.num_unlocked_achievements, sum.num_core_achievements);
        emit g_ach->gameLoaded(true, (g && g->title) ? QString::fromUtf8(g->title) : QString(),
                               (int)sum.num_unlocked_achievements, (int)sum.num_core_achievements);
    }
    else
    {
        qWarning("RA: game load failed (rc %d): %s", result, error_message ? error_message : "");
        emit g_ach->gameLoaded(false, QString::fromUtf8(error_message ? error_message : ""), 0, 0);
    }
}

} // namespace

Achievements::Achievements(QObject* parent) : QObject(parent)
{
    auto* st = new RAState();
    impl_ = st; g_ach = this; g_st = st;
    st->nam = new QNetworkAccessManager(this);
    st->client = rc_client_create(readMemoryCb, serverCallCb);
    if (st->client)
    {
        rc_client_set_event_handler(st->client, eventHandlerCb);
        // Never read emulator core RAM from inside a network-reply callback. By default rc_client validates a
        // game's achievement addresses (rc_client_validate_addresses -> our readMemoryCb -> rc_libretro_memory_read)
        // SYNCHRONOUSLY in the async `startsession` response handler. That handler can land during the load/
        // resume-prompt window when the core's memory map (`regions->data[i]`) is not valid to dereference, giving
        // a 0xc0000005 reading stale core RAM (crash while the "Resume where you left off?" prompt is up). Turning
        // background reads OFF makes rc_client DEFER those reads to rc_client_do_frame — which EB only pumps from
        // RetroView::tick() while the core is running and un-paused — so core RAM is only ever read from a live core.
        // This is rcheevos' sanctioned mechanism for emulators (rc_client.h). Distinct from the game-teardown reply
        // UAF fixed separately by abortPendingReplies().
        rc_client_set_allow_background_memory_reads(st->client, 0);
        // Initialise from the persisted opt-in (issue #94): softcore (0) unless the user has chosen hardcore.
        // DEFAULT is off, so a fresh install is byte-for-byte the old softcore behaviour. No game is loaded yet,
        // so this is a plain flag write — no session to reset (setHardcore does that mid-session).
        rc_client_set_hardcore_enabled(st->client, Settings::hardcoreAchievements() ? 1 : 0);
    }
}

Achievements::~Achievements()
{
    auto* st = static_cast<RAState*>(impl_);
    // Null the globals FIRST: our QNetworkAccessManager is a child QObject destroyed AFTER this body runs, and
    // aborting its in-flight replies fires their finished handlers — which must now see the client as gone and
    // skip the rcheevos callback rather than call into the memory we free just below.
    if (g_ach == this) { g_ach = nullptr; g_st = nullptr; }
    if (st)
    {
        if (st->memReady) rc_libretro_memory_destroy(&st->regions);
        if (st->client) rc_client_destroy(st->client);
        delete st;
    }
    impl_ = nullptr;
}

bool Achievements::isLoggedIn() const { auto* st = static_cast<RAState*>(impl_); return st && st->loggedIn; }
QString Achievements::username() const { auto* st = static_cast<RAState*>(impl_); return st ? st->user : QString(); }

void Achievements::loginWithPassword(const QString& user, const QString& password)
{
    auto* st = static_cast<RAState*>(impl_);
    if (!st || !st->client) return;
    rc_client_begin_login_with_password(st->client, user.toUtf8().constData(),
                                        password.toUtf8().constData(), loginCb, nullptr);
}

void Achievements::tryLoginWithStoredToken()
{
    auto* st = static_cast<RAState*>(impl_);
    if (!st || !st->client) return;
    const QString user = store().value(QStringLiteral("ra/user")).toString();
    const QString token = store().value(QStringLiteral("ra/token")).toString();
    if (user.isEmpty() || token.isEmpty()) return;
    rc_client_begin_login_with_token(st->client, user.toUtf8().constData(),
                                     token.toUtf8().constData(), loginCb, nullptr);
}

void Achievements::logout()
{
    auto* st = static_cast<RAState*>(impl_);
    if (st && st->client) rc_client_logout(st->client);
    if (st) { st->loggedIn = false; st->user.clear(); }
    store().remove(QStringLiteral("ra/token"));
    store().sync();
}

void Achievements::loadGame(LibretroCore* core, unsigned console, const QString& romPath)
{
    auto* st = static_cast<RAState*>(impl_);
    if (!st) return;
    // Start clean: a load without a prior explicit unloadGame() must not leave a reply from the previous game
    // able to deliver into the fresh session.
    abortPendingReplies(st);
    if (!st->client || !st->loggedIn || console == 0 || !core) return; // no RA without login / known system
    st->core = core;
    std::memset(&st->regions, 0, sizeof(st->regions));
    rc_libretro_memory_init(&st->regions, core->memoryMap(), coreMemInfoCb, console);
    st->memReady = true;
    rc_client_begin_identify_and_load_game(st->client, console, romPath.toUtf8().constData(),
                                           nullptr, 0, gameLoadCb, nullptr);
}

void Achievements::unloadGame()
{
    auto* st = static_cast<RAState*>(impl_);
    if (!st) return;
    // Abort in-flight replies BEFORE rc_client_unload_game frees the per-game rc_buffer, so no late finished
    // handler can deliver a response that copies strings out of that freed arena (the use-after-free crash).
    abortPendingReplies(st);
    if (st->client) rc_client_unload_game(st->client);
    if (st->memReady) { rc_libretro_memory_destroy(&st->regions); st->memReady = false; }
    st->core = nullptr;
}

void Achievements::doFrame()
{
    auto* st = static_cast<RAState*>(impl_);
    if (st && st->client) rc_client_do_frame(st->client);
}

void Achievements::setHardcore(bool on)
{
    auto* st = static_cast<RAState*>(impl_);
    if (!st || !st->client) return;
    rc_client_set_hardcore_enabled(st->client, on ? 1 : 0);
    // Enabling hardcore with a game loaded raises RC_CLIENT_EVENT_RESET and SUSPENDS achievement processing
    // until rc_client_reset is called (rc_client.h). That is exactly the site's "enabling hardcore mid-session
    // resets it" rule: reset clears the softcore-tainted run and re-enables processing so the hardcore session
    // starts clean. With no game loaded it is a harmless no-op. (Disabling never resets — dropping to softcore
    // keeps playing.) The emulator core itself is NOT force-reset here; the RA session is what resets.
    if (on) rc_client_reset(st->client);
}

bool Achievements::hardcoreActive() const
{
    auto* st = static_cast<RAState*>(impl_);
    // Live truth for the emulator's gates: hardcore is enabled on the client AND a game with an achievement
    // session is actually loaded. Either being false means nothing to protect — the gates no-op.
    return st && st->client && rc_client_get_hardcore_enabled(st->client) != 0
        && rc_client_get_game_info(st->client) != nullptr;
}

unsigned Achievements::consoleIdForExtension(const QString& e)
{
    if (e == QLatin1String("gba")) return RC_CONSOLE_GAMEBOY_ADVANCE;
    if (e == QLatin1String("gbc")) return RC_CONSOLE_GAMEBOY_COLOR;
    if (e == QLatin1String("gb") || e == QLatin1String("sgb") || e == QLatin1String("dmg")) return RC_CONSOLE_GAMEBOY;
    if (e == QLatin1String("nes") || e == QLatin1String("fds") || e == QLatin1String("unif") || e == QLatin1String("unf")) return RC_CONSOLE_NINTENDO;
    if (e == QLatin1String("sfc") || e == QLatin1String("smc") || e == QLatin1String("bs") || e == QLatin1String("st")) return RC_CONSOLE_SUPER_NINTENDO;
    if (e == QLatin1String("sms")) return RC_CONSOLE_MASTER_SYSTEM;
    if (e == QLatin1String("gg"))  return RC_CONSOLE_GAME_GEAR;
    if (e == QLatin1String("sg"))  return RC_CONSOLE_SG1000;
    if (e == QLatin1String("md") || e == QLatin1String("gen") || e == QLatin1String("smd")) return RC_CONSOLE_MEGA_DRIVE;
    if (e == QLatin1String("n64") || e == QLatin1String("z64") || e == QLatin1String("v64") || e == QLatin1String("ndd")) return RC_CONSOLE_NINTENDO_64;
    if (e == QLatin1String("pce") || e == QLatin1String("sgx")) return RC_CONSOLE_PC_ENGINE;
    if (e == QLatin1String("ws") || e == QLatin1String("wsc")) return RC_CONSOLE_WONDERSWAN;
    if (e == QLatin1String("a26")) return RC_CONSOLE_ATARI_2600;
    if (e == QLatin1String("cue") || e == QLatin1String("chd") || e == QLatin1String("pbp")
        || e == QLatin1String("m3u") || e == QLatin1String("ccd")) return RC_CONSOLE_PLAYSTATION;
    return 0;
}
