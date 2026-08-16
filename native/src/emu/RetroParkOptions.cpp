#include "RetroParkOptions.h"
#include <retropark/retropark.h>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>

std::vector<CoreOption> RetroParkOptions::parse(const QByteArray& json)
{
    std::vector<CoreOption> out;
    const QJsonArray arr = QJsonDocument::fromJson(json).array();
    for (const auto& e : arr) {
        const QJsonObject o = e.toObject();
        CoreOption co;
        co.key = o["key"].toString().toStdString();
        co.desc = o["desc"].toString().toStdString();
        co.info = o["info"].toString().toStdString();
        co.defaultValue = o["default"].toString().toStdString();
        for (const auto& ve : o["values"].toArray()) {
            const QJsonObject vo = ve.toObject();
            co.values.emplace_back(vo["value"].toString().toStdString(),
                                   vo["label"].toString().toStdString());
        }
        out.push_back(std::move(co));
    }
    return out;
}

std::vector<CoreOption> RetroParkOptions::harvest(const QString& coreDir)
{
    // NOTE (from Phase A): RP_GFX_NONE builds NO backend, so load_core returns RP_ERR_DEVICE before the
    // core is ever created (and options are registered at create). Use RP_GFX_D3D11 + a tiny resize --
    // harvest is backend-independent; this is the pattern the RetroPark tests use headlessly.
    rp_runtime* rt = rp_runtime_create(RP_GFX_D3D11, nullptr);
    if (!rt) return {};
    rp_runtime_resize(rt, 64, 64);
    std::vector<CoreOption> out;
    if (rp_runtime_load_core(rt, coreDir.toUtf8().constData()) == RP_OK) {
        const char* j = rp_runtime_core_options_json(rt);
        out = parse(QByteArray(j ? j : "[]"));
    }
    rp_runtime_destroy(rt);
    return out;
}
