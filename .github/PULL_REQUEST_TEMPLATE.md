## What changed

<!-- The change itself, in plain terms. Link the issue if there is one. -->

## Why

<!-- What was wrong or missing. If you worked around something surprising, say
     what surprised you — that context is the part a future reader can't
     reconstruct from the diff. -->

## How it was verified

<!-- What you actually ran or clicked, and what you saw. "Builds clean" is not
     verification. Screenshots for anything visual; for a TV-surface change, say
     whether you drove it with a controller/D-pad, since arrow-key-only testing
     misses a whole class of navigation bugs. -->

## Probes

<!-- Which probes cover this change, and whether you added one. A new pure
     component should come with a probe registered in all three places: its
     add_executable in native/CMakeLists.txt, the runner list in
     native/tools/run-headless-probes.sh, and the --target list in
     .github/workflows/ci.yml. An unregistered probe silently never runs. -->

- Probes run:
- Probe added: <!-- name, or "none — no new pure component" -->

## Checklist

- [ ] `BUILD_DIR=build bash native/tools/run-headless-probes.sh` prints `ALL HEADLESS PROBES PASSED` locally
- [ ] No new `QDialog` / `QMessageBox` / `QInputDialog` / top-level window — modal UI goes through `src/ui/nav`
- [ ] Any user-facing setting was added to **both** builders (themed and classic)
- [ ] Commit messages use a conventional prefix (`feat:` / `fix:` / `docs:` / `refactor:`)
