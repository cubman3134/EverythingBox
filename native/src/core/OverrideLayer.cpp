#include "OverrideLayer.h"

namespace OverrideLayer
{
    Map effective(const Map& baseline, const QVector<Map>& layers)
    {
        Map out = baseline;
        for (const Map& layer : layers)
            for (auto it = layer.constBegin(); it != layer.constEnd(); ++it)
                out.insert(it.key(), it.value());   // later layers win; a new key is added
        return out;
    }

    Map normalizeDelta(const Map& baseline, const Map& desired)
    {
        Map out;
        for (auto it = desired.constBegin(); it != desired.constEnd(); ++it)
        {
            const auto b = baseline.constFind(it.key());
            // Keep the key only when it is a genuine override: the baseline either does not carry it, or
            // carries a different value. A value equal to the baseline is NOT stored — that is the whole of
            // the no-leak rail (a reset-to-default is the absence of the key, never the key holding the
            // default value, so it can never be mistaken for an override on the next game).
            if (b == baseline.constEnd() || b.value() != it.value())
                out.insert(it.key(), it.value());
        }
        return out;
    }

    Map withKey(Map delta, const QString& key, const QString& baselineVal, const QString& desiredVal)
    {
        if (desiredVal == baselineVal)
            delta.remove(key);          // reset this row to the baseline: drop the override entirely
        else
            delta.insert(key, desiredVal);
        return delta;
    }
}
