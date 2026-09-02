// A FIXTURE stand-in for the build-tree BuiltinSecrets.h, used by probe_scrobble and by nothing else.
//
// WHY THE PROBE DOES NOT JUST USE THE REAL ONE. The real header is generated at configure time from
// native/secrets/lastfm.secrets, which is git-ignored and absent in every clone and on CI — so in the build
// that actually runs the gate, both Last.fm slots are EMPTY. A probe compiled against that can assert the
// "not available in this build" arm and nothing else: no signature over a real key, no auth exchange, no
// scrobble, no love. Every interesting thing this provider does would be untested in exactly the build where
// it matters.
//
// WHY NOT A RUNTIME SETTER INSTEAD. A `setAppCredentialsForTests()` on LastFmClient would be a second way for
// something in the shipped binary to choose which application key is used, and it would skip the
// de-obfuscation entirely — so the one piece of this that CANNOT be checked by a compiler (the runtime XOR
// mirroring GenerateSecrets.cmake's, byte for byte, in a different language) would be the one piece never
// exercised. Substituting the HEADER keeps the whole real path: the same arrays, the same lengths, the same
// BuiltinSecret::join, the same everything above it.
//
// THE VALUES ARE NOT CREDENTIALS. They are the literal strings below, they are meaningless to Last.fm, and
// the probe only ever sends them to a QTcpServer it opened on 127.0.0.1 itself (LastFmClient::apiRoot's test
// hook refuses anything that is not loopback). §7 of probe_scrobble asserts that neither of them appears in
// any message the app would show or log.
//
// HOW THE BYTES WERE PRODUCED — the same formula as native/cmake/GenerateSecrets.cmake, which is the point:
//     obf[i] = plain[i] ^ KEY[i % 8] ^ (i & 0xFF),  KEY = { 90, 195, 23, 158, 66, 189, 47, 113 }
// then split in half across the A and B arrays. probe_scrobble asserts that de-obfuscating these arrays
// yields the plaintext below EXACTLY, which is what pins the C++ reverse against the CMake forward: if
// somebody edits one of the two formulas, this assertion is what notices.
#pragma once

namespace eb_secrets {

// plaintext: "probe-not-a-real-key" (20 bytes)
inline constexpr unsigned char kLastFm_Key_A[]     = { 42, 176, 122, 255, 35, 149, 71, 25, 38, 231 };
inline constexpr unsigned char kLastFm_Key_B[]     = { 124, 184, 60, 213, 64, 18, 103, 185, 96, 244 };
inline constexpr int           kLastFm_Key_ALen    = 10;
inline constexpr int           kLastFm_Key_BLen    = 10;

// plaintext: "probe-not-a-real-secret" (23 bytes)
inline constexpr unsigned char kLastFm_Secret_A[]  = { 42, 176, 122, 255, 35, 149, 71, 25, 38, 231, 124, 184 };
inline constexpr unsigned char kLastFm_Secret_B[]  = { 60, 213, 64, 18, 103, 161, 96, 238, 36, 205, 77 };
inline constexpr int           kLastFm_Secret_ALen = 12;
inline constexpr int           kLastFm_Secret_BLen = 11;

} // namespace eb_secrets
