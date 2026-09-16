#pragma once
// Desktop Unix quit signals (issue #409): SIGTERM, SIGINT and SIGHUP end the app the way closing its window does.
//
// WHY THE APP HAS TO HANDLE THEM ITSELF. A desktop logout, `systemctl --user stop`, a Steam Deck game-mode exit
// and a plain `kill <pid>` all send SIGTERM and follow with SIGKILL a few seconds later. Until #409 the Linux app
// outlived every SIGTERM: SDL's event subsystem, brought up by the gamepad thread, installs SIGINT/SIGTERM
// handlers whose only action is to post an SDL_QUIT event, and nothing reads SDL_QUIT. The signal was caught and
// dropped, so the process sat there until the SIGKILL, which skips the resume-position flush, the battery-save
// write and the sync push on exit. The SDL half of the fix is SDL_HINT_NO_SIGNAL_HANDLERS at both SDL init sites;
// this unit is the other half: with SDL out of the way the DEFAULT action would terminate the process just as
// abruptly, so the app catches the signal and closes itself properly.
//
// THE SHAPE, AND THE RULES IT KEEPS.
//   * The handler does nothing but async-signal-safe work: it puts all three signals back to SIG_DFL and writes
//     the signal number, one byte, into a non-blocking self-pipe. errno is preserved.
//   * A QSocketNotifier on the pipe's read end runs on the event loop of `context`'s thread, drains the pipe and
//     calls `onQuit(signal)` there. Only ever once per install.
//   * A signal the process started with IGNORED (nohup's SIGHUP, a background job's SIGINT) is left ignored.
//   * Because the dispositions are back to SIG_DFL from the moment the first signal lands, a SECOND SIGTERM while
//     the app is shutting down (a stuck network push, a user who means it) terminates the process the ordinary
//     way. The orderly path is offered once, never forced.
//
// Desktop Unix only (Linux, macOS). Windows does not deliver these signals for a logout; Android and iOS end apps
// through their lifecycle, not signals. On those platforms supported() is false and install() does nothing.
//
// QtCore only (no window), so probe_quitsignals links it without the app.
#include <functional>

class QObject;

namespace QuitSignals
{
// True where install() does something: desktop Unix.
bool supported();

// Catch SIGTERM, SIGINT and SIGHUP. `onQuit` runs once, on `context`'s thread's event loop, with the signal that
// arrived first; the notifier is parented to `context`. Returns false (and installs nothing) when unsupported,
// when already installed in this process, or when the self-pipe cannot be created.
bool install(QObject* context, std::function<void(int signal)> onQuit);
} // namespace QuitSignals
