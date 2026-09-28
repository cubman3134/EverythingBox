// everythingbox:// deep links (issue #80): how a link REACHES the running app. QtCore + QtNetwork.
//
// The OS starts `EverythingBox "everythingbox://addon/…"` once per link. Three ways in, one gate:
//   * the command line of THIS process (the app was not running, so it starts normally and handles the link
//     once its window is ready);
//   * a second process's command line, handed over a dedicated QLocalServer — never the UI-test pipe — after
//     which that process exits (handOffFromArguments());
//   * macOS, which delivers the URL as a QFileOpenEvent instead of an argument (eventFilter()).
// Every one of them goes through DeepLink::parse / validateManifestUrl before anything is emitted, and the
// receiver re-validates what a socket sends it: any program of this user can write to that socket.
//
// The socket name is per user and per login session (DeepLink::handoffServerName) and the server is created
// with QLocalServer::UserAccessOption, so another account cannot connect to it. Under the UI-test channel the
// name takes the rig's pipe name as a suffix, so a test rig never receives the live install's links or the
// other way round.
//
// Links are HELD until setReady(): the window, the add-on manager and the profile gate have to exist before a
// card can be shown. Only the newest link is kept; an older one waiting with it is superseded, exactly as an
// open card is (DeepLink::Inbox).
//
// Nothing here logs a manifest URL. The app logs through LogSafeText::url().
#pragma once
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVector>

class QLocalServer;
class QLocalSocket;

class DeepLinkChannel : public QObject
{
    Q_OBJECT
public:
    enum class Handoff
    {
        NoInstance,   // nobody is listening: this process should start normally and handle the link itself
        Delivered,    // the running app has it: this process should exit
        Refused,      // the running app re-validated it and said no: this process should exit
    };

    // This process's scheme / the scheme it may register / its socket name, from the environment
    // (EB_UITEST + EB_UITEST_DEEPLINK_SCHEME + EB_UITEST_PIPE; see DeepLink::schemeFor and friends).
    static QString activeScheme();
    static QString registrationScheme();
    static QString serverName();

    // Blocking: hand `manifestUrl` (already validated) to whoever listens on `name`.
    static Handoff sendToRunning(const QString& name, const QString& manifestUrl, int timeoutMs = 2000);
    // Blocking: tell whoever listens on `name` that a link was refused here, and why (no URL crosses).
    static Handoff sendRefusalToRunning(const QString& name, int refusal, int timeoutMs = 2000);

    explicit DeepLinkChannel(QObject* parent = nullptr);
    ~DeepLinkChannel() override;

    // Look through `args` (QCoreApplication::arguments()) for a link to activeScheme(). If there is one and an
    // instance is running, hand it over and return true: the caller exits. Otherwise the link (or its refusal)
    // is held for setReady() and the caller starts normally.
    bool handOffFromArguments(const QStringList& args);

    // Start receiving on `name`. False when the name cannot be taken (logged by the caller, never fatal).
    bool listen(const QString& name);
    bool listening() const;

    // A raw link from anywhere (argv, QFileOpenEvent), for `scheme`. Parsed here; held until ready.
    void offer(const QString& rawLink, const QString& scheme);

    // The window is up: deliver whatever is held (queued, never inside the caller's own stack), and deliver
    // everything after this as it arrives.
    void setReady();

signals:
    void linkArrived(const QString& manifestUrl);   // validated, twice where it crossed a socket
    void linkRefused(int refusal);                  // a DeepLink::Refusal

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;   // macOS: QEvent::FileOpen with a URL

private:
    void accept(QLocalSocket* socket);
    void deliverAccepted(const QString& manifestUrl);
    void deliverRefused(int refusal);
    void flush();

    QLocalServer* server_ = nullptr;
    bool ready_ = false;
    QString heldUrl_;          // newest held link (a newer one replaces it)
    QVector<int> heldRefusals_;
};
