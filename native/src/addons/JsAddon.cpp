#include "JsAddon.h"
#include "AddonContext.h"

#include "duktape.h"
#include <chrono>
#include <atomic>

// Per-context execution deadline (steady-clock ms). Stored as the Duktape heap udata so the executor's
// timeout check can read it. deadlineMs == 0 means "no active limit".
namespace {
struct ExecLimit { std::atomic<long long> deadlineMs{ 0 }; };

long long nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
const long long kAddonTimeoutMs = 5000; // matches the Unity Jint 5s limit
}

// Called periodically by the Duktape bytecode executor (wired via DUK_USE_EXEC_TIMEOUT_CHECK).
extern "C" int goliath_duk_exec_timeout(void* udata)
{
    auto* l = static_cast<ExecLimit*>(udata);
    if (!l) return 0;
    const long long d = l->deadlineMs.load();
    return (d != 0 && nowMs() >= d) ? 1 : 0;
}

// Retrieve the AddonContext* stashed in the heap's global stash (set in load()).
static AddonContext* ctxOf(duk_context* d)
{
    duk_push_global_stash(d);
    duk_get_prop_string(d, -1, "\xff" "ctx");
    auto* c = static_cast<AddonContext*>(duk_get_pointer(d, -1));
    duk_pop_2(d);
    return c;
}

static duk_ret_t js_log(duk_context* d)
{
    if (AddonContext* c = ctxOf(d)) c->log(QString::fromUtf8(duk_safe_to_string(d, 0)));
    return 0;
}

static duk_ret_t js_httpGet(duk_context* d)
{
    AddonContext* c = ctxOf(d);
    const QString body = c ? c->httpGet(QString::fromUtf8(duk_safe_to_string(d, 0))) : QString();
    duk_push_string(d, body.toUtf8().constData());
    return 1;
}

static duk_ret_t js_httpRequest(duk_context* d)
{
    AddonContext* c = ctxOf(d);
    // Accept either a JSON string or an object argument.
    QString opts;
    if (duk_is_object(d, 0)) { duk_json_encode(d, 0); opts = QString::fromUtf8(duk_safe_to_string(d, 0)); }
    else opts = QString::fromUtf8(duk_safe_to_string(d, 0));
    const QString body = c ? c->httpRequest(opts) : QString();
    duk_push_string(d, body.toUtf8().constData());
    return 1;
}

static duk_ret_t js_getStorage(duk_context* d)
{
    AddonContext* c = ctxOf(d);
    const QString v = c ? c->getStorage(QString::fromUtf8(duk_safe_to_string(d, 0))) : QString();
    duk_push_string(d, v.toUtf8().constData());
    return 1;
}

static duk_ret_t js_setStorage(duk_context* d)
{
    if (AddonContext* c = ctxOf(d))
        c->setStorage(QString::fromUtf8(duk_safe_to_string(d, 0)), QString::fromUtf8(duk_safe_to_string(d, 1)));
    return 0;
}

static duk_ret_t js_getConfig(duk_context* d)
{
    AddonContext* c = ctxOf(d);
    const QString v = c ? c->getConfig(QString::fromUtf8(duk_safe_to_string(d, 0))) : QString();
    duk_push_string(d, v.toUtf8().constData());
    return 1;
}

std::unique_ptr<JsAddon> JsAddon::load(const QString& source, std::unique_ptr<AddonContext> ctx, QString* error)
{
    auto* limit = new ExecLimit();
    duk_context* d = duk_create_heap(nullptr, nullptr, nullptr, limit, nullptr); // udata = limit
    if (!d) { delete limit; if (error) *error = QStringLiteral("could not create JS heap"); return nullptr; }

    // Stash the context pointer so the host C functions can reach it.
    duk_push_global_stash(d);
    duk_push_pointer(d, ctx.get());
    duk_put_prop_string(d, -2, "\xff" "ctx");
    duk_pop(d);

    // Bind the sandboxed host API as globals.
    duk_push_c_function(d, js_log,        1); duk_put_global_string(d, "log");
    duk_push_c_function(d, js_httpGet,    1); duk_put_global_string(d, "httpGet");
    duk_push_c_function(d, js_httpRequest,1); duk_put_global_string(d, "httpRequest");
    duk_push_c_function(d, js_getStorage, 1); duk_put_global_string(d, "getStorage");
    duk_push_c_function(d, js_setStorage, 2); duk_put_global_string(d, "setStorage");
    duk_push_c_function(d, js_getConfig,  1); duk_put_global_string(d, "getConfig");

    // Evaluate the addon body (protected, and time-bounded against a top-level runaway loop).
    const QByteArray src = source.toUtf8();
    limit->deadlineMs = nowMs() + kAddonTimeoutMs;
    const bool evalFailed = duk_peval_lstring(d, src.constData(), static_cast<duk_size_t>(src.size())) != 0;
    limit->deadlineMs = 0;
    if (evalFailed)
    {
        if (error) *error = QString::fromUtf8(duk_safe_to_string(d, -1));
        duk_destroy_heap(d);
        delete limit;
        return nullptr;
    }
    duk_pop(d); // discard the eval result

    std::unique_ptr<JsAddon> a(new JsAddon());
    a->d_ = d;
    a->limit_ = limit;
    a->ctx_ = std::move(ctx);
    return a;
}

JsAddon::~JsAddon()
{
    if (d_) duk_destroy_heap(d_);
    delete static_cast<ExecLimit*>(limit_);
}

bool JsAddon::hasFunction(const QString& name)
{
    duk_get_global_string(d_, name.toUtf8().constData());
    const bool isFn = duk_is_function(d_, -1);
    duk_pop(d_);
    return isFn;
}

QString JsAddon::invoke(const QString& name, const QString& jsonArg)
{
    duk_get_global_string(d_, name.toUtf8().constData());
    if (!duk_is_function(d_, -1)) { duk_pop(d_); return QString(); }

    const QByteArray arg = jsonArg.toUtf8();
    duk_push_lstring(d_, arg.constData(), static_cast<duk_size_t>(arg.size()));

    auto* limit = static_cast<ExecLimit*>(limit_);
    if (limit) limit->deadlineMs = nowMs() + kAddonTimeoutMs; // bound this call
    const bool threw = duk_pcall(d_, 1) != 0;
    if (limit) limit->deadlineMs = 0;
    if (threw)
    {
        if (ctx_) ctx_->log(QStringLiteral("'%1' threw: %2").arg(name, QString::fromUtf8(duk_safe_to_string(d_, -1))));
        duk_pop(d_);
        return QString();
    }
    QString result = QString::fromUtf8(duk_safe_to_string(d_, -1));
    duk_pop(d_);
    return result;
}
