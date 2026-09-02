// ONE de-obfuscator for every build-time embedded credential (#81), and the reason it is a header rather
// than a copy in each consumer.
//
// The obfuscation formula lives in native/cmake/GenerateSecrets.cmake and is applied at CONFIGURE time; the
// reverse has to be applied at RUNTIME, and the two halves are in different languages, in different files,
// with no compiler to notice when one of them changes. That is already one drift hazard and it is documented
// where the formula is written. A SECOND runtime copy — one in AddonContext.cpp for the bundled art
// providers, one in LastFmClient.cpp for the scrobble app key — would be a drift hazard that no build and no
// probe could see: a credential de-obfuscated by a stale copy comes out as plausible-looking mojibake, is
// sent to the service, and is refused as "invalid key". So there is exactly one runtime copy, here.
//
// WHAT IT IS NOT. Not cryptography, and nothing here pretends otherwise: a rolling XOR whose key is a
// constant in the repository. Its whole job is to keep the plaintext out of a `strings` scan of the shipped
// binary, and anyone holding the binary plus this source recovers the value trivially. See
// native/secrets/README.md.
//
// THE SPLIT ARRAYS. Every embedded value is stored as TWO arrays so that no single array in the binary holds
// a contiguous credential; joining them is this function's other job. A value that was absent or blank at
// configure time embeds as a single 0 byte with BOTH lengths 0, and comes back out of here as an empty
// string — which is what every consumer tests for when it decides whether the feature exists in this build.
#pragma once
#include <QByteArray>
#include <QString>

namespace BuiltinSecret
{
    // Reverse the rolling XOR of one embedded value: join its two split arrays, then de-obfuscate byte by
    // byte. MUST mirror GenerateSecrets.cmake's formula exactly:
    //     obf[i] = plain[i] ^ KEY[i % keylen] ^ (i & 0xFF)      (i indexed WITHIN this value's blob)
    inline QString join(const unsigned char* a, int aLen, const unsigned char* b, int bLen)
    {
        const int total = aLen + bLen;
        if (total <= 0) return QString();  // absent/blank at build time -> nothing embedded

        static const unsigned char KEY[] = { 90, 195, 23, 158, 66, 189, 47, 113 };
        const int keyLen = static_cast<int>(sizeof(KEY));

        QByteArray blob;
        blob.reserve(total);
        for (int i = 0; i < total; ++i)
        {
            const unsigned char ob = (i < aLen) ? a[i] : b[i - aLen];
            const unsigned char pb = static_cast<unsigned char>((ob ^ KEY[i % keyLen]) ^ (i & 0xFF));
            blob.append(static_cast<char>(pb));
        }
        return QString::fromUtf8(blob);
    }
}
