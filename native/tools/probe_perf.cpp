// Headless test for the PerfTrace span harness (phase-2 perf track): gating, line format,
// begin/end orphan semantics, and the disabled-path overhead budget. Prints PERF-OK.
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QThread>
#include "../src/core/PerfTrace.h"

static int fails = 0;
#define CHECK(cond, name) do { if (cond) printf("PASS %s\n", name); \
    else { printf("FAIL %s\n", name); ++fails; } } while (0)

static QStringList lines(const QString& p)
{
    QFile f(p);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    return QString::fromUtf8(f.readAll()).split('\n', Qt::SkipEmptyParts);
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir tmp;
    const QString log = tmp.filePath("perf.log");

    CHECK(!PerfTrace::enabled(), "disabled by default (no MMV_PERF)");
    { PERF_SPAN("dead.span"); }              // must be a no-op while disabled
    PerfTrace::begin("dead.b"); PerfTrace::end("dead.b");
    CHECK(lines(log).isEmpty(), "disabled emits nothing to the test log");
    // (Do NOT assert the app's real perf_trace.log is absent — a prior MMV_PERF run may have left one.)

    // Disabled-path overhead budget: 1M scoped spans well under 200ms (it's one branch each).
    { QElapsedTimer t; t.start();
      for (int i = 0; i < 1000000; ++i) { PERF_SPAN("dead.hot"); }
      CHECK(t.elapsed() < 200, "disabled span overhead under budget"); }

    PerfTrace::forceEnableForTest(log);
    CHECK(PerfTrace::enabled(), "forceEnableForTest enables");

    { PERF_SPAN("unit.scope"); QThread::msleep(12); }
    PerfTrace::begin("unit.be");
    QThread::msleep(5);
    PerfTrace::end("unit.be", QStringLiteral("n=3"));
    PerfTrace::end("unit.orphan");           // no begin -> silent no-op
    PerfTrace::begin("unit.restart");
    QThread::msleep(30);
    PerfTrace::begin("unit.restart");        // overwrite restarts the clock
    QThread::msleep(5);
    PerfTrace::end("unit.restart");

    const QStringList out = lines(log);
    CHECK(out.size() == 3, "exactly the three real spans logged");
    // Format: ISO-ts | span | ms | detail(optional)
    const QRegularExpression re(QStringLiteral(
        "^\\d{4}-\\d{2}-\\d{2}T[0-9:.]+ \\| [a-z.]+ \\| \\d+(?: \\| .*)?$"));
    bool fmt = true;
    for (const QString& l : out) fmt = fmt && re.match(l).hasMatch();
    CHECK(fmt, "line format ISO-ts | span | ms | detail");
    CHECK(out[0].contains("unit.scope"), "scope span logged");
    CHECK(out[1].contains("unit.be") && out[1].contains("n=3"), "begin/end span carries detail");
    bool restartOk = false;
    { const QStringList parts = out[2].split(QStringLiteral(" | "));
      restartOk = parts.size() >= 3 && parts[1] == QStringLiteral("unit.restart")
                  && parts[2].toLongLong() < 25; }   // 5ms run, NOT 35ms — overwrite restarted it
    CHECK(restartOk, "begin-overwrite restarts the clock");

    if (fails == 0) printf("PERF-OK\n");
    return fails == 0 ? 0 : 1;
}
