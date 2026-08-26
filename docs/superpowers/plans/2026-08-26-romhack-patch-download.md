# Romhack patch download Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let a romhack patch's own HTTP response decide how it is fetched — under 16 MiB it finishes inline exactly as today, over 16 MiB it streams through `DownloadManager` with resume, progress and cancel, and never appears in Recent or the Downloaded folder.

**Architecture:** One new pure-ish unit, `BoundedFetch`, performs a single GET and aborts it the moment the response reveals it is over a caller-supplied ceiling — reading `Content-Length` at the first `readyRead` (the first point every redirect hop is behind us) and falling back to a running byte count when no length is declared. `MainWindow::showRomhacks` calls it instead of `fetchUrlBlocking`, writes the result to a stable cache path, and on `TooBig` enqueues a non-recording `DownloadJob` for the same URL, continuing the install from a one-shot `jobCompleted` handler. `PendingRomhack` stops carrying `QByteArray` and carries the cache path.

**Tech Stack:** C++17, Qt 6.8.3 (Core + Network), CMake + MSVC, headless `probe_*` executables driven by `native/tools/run-headless-probes.sh`.

**Spec:** [`docs/superpowers/specs/2026-08-26-romhack-patch-download-design.md`](../specs/2026-08-26-romhack-patch-download-design.md)

### One deliberate divergence from the spec

The spec's §5 says the queued-patch route gets "its own slot and one-shot, `pendingRomhackPatch_` and
`romhackPatchConn_`, mirroring the base-ROM pair". Task 5 does **not** do that. It uses the shape the
finished-ROM route uses instead: a heap-allocated `QMetaObject::Connection` owned by the connection
itself, with the request captured by value, plus the `romhackPatchDownloads_` set.

The spec's stated reason for a separate slot — that the patch handler and the base-ROM handler can be
live in sequence for one install, so they must not share — is satisfied either way. But a member slot
also makes **two different hacks** queued at the same time mutually exclusive, for no reason, which is
exactly the fault the `romhackRomDownloads_` comment already records against a single-slot design. A
self-owned one-shot has neither problem, and it means the two queued routes in this function are the
same shape rather than two shapes doing one job.

`pendingRomhack_`/`pendingRomhackConn_` (the base-ROM pair) are untouched.

## Global Constraints

- **Public repo.** Name no content source and no site, in code, comments, commit messages or test fixtures. Fixture hosts are `127.0.0.1` only.
- **No AI attribution** in any commit message. No `Co-Authored-By`, no generated-by line, no tool name. Conventional prefixes (`feat:`, `fix:`, `docs:`, `refactor:`) apply.
- **All UI goes through the nav kit** in `src/ui/nav` (`NavMenu`, `NavConfirm`, `notify`). Never `QDialog`, `QMessageBox`, `QInputDialog`, or a top-level window.
- **Never open a nested event loop inside a signal emission.** Anything reaching `NavConfirm::ask` from a `jobCompleted` handler must go through `MainWindow::deferPastQmlEmission` first. This is crash class #28.
- **Byte-exact edits.** `native/src/ui/MainWindow.cpp` and `native/tools/probe_*.cpp` are **CRLF**; `native/CMakeLists.txt` is CRLF **with a lone CR hidden in it**; `native/tools/run-headless-probes.sh` is CRLF. Use the `Edit` tool for targeted replacements. Never rewrite one of these files wholesale, never run a formatter over one, and never normalise line endings — it breaks the build or the gate silently.
- **`git config core.autocrlf` is `true`** in this repo. The warning `LF will be replaced by CRLF` on commit is normal; a diff that shows every line changed is not — if you see one, you normalised a file and must undo it.
- **Ceiling is 16 MiB** = `16 * 1024 * 1024`, a named constant at the call site. `BoundedFetch` takes it as a parameter and holds no policy of its own.
- **Inline patch deadline is 60 s** (down from 180 s).
- **Logging discipline.** Any log line goes through `mwLog(...)` with URLs passed through `logSafeUrl(...)`, both already at the top of `MainWindow.cpp`. The log-discipline gate matches call sites by the `…Log(` name shape, so a helper under another name is a hole in it. `BoundedFetch` itself logs nothing.

### Build and test commands

Configure once per worktree (the main repo's `build/` points at the main source tree):

```bash
git submodule update --init external/RetroPark
```

```bash
cmake -S native -B build -G "Visual Studio 18 2026" -A x64 -DEVERYTHINGBOX_BUILD_APP=ON -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64 -DMPV_INCLUDE_DIR=C:/mpv-dev/include -DMPV_LIBRARY=C:/mpv-dev/libmpv.lib
```

Keep `EVERYTHINGBOX_BUILD_APP=ON` even when building only a probe: with `OFF` the configure succeeds but generates 2 probe targets instead of 51, and `--target probe_romhack` then fails with `MSB1009`. The configure line `data-dir isolation applied to N probe target(s)` is the tell — `N == 2` means reconfigure.

Build a probe (never run a target-less `cmake --build build` — it builds 52 harnesses):

```bash
cmake --build build --config Release --target probe_romhack --parallel
```

Run one probe (exit 127 means a missing Qt DLL, not a test failure):

```bash
PATH="/c/Qt/6.8.3/msvc2022_64/bin:$PATH" QT_QPA_PLATFORM=offscreen QT_PLUGIN_PATH=/c/Qt/6.8.3/msvc2022_64/plugins ./build/Release/probe_romhack.exe
```

Run the whole gate before the final commit of the last task:

```bash
BUILD_DIR=build bash native/tools/run-headless-probes.sh | tail -3
```

## File Structure

| File | Responsibility | Task |
|---|---|---|
| `native/src/core/BoundedFetch.h` (create) | The verdict type and the one entry point | 1 |
| `native/src/core/BoundedFetch.cpp` (create) | One GET, the ceiling decision, the abort | 1 |
| `native/CMakeLists.txt` (modify) | Add `BoundedFetch` to the app target and to `probe_romhack`; give `probe_romhack` `Qt6::Network` | 1 |
| `native/tools/probe_romhack.cpp` (modify) | New final section: a loopback server driving `BoundedFetch` | 1 |
| `native/src/core/DownloadManager.h` (modify) | `DownloadJob::record` | 2 |
| `native/src/core/DownloadManager.cpp` (modify) | Persist and restore `record` | 2 |
| `native/tools/probe_stremio.cpp` (modify) | `record` round-trips through `queue.json` | 2 |
| `native/src/ui/MainWindow.h` (modify) | `PendingRomhack::patchPath`; the patch-route slot, connection and in-flight set; three new private methods | 3, 5, 6 |
| `native/src/ui/MainWindow.cpp` (modify) | The `record` opt-out; the cache path helpers; the reorder; the `BoundedFetch` call and the queue route; the prune | 2–6 |

---

### Task 1: `BoundedFetch` — a GET that judges its own size

**Files:**
- Create: `native/src/core/BoundedFetch.h`
- Create: `native/src/core/BoundedFetch.cpp`
- Modify: `native/CMakeLists.txt:726` (app target sources), `native/CMakeLists.txt:1259-1272` (`probe_romhack` target)
- Test: `native/tools/probe_romhack.cpp` (new final section)

**Interfaces:**
- Consumes: nothing.
- Produces: `BoundedFetch::Result` with fields `verdict` (`Ok` | `TooBig` | `Failed`), `body` (`QByteArray`), `declared` (`qint64`, `-1` when the response stated no length), `read` (`qint64`, bytes actually received), `status` (`int`, HTTP status or `0`), `error` (`QString`); and `BoundedFetch::Result BoundedFetch::get(const QString& url, int timeoutMs, qint64 ceilingBytes)`. Task 5 is the only consumer.

- [ ] **Step 1: Create the header**

Create `native/src/core/BoundedFetch.h`:

```cpp
// One blocking GET that refuses to become a large download. It reads the response's own declaration of its
// size and stops as soon as that declaration — or the bytes themselves — cross a ceiling the CALLER sets.
//
// It exists because "fetch this small file" and "download this large one" are different operations with
// different UI, and the only thing that can tell them apart is the response. A file's NAME cannot: a romhack
// patch's format is asserted by its source and never sniffed, and a two-byte tweak and a disc-scale rebuild
// arrive under the same extension. A HEAD would answer the question at the cost of a round trip on every
// call, for the common case that needs none, and only where the route answers HEAD at all.
//
// So the request judges itself. Under the ceiling this is exactly a blocking fetch and the body is returned.
// Over it, the transfer is ABANDONED and the caller is told so — the point being that the caller can then
// hand the same url to something built for large transfers, having spent one response head rather than the
// whole file.
//
// Holds no policy: the ceiling and the deadline are parameters, and nothing here logs. What counts as "too
// big", what to say about it, and where a refused url goes next all belong to the call site.
#pragma once
#include <QByteArray>
#include <QString>

namespace BoundedFetch
{
    struct Result
    {
        enum Verdict
        {
            Ok,      // the whole body arrived and fitted; `body` is it
            TooBig,  // the response was over the ceiling and was abandoned; `body` is empty
            Failed,  // no response, a refused one, or the deadline; `body` is empty
        };

        Verdict    verdict  = Failed;
        QByteArray body;          // the complete body, and ONLY when verdict == Ok
        // What the RESPONSE said about its own length, or -1 when it declared none. Reported rather than
        // inferred: "the server said 500 MB" and "the server said nothing and we stopped at the ceiling" are
        // different facts, and only the first can be put in a sentence.
        qint64     declared = -1;
        // Bytes actually received before the verdict was reached, whatever the verdict. This is the property
        // that makes the ceiling testable: a TooBig returned after quietly reading the whole body is the
        // exact bug this unit exists to prevent, and it is indistinguishable from a correct one without this.
        qint64     read     = 0;
        int        status   = 0;  // HTTP status, or 0 when no response head ever arrived
        QString    error;         // Qt's reason, for a log — never for a user; one of its values is
                                  // "Operation canceled", which is the one thing this must not be read as
    };

    // Fetch `url`, giving up after `timeoutMs`, refusing anything over `ceilingBytes`.
    //
    // Redirects are followed (NoLessSafeRedirectPolicy). That is a decision and not a default left in place:
    // the request carries no headers, no cookies and no credentials, so a hop leaks nothing, and a server
    // behind a reverse proxy or a CDN needs the hop followed or its files are simply unreachable.
    Result get(const QString& url, int timeoutMs, qint64 ceilingBytes);
}
```

- [ ] **Step 2: Write the failing test**

Append this section to `native/tools/probe_romhack.cpp`, immediately **before** the closing `QDir(root).removeRecursively();` at the end of `main`. Add these includes to the file's include block first:

```cpp
#include "BoundedFetch.h"

#include <QEventLoop>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <functional>
#include <memory>
```

Then, above `int main(...)` (near the other file-scope helpers), add the loopback server. It is a trimmed copy of the one in `probe_stremio.cpp` — per-probe servers are this tree's convention (`probe_serversync`, `probe_docbridge`, `probe_manual` and `probe_scrobble` each carry their own), and this copy keeps only what a size decision needs:

```cpp
// ---- the loopback half of the BoundedFetch section --------------------------------------------
// A minimal HTTP responder, one answer per connection. Binds an EPHEMERAL port — listen(…, 0) — so there is
// no fixed port to be unlucky with on a busy CI box.
//
// `pieces` is what makes the ceiling testable at all: it writes the body in several parts with the event loop
// turning in between, so the reply raises more than one readyRead. A single-shot body would let a fetch that
// only ever checks its size ONCE still look correct, which is precisely the mistake being guarded against.
struct Loopback
{
    QTcpServer srv;
    std::function<QList<QByteArray>(const QByteArray& path)> pieces;
    // A path whose socket is written to and then simply LEFT OPEN. Without it there is no way to test a
    // deadline at all: a server that closes when it runs out of pieces produces a prompt
    // RemoteHostClosedError, which is a different failure reaching the same verdict by a route that proves
    // nothing about the timeout.
    QByteArray stallPath;

    static void writePieces(QTcpSocket* c, std::shared_ptr<QList<QByteArray>> parts, int i, bool keepOpen)
    {
        if (!c || c->state() != QAbstractSocket::ConnectedState) return;
        if (i >= parts->size())
        {
            c->flush();
            if (!keepOpen) c->disconnectFromHost();
            return;
        }
        c->write(parts->at(i));
        c->flush();
        QTimer::singleShot(10, c, [c, parts, i, keepOpen] { writePieces(c, parts, i + 1, keepOpen); });
    }

    bool start()
    {
        if (!srv.listen(QHostAddress::LocalHost, 0)) return false;
        QObject::connect(&srv, &QTcpServer::newConnection, &srv, [this] {
            QTcpSocket* c = srv.nextPendingConnection();
            if (!c) return;
            auto buf = std::make_shared<QByteArray>();
            auto answered = std::make_shared<bool>(false);
            QObject::connect(c, &QTcpSocket::readyRead, c, [this, c, buf, answered] {
                buf->append(c->readAll());
                const int end = buf->indexOf("\r\n\r\n");
                if (end < 0 || *answered) return;
                *answered = true;
                const QByteArray path = buf->left(end).split('\n').value(0).trimmed().split(' ').value(1);
                writePieces(c, std::make_shared<QList<QByteArray>>(pieces(path)), 0,
                            !stallPath.isEmpty() && path == stallPath);
            });
            QObject::connect(c, &QTcpSocket::disconnected, c, &QObject::deleteLater);
        });
        return true;
    }

    QString url(const QString& path) const
    { return QStringLiteral("http://127.0.0.1:%1%2").arg(srv.serverPort()).arg(path); }
};

// One head, with or without a Content-Length. Split from the body so a response can DECLARE a length it never
// delivers. "No Content-Length" is spelled as CHUNKED rather than as a body ended by the connection closing:
// chunked is what a real server without a length actually sends, and it ends the body definitively, where a
// close-delimited body leaves "the transfer finished" and "the peer went away" as the same event.
static QByteArray head200(qint64 declaredLength)
{
    QByteArray h = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n";
    if (declaredLength >= 0) h += "Content-Length: " + QByteArray::number(declaredLength) + "\r\n";
    else                     h += "Transfer-Encoding: chunked\r\n";
    h += "\r\n";
    return h;
}
static QByteArray chunked(const QByteArray& data)
{ return QByteArray::number(data.size(), 16) + "\r\n" + data + "\r\n"; }
static QByteArray chunkedEnd() { return QByteArray("0\r\n\r\n"); }
```

Now the section itself:

```cpp
    // ---------------------------------- 8. BoundedFetch: the response decides how it should be fetched
    // A romhack patch is a few kilobytes or it is disc-scale, and nothing but the response can say which.
    // What is pinned here is the DECISION and its cost: a refusal that arrives after the whole body has been
    // read is not a refusal, it is the bug with a different return value — so every over-ceiling case
    // asserts the BYTE COUNT and not merely the verdict.
    {
        const qint64 kCeiling = 4096;          // small, so a fixture stays a fixture; the app's is 16 MiB

        Loopback lb;
        const QByteArray small(1000, 'a');
        const QByteArray big(60000, 'b');
        lb.stallPath = "/stall";
        lb.pieces = [&](const QByteArray& path) -> QList<QByteArray> {
            if (path == "/small") return { head200(small.size()), small };
            // Sliced, and that is not decoration. A 60 KB body written in ONE piece arrives in ONE readyRead,
            // so `read` would be 60000 however early the decision was made and the "it stopped" assertion
            // below could not fail. Six-kilobyte slices with the loop turning between them are what make the
            // difference between deciding at the head and deciding at the end observable at all.
            if (path == "/big")
            {
                QList<QByteArray> parts{ head200(big.size()) };
                for (int i = 0; i < 10; ++i) parts << big.mid(i * 6000, 6000);
                return parts;
            }
            if (path == "/small-undeclared") return { head200(-1), chunked(small), chunkedEnd() };
            if (path == "/big-undeclared")
            {
                QList<QByteArray> parts{ head200(-1) };
                for (int i = 0; i < 10; ++i) parts << chunked(QByteArray(6000, 'c'));
                parts << chunkedEnd();
                return parts;
            }
            if (path == "/missing") return { QByteArray("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n") };
            if (path == "/stall")   return { head200(1000000) };   // head only, and the socket stays open
            return { QByteArray("HTTP/1.1 500 Server Error\r\nContent-Length: 0\r\n\r\n") };
        };
        CHECK(lb.start());

        // (a) Declared, under the ceiling: an ordinary fetch, and the body is byte-exact.
        {
            const BoundedFetch::Result r = BoundedFetch::get(lb.url(QStringLiteral("/small")), 10000, kCeiling);
            CHECK(r.verdict == BoundedFetch::Result::Ok);
            CHECK(r.body == small);
            CHECK(r.declared == small.size());
            CHECK(r.status == 200);
        }

        // (b) Declared, over the ceiling: refused, and refused AT THE HEAD. The byte count is the assertion —
        // a verdict-only check passes just as happily on an implementation that read all 60 000 bytes first.
        {
            const BoundedFetch::Result r = BoundedFetch::get(lb.url(QStringLiteral("/big")), 10000, kCeiling);
            CHECK(r.verdict == BoundedFetch::Result::TooBig);
            CHECK(r.declared == big.size());          // the fact the caller can put in a sentence
            // It STOPPED; it did not merely disapprove afterwards. One 6 KB slice is what a decision made at
            // the head costs; the bound allows a couple to coalesce and still fails an implementation that
            // read the body out before looking at its length.
            CHECK(r.read < 20000);
            CHECK(r.body.isEmpty());                  // and it hands back nothing it refused
        }

        // (c) No Content-Length at all, under the ceiling: still an ordinary fetch. `declared` stays -1,
        // which is the difference between "the server said" and "we found out".
        {
            const BoundedFetch::Result r =
                BoundedFetch::get(lb.url(QStringLiteral("/small-undeclared")), 10000, kCeiling);
            CHECK(r.verdict == BoundedFetch::Result::Ok);
            CHECK(r.body == small);
            CHECK(r.declared == -1);
        }

        // (d) No Content-Length, over the ceiling: the running count catches it mid-stream. This is the case
        // a head-only implementation gets wrong — it has nothing to read, so it reads everything.
        {
            const BoundedFetch::Result r =
                BoundedFetch::get(lb.url(QStringLiteral("/big-undeclared")), 10000, kCeiling);
            CHECK(r.verdict == BoundedFetch::Result::TooBig);
            CHECK(r.declared == -1);
            CHECK(r.read > kCeiling);                 // it had to cross the line to know
            CHECK(r.read < 20000);                    // …and stopped there rather than finishing
            CHECK(r.body.isEmpty());
        }

        // (e) A refusal is a failure, and it says which one. `status` is what lets the caller tell "the server
        // answered and the file is gone" from "the server never answered" — two different things to do next.
        {
            const BoundedFetch::Result r = BoundedFetch::get(lb.url(QStringLiteral("/missing")), 10000, kCeiling);
            CHECK(r.verdict == BoundedFetch::Result::Failed);
            CHECK(r.status == 404);
            CHECK(r.body.isEmpty());
        }

        // (f) A head that arrives and a body that never does: the deadline ends it, and it ends NEAR the
        // deadline rather than hanging. The elapsed check is the half that matters — a call that returns the
        // right verdict after blocking forever has not passed.
        {
            QElapsedTimer t; t.start();
            const BoundedFetch::Result r = BoundedFetch::get(lb.url(QStringLiteral("/stall")), 1200, kCeiling);
            CHECK(r.verdict == BoundedFetch::Result::Failed);
            CHECK(t.elapsed() >= 1000);
            CHECK(t.elapsed() < 8000);
            CHECK(r.body.isEmpty());
            // The property the CALLER's two messages hang off: a head that said 200 and then stalled must not
            // arrive looking like a refusal, or a timeout would be reported as "the file is gone from the
            // server" and send someone to re-open a chooser that was never the problem.
            CHECK(r.status < 400);
        }

        // (g) An unreachable host is a failure with no status at all — nothing answered, so there is nothing
        // to report about what it said. The control for (e): without this, `status == 0` and `status == 404`
        // could both be produced by a stub that never sets it.
        {
            const BoundedFetch::Result r =
                BoundedFetch::get(QStringLiteral("http://127.0.0.1:1/nothing"), 3000, kCeiling);
            CHECK(r.verdict == BoundedFetch::Result::Failed);
            CHECK(r.status == 0);
        }
    }
```

Add `#include <QElapsedTimer>` to the probe's include block for case (f).

- [ ] **Step 3: Wire the new unit and the probe's new link requirement into CMake**

In `native/CMakeLists.txt`, in the **app target's** source list, immediately after line 726 (`src/core/RomhackClient.cpp src/core/RomhackClient.h`), add:

```cmake
        src/core/BoundedFetch.cpp src/core/BoundedFetch.h
```

Then replace the `probe_romhack` block (lines 1259-1272). Its comment currently claims no `Qt6::Network`, which stops being true here, so the comment changes with the code:

```cmake
    # Installing a romhack as a playable game (RomhackInstall): verify -> apply -> write into the ROMs folder,
    # with the original left untouched and a refused patch writing nothing. Plus BoundedFetch, the decision
    # that sends a disc-scale patch to the download queue instead of buffering it — which is why this target
    # links Qt6::Network where it once did not: the decision is about a real RESPONSE, and cannot be reached
    # from a request that was never sent. The last section puts a loopback server on an ephemeral port in
    # front of it. Metadata writing is still deliberately NOT in RomhackInstall, so nothing else is pulled in.
    add_executable(probe_romhack tools/probe_romhack.cpp
        src/core/RomhackInstall.cpp src/core/RomhackInstall.h
        src/core/RomhackClient.cpp src/core/RomhackClient.h
        src/core/BoundedFetch.cpp src/core/BoundedFetch.h
        src/core/RomPatch.cpp src/core/RomPatch.h
        src/core/Settings.cpp src/core/Settings.h
        src/theme2/FormFactor.cpp src/theme2/FormFactor.h
        src/core/LifecyclePolicy.h
        src/browse/RomhackTarget.h src/core/SystemCatalog.h
        src/browse/RemoteLeafResolve.h)   # the remote-leaf id->title+console fallback
    target_include_directories(probe_romhack PRIVATE src src/core src/browse src/theme2)
    target_link_libraries(probe_romhack PRIVATE Qt6::Core Qt6::Network)
```

No probe registration changes: `probe_romhack ROMHACK-OK` is already in `native/tools/run-headless-probes.sh:676`, and that file must not be touched (it is CRLF and the gate depends on it).

- [ ] **Step 4: Run the test to verify it fails**

```bash
cmake --build build --config Release --target probe_romhack --parallel
```

Expected: **FAIL to compile**, with `Cannot open include file: 'BoundedFetch.h'` — the header exists but `BoundedFetch.cpp` does not, so the link would fail too. This is the failing state; do not proceed until you have seen it.

- [ ] **Step 5: Write the implementation**

Create `native/src/core/BoundedFetch.cpp`:

```cpp
#include "BoundedFetch.h"

#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QScopedPointer>
#include <QTimer>
#include <QUrl>
#include <QVariant>

namespace BoundedFetch
{

Result get(const QString& url, int timeoutMs, qint64 ceilingBytes)
{
    Result r;

    QNetworkAccessManager nam;
    QNetworkRequest rq{ QUrl(url) };
    rq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QScopedPointer<QNetworkReply> reply(nam.get(rq));

    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);

    // WE aborted, so the OperationCanceledError this produces is an answer and not a failure. The same
    // distinction DownloadManager has to draw with redirectRefused_, and for the same reason: Qt reports a
    // deliberate abort with the identical error the user's own Cancel produces.
    bool overCeiling = false;
    bool headRead = false;

    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply.data(), &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(reply.data(), &QNetworkReply::readyRead, reply.data(), [&] {
        if (overCeiling) return;                       // already abandoned; drain nothing further
        const QByteArray chunk = reply->readAll();
        r.read += chunk.size();

        // Read the declaration ONCE, and here rather than in metaDataChanged. metaDataChanged fires once per
        // redirect hop, so a 3xx head can be mistaken for the real one; body bytes only ever arrive on the
        // final response, so by the first readyRead the head being read is the head that describes these
        // bytes. An absent Content-Length leaves `declared` at -1 — toLongLong() on an invalid QVariant is
        // 0, which would read as "the server declared an empty body" and let anything through.
        if (!headRead)
        {
            headRead = true;
            const QVariant cl = reply->header(QNetworkRequest::ContentLengthHeader);
            if (cl.isValid()) r.declared = cl.toLongLong();
        }

        // One predicate for both cases. A declared length answers at byte zero; an undeclared one answers
        // when the bytes themselves cross the line. Nothing about a chunked or connection-delimited response
        // needs a branch of its own.
        if (r.declared > ceilingBytes || r.read > ceilingBytes)
        {
            overCeiling = true;
            r.body.clear();          // hand back nothing we refused, so a caller cannot half-use it
            reply->abort();
            return;
        }
        r.body.append(chunk);
    });

    deadline.start(timeoutMs);
    loop.exec();

    r.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    if (overCeiling) { r.verdict = Result::TooBig; return r; }

    if (!reply->isFinished())
    {
        reply->abort();
        r.body.clear();
        r.error = QStringLiteral("deadline");
        r.verdict = Result::Failed;
        return r;
    }
    if (reply->error() != QNetworkReply::NoError)
    {
        r.body.clear();          // a 4xx still has a body, and it is an error page, not the file
        r.error = reply->errorString();
        r.verdict = Result::Failed;
        return r;
    }

    r.verdict = Result::Ok;
    return r;
}

} // namespace BoundedFetch
```

- [ ] **Step 6: Run the test to verify it passes**

```bash
cmake --build build --config Release --target probe_romhack --parallel
```

```bash
PATH="/c/Qt/6.8.3/msvc2022_64/bin:$PATH" QT_QPA_PLATFORM=offscreen QT_PLUGIN_PATH=/c/Qt/6.8.3/msvc2022_64/plugins ./build/Release/probe_romhack.exe
```

Expected: `ROMHACK-OK`, exit 0. Any `ROMHACK-FAIL <cond> (<line>)` names the assertion and its line.

- [ ] **Step 7: Commit**

```bash
git add native/src/core/BoundedFetch.h native/src/core/BoundedFetch.cpp native/CMakeLists.txt native/tools/probe_romhack.cpp
```

```bash
git commit -m "feat: a GET that refuses to become a large download

BoundedFetch performs one GET and abandons it as soon as the response says
it is over a ceiling the caller sets - reading Content-Length at the first
readyRead, which is the first point every redirect hop is behind us, and
falling back to a running byte count when no length is declared.

The romhack patch fetch needs this because nothing but the response can say
whether a patch is two kilobytes or disc-scale: the format is asserted by its
source and never sniffed, and both arrive under the same extension.

probe_romhack gains a loopback server and asserts the BYTE COUNT on every
over-ceiling case, not just the verdict - a refusal returned after reading the
whole body is the bug this exists to prevent, and the two are indistinguishable
otherwise. It links Qt6::Network for the first time, because the decision is
about a real response and cannot be reached from an unsent request."
```

---

### Task 2: `DownloadJob::record` — a job that streams but is not a download

**Files:**
- Modify: `native/src/core/DownloadManager.h:20-45` (the `DownloadJob` struct)
- Modify: `native/src/core/DownloadManager.cpp:471-490` (`save`), `native/src/core/DownloadManager.cpp:492-515` (`load`)
- Modify: `native/src/ui/MainWindow.cpp:663-667` (the recording handler)
- Test: `native/tools/probe_stremio.cpp` (§18, the queue.json persistence section)

**Interfaces:**
- Consumes: nothing.
- Produces: `bool DownloadJob::record` — default `true`; when `false`, the completion handler in `MainWindow` writes nothing to `RecentStore` or `DownloadsStore` and shows no "Downloaded" toast. Persisted in `queue.json` as `"record"`, restored as `true` when the key is absent. Task 5 sets it to `false`.

- [ ] **Step 1: Write the failing test**

In `native/tools/probe_stremio.cpp`, inside section 18, insert this immediately **after** the closing `}` of the `restored` block (after the three `plainIdx` assertions, before the `}` that ends section 18):

```cpp
        // A job that STREAMS but is not a download. A romhack patch is an intermediate: it belongs in the
        // Downloads panel, where its progress and its Cancel are, and nowhere else. `record` is what the
        // completion handler reads to keep it out of Recent and the Downloaded folder — so it has to survive
        // a restart, or a patch interrupted by quitting comes back and files itself in the library.
        {
            QFile::remove(downloads + QStringLiteral("/queue.json"));
            {
                DownloadManager dm;
                DownloadJob patch;
                patch.title = QStringLiteral("Hack (patch)");
                patch.url = url;
                patch.dest = downloads + QStringLiteral("/abc123.patch");
                patch.kind = QStringLiteral("patch");
                patch.record = false;
                dm.enqueue(patch);
                CHECK(dm.jobs().size() == 1, "the patch job is queued");
                CHECK(!dm.jobs().at(0).record, "…and is marked as not one to record");
            }
            QFile pf(downloads + QStringLiteral("/queue.json"));
            CHECK(pf.open(QIODevice::ReadOnly), "the patch job was written");
            const QByteArray patchOnDisk = pf.readAll();
            pf.close();
            // Asserted as the JSON the flag serialises to, never as a bare substring: "record" and "patch"
            // both occur elsewhere in this record (the dest ends .patch), so a substring test is satisfied by
            // the FILE NAME whether or not save() writes the flag at all. That exact mistake was made with
            // "gated" thirty lines above, and pinning it here is the whole reason this line is spelled out.
            CHECK(patchOnDisk.contains("\"record\":false"),
                  "the flag IS written, as a flag and not as part of the file name");
            {
                DownloadManager restoredPatch;    // the restart
                CHECK(restoredPatch.jobs().size() == 1, "the patch job comes back");
                CHECK(!restoredPatch.jobs().at(0).record,
                      "…still not one to record, so quitting mid-transfer cannot file it in the library");
            }
            // The default, and the control that gives the two lines above meaning: an ORDINARY job must come
            // back recordable. A load() that returned false for everything would satisfy every assertion so
            // far while having silently emptied the Downloaded folder for all downloads.
            QFile::remove(downloads + QStringLiteral("/queue.json"));
            {
                DownloadManager dm;
                DownloadJob ordinary;
                ordinary.title = QStringLiteral("Ordinary");
                ordinary.url = url;
                ordinary.dest = downloads + QStringLiteral("/ordinary.bin");
                ordinary.kind = QStringLiteral("video");
                dm.enqueue(ordinary);
                CHECK(dm.jobs().at(0).record, "an ordinary job records by default");
            }
            {
                DownloadManager restoredOrdinary;
                CHECK(restoredOrdinary.jobs().size() == 1, "the ordinary job comes back");
                CHECK(restoredOrdinary.jobs().at(0).record, "…still recordable");
            }
            // An OLD queue.json, from a build before this field existed. Absence must read as true: a user
            // upgrading mid-download would otherwise lose the Downloaded-folder entry for every job in
            // flight, silently, and only for that one restart.
            QFile::remove(downloads + QStringLiteral("/queue.json"));
            {
                QFile legacy(downloads + QStringLiteral("/queue.json"));
                CHECK(legacy.open(QIODevice::WriteOnly), "an old-format queue can be written");
                legacy.write(QJsonDocument(QJsonArray{ QJsonObject{
                    { QStringLiteral("id"), QStringLiteral("legacy-1") },
                    { QStringLiteral("title"), QStringLiteral("Legacy") },
                    { QStringLiteral("url"), url },
                    { QStringLiteral("dest"), downloads + QStringLiteral("/legacy.bin") },
                    { QStringLiteral("kind"), QStringLiteral("video") },
                    { QStringLiteral("state"), int(DownloadJob::Paused) } } }).toJson(QJsonDocument::Compact));
                legacy.close();
            }
            {
                DownloadManager legacyDm;
                CHECK(legacyDm.jobs().size() == 1, "the pre-field job loads");
                CHECK(legacyDm.jobs().at(0).record,
                      "…and records, because a field that was never written is not a field set to false");
            }
            QFile::remove(downloads + QStringLiteral("/queue.json"));
        }
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cmake --build build --config Release --target probe_stremio --parallel
```

Expected: **FAIL to compile**, with `'record': is not a member of 'DownloadJob'`.

- [ ] **Step 3: Add the field**

In `native/src/core/DownloadManager.h`, immediately after the `bool headerGated = false;` line and its comment block, add:

```cpp
    // Whether finishing this job means the user asked for the FILE. Nearly always they did — a job exists
    // because someone pressed Download. A romhack patch is the exception: it is an INTERMEDIATE, streamed
    // through here for the resume, the progress and the Cancel, and the thing the user asked for is the
    // patched game that gets written afterwards. false keeps it out of Recent and the Downloaded folder,
    // where a raw .ips is noise nobody asked for; it still appears in the Downloads panel, which is the
    // entire reason it came this way.
    //
    // Not expressed as a `kind`: nothing at the recording site reads kind, so a new one there would be
    // recorded exactly like a game — a field that looks like it should have worked.
    //
    // Persisted, and ABSENT MEANS TRUE, so a queue.json written before this field existed keeps recording.
    bool record = true;
```

- [ ] **Step 4: Persist it**

In `native/src/core/DownloadManager.cpp`, in `save()`, add a line to the `QJsonObject` initialiser immediately after the `{ QStringLiteral("gated"), j.headerGated },` line:

```cpp
            { QStringLiteral("record"), j.record },
```

In `load()`, immediately after the `j.headerGated = ...` line, add:

```cpp
        // Absent means true: a queue.json from before this field existed describes ordinary downloads, and
        // reading a missing key as false would silently empty the Downloaded folder for everything in flight
        // across exactly one upgrade.
        j.record = o.value(QStringLiteral("record")).toBool(true);
```

- [ ] **Step 5: Honour it at the one recording site**

In `native/src/ui/MainWindow.cpp`, replace the handler at lines 663-667:

```cpp
    // A finished download joins Recent + the catalogue's Downloaded folder (offline-openable).
    connect(dm_, &DownloadManager::jobCompleted, this, [this](const DownloadJob& j) {
        RecentStore::add({ j.dest, j.title, j.kind, j.thumb, j.key, j.sysId });
        DownloadsStore::add({ j.dest, j.title, j.kind, j.thumb, j.key, j.sysId });
        notify(tr("Downloaded “%1”.").arg(j.title), 4000);
    });
```

with:

```cpp
    // A finished download joins Recent + the catalogue's Downloaded folder (offline-openable).
    connect(dm_, &DownloadManager::jobCompleted, this, [this](const DownloadJob& j) {
        // …unless the file was a means rather than an end. A romhack patch streams through the manager for
        // the resume and the Cancel, and what the user asked for is the patched game written afterwards —
        // so it gets no Recent row, no Downloaded-folder entry and no toast announcing it. All three are
        // this handler, and all three are wrong for an intermediate, which is why one return covers them.
        if (!j.record) return;
        RecentStore::add({ j.dest, j.title, j.kind, j.thumb, j.key, j.sysId });
        DownloadsStore::add({ j.dest, j.title, j.kind, j.thumb, j.key, j.sysId });
        notify(tr("Downloaded “%1”.").arg(j.title), 4000);
    });
```

- [ ] **Step 6: Run the tests to verify they pass**

```bash
cmake --build build --config Release --target probe_stremio --parallel
```

```bash
PATH="/c/Qt/6.8.3/msvc2022_64/bin:$PATH" QT_QPA_PLATFORM=offscreen QT_PLUGIN_PATH=/c/Qt/6.8.3/msvc2022_64/plugins ./build/Release/probe_stremio.exe
```

Expected: `STREMIO-OK`, exit 0.

- [ ] **Step 7: Commit**

```bash
git add native/src/core/DownloadManager.h native/src/core/DownloadManager.cpp native/src/ui/MainWindow.cpp native/tools/probe_stremio.cpp
```

```bash
git commit -m "feat: a download job that streams without being recorded

Every completed job is written to Recent and the Downloaded folder. That is
right for a download and wrong for an intermediate: a romhack patch wants the
manager's resume, progress and Cancel, but the thing the user asked for is the
patched game written afterwards, and a raw .ips filed in the library is noise.

DownloadJob::record defaults to true and is honoured by one early return at the
single recording site, which covers the Recent write, the Downloaded write and
the toast together. It persists, and an absent key reads as true so a queue
written before the field existed keeps recording.

A kind string could not do this: nothing at that site reads kind, so a new one
would be recorded exactly like a game."
```

---

### Task 3: `PendingRomhack` carries a path, not bytes

**Files:**
- Modify: `native/src/ui/MainWindow.h:1362-1374` (`PendingRomhack`), and the private-methods area near `applyRomhack`
- Modify: `native/src/ui/MainWindow.cpp:13991-14020` (write the fetched bytes to the cache), `native/src/ui/MainWindow.cpp:14163` (read them back at apply time)

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: `QString PendingRomhack::patchPath` replacing `QByteArray patchBytes`; two file-scope helpers in `MainWindow.cpp` — `static QString romhackPatchCacheDir()` and `static QString romhackPatchCachePath(const QString& hackId, const QString& patchName)`. Tasks 5 and 6 use both.

This task changes no behaviour. Its whole purpose is that the two async routes stop copying the patch into a lambda.

- [ ] **Step 1: Add the cache-path helpers**

In `native/src/ui/MainWindow.cpp`, immediately after the `romLibraryFolderFor` function (which ends just before the `romMatchesTarget` comment block, around line 13640), add:

```cpp
// Where a downloaded patch waits between arriving and being applied. Under downloads/ because that is where
// the manager's ".part" siblings already live, and because a patch that came through the queue and one that
// came down inline must land in the SAME place — the retry path cannot care which way it arrived.
static QString romhackPatchCacheDir()
{
    return AppPaths::dataDir() + QStringLiteral("/downloads/patches");
}

// A stable, path-safe name for one hack's one patch file. Hashed rather than sanitised: a source's file name
// is not ours to trust as a path component, and any sanitisation broad enough to make it safe is also broad
// enough to map two different patches onto one name — which would hand back the wrong file, silently, to
// someone who had asked for the second. Stable is the load-bearing property: it is what makes enqueue()'s
// de-dup-by-destination resume an interrupted transfer instead of starting a second one.
static QString romhackPatchCachePath(const QString& hackId, const QString& patchName)
{
    const QByteArray key = hackId.toUtf8() + '\0' + patchName.toUtf8();
    return romhackPatchCacheDir() + QLatin1Char('/')
         + QString::fromLatin1(QCryptographicHash::hash(key, QCryptographicHash::Sha1).toHex())
         + QStringLiteral(".patch");
}
```

- [ ] **Step 2: Change the field**

In `native/src/ui/MainWindow.h`, replace the `patchBytes` member of `PendingRomhack` (lines 1367-1372):

```cpp
        // The patch file itself, fetched from the chosen RomhackPatchFile's url once the user has committed.
        // Held here rather than fetched at apply time because applyRomhack can run an hour later, behind a
        // base-ROM download, and the server keeps a fetched file only for a while. The chosen
        // RomhackPatchFile is deliberately NOT kept beside it: two fields a word apart, one of them the
        // bytes and one of them a description of them, is how a later change reaches for the wrong one.
        QByteArray patchBytes;
```

with:

```cpp
        // WHERE the patch is, on our own disk — not the bytes. Acquired once the user has committed, because
        // applyRomhack can run an hour later behind a base-ROM download and the server keeps a fetched file
        // only for a while; a path rather than a buffer because at disc scale a buffer is not a field, it is
        // a liability. Both routes into this struct are copied BY VALUE into a lambda that outlives the frame
        // that built it, so a QByteArray here meant the patch was held twice for the length of a download.
        // The chosen RomhackPatchFile is deliberately NOT kept beside it: two fields a word apart, one of
        // them the file and one of them a description of it, is how a later change reaches for the wrong one.
        QString patchPath;
```

- [ ] **Step 3: Write the fetched bytes to the cache**

In `native/src/ui/MainWindow.cpp`, replace lines 14012-14020 — the fetch and its failure check:

```cpp
    req.patchBytes = fetchUrlBlocking(
        RomhackClient::fileUrl(serverForId.value(chosen.id), patch.url), 180000);
    if (req.patchBytes.isEmpty())
    {
        // Reachable by design, not only by failure: the server keeps a fetched file on a timer, so a
        // chooser left open long enough outlives it.
        notify(tr("Couldn't download %1's patch — try again.").arg(chosen.title), 7000);
        return;
    }
```

with:

```cpp
    const QByteArray patchBytes = fetchUrlBlocking(
        RomhackClient::fileUrl(serverForId.value(chosen.id), patch.url), 180000);
    if (patchBytes.isEmpty())
    {
        // Reachable by design, not only by failure: the server keeps a fetched file on a timer, so a
        // chooser left open long enough outlives it.
        notify(tr("Couldn't download %1's patch — try again.").arg(chosen.title), 7000);
        return;
    }
    // Onto our own disk before anything else can happen to it. The apply can be an hour away, behind a
    // base-ROM download, and a buffer that has to survive that trip gets copied into every lambda on the way.
    req.patchPath = romhackPatchCachePath(chosen.id, patch.name);
    QDir().mkpath(romhackPatchCacheDir());
    {
        QFile pf(req.patchPath);
        if (!pf.open(QIODevice::WriteOnly) || pf.write(patchBytes) != patchBytes.size())
        {
            notify(tr("Couldn't save %1's patch — check there's space for it.").arg(chosen.title), 8000);
            return;
        }
    }
```

- [ ] **Step 4: Read it back at apply time**

In `native/src/ui/MainWindow.cpp`, replace lines 14163-14164 — the `install` call inside `applyRomhack`:

```cpp
    const QString installed = RomhackInstall::install(patchSource, req.patchBytes, chosen.title,
                                                      targetDir, &err, title);
```

with:

```cpp
    // Read HERE, immediately before the apply, so the patch is resident for the apply and not for the hour
    // that may have preceded it. RomPatch is an in-memory applier and always will be within this change, so
    // this is the peak either way — what goes away is the duration, and the second copy in the lambda.
    QFile patchFile(req.patchPath);
    if (!patchFile.open(QIODevice::ReadOnly))
    {
        notify(tr("%1's patch is missing, so it wasn't installed.").arg(chosen.title), 8000);
        return;
    }
    const QByteArray patchBytes = patchFile.readAll();
    patchFile.close();
    const QString installed = RomhackInstall::install(patchSource, patchBytes, chosen.title,
                                                      targetDir, &err, title);
```

- [ ] **Step 5: Build the app and confirm nothing else referenced the old field**

```bash
cmake --build build --config Release --target everythingbox --parallel
```

Expected: **PASS** with no errors. If any line still names `patchBytes` on a `PendingRomhack`, the compiler names it — fix that site rather than reintroducing the field.

```bash
grep -rn "patchBytes" native/src | grep -i "req\.\|pending\." || echo "no PendingRomhack::patchBytes references remain"
```

Expected: `no PendingRomhack::patchBytes references remain`.

- [ ] **Step 6: Run the romhack probe to confirm the install seam is untouched**

```bash
PATH="/c/Qt/6.8.3/msvc2022_64/bin:$PATH" QT_QPA_PLATFORM=offscreen QT_PLUGIN_PATH=/c/Qt/6.8.3/msvc2022_64/plugins ./build/Release/probe_romhack.exe
```

Expected: `ROMHACK-OK`, exit 0.

- [ ] **Step 7: Commit**

```bash
git add native/src/ui/MainWindow.h native/src/ui/MainWindow.cpp
```

```bash
git commit -m "refactor: a pending romhack carries its patch's path, not its bytes

PendingRomhack held the whole patch in a QByteArray, and both async routes
copy the struct by value into a lambda that outlives the frame that built it -
so a disc-scale patch was resident twice, for as long as a base-ROM download
took.

It now carries a path into a per-hack cache file under downloads/patches, named
by a hash of the hack id and the patch's own file name: hashed rather than
sanitised because a source's file name is not ours to trust as a path
component, and stable because a stable destination is what will let an
interrupted transfer resume rather than restart.

applyRomhack reads the file immediately before the apply. RomPatch is an
in-memory applier, so that is still the peak; what goes away is the duration
and the duplicate. No behaviour change."
```

---

### Task 4: Ask every question above the transfer

**Files:**
- Modify: `native/src/ui/MainWindow.cpp:13991-14047` (the tail of `showRomhacks`)

**Interfaces:**
- Consumes: `PendingRomhack::patchPath` (Task 3).
- Produces: a local `bool needBaseRom` decided before the patch is acquired, and a new private method `void MainWindow::resumeRomhackAfterPatch(const PendingRomhack& req, bool needBaseRom)` used by both the inline path and (in Task 5) the queued one.

- [ ] **Step 1: Declare the shared continuation**

In `native/src/ui/MainWindow.h`, immediately after the `downloadBaseRomThenApply` declaration (line 1385), add:

```cpp
    // The tail of the flow, once the patch is on disk and every question has been answered: either the base
    // game still has to be fetched, or it is already there and this applies straight away. One function
    // because there are two ways to arrive here — the patch came down inline, or it came through the download
    // queue minutes later — and they must not drift into two slightly different endings.
    void resumeRomhackAfterPatch(const PendingRomhack& req, bool needBaseRom);
```

- [ ] **Step 2: Move the base-ROM question above the fetch**

In `native/src/ui/MainWindow.cpp`, cut the entire block currently at lines 14022-14047 — from the comment `// A hack can be browsed and chosen for a game that is not downloaded yet` through `applyRomhack(baseRom, req);` — and replace it with nothing for the moment. Then, immediately **above** the `// Fetch the patch itself now the choice is made.` comment at line 13991, insert:

```cpp
    // Asked BEFORE the patch is fetched, for the same reason the warning above is: it is a question about the
    // ROM and needs no patch to answer. Asking it afterwards means interrupting someone minutes after a
    // disc-scale patch finished downloading, to ask about something that was knowable before it started —
    // and on the queued route the confirm would arrive with the flow long since off screen. From here on
    // there are no more questions, only transfers.
    const QString baseRom = item.url;
    const bool needBaseRom = baseRom.isEmpty() || !QFileInfo::exists(baseRom);
    if (needBaseRom)
    {
        hideNotice();
        QString msg = tr("You don't have %1 yet, and a hack is a patch for it — so both are needed.\n\n"
                         "%1 downloads to your library as an ordinary game, and %2 installs beside it "
                         "as a separate copy.").arg(title, chosen.title);
        // Say WHICH release it needs, while there is still a decision to make. A translation is normally
        // built against the Japanese dump, and what downloads here is whatever your sources offer for the
        // title — so knowing the target now is the difference between a working install and a refusal after
        // the download has already run.
        const QString wanted = describeTarget(req.target);
        if (!wanted.isEmpty())
            msg += tr("\n\n%1 was built for %2. If the copy that downloads isn't that release it won't be "
                      "installed, and nothing will be written to your library.").arg(chosen.title, wanted);
        if (NavConfirm::ask(tr("Download %1 first?").arg(title), msg,
                            { tr("Cancel"), tr("Download both") }, /*focusIndex*/ 1, /*cancelIndex*/ 0, this) != 1)
            return;
    }

```

- [ ] **Step 3: End the function through the shared continuation**

At the end of `showRomhacks`, immediately after the cache-write block added in Task 3 (the closing `}` of the `QFile pf(...)` scope), the function must now end with:

```cpp
    resumeRomhackAfterPatch(req, needBaseRom);
}
```

- [ ] **Step 4: Write the continuation**

In `native/src/ui/MainWindow.cpp`, immediately **above** `bool MainWindow::downloadBaseRomThenApply(const PendingRomhack& req)` (around line 14049), add:

```cpp
// Everything after the patch is on disk. Two callers: the patch came down inline and this runs on the same
// frame, or it came through the download queue and this runs from a deferred completion handler minutes
// later. `needBaseRom` was decided before either transfer started, because it is a question that needed
// answering while someone was still looking at the screen.
void MainWindow::resumeRomhackAfterPatch(const PendingRomhack& req, bool needBaseRom)
{
    if (needBaseRom) { downloadBaseRomThenApply(req); return; }
    applyRomhack(req.base.url, req);
}
```

- [ ] **Step 5: Build and verify the order by reading it back**

```bash
cmake --build build --config Release --target everythingbox --parallel
```

Expected: **PASS**.

```bash
grep -n "Download %1 first?\|Fetching %1's patch\|resumeRomhackAfterPatch(req, needBaseRom)" native/src/ui/MainWindow.cpp
```

Expected: exactly three lines, with **strictly increasing** line numbers in this order:

1. `Download %1 first?` — the base-ROM confirm
2. `Fetching %1's patch…` — the fetch's status note
3. `resumeRomhackAfterPatch(req, needBaseRom)` — the tail

If (1) comes after (2), the move did not happen and the confirm still fires at the end of the transfer, which is the entire fault this task removes.

- [ ] **Step 6: Commit**

```bash
git add native/src/ui/MainWindow.h native/src/ui/MainWindow.cpp
```

```bash
git commit -m "refactor: ask about the base game before fetching the patch, not after

The 'you don't have the base game yet' confirm sat below the patch fetch, so
it arrived at the end of a transfer that could take minutes - to ask something
that was knowable before it started. It is a question about the ROM and needs
no patch to answer, which is the same reasoning already written down for why
the target warning is asked early.

The tail of the flow moves into resumeRomhackAfterPatch, one function because
there are about to be two ways to reach it and they must not drift apart.

From the confirm onward there are no more questions, only transfers."
```

---

### Task 5: Route the large case through the download queue

**Files:**
- Modify: `native/src/ui/MainWindow.h` (the romhack member block around lines 1404-1419)
- Modify: `native/src/ui/MainWindow.cpp:13991-14020` (replace the inline fetch with the bounded one plus the queue route)

**Interfaces:**
- Consumes: `BoundedFetch::get` (Task 1), `DownloadJob::record` (Task 2), `romhackPatchCachePath` / `romhackPatchCacheDir` (Task 3), `resumeRomhackAfterPatch` (Task 4).
- Produces: `QSet<QString> romhackPatchDownloads_` — the hack ids whose PATCH transfer is in flight.

- [ ] **Step 1: Add the include and the in-flight set**

In `native/src/ui/MainWindow.cpp`, add to the include block beside the other core headers (after line 84's `#include "../core/RomhackClient.h"`):

```cpp
#include "../core/BoundedFetch.h"
```

In `native/src/ui/MainWindow.h`, immediately after the `QSet<QString> romhackRomDownloads_;` declaration (line 1419), add:

```cpp
    // The hack ids whose PATCH transfer is in flight. Separate from romhackRomDownloads_ above because they
    // are different transfers with different endings — a finished ROM is the install, a patch is the step
    // before it — and a shared set would make one hack's patch refuse another hack's finished ROM.
    //
    // Same three reasons that one exists, all of which apply identically here: romhackBusy_ is released when
    // showRomhacks returns, which on this route is the moment the job is enqueued; only the ".part" exists so
    // no destination check catches a repeat; and enqueue() de-dups by dest while the handler matches on key,
    // so a second press would fold into ONE job carrying TWO handlers, the second firing inside the first's
    // nested loop (#28).
    QSet<QString> romhackPatchDownloads_;
```

- [ ] **Step 2: Replace the inline fetch with the bounded fetch and the queue route**

In `native/src/ui/MainWindow.cpp`, replace the whole block from the `// Fetch the patch itself now the choice is made.` comment (line 13991) through the end of the cache-write scope added in Task 3, with:

```cpp
    // Acquire the patch, and let the RESPONSE decide how. Usually small — a disc-scale RELEASE arrives as a
    // finished ROM and left through the download queue above — but a patch BUILT AGAINST a disc image is
    // itself disc-scale, and nothing here can tell the two apart in advance: the format is asserted by the
    // source and never sniffed, so a two-byte tweak and a rebuild of a whole disc arrive under one extension.
    //
    // So the fetch judges itself. Under the ceiling it finishes here, on this frame, off the queue and out of
    // the Downloaded folder — which is what the queue could not offer, running one job at a time and
    // recording everything it finishes. Over the ceiling it is abandoned at the cost of one response head and
    // handed to the manager, which streams to a ".part", resumes with a Range request, and shows progress and
    // a Cancel. The one thing that was never acceptable is what used to happen: a disc-scale transfer with
    // none of that, behind a deadline it could not meet.
    //
    // BoundedFetch leaves Qt on NoLessSafeRedirectPolicy, so this request WILL follow a cross-host 302 —
    // RomhackClient::fileUrl only guarantees where the transfer STARTS, not where it ends. That is a decision
    // and not an oversight: the request carries no headers, no cookies and no credentials, so a redirect leaks
    // nothing, and a server behind a reverse proxy or a CDN needs the hop followed or its files are simply
    // unreachable. An unexamined default and a considered one look identical in code, so it is written here.
    const QString patchUrl = RomhackClient::fileUrl(serverForId.value(chosen.id), patch.url);
    req.patchPath = romhackPatchCachePath(chosen.id, patch.name);
    QDir().mkpath(romhackPatchCacheDir());

    // Already here? Then it was fetched before and not yet consumed — an install that failed, or one whose
    // patch download outlived the app. Use it, and touch nothing. Answered HERE rather than by letting
    // enqueue() notice, because enqueue() reports an existing file by emitting jobCompleted SYNCHRONOUSLY,
    // and what that handler reaches opens NavConfirm — which must never run inside another emission (#28).
    if (QFileInfo::exists(req.patchPath) && QFileInfo(req.patchPath).size() > 0)
    {
        resumeRomhackAfterPatch(req, needBaseRom);
        return;
    }

    // Names the PATCH, where the note before the chooser named the hack. They are two network operations with
    // two deadlines, and one unchanging sentence across both means a stall in the second reads as a stall in
    // the first.
    notify(tr("Fetching %1's patch…").arg(chosen.title), 0);
    // Sixty seconds, not the three minutes this used to take. The deadline no longer has to cover a
    // disc-scale transfer, because a disc-scale transfer no longer happens here — so it can be sized for what
    // does happen, and "couldn't download it, try again" becomes true for the first time.
    static constexpr qint64 kPatchInlineCeiling = 16 * 1024 * 1024;
    const BoundedFetch::Result fetchedPatch = BoundedFetch::get(patchUrl, 60000, kPatchInlineCeiling);

    if (fetchedPatch.verdict == BoundedFetch::Result::Failed)
    {
        mwLog(QStringLiteral("romhack: patch fetch failed for \"%1\" — status %2, %3, from %4")
                  .arg(chosen.title).arg(fetchedPatch.status)
                  .arg(fetchedPatch.error.isEmpty() ? QStringLiteral("-") : fetchedPatch.error,
                       logSafeUrl(patchUrl)));
        // Two different things to do next, so two different sentences. A server that ANSWERED and refused is
        // most often one whose fetched file has aged out of its timed store — the chooser was left open too
        // long — and the fix is to ask for the hack again, not to press the same dead reference. A server
        // that never answered is a source or a network problem, and retrying is exactly right. The old single
        // sentence sent everyone down the second road, including everyone for whom it led nowhere.
        notify(fetchedPatch.status >= 400
                   ? tr("%1's patch isn't on the server any more — open the hack again to refresh it.")
                         .arg(chosen.title)
                   : tr("Couldn't download %1's patch — the source didn't answer in time.").arg(chosen.title),
               8000);
        return;
    }

    if (fetchedPatch.verdict == BoundedFetch::Result::Ok)
    {
        // Onto our own disk before anything else can happen to it: the apply can be an hour away, behind a
        // base-ROM download.
        QFile pf(req.patchPath);
        if (!pf.open(QIODevice::WriteOnly) || pf.write(fetchedPatch.body) != fetchedPatch.body.size())
        {
            notify(tr("Couldn't save %1's patch — check there's space for it.").arg(chosen.title), 8000);
            return;
        }
        pf.close();
        resumeRomhackAfterPatch(req, needBaseRom);
        return;
    }

    // Over the ceiling. Through the ORDINARY download path from here — one queue, one progress UI, one place
    // to cancel — because at this size that is what the transfer needs, and it is what the finished-ROM route
    // ten lines up already does for the same reason.
    if (!dm_)
    {
        notify(tr("Couldn't download %1's patch.").arg(chosen.title), 8000);
        return;
    }

    // Already fetching this exact hack's patch? Say so and stop. Refused only while the manager STILL HOLDS
    // the job: cancelling from the Downloads panel drops it without ever emitting jobCompleted, so the id
    // would otherwise sit in the set until the process ended and a perfectly reasonable retry would be told a
    // lie. Letting the retry through re-arms a second handler beside the cancelled one's, which nothing can
    // disconnect — so the handler below honours only the FIRST completion per id.
    DownloadJob::State heldState = DownloadJob::Done;
    const QString patchDest = req.patchPath;
    const bool patchJobHeld = [&] {
        for (const DownloadJob& j : dm_->jobs()) if (j.dest == patchDest) { heldState = j.state; return true; }
        return false;
    }();
    if (romhackPatchDownloads_.contains(chosen.id) && patchJobHeld)
    {
        // "Still held" is not "still moving": a failed job stays in the list, and so does a paused one. Being
        // told a patch "is already downloading" when its transfer died an hour ago sends someone to watch a
        // bar that will never advance, so a stopped job says it is stopped and names where the Retry lives.
        const bool stopped = heldState == DownloadJob::Failed || heldState == DownloadJob::Paused;
        notify(stopped
                   ? tr("%1's patch stopped downloading — resume or retry it in Downloads.").arg(chosen.title)
                   : tr("%1's patch is already downloading — it's in Downloads.").arg(chosen.title),
               6000);
        return;
    }

    DownloadJob patchJob;
    // Reads as an intermediate, not as the game. Someone scanning Downloads must not conclude the hack has
    // already arrived — it has not; this is the step before it.
    patchJob.title = tr("%1 (patch)").arg(chosen.title);
    patchJob.url = patchUrl;
    patchJob.dest = req.patchPath;
    patchJob.kind = QStringLiteral("patch");
    patchJob.thumb = item.thumbnailUrl;   // the row is otherwise blank, and the art says which install this is
    patchJob.key = chosen.id;             // the only handle back; the job id is minted inside the manager
    patchJob.record = false;              // an intermediate: the Downloads panel, and nowhere else

    // A one-shot owned by the connection itself rather than by a member slot — a member would make two hacks
    // queued together exclusive for no reason.
    romhackPatchDownloads_.insert(chosen.id);
    auto* patchConn = new QMetaObject::Connection;
    const QString wantPatchKey = chosen.id;
    const PendingRomhack pendingPatch = req;    // by value: this outlives the frame that built it — and it is
                                                // a path now, so the copy costs nothing
    const bool wantBaseRom = needBaseRom;
    const QString patchHackTitle = chosen.title;
    *patchConn = connect(dm_, &DownloadManager::jobCompleted, this,
                         [this, patchConn, wantPatchKey, pendingPatch, wantBaseRom, patchHackTitle]
                         (const DownloadJob& done) {
        if (done.key != wantPatchKey) return;
        disconnect(*patchConn);
        delete patchConn;
        // Disarmed above the branch, so EVERY way out clears it. Its absence is also how a completion is
        // honoured EXACTLY ONCE: a cancel-then-retry leaves an older handler armed on the same key with no
        // way to disconnect it, so two can see the same finish. The first takes the id; the rest stand down.
        // Two that both ran would stack a second confirm inside the first's nested loop — the #28 shape.
        if (!romhackPatchDownloads_.remove(wantPatchKey)) return;
        if (done.dest.isEmpty() || !QFileInfo::exists(done.dest))
        {
            notify(tr("%1's patch didn't download, so it wasn't installed.").arg(patchHackTitle), 8000);
            return;
        }
        // Off this frame before anything opens a nested loop: jobCompleted is emitted from inside
        // finishActive, which still holds a live reference into its own jobs_ vector and has not yet cleared
        // activeId_, saved, or pumped — and this continuation ends in NavConfirm. See #28. Nothing index-like
        // survives the hop: `pendingPatch` is already a copy and carries a plain path.
        deferPastQmlEmission([this, pendingPatch, wantBaseRom] {
            resumeRomhackAfterPatch(pendingPatch, wantBaseRom);
        });
    });

    // Bounded, NOT sticky. A sticky notice here is only ever cleared by hideNotice() or the next notify(),
    // and cancelling the job from the Downloads panel goes through dm_->cancel(), which emits no
    // jobCompleted — so the overlay would go on announcing a download that had been stopped. This line only
    // has to say the transfer started; the panel holds the progress and the Cancel.
    notify(tr("Downloading %1's patch…").arg(chosen.title), 8000);
    dm_->enqueue(patchJob);
}
```

Note the final `}` — this replacement now closes `showRomhacks`, so the `resumeRomhackAfterPatch(req, needBaseRom);` line and closing brace added in Task 4 Step 3 are subsumed by the two `resumeRomhackAfterPatch` returns above. Confirm the function has exactly one closing brace.

- [ ] **Step 3: Build**

```bash
cmake --build build --config Release --target everythingbox --parallel
```

Expected: **PASS**.

- [ ] **Step 4: Verify the guarantees that no probe can reach, by reading**

The queue route runs through UI the probe suite cannot drive, so these four are confirmed by inspection. Run each and read the result:

```bash
grep -n "deferPastQmlEmission" native/src/ui/MainWindow.cpp | sed -n '1,20p'
```

Expected: a hit inside the patch-completion lambda. A `resumeRomhackAfterPatch` reached from `jobCompleted` **without** one is crash #28.

```bash
grep -c "romhackPatchDownloads_" native/src/ui/MainWindow.cpp
```

Expected: `3` — the declaration is in the header; here it is the `contains` guard, the `insert`, and the `remove`.

```bash
grep -n "patchJob.record" native/src/ui/MainWindow.cpp
```

Expected: exactly one line, `patchJob.record = false;`.

```bash
grep -n "fetchUrlBlocking" native/src/ui/MainWindow.cpp
```

Expected: three hits — the definition and the two remaining JSON callers (list and fetch). **No** patch-file caller. If a fourth remains, the old path is still live.

- [ ] **Step 5: Run both affected probes**

```bash
PATH="/c/Qt/6.8.3/msvc2022_64/bin:$PATH" QT_QPA_PLATFORM=offscreen QT_PLUGIN_PATH=/c/Qt/6.8.3/msvc2022_64/plugins ./build/Release/probe_romhack.exe
```

```bash
PATH="/c/Qt/6.8.3/msvc2022_64/bin:$PATH" QT_QPA_PLATFORM=offscreen QT_PLUGIN_PATH=/c/Qt/6.8.3/msvc2022_64/plugins ./build/Release/probe_stremio.exe
```

Expected: `ROMHACK-OK` and `STREMIO-OK`, both exit 0.

- [ ] **Step 6: Commit**

```bash
git add native/src/ui/MainWindow.h native/src/ui/MainWindow.cpp
```

```bash
git commit -m "feat: stream a disc-scale romhack patch instead of buffering it

A patch under 16 MiB still arrives inline on the same frame, off the download
queue and out of the Downloaded folder - which is what the queue could not
offer, running one job at a time and recording everything it finishes. Over
16 MiB the fetch is abandoned at the cost of one response head and the same url
goes to DownloadManager, which streams to a .part, resumes with a Range request
and shows progress and a Cancel in the Downloads panel.

The deadline drops from 180 s to 60 s, because it no longer has to cover a
disc-scale transfer. A failure now says which failure: a server that answered
and refused is told apart from one that never answered, because the first means
the fetched file aged out and the fix is to open the hack again, while only the
second is worth retrying.

The queued route takes the three defences the finished-ROM route already
carries: deferPastQmlEmission before the continuation, an in-flight set
honouring only the first completion per id, and a bounded rather than sticky
progress note, because a cancel emits no completion at all."
```

---

### Task 6: The cache's lifetime

**Files:**
- Modify: `native/src/ui/MainWindow.h` (private methods, beside `resumeRomhackAfterPatch`)
- Modify: `native/src/ui/MainWindow.cpp` (the top of `showRomhacks`; the tail of `applyRomhack`)

**Interfaces:**
- Consumes: `romhackPatchCacheDir` (Task 3), `dm_->jobs()`.
- Produces: `void MainWindow::pruneRomhackPatchCache()`.

- [ ] **Step 1: Declare the prune**

In `native/src/ui/MainWindow.h`, immediately after the `resumeRomhackAfterPatch` declaration added in Task 4, add:

```cpp
    // Drop patch files nobody is coming back for. A patch is kept after a FAILED install on purpose — that is
    // the retry cache, and it is what makes a second press cost nothing — but "kept" cannot mean "forever" at
    // disc scale.
    void pruneRomhackPatchCache();
```

- [ ] **Step 2: Write it**

In `native/src/ui/MainWindow.cpp`, immediately above `void MainWindow::resumeRomhackAfterPatch(...)`, add:

```cpp
void MainWindow::pruneRomhackPatchCache()
{
    const QDir dir(romhackPatchCacheDir());
    if (!dir.exists()) return;

    // Never delete a file the download manager still has a job for. A PAUSED job's ".part" can easily be
    // older than the cutoff — that is what paused means — and removing it throws away the resume the whole
    // queue route exists to provide, turning a Resume press into a fresh disc-scale download. Both names are
    // held because the manager writes the ".part" and renames to the destination only at the end.
    QSet<QString> held;
    if (dm_)
        for (const DownloadJob& j : dm_->jobs())
        {
            held.insert(QFileInfo(j.dest).fileName());
            held.insert(QFileInfo(j.dest + QStringLiteral(".part")).fileName());
        }

    const QDateTime cutoff = QDateTime::currentDateTime().addDays(-7);
    for (const QFileInfo& fi : dir.entryInfoList(QDir::Files))
    {
        if (held.contains(fi.fileName())) continue;
        if (fi.lastModified() < cutoff) QFile::remove(fi.absoluteFilePath());
    }
}
```

- [ ] **Step 3: Call it once per flow**

In `native/src/ui/MainWindow.cpp`, in `showRomhacks`, immediately after the `romhackBusy_ = true;` / `const RomhackBusyGuard busyGuard(&romhackBusy_);` pair, add:

```cpp
    // Here rather than at startup: this is the only feature that writes to that folder, so it is the only
    // place that has any business sweeping it, and it costs one directory listing on a path someone is
    // already waiting on the network for.
    pruneRomhackPatchCache();
```

- [ ] **Step 4: Delete a consumed patch**

In `native/src/ui/MainWindow.cpp`, in `applyRomhack`, immediately after the `if (installed.isEmpty()) { ... return; }` block and **before** the `finishRomhackInstall(...)` call, add:

```cpp
    // Consumed. The patched game is on disk and is what was actually wanted; keeping the patch beside it
    // would leave a disc-scale intermediate behind after every successful install. A FAILED install keeps
    // its patch — see pruneRomhackPatchCache — because that is the case a retry is coming for.
    QFile::remove(req.patchPath);
```

- [ ] **Step 5: Build and verify**

```bash
cmake --build build --config Release --target everythingbox --parallel
```

Expected: **PASS**.

```bash
grep -n "pruneRomhackPatchCache\|QFile::remove(req.patchPath)" native/src/ui/MainWindow.cpp
```

Expected: four lines — the definition, the call in `showRomhacks`, and the removal in `applyRomhack` (plus the declaration is in the header). Confirm the removal sits **after** the `installed.isEmpty()` early return, not before it: deleting the patch on a failed install destroys the retry cache.

- [ ] **Step 6: Run the full probe gate**

```bash
BUILD_DIR=build bash native/tools/run-headless-probes.sh | tail -3
```

Expected: `VERDICT=PASS`. If probes fail with "not built" / "not rebuilt", the worktree's `build/` is partial — build the full CI probe list from the "Build probes" step of `.github/workflows/ci.yml`, plus `probe_mpvpreview`, and re-run.

```bash
cat build/headless-probes.verdict
```

Expected: `VERDICT=PASS`.

- [ ] **Step 7: Commit**

```bash
git add native/src/ui/MainWindow.h native/src/ui/MainWindow.cpp
```

```bash
git commit -m "feat: retire a romhack patch once nobody is coming back for it

A patch is deleted after a successful install - the patched game is on disk and
is what was wanted - and kept after a failed one, because that is the retry
cache and it is what makes a second press cost nothing. Kept cannot mean
forever at disc scale, so files older than seven days are swept when the flow
next runs.

The sweep skips any file the download manager still holds a job for. A paused
job's .part is older than the cutoff by definition, and deleting it would turn
a Resume press into a fresh disc-scale download - the exact thing the queue
route was added to prevent."
```

---

## Manual verification

The probe suite cannot drive this flow — it is UI over a network. After Task 6, confirm on the running app:

1. **Small patch, unchanged.** Install a cart-era hack (NES/SNES). Expect: no Downloads row appears, no "Downloaded" toast, nothing new in the Downloaded folder, and the install completes as quickly as it did before.
2. **Large patch, queued.** Install a hack whose patch is over 16 MiB. Expect: the flow asks its questions, then returns; a row titled `<hack> (patch)` appears in Downloads with a moving bar and a working Cancel; when it finishes the install continues by itself and ends in "Play it now?"; and nothing named `.patch` appears in Recent or the Downloaded folder.
3. **Cancel.** Cancel that row mid-transfer. Expect: no stranded sticky note, and pressing the hack again starts a fresh transfer rather than refusing.
4. **Resume.** Quit mid-transfer, relaunch, press the hack again. Expect: the transfer resumes from the `.part` rather than restarting.

Record what happened for each; item 2 is the one the whole change exists for.
