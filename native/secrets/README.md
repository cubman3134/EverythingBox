# native/secrets

Build-time secrets for embedding **provider developer credentials** into the app.
Everything in this directory is git-ignored **except this README** — the real
secrets file is never committed.

## screenscraper.secrets

The ScreenScraper *developer* credentials (`devid` / `devpassword`) that the
bundled ScreenScraper addon uses to authenticate against the jeuInfos.php API.
These are **not** the end user's account (`ssid`/`sspassword`) — those stay in
the addon's user settings.

Create `native/secrets/screenscraper.secrets` with exactly this format (two
lines, `key=value`, no quotes, no spaces around `=`):

```
devid=YOUR_DEV_ID
devpassword=YOUR_DEV_PASSWORD
```

At **configure time**, `native/cmake/GenerateSecrets.cmake` reads this file,
obfuscates the bytes (rolling XOR — best-effort, *not* cryptography), and emits
`BuiltinSecrets.h` into the **build tree** (never the source tree). The app
de-obfuscates on demand via `AddonContext::builtinCredential()`.

If this file is **absent**, the build still succeeds: the generated header holds
empty arrays, CMake prints a loud `STATUS` line, and the addon silently falls
back to whatever `devid`/`devpassword` the user set in the addon settings.

**Note:** after adding a previously-**absent** secrets file, run a manual CMake
re-configure (`cmake -S native -B build`). `CMAKE_CONFIGURE_DEPENDS` reliably
re-runs generation when the file's mtime *changes*, but the absent→present
transition is not guaranteed to trigger a re-configure on every generator.

**Never commit real credential values** — not here, not in logs, not in the
built binary as plaintext (the XOR obfuscation guarantees the latter).

## android-release.keystore + android-keystore.pass

The **persistent Android release signing key** for the `org.mymediavault.app`
APK, and the password that unlocks it. Both are git-ignored (the `.keystore` and
`.pass` are covered by `/native/secrets/*`; only this README is committed).

- `android-release.keystore` — a Java keystore: RSA 4096, alias **`mmv-release`**,
  `CN=MyMediaVault`, 10000-day validity. Created with
  `keytool -genkeypair -keyalg RSA -keysize 4096 -validity 10000 -alias mmv-release`.
- `android-keystore.pass` — the store/key password (same value for both), 32
  random alphanumeric chars, **no trailing newline**. Used as both
  `-storepass` and `-keypass`.

### GitHub Actions secrets (repo `cubman3134/MyMediaVault`)

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
base64 -w0 android-release.keystore | gh secret set ANDROID_KEYSTORE_B64  --repo cubman3134/MyMediaVault
gh secret set ANDROID_KEYSTORE_PASS --repo cubman3134/MyMediaVault < android-keystore.pass
printf 'mmv-release' | gh secret set ANDROID_KEY_ALIAS --repo cubman3134/MyMediaVault
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
