#include "device/CaptureDevice.h"

#include "device/DeviceDiscovery.h"

#include <QSocketNotifier>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace quadcap::device {
namespace {

QString systemError(const char *operation) {
    return QStringLiteral("%1: %2").arg(QString::fromLatin1(operation),
                                        QString::fromLocal8Bit(std::strerror(errno)));
}

SignalMode toMode(const v4l2_dv_timings &timings) {
    if (timings.type != V4L2_DV_BT_656_1120) {
        return {};
    }
    const auto &bt = timings.bt;
    SignalMode result;
    result.width = bt.width;
    result.height = bt.height;
    result.pixelClockHz = bt.pixelclock;
    result.totalWidth = bt.width + bt.hfrontporch + bt.hsync + bt.hbackporch;
    result.totalHeight = bt.height + bt.vfrontporch + bt.vsync + bt.vbackporch + bt.il_vfrontporch +
                         bt.il_vsync + bt.il_vbackporch;
    result.interlaced = bt.interlaced != 0;
    return result;
}

} // namespace

CaptureDevice::CaptureDevice(QObject *parent) : QObject(parent) {}

CaptureDevice::~CaptureDevice() {
    stopMonitoring();
}

DeviceStatus CaptureDevice::probe() {
    stopMonitoring();

    QString pciAddress;
    if (!DeviceDiscovery::cardPresent(&pciAddress)) {
        DeviceStatus missing;
        missing.state = DeviceState::NoCard;
        missing.error = QStringLiteral("No supported 12ab:0710 capture card was found");
        publish(std::move(missing));
        return status_;
    }

    const auto devices = DeviceDiscovery::videoDevices();
    if (devices.isEmpty()) {
        DeviceStatus unavailable;
        unavailable.state = DeviceState::NoDriver;
        unavailable.pciAddress = pciAddress;
        unavailable.error =
            QStringLiteral(
                "Card found at %1, but sc0710 is not bound or no V4L2 node is accessible")
                .arg(pciAddress);
        publish(std::move(unavailable));
        return status_;
    }

    QString error;
    if (!openNode(devices.front().node, &error)) {
        DeviceStatus failed;
        failed.state = DeviceState::Error;
        failed.deviceNode = devices.front().node;
        failed.pciAddress = devices.front().pciAddress;
        failed.cardName = devices.front().cardName;
        failed.error = error;
        publish(std::move(failed));
        return status_;
    }

    status_.deviceNode = devices.front().node;
    status_.pciAddress = devices.front().pciAddress;
    status_.cardName = devices.front().cardName;
    publish(queryStatus());
    return status_;
}

DeviceStatus CaptureDevice::status() const {
    return status_;
}

QByteArray CaptureDevice::readEdid(QString *error) const {
    if (fd_ < 0) {
        if (error) {
            *error = QStringLiteral("Capture device is not open");
        }
        return {};
    }

    QByteArray bytes(256, Qt::Uninitialized);
    v4l2_edid edid{};
    edid.pad = 0;
    edid.start_block = 0;
    edid.blocks = 2;
    edid.edid = reinterpret_cast<quint8 *>(bytes.data());
    if (::ioctl(fd_, VIDIOC_G_EDID, &edid) < 0) {
        if (error) {
            *error = systemError("VIDIOC_G_EDID");
        }
        return {};
    }
    bytes.resize(static_cast<qsizetype>(edid.blocks) * 128);
    return bytes;
}

bool CaptureDevice::writeEdid(const QByteArray &edidBytes, QString *error) const {
    if (fd_ < 0) {
        if (error) {
            *error = QStringLiteral("Capture device is not open");
        }
        return false;
    }
    if (edidBytes.isEmpty() || edidBytes.size() % 128 != 0 || edidBytes.size() > 256) {
        if (error) {
            *error = QStringLiteral("EDID must contain one or two complete 128-byte blocks");
        }
        return false;
    }

    v4l2_edid edid{};
    edid.pad = 0;
    edid.start_block = 0;
    edid.blocks = static_cast<quint32>(edidBytes.size() / 128);
    edid.edid = reinterpret_cast<quint8 *>(const_cast<char *>(edidBytes.constData()));
    if (::ioctl(fd_, VIDIOC_S_EDID, &edid) < 0) {
        if (error) {
            *error = systemError("VIDIOC_S_EDID");
        }
        return false;
    }
    return true;
}

quint32 CaptureDevice::edidSourceControlId() const {
    if (fd_ < 0) {
        return 0;
    }
    // Walk the driver's control list instead of hard-coding the vendor id, which is private to
    // sc0710 and has no guarantee of staying put across driver versions.
    v4l2_queryctrl query{};
    query.id = V4L2_CTRL_FLAG_NEXT_CTRL;
    while (::ioctl(fd_, VIDIOC_QUERYCTRL, &query) == 0) {
        const auto name = QString::fromLatin1(reinterpret_cast<const char *>(query.name)).trimmed();
        if (name.compare(QLatin1String("EDID Source"), Qt::CaseInsensitive) == 0) {
            return query.id;
        }
        query.id |= V4L2_CTRL_FLAG_NEXT_CTRL;
    }
    return 0;
}

bool CaptureDevice::edidSource(EdidSource *source, QString *error) const {
    const auto id = edidSourceControlId();
    if (id == 0) {
        if (error) {
            *error = QStringLiteral("This driver does not expose an EDID source control");
        }
        return false;
    }

    v4l2_control control{};
    control.id = id;
    if (::ioctl(fd_, VIDIOC_G_CTRL, &control) < 0) {
        if (error) {
            *error = systemError("VIDIOC_G_CTRL");
        }
        return false;
    }
    if (source) {
        *source = static_cast<EdidSource>(control.value);
    }
    return true;
}

bool CaptureDevice::setEdidSource(const EdidSource source, QString *error) const {
    const auto id = edidSourceControlId();
    if (id == 0) {
        if (error) {
            *error = QStringLiteral("This driver does not expose an EDID source control");
        }
        return false;
    }

    v4l2_control control{};
    control.id = id;
    control.value = static_cast<qint32>(source);
    if (::ioctl(fd_, VIDIOC_S_CTRL, &control) < 0) {
        if (error) {
            *error = systemError("VIDIOC_S_CTRL");
        }
        return false;
    }
    return true;
}

bool CaptureDevice::startMonitoring(QString *error) {
    if (fd_ < 0) {
        if (error) {
            *error = QStringLiteral("Capture device is not open");
        }
        return false;
    }

    v4l2_event_subscription subscription{};
    subscription.type = V4L2_EVENT_SOURCE_CHANGE;
    if (::ioctl(fd_, VIDIOC_SUBSCRIBE_EVENT, &subscription) < 0) {
        if (error) {
            *error = systemError("VIDIOC_SUBSCRIBE_EVENT");
        }
        return false;
    }

    /*
     * V4L2 signals a pending event with POLLPRI, not POLLIN, and QSocketNotifier::Exception is what
     * maps to POLLPRI here. Watching Read instead woke us on ordinary readability and then found no
     * event waiting, which is how a perfectly healthy device ended up reported as broken.
     */
    notifier_ = std::make_unique<QSocketNotifier>(fd_, QSocketNotifier::Exception, this);
    connect(notifier_.get(), &QSocketNotifier::activated, this, &CaptureDevice::drainEvents);
    return true;
}

void CaptureDevice::stopMonitoring() {
    notifier_.reset();
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

void CaptureDevice::drainEvents() {
    bool sourceChanged = false;
    while (true) {
        v4l2_event event{};
        if (::ioctl(fd_, VIDIOC_DQEVENT, &event) < 0) {
            // An empty queue is the normal way out of this loop: ENOENT means nothing is pending,
            // EAGAIN that nothing is pending yet. Neither says anything about the device's health.
            if (errno != EAGAIN && errno != ENOENT) {
                auto failed = status_;
                failed.state = DeviceState::Error;
                failed.error = systemError("VIDIOC_DQEVENT");
                publish(std::move(failed));
            }
            break;
        }
        sourceChanged |= event.type == V4L2_EVENT_SOURCE_CHANGE;
    }
    if (sourceChanged) {
        publish(queryStatus());
    }
}

bool CaptureDevice::openNode(const QString &node, QString *error) {
    const auto path = node.toLocal8Bit();
    fd_ = ::open(path.constData(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0) {
        if (error) {
            *error = systemError("open");
        }
        return false;
    }
    return true;
}

DeviceStatus CaptureDevice::queryStatus() const {
    auto next = status_;
    next.error.clear();
    v4l2_dv_timings timings{};
    if (::ioctl(fd_, VIDIOC_QUERY_DV_TIMINGS, &timings) < 0) {
        if (errno == ENOLINK || errno == ENOLCK || errno == ERANGE || errno == ENODATA) {
            next.state = DeviceState::NoSignal;
            next.mode = {};
            return next;
        }
        next.state = DeviceState::Error;
        next.mode = {};
        next.error = systemError("VIDIOC_QUERY_DV_TIMINGS");
        return next;
    }
    next.mode = toMode(timings);
    next.state = next.mode.isValid() ? DeviceState::Locked : DeviceState::NoSignal;
    return next;
}

void CaptureDevice::publish(DeviceStatus next) {
    if (next == status_) {
        return;
    }
    status_ = std::move(next);
    emit statusChanged(status_);
}

} // namespace quadcap::device
