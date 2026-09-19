#pragma once

#include <QLocalServer>
#include <QObject>
#include <QString>

#include <memory>

namespace quadcap::ui {

/*!
 * Ensures one running copy of the application per user, and lets a second launch raise the first.
 *
 * This is not tidiness. The capture card's ALSA node is opened exclusively, so a second instance
 * cannot take the audio and its pipeline fails with "GStreamer refused to start the pipeline" —
 * a message that says nothing about the real cause and sends you looking at the wrong thing.
 *
 * Detection is a Unix socket rather than a lock file: a lock file left behind by a crash looks
 * exactly like a running instance, whereas a socket nobody is listening on simply refuses the
 * connection, so recovery is automatic.
 */
class SingleInstance final : public QObject {
    Q_OBJECT

public:
    /*!
     * \a name identifies the application; the socket is namespaced per user so two people logged
     * into the same machine do not block each other.
     */
    explicit SingleInstance(const QString &name, QObject *parent = nullptr);
    ~SingleInstance() override;

    SingleInstance(const SingleInstance &) = delete;
    SingleInstance &operator=(const SingleInstance &) = delete;

    //! True when this process claimed the socket and should carry on starting up.
    [[nodiscard]] bool isPrimary() const;

    //! Asks the running instance to come to the front. Only meaningful when !isPrimary().
    bool raiseExisting();

signals:
    //! Another launch asked us to show ourselves.
    void raiseRequested();

private:
    QString socketPath_;
    std::unique_ptr<QLocalServer> server_;
    bool primary_ = false;
};

} // namespace quadcap::ui
