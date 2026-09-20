# LAN file drop (issue #115)

Upload ROMs and media to this device from any browser on the same network. Open `http://<device>:<port>/drop`,
pair once with the code the device puts on its own screen, pick a folder, drag files in.

**Off by default.** Settings ▸ General ▸ Remote control ▸ *Receive files over the network (file drop)*, on both
the themed and the classic settings surfaces.

## What it does

- **One page, served from the binary.** `GET /drop` answers with `resources/filedrop/drop.html` and nothing
  else — no query, header or path selects anything different. The page is self-contained (no external script,
  style, font or image), so it works on a LAN with no internet, and it is served with
  `Content-Security-Policy: default-src 'none'; … connect-src 'self'; frame-ancestors 'none'`.
- **Pairing is the existing one.** "Pair" calls `POST /pair` with no code, which makes the device show a code
  on its screen (exactly as Play-on-device does); typing that code redeems it for a token. The token lives in
  a JavaScript variable only — no cookie, no web storage — so a reload means pairing again.
- **Fixed destinations.** `GET /drop/destinations` lists every ROM system folder (friendly console name) plus
  the video, music and photo library roots that exist, each with an **opaque id**. The client only ever names
  an id and a bare file name; the server maps the id to a directory. **A path from the client is never used as
  a path**, and no answer or listing ever contains one.
- **Chunked and resumable.**

  | Route | Body | Answer |
  |---|---|---|
  | `POST /drop/start` | `{dest, name, size}` | `{uploadId, received}` |
  | `PUT /drop/chunk?id=&offset=` | up to 8 MiB of bytes, streamed | `{received}` |
  | `GET /drop/status?id=` | — | `{received, size}` |
  | `POST /drop/finish` | `{id[, name]}` | `{landed:true}` |

  A piece is accepted only when `offset` equals the bytes already received. Re-issuing `start` for the same
  `(dest, name, size)` returns the same upload, with what is on disk — after a dropped connection, a page
  reload, or an app restart.
- **Never half a file under its real name.** Bytes go to a hidden `.eb-drop-<uploadId>.part` file **inside the
  destination directory**, so the final move is on one filesystem. `finish` fsyncs, then renames without
  replacing (`MoveFileEx` with no `REPLACE_EXISTING` / `link()`+`unlink()`). `QFile::rename` is deliberately not
  used: its copy-with-truncate fallback would write *through* a dangling symlink sitting at the final name.
- **Then the scan.** A landed file triggers the incremental scan of that root (ROMs: the Downloaded sync;
  video and music: their rescans), so it appears within seconds. Photos are read from the folder on open.
- **Caps.** 64 GiB per file; a start is refused when the declared size — plus every other upload still
  outstanding — would leave under 1 GiB free; 8 MiB per `PUT`; 64 uploads in progress at once.
- **Sweep.** Part files untouched for 24 h are removed when file drop is turned on (including at startup, off
  the GUI thread). Only names matching `.eb-drop-<32 hex>.part` are ever candidates, and never one with a piece
  in flight.

## Security posture

- **One origin gate in front of everything (issue #423).** Before a request is routed, before the token is
  weighed and before a body byte is read, `RemoteApi::requestAllowed` decides whether this listener will
  answer it at all:

  | | accepted | refused |
  |---|---|---|
  | `Host` | an IP literal with an optional port (`192.168.1.5:8080`, `127.0.0.1`, `[::1]:8080`); `localhost[:port]`; this device's own advertised mDNS name (`PlayOn::advertisedHostName` = `<deviceId>.local`) | every other DNS name (`evil.example.com`), anything that merely ends with an accepted one (`192.168.1.5.evil.com`, `evilocalhost`, `<id>.local.evil.com`), another device's `.local`, a missing or empty `Host`, two `Host` headers, a `Host` carrying credentials, a path, a query or whitespace |
  | `Origin` | absent (a same-origin `fetch` may send none); an `http` origin whose host passes the same rule **and** names the same host and port as `Host` | every cross-origin value, `null`, a non-`http` scheme, a value with a path, two `Origin` headers |
  | `OPTIONS` | — | always `403`: no CORS preflight is answered, and **no response on this listener ever carries an `Access-Control-*` header**, so a browser cannot read these answers cross-origin either |

  A refusal is `403` with a ten-byte plain-text body that says nothing about what is served here. It happens
  before any spool or `.part` file exists, so **a refused request never touches disk**. This is what closes
  DNS rebinding: a hostile page can point a name it controls at this device's LAN address, but the request it
  then makes carries that name in `Host` and is refused — so it cannot put this device's own "type the code
  from your screen" page in front of the user. `/pair` is covered for the same reason: it is the other place
  that code is typed.
- **Token-gated.** Every `/drop` route except the page itself is in `PlayOn::routeNeedsToken` (the whole
  `/drop/` prefix), so an unpaired caller is refused 401 before anything is looked up, and — for a streamed
  piece — before a single body byte is read.
- **Name rule shared with #292.** `LibraryBundle::safePathSegment`, reused rather than copied: no separator,
  no `..`, no leading dot, no drive or stream colon, no control character, no trailing dot or space, no
  Windows device name, 255 characters.
- **Upload only.** No download, no delete, no rename of a user's file, no directory listing, no overwrite. A
  name that is already taken is refused both at `start` and again at the rename; the page then offers "rename
  and retry", which re-sends nothing.
- **The listener.** File drop rides #76's already-LAN-bound listener. If the remote control is off, turning
  file drop on opens that listener **for file drop alone**: the control and hand-off routes (`/state`,
  `/player`, `/input`, `/open`, `/inventory`, `/bundle`, `/gamelists`) answer 404, and this device does not
  advertise itself as a Play-on-device target. The toast says the listener was opened.
- **Nothing is logged that should not be.** `stream_debug.log` gets file names and counts; never a pairing
  code, never a token.

## What it deliberately does not do

- **No Android SAF destinations yet.** On Android the destinations are the folders this process can write
  directly; writing to an SD card through #112's persisted tree grants is the remaining increment of #115, so
  the PR for this work says `Refs #115`.
- **No file manager.** It cannot show you what is on the device, fetch a file back, delete, rename or make a
  folder. That is what keeps the surface reviewable.
- **No server-side extraction.** Archives land as they are; `ArchiveRom` already opens zip/7z at launch time.
- **No HTTPS and no accounts.** It is a LAN tool: a plain HTTP listener, one pairing credential, off by
  default. Do not port-forward it.
- **One piece at a time per upload.** A second `PUT` for the same upload while one is in flight is refused
  ("busy"); the page backs off and resumes.
- **A dangling-symlink race remains on filesystems without hard links** (FAT/exFAT): there the landing falls
  back to `rename()` after an `lstat`, which is checked-then-acted rather than atomic. The same caveat applies
  to two uploads racing on one name: on NTFS and on Linux the no-replace rename is atomic, so the second is
  refused `exists`; on FAT/exFAT it degrades to check-then-rename, and a sufficiently tight race could clobber.
- **No cross-origin access, and no CORS.** The origin gate above is not a CORS policy: nothing is ever
  permitted cross-origin, so no `Access-Control-*` header is sent and a preflight is refused like anything
  else.

## Where the code is

| | |
|---|---|
| `native/src/core/FileDrop.{h,cpp}` | the pure core: destinations, names, the start/piece/finish state machine, caps, sweep, the durable landing |
| `native/src/core/RemoteApi.{h,cpp}` | routing (`/drop*`), `PUT`, the streaming plan for a piece |
| `native/src/core/RemoteServer.{h,cpp}` | the routes, the streamed piece, the control-surface switch |
| `native/src/core/PlayOnDevice.cpp` | `routeNeedsToken`, `advertisedHostName` (the one `.local` name the Host gate accepts) |
| `native/resources/filedrop/drop.html` | the page (embedded through `qt_add_resources`) |
| `native/src/ui/MainWindowFileDrop.cpp` | the destination roots, the sweep, the scan after landing, the settings toggle |
| `native/tools/probe_filedrop.cpp` | the probe, including the real-socket 200 MiB upload, the resumed one, and the rebinding-`Host` refusals |
| `native/tools/probe_remoteapi.cpp` | the origin gate's whole accept/refuse table (#423) |
