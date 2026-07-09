// Local UI-automation channel, so the UI can be TESTED without bringing the window to the front or giving
// it OS focus: an agent/script drives navigation and captures what the user would see while another app
// keeps the foreground. Line-based protocol over a QLocalSocket (a named pipe on Windows, a unix socket
// elsewhere), served on the GUI thread:
//
//   key <up|down|left|right|enter|back|escape|Nx>   inject a nav key through sendNavKey (Nx = raw Qt::Key int)
//   state                                           -> "ok {json}": page, focus widget, overlays, geometry
//   shot <absolute-path.png>                        render the whole window (works occluded/backgrounded)
//
// Injected keys use the app's own routing (overlays -> rings -> back actions), not OS input, so they need
// no focus; before each one the window is given Qt-INTERNAL activation (no OS foreground change) so focus
// styling and the watchdog behave exactly as they would live. See native/tools/uitest.py for the client.
//
// OFF by default. Enabled only by MMV_UITEST=1 in the environment or a --uitest command-line argument.
#pragma once
#include <QObject>
#include <QString>
#include <functional>

class UiTestServer : public QObject
{
    Q_OBJECT
public:
    struct Hooks
    {
        std::function<void(int)> sendKey;                 // deliver a synthetic nav key (Qt::Key_*)
        std::function<QString()> state;                   // compact JSON snapshot of the UI state
        std::function<bool(const QString&)> screenshot;   // render the window to a PNG path
    };

    static bool wanted();                                 // MMV_UITEST=1 or --uitest present
    explicit UiTestServer(const Hooks& hooks, QObject* parent = nullptr);

    static QString serverName() { return QStringLiteral("MyMediaVault-uitest"); }

private:
    QString handle(const QString& line);                  // one command -> one response line
    Hooks hooks_;
};
