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

**Never commit real credential values** — not here, not in logs, not in the
built binary as plaintext (the XOR obfuscation guarantees the latter).
