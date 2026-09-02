# native/secrets

Build-time secrets for embedding **provider developer credentials** into the app.
Everything in this directory is git-ignored **except this README** — the real
secrets file is never committed.

## Provider credential files (game & subtitle scraping)

Each bundled game-art provider reads its *developer* credentials from a file
here, so a fresh install scrapes without the user creating accounts. Every file
is optional and every line is `key=value` (no quotes, no spaces around `=`). A
missing file or a blank value just means that provider has no builtin and falls
back to the user's addon settings.

| File | Keys | Provider |
| --- | --- | --- |
| `screenscraper.secrets` | `devid`, `devpassword` | ScreenScraper software creds. **Not** the user account (`ssid`/`sspassword`) — those stay in the addon's user settings and remain optional (they raise the rate limit). |
| `thegamesdb.secrets` | `apikey` | TheGamesDB public API key. |
| `igdb.secrets` | `clientId`, `clientSecret` | IGDB / Twitch developer app credentials. |
| `steamgriddb.secrets` | `apikey` | SteamGridDB. **Normally left blank** — SteamGridDB issues keys per user account, so a project-wide key is against the grain of their model and this provider stays user-supplied. The slot exists only so a build *may* embed one. |

## Music scrobbling (issue #192)

| File | Keys | Provider |
| --- | --- | --- |
| `lastfm.secrets` | `apikey`, `secret` | Last.fm **application** API key + shared secret. Not a user credential and not an addon credential: `core/LastFmClient.cpp` reads these two slots directly, because every Last.fm call — including `auth.getToken`, the *first* step of authorising a user — is signed with the shared secret. There is no user-supplied fallback, because a per-user API key is not the shape Last.fm's desktop-auth flow is built around. |

Example `lastfm.secrets`:

```
apikey=YOUR_LASTFM_API_KEY
secret=YOUR_LASTFM_SHARED_SECRET
```

**To fill this slot** (a one-off, and it is the repository owner's to do): create an API account at
<https://www.last.fm/api/account/create>, then write the two values it gives you into
`native/secrets/lastfm.secrets` and re-configure (`cmake -S native -B build`). Nothing else changes: the
Last.fm row in Settings switches from *"Last.fm is not available in this build"* to a working **Connect to
Last.fm** button, and the provider is installed beside ListenBrainz.

**With the slot empty** — which is every clone of this repository, and CI — the app is complete and correct:
the Last.fm provider is not installed at all, the settings row says *"Last.fm is not available in this
build"* and offers nothing else, and ListenBrainz is unaffected. `probe_scrobble` still exercises the whole
provider, against a fixture header (`native/tools/fixtures/lastfm/BuiltinSecrets.h`) carrying an obviously
fake key and an in-process loopback fake service.


Example `screenscraper.secrets`:

```
devid=YOUR_DEV_ID
devpassword=YOUR_DEV_PASSWORD
```

At **configure time**, `native/cmake/GenerateSecrets.cmake` reads each file,
obfuscates every value (rolling XOR — best-effort, *not* cryptography), and emits
`BuiltinSecrets.h` into the **build tree** (never the source tree). The app
de-obfuscates on demand — addon credentials via `AddonContext::builtinCredential()`
(which is allow-listed per addon), the Last.fm application key via
`LastFmClient::appKey()` / `appSecret()`. Both go through the single runtime
de-obfuscator in `core/BuiltinSecretBlob.h`; a second copy of that formula would
drift from this CMake script invisibly. An addon picks user-vs-builtin through
`AddonContext::selectCredential()` (user always wins; else builtin; else the
provider stays dormant).

If a file is **absent or blank**, the build still succeeds: that provider's
header slot holds an empty array, CMake prints a `STATUS` line counting how many
slots were filled, and the addon falls back to the user's addon settings.

**Note:** after adding a previously-**absent** secrets file, run a manual CMake
re-configure (`cmake -S native -B build`). `CMAKE_CONFIGURE_DEPENDS` reliably
re-runs generation when a file's mtime *changes*, but the absent→present
transition is not guaranteed to trigger a re-configure on every generator.

**Never commit real credential values** — not here, not in logs, not in the
built binary as plaintext (the XOR obfuscation guarantees the latter).

## android-release.keystore + android-keystore.pass

The **persistent Android release signing key** for the `org.everythingbox.app`
APK, and the password that unlocks it. Both are git-ignored (the `.keystore` and
`.pass` are covered by `/native/secrets/*`; only this README is committed).

**The names below still say `mmv` — deliberately.** A keystore's alias and
subject are baked into the file at creation and cannot be edited; renaming them
here would only make this document wrong and the `ANDROID_KEY_ALIAS` secret
mismatch the keystore, which fails signing. This file is exempt from the suite's
old-brand gate for that reason (see `native/tools/run-headless-probes.sh`).

**Package id changed with the rebrand.** The APK is now `org.everythingbox.app`;
it was `org.mymediavault.app`. Android identifies an app by *package id first*,
signature second, so this keystore signing the new id does **not** make the new
build an upgrade of an installed old one: devices carrying a pre-rename APK must
uninstall it (losing its app data) and install the new package fresh. Both can
sit side by side until then. The keystore itself is unchanged and still the one
to preserve.

- `android-release.keystore` — a Java keystore: RSA 4096, alias **`mmv-release`**,
  `CN=MyMediaVault`, 10000-day validity. Created with
  `keytool -genkeypair -keyalg RSA -keysize 4096 -validity 10000 -alias mmv-release`.
  Alias and CN predate the rename and stay as-is (see the note above).
- `android-keystore.pass` — the store/key password (same value for both), 32
  random alphanumeric chars, **no trailing newline**. Used as both
  `-storepass` and `-keypass`.

### GitHub Actions secrets (repo `cubman3134/EverythingBox`)

The release workflow (`.github/workflows/release.yml`, `android` job) signs each
release APK via Qt 6.8's androiddeployqt env route. Three repo secrets feed it:

| Secret | Value |
| --- | --- |
| `ANDROID_KEYSTORE_B64` | `base64 -w0` of `android-release.keystore` |
| `ANDROID_KEYSTORE_PASS` | contents of `android-keystore.pass` |
| `ANDROID_KEY_ALIAS` | `mmv-release` |

The workflow decodes the keystore to the runner and exports
`QT_ANDROID_SIGN_APK=1`, `QT_ANDROID_KEYSTORE_PATH`, `QT_ANDROID_KEYSTORE_ALIAS`,
`QT_ANDROID_KEYSTORE_STORE_PASS`, `QT_ANDROID_KEYSTORE_KEY_PASS`. The signing
step is guarded on `ANDROID_KEYSTORE_B64` being non-empty, so forks/PRs without
the secret still build an *unsigned* release APK.

To rotate/re-provision the secrets from the local files:

```
base64 -w0 android-release.keystore | gh secret set ANDROID_KEYSTORE_B64  --repo cubman3134/EverythingBox
gh secret set ANDROID_KEYSTORE_PASS --repo cubman3134/EverythingBox < android-keystore.pass
printf 'mmv-release' | gh secret set ANDROID_KEY_ALIAS --repo cubman3134/EverythingBox
```

### ⚠️ Recovery warning — DO NOT LOSE THIS KEYSTORE

Android identifies an app update by its **signing certificate**. If this exact
keystore + password is lost, you can **never ship an in-place update** to any
device (or store listing) that installed a build signed with it — users must
uninstall and reinstall, losing all app data, and any store listing is
orphaned. There is no recovery, reset, or Google-side override for a self-signed
upload key. **Back up `android-release.keystore` + `android-keystore.pass`
off-machine** (encrypted). The `ANDROID_KEYSTORE_B64` GitHub secret is a
convenience for CI, **not** a backup — secrets are write-only and cannot be read
back out.
