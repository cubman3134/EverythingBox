#include "Achievements.h"
#include "AppPaths.h"
#include "../libretro/LibretroCore.h"

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QSettings>
#include <QCoreApplication>
#include <QUrl>
#include <QByteArray>
#include <cstring>

#include "rc_client.h"
#include "rc_libretro.h"
#include "rc_consoles.h"
#include "rc_api_request.h"
#include "rc_error.h"

namespace {

QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/mymediavault.ini"),
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
};

Achievements* g_ach = nullptr;   // the single instance, for the C trampolines
RAState* g_st = nullptr;

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

// rc_client server call -> HTTP via Qt. The response is handed back through the rcheevos callback.
void serverCallCb(const rc_api_request_t* request, rc_client_server_callback_t callback, void* callback_data, rc_client_t*)
{
    if (!g_st || !g_st->nam) { callback(nullptr, callback_data); return; }
    QNetworkRequest rq{ QUrl(QString::fromUtf8(request->url)) };
    rq.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MyMediaVault"));
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
    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, callback, callback_data] {
        const QByteArray body = reply->readAll();
        const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
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
        emit g_ach->achievementUnlocked(QString::fromUtf8(event->achievement->title),
                                        QString::fromUtf8(event->achievement->description),
                                        (int)event->achievement->points);
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
        emit g_ach->gameLoaded(true, (g && g->title) ? QString::fromUtf8(g->title) : QString(),
                               (int)sum.num_unlocked_achievements, (int)sum.num_core_achievements);
    }
    else
    {
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
        rc_client_set_hardcore_enabled(st->client, 0); // softcore: save states stay allowed
    }
}

Achievements::~Achievements()
{
    auto* st = static_cast<RAState*>(impl_);
    if (st)
    {
        if (st->memReady) rc_libretro_memory_destroy(&st->regions);
        if (st->client) rc_client_destroy(st->client);
        delete st;
    }
    if (g_ach == this) { g_ach = nullptr; g_st = nullptr; }
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
    if (!st || !st->client || !st->loggedIn || console == 0 || !core) return; // no RA without login / known system
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
    if (st->client) rc_client_unload_game(st->client);
    if (st->memReady) { rc_libretro_memory_destroy(&st->regions); st->memReady = false; }
    st->core = nullptr;
}

void Achievements::doFrame()
{
    auto* st = static_cast<RAState*>(impl_);
    if (st && st->client) rc_client_do_frame(st->client);
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
