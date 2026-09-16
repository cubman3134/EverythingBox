#include "QuitSignals.h"

#include <QtGlobal>

#if defined(Q_OS_UNIX) && !defined(Q_OS_ANDROID) && !defined(Q_OS_IOS)
#define EB_QUIT_SIGNALS 1
#endif

#ifdef EB_QUIT_SIGNALS
#include <QObject>
#include <QSocketNotifier>

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <memory>
#include <unistd.h>

namespace
{
const int kSignals[] = { SIGTERM, SIGINT, SIGHUP };

// Written before any handler is installed, and only read afterwards (from the handler).
int g_writeFd = -1;
bool g_taken[3] = { false, false, false }; // which of kSignals this unit installed its handler for
bool g_installed = false;

void resetToDefault()
{
    struct sigaction dfl;
    dfl.sa_handler = SIG_DFL;
    dfl.sa_flags = 0;
    sigemptyset(&dfl.sa_mask);
    for (int i = 0; i < 3; ++i)
        if (g_taken[i]) ::sigaction(kSignals[i], &dfl, nullptr);
}

// Async-signal-safe only: sigaction() and write() are both on the POSIX list, and errno is put back.
void onSignal(int sig)
{
    const int savedErrno = errno;
    resetToDefault();                       // the NEXT one of these signals takes the default action
    const unsigned char b = static_cast<unsigned char>(sig);
    const ssize_t n = ::write(g_writeFd, &b, 1); // non-blocking: a full pipe already holds a wake-up
    (void)n;
    errno = savedErrno;
}

#ifndef Q_OS_LINUX
bool setNonBlockingCloexec(int fd)
{
    const int fl = ::fcntl(fd, F_GETFL);
    const int fdfl = ::fcntl(fd, F_GETFD);
    return fl != -1 && fdfl != -1 && ::fcntl(fd, F_SETFL, fl | O_NONBLOCK) != -1
           && ::fcntl(fd, F_SETFD, fdfl | FD_CLOEXEC) != -1;
}
#endif
} // namespace

bool QuitSignals::supported() { return true; }

bool QuitSignals::install(QObject* context, std::function<void(int)> onQuit)
{
    if (g_installed || !context || !onQuit) return false;
    int fds[2];
#ifdef Q_OS_LINUX
    if (::pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0) return false;
#else
    if (::pipe(fds) != 0) return false;
    if (!setNonBlockingCloexec(fds[0]) || !setNonBlockingCloexec(fds[1]))
    {
        ::close(fds[0]);
        ::close(fds[1]);
        return false;
    }
#endif
    g_writeFd = fds[1];

    // The notifier exists BEFORE the handlers do, so no signal can land with nothing listening for its byte.
    auto* notifier = new QSocketNotifier(fds[0], QSocketNotifier::Read, context);
    auto fired = std::make_shared<bool>(false);
    QObject::connect(notifier, &QSocketNotifier::activated, context,
                     [notifier, readFd = fds[0], fired, onQuit = std::move(onQuit)]() {
        int first = 0;
        unsigned char buf[16];
        ssize_t n;
        while ((n = ::read(readFd, buf, sizeof buf)) > 0)
            if (!first) first = buf[0];
        if (*fired || !first) return;
        *fired = true;
        notifier->setEnabled(false);
        onQuit(first);
    });

    struct sigaction act;
    act.sa_handler = onSignal;
    act.sa_flags = SA_RESTART;
    sigemptyset(&act.sa_mask);
    for (int s : kSignals) sigaddset(&act.sa_mask, s); // one handler at a time; the second is then SIG_DFL
    // A signal the app was started with IGNORED stays ignored: `nohup` ignores SIGHUP, and a shell's background
    // job starts with SIGINT ignored. Overriding that would make the app quit on a signal its launcher asked it
    // to survive. (This is also SDL's rule, which is why only SIGTERM was ever caught in a backgrounded run.)
    for (int i = 0; i < 3; ++i)
    {
        struct sigaction cur;
        if (::sigaction(kSignals[i], nullptr, &cur) != 0) continue;
        if (!(cur.sa_flags & SA_SIGINFO) && cur.sa_handler == SIG_IGN) continue;
        g_taken[i] = true;
    }
    for (int i = 0; i < 3; ++i)
        if (g_taken[i]) ::sigaction(kSignals[i], &act, nullptr);
    g_installed = true;
    return true;
}

#else // not desktop Unix

bool QuitSignals::supported() { return false; }

bool QuitSignals::install(QObject*, std::function<void(int)>) { return false; }

#endif
