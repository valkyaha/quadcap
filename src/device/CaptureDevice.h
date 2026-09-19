#pragma once

#include "device/DeviceTypes.h"

#include <QByteArray>
#include <QObject>

#include <memory>

class QSocketNotifier;

namespace quadcap::device {

class CaptureDevice final : public QObject {
    Q_OBJECT

  public:
    explicit CaptureDevice(QObject *parent = nullptr);
    ~CaptureDevice() override;

    CaptureDevice(const CaptureDevice &) = delete;
    CaptureDevice &operator=(const CaptureDevice &) = delete;

    [[nodiscard]] DeviceStatus probe();
    [[nodiscard]] DeviceStatus status() const;
    [[nodiscard]] QByteArray readEdid(QString *error = nullptr) const;
    [[nodiscard]] bool writeEdid(const QByteArray &edid, QString *error = nullptr) const;

    /*!
     * Reads or sets which EDID the card advertises to the source.
     *
     * The driver resets this to Internal every probe, so it has to be applied after each load.
     * Changing it bounces HPD and the source renegotiates, which drops the signal for a moment.
     */
    [[nodiscard]] bool edidSource(EdidSource *source, QString *error = nullptr) const;
    [[nodiscard]] bool setEdidSource(EdidSource source, QString *error = nullptr) const;
    [[nodiscard]] bool startMonitoring(QString *error = nullptr);
    void stopMonitoring();

  signals:
    void statusChanged(const quadcap::device::DeviceStatus &status);

  private slots:
    void drainEvents();

  private:
    [[nodiscard]] bool openNode(const QString &node, QString *error);
    [[nodiscard]] DeviceStatus queryStatus() const;

    //! Looks the control up by name rather than a hard-coded vendor id. 0 when the driver lacks it.
    [[nodiscard]] quint32 edidSourceControlId() const;
    void publish(DeviceStatus next);

    int fd_ = -1;
    DeviceStatus status_;
    std::unique_ptr<QSocketNotifier> notifier_;
};

} // namespace quadcap::device
