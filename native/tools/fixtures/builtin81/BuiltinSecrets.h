// A FIXTURE stand-in for the build-tree BuiltinSecrets.h, used by probe_trakt, probe_tracker and probe_subs and
// by nothing else. The #81 twin of tools/fixtures/lastfm/BuiltinSecrets.h, for the same two reasons.
//
// WHY NOT THE REAL ONE. The real header is generated at configure time from native/secrets/*.secrets, which are
// git-ignored and absent in every clone and on CI, so in the build that runs the gate all four of these slots
// are EMPTY. A probe compiled against that could assert "nothing built in" and nothing else: never that a
// built-in value is used when the user has none, never that the user's own value still beats it.
//
// WHY NOT A RUNTIME SETTER. A setter would be a second way for the shipped binary to choose which application
// identity it presents, and it would skip BuiltinSecret::join entirely. Substituting the HEADER keeps the whole
// real path: the same arrays, the same lengths, the same de-obfuscation, the same resolve rule above them.
//
// THE VALUES ARE NOT CREDENTIALS. They are the literal TEST-* strings below, meaningless to every service, and
// the probes only ever send them to a QTcpServer they opened on 127.0.0.1 themselves.
//
// HOW THE BYTES WERE PRODUCED: by running native/cmake/GenerateSecrets.cmake itself (cmake -P) over scratch
// .secrets files holding the plaintexts below, and copying the generated blocks. The probes assert that
// de-obfuscating each block yields its plaintext EXACTLY, which pins the C++ reverse against the CMake forward.
//
// This directory must never be on the APP target's include path: only the three probe targets name it.
#pragma once

namespace eb_secrets {

// plaintext: "TEST-TRAKT-ID" (13 bytes)
inline constexpr unsigned char kTrakt_Id_A[]       = { 14, 135, 70, 201, 107, 236, 123 };
inline constexpr unsigned char kTrakt_Id_B[]       = { 55, 25, 158, 48, 220, 10 };
inline constexpr int           kTrakt_Id_ALen      = 7;
inline constexpr int           kTrakt_Id_BLen      = 6;

// plaintext: "TEST-TRAKT-SECRET" (17 bytes)
inline constexpr unsigned char kTrakt_Secret_A[]   = { 14, 135, 70, 201, 107, 236, 123, 55, 25 };
inline constexpr unsigned char kTrakt_Secret_B[]   = { 158, 48, 198, 11, 243, 115, 59, 30 };
inline constexpr int           kTrakt_Secret_ALen  = 9;
inline constexpr int           kTrakt_Secret_BLen  = 8;

// plaintext: "TEST-OPENSUBTITLES-KEY" (22 bytes)
inline constexpr unsigned char kOpenSubs_Key_A[]   = { 14, 135, 70, 201, 107, 247, 121, 51, 28, 153, 72 };
inline constexpr unsigned char kOpenSubs_Key_B[]   = { 215, 26, 249, 117, 50, 15, 129, 40, 198, 19, 241 };
inline constexpr int           kOpenSubs_Key_ALen  = 11;
inline constexpr int           kOpenSubs_Key_BLen  = 11;

// plaintext: "TEST-ANILIST-ID" (15 bytes)
inline constexpr unsigned char kAniList_Id_A[]     = { 14, 135, 70, 201, 107, 249, 103, 63 };
inline constexpr unsigned char kAniList_Id_B[]     = { 30, 131, 78, 193, 99, 249, 101 };
inline constexpr int           kAniList_Id_ALen    = 8;
inline constexpr int           kAniList_Id_BLen    = 7;

// plaintext: "TEST-ANILIST-SECRET" (19 bytes)
inline constexpr unsigned char kAniList_Secret_A[] = { 14, 135, 70, 201, 107, 249, 103, 63, 30, 131 };
inline constexpr unsigned char kAniList_Secret_B[] = { 78, 193, 99, 227, 100, 61, 24, 151, 81 };
inline constexpr int           kAniList_Secret_ALen = 10;
inline constexpr int           kAniList_Secret_BLen = 9;

// plaintext: "TEST-MAL-ID" (11 bytes)
inline constexpr unsigned char kMal_Id_A[]         = { 14, 135, 70, 201, 107, 245 };
inline constexpr unsigned char kMal_Id_B[]         = { 104, 58, 127, 131, 89 };
inline constexpr int           kMal_Id_ALen        = 6;
inline constexpr int           kMal_Id_BLen        = 5;

} // namespace eb_secrets
