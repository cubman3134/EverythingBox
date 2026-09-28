// The app's own identity with Trakt, OpenSubtitles, AniList and MyAnimeList (#81): the four build-time slots, each
// resolved against the user's typed value by the ONE rule in BuiltinSecret::resolve.
//
// Every function takes the user's values as arguments rather than reading Settings or the tracker store itself,
// so this unit depends on nothing but the generated header: each client passes in what the user typed, from
// wherever that client keeps it, and gets back the credential to present plus where it came from.
//
// This is the only translation unit that includes BuiltinSecrets.h for these four slots. A target that links it
// needs the generated header on its include path (the app) or a fixture in its place (the probes).
#pragma once
#include "BuiltinSecretBlob.h"

#include <QString>

namespace BuiltinCredentials
{
    // Trakt: client id + secret.
    BuiltinSecret::Resolved trakt(const QString& userId, const QString& userSecret);
    // OpenSubtitles: the consumer API key (id holds it; secret is always empty).
    BuiltinSecret::Resolved openSubtitles(const QString& userKey);
    // AniList: client id + secret.
    BuiltinSecret::Resolved aniList(const QString& userId, const QString& userSecret);
    // MyAnimeList: a public client's id. The built-in has no secret; a user's own client may have one.
    BuiltinSecret::Resolved myAnimeList(const QString& userId, const QString& userSecret);

    // What is embedded in THIS build, with no user value considered. Empty when the slot is empty. For the
    // settings rows, which say "Built in" only over a row whose built-in value actually exists.
    BuiltinSecret::Resolved builtinTrakt();
    BuiltinSecret::Resolved builtinOpenSubtitles();
    BuiltinSecret::Resolved builtinAniList();
    BuiltinSecret::Resolved builtinMyAnimeList();
}
