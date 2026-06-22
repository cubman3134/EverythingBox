// One loaded addon script, backed by a Duktape JavaScript context. Mirrors the Unity JintScriptEngine:
// the host API (log/httpGet/getStorage/setStorage) is bound as globals, and addon functions are called
// with a JSON-string argument and return a JSON string. Errors are contained (protected calls) - a
// throwing addon yields an empty result rather than crashing the app.
#pragma once
#include <QString>
#include <memory>

class AddonContext;
struct duk_hthread; // == duk_context

class JsAddon
{
public:
    // Loads source into a fresh JS context, taking ownership of ctx. Returns null on a load/parse error.
    static std::unique_ptr<JsAddon> load(const QString& source, std::unique_ptr<AddonContext> ctx,
                                         QString* error = nullptr);
    ~JsAddon();
    JsAddon(const JsAddon&) = delete;
    JsAddon& operator=(const JsAddon&) = delete;

    bool hasFunction(const QString& name);
    QString invoke(const QString& name, const QString& jsonArg); // "" if missing / threw

private:
    JsAddon() = default;
    duk_hthread* d_ = nullptr;
    void* limit_ = nullptr;   // ExecLimit* (heap udata) driving the execution timeout
    std::unique_ptr<AddonContext> ctx_;
};
