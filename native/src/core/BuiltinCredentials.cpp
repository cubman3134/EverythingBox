#include "BuiltinCredentials.h"
#include "BuiltinSecrets.h" // generated into the BUILD TREE by cmake/GenerateSecrets.cmake (a fixture in probes)

using BuiltinSecret::join;
using BuiltinSecret::resolve;
using BuiltinSecret::Resolved;
using BuiltinSecret::Source;

namespace
{
Resolved embedded(const QString& id, const QString& secret)
{
    return { id, secret, id.isEmpty() ? Source::None : Source::Builtin };
}
} // namespace

Resolved BuiltinCredentials::builtinTrakt()
{
    return embedded(join(eb_secrets::kTrakt_Id_A, eb_secrets::kTrakt_Id_ALen,
                         eb_secrets::kTrakt_Id_B, eb_secrets::kTrakt_Id_BLen),
                    join(eb_secrets::kTrakt_Secret_A, eb_secrets::kTrakt_Secret_ALen,
                         eb_secrets::kTrakt_Secret_B, eb_secrets::kTrakt_Secret_BLen));
}

Resolved BuiltinCredentials::builtinOpenSubtitles()
{
    return embedded(join(eb_secrets::kOpenSubs_Key_A, eb_secrets::kOpenSubs_Key_ALen,
                         eb_secrets::kOpenSubs_Key_B, eb_secrets::kOpenSubs_Key_BLen),
                    QString());
}

Resolved BuiltinCredentials::builtinAniList()
{
    return embedded(join(eb_secrets::kAniList_Id_A, eb_secrets::kAniList_Id_ALen,
                         eb_secrets::kAniList_Id_B, eb_secrets::kAniList_Id_BLen),
                    join(eb_secrets::kAniList_Secret_A, eb_secrets::kAniList_Secret_ALen,
                         eb_secrets::kAniList_Secret_B, eb_secrets::kAniList_Secret_BLen));
}

Resolved BuiltinCredentials::builtinMyAnimeList()
{
    return embedded(join(eb_secrets::kMal_Id_A, eb_secrets::kMal_Id_ALen,
                         eb_secrets::kMal_Id_B, eb_secrets::kMal_Id_BLen),
                    QString());
}

Resolved BuiltinCredentials::trakt(const QString& userId, const QString& userSecret)
{
    const Resolved b = builtinTrakt();
    return resolve(userId, userSecret, b.id, b.secret);
}

Resolved BuiltinCredentials::openSubtitles(const QString& userKey)
{
    return resolve(userKey, builtinOpenSubtitles().id);
}

Resolved BuiltinCredentials::aniList(const QString& userId, const QString& userSecret)
{
    const Resolved b = builtinAniList();
    return resolve(userId, userSecret, b.id, b.secret);
}

Resolved BuiltinCredentials::myAnimeList(const QString& userId, const QString& userSecret)
{
    const Resolved b = builtinMyAnimeList();
    return resolve(userId, userSecret, b.id, b.secret);
}
