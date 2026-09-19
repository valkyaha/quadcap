#include "ui/SingleInstance.h"

#include <QLocalSocket>

#include <unistd.h>

namespace quadcap::ui {
namespace {

// Long enough to cross a loaded machine, short enough that a stale socket does not delay startup.
constexpr int ConnectTimeoutMs = 300;
constexpr auto RaiseMessage = "raise\n";

} // namespace

SingleInstance::SingleInstance(const QString &name, QObject *parent)
    : QObject(parent), socketPath_(QStringLiteral("%1-%2").arg(name).arg(::getuid())) {
    /*
     * Try to connect first. Success means somebody is listening, so this process is the second one.
     * Failure means either nothing is running or a previous run died and left the socket file
     * behind — indistinguishable from here, and both are handled by taking it over below.
     */
    QLocalSocket probe;
    probe.connectToServer(socketPath_);
    if (probe.waitForConnected(ConnectTimeoutMs)) {
        probe.disconnectFromServer();
        primary_ = false;
        return;
    }

    // Nobody answered. Clear any socket a crashed run left behind, then claim it.
    QLocalServer::removeServer(socketPath_);
    server_ = std::make_unique<QLocalServer>(this);
    connect(server_.get(), &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket *client = server_->nextPendingConnection()) {
            connect(client, &QLocalSocket::disconnected, client, &QLocalSocket::deleteLater);
            emit raiseRequested();
        }
    });
    primary_ = server_->listen(socketPath_);
    if (!primary_) {
        // Losing the race is not a failure worth blocking on; treat it as "someone else is
        // primary".
        server_.reset();
    }
}

SingleInstance::~SingleInstance() {
    if (server_) {
        server_->close();
    }
}

bool SingleInstance::isPrimary() const {
    return primary_;
}

bool SingleInstance::raiseExisting() {
    QLocalSocket socket;
    socket.connectToServer(socketPath_);
    if (!socket.waitForConnected(ConnectTimeoutMs)) {
        return false;
    }
    socket.write(RaiseMessage);
    const bool sent = socket.waitForBytesWritten(ConnectTimeoutMs);
    socket.disconnectFromServer();
    return sent;
}

} // namespace quadcap::ui
