#include "device/DeviceDiscovery.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace quadcap::device {
namespace {

QString readTrimmed(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QString::fromLatin1(file.readAll()).trimmed().toLower();
}

QString extractPciAddress(const QString &path) {
    const auto pieces = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (auto it = pieces.crbegin(); it != pieces.crend(); ++it) {
        if (it->size() == 12 && (*it)[4] == QLatin1Char(':') && (*it)[7] == QLatin1Char(':') &&
            (*it)[10] == QLatin1Char('.')) {
            return *it;
        }
    }
    return {};
}

} // namespace

QString DeviceDiscovery::obsLoopbackDevice() {
    const QDir sys(QStringLiteral("/sys/class/video4linux"));
    const auto wanted = QString::fromLatin1(ObsLoopbackLabel).toLower();
    for (const auto &entry : sys.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        // v4l2loopback publishes its card_label as the device name, which is what the installer
        // sets and what tells this node apart from the capture card.
        if (readTrimmed(sys.filePath(entry) + QStringLiteral("/name")) != wanted) {
            continue;
        }
        const auto node = QStringLiteral("/dev/") + entry;
        if (QFileInfo::exists(node)) {
            return node;
        }
    }
    return {};
}

bool DeviceDiscovery::cardPresent(QString *pciAddress) {
    const QDir pci(QStringLiteral("/sys/bus/pci/devices"));
    for (const auto &entry : pci.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const auto root = pci.absoluteFilePath(entry);
        if (readTrimmed(root + QStringLiteral("/vendor")) == QLatin1String(VendorId) &&
            readTrimmed(root + QStringLiteral("/device")) == QLatin1String(DeviceId)) {
            if (pciAddress) {
                *pciAddress = entry;
            }
            return true;
        }
    }
    return false;
}

QVector<DiscoveredDevice> DeviceDiscovery::videoDevices() {
    QVector<DiscoveredDevice> result;
    const QDir videoClass(QStringLiteral("/sys/class/video4linux"));
    const auto entries =
        videoClass.entryList({QStringLiteral("video*")}, QDir::Dirs | QDir::System);

    for (const auto &entry : entries) {
        const auto node = QStringLiteral("/dev/") + entry;
        const auto pathBytes = node.toLocal8Bit();
        const int fd = ::open(pathBytes.constData(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }

        v4l2_capability caps{};
        const int queryResult = ::ioctl(fd, VIDIOC_QUERYCAP, &caps);
        ::close(fd);
        if (queryResult < 0) {
            continue;
        }

        DiscoveredDevice device;
        device.node = node;
        device.cardName = QString::fromLocal8Bit(reinterpret_cast<const char *>(caps.card));
        device.driver = QString::fromLatin1(reinterpret_cast<const char *>(caps.driver));
        device.busInfo = QString::fromLatin1(reinterpret_cast<const char *>(caps.bus_info));
        device.pciAddress = pciAddressFromSysfs(node);
        if (device.driver == QLatin1String(DriverName) &&
            (device.pciAddress.isEmpty() || device.busInfo.contains(device.pciAddress))) {
            result.push_back(std::move(device));
        }
    }
    return result;
}

QString DeviceDiscovery::pciAddressFromSysfs(const QString &videoNode) {
    const auto name = QFileInfo(videoNode).fileName();
    const QFileInfo deviceLink(QStringLiteral("/sys/class/video4linux/%1/device").arg(name));
    return extractPciAddress(deviceLink.canonicalFilePath());
}

QString DeviceDiscovery::alsaDeviceForPci(const QString &pciAddress) {
    if (pciAddress.isEmpty()) {
        return {};
    }

    const QDir soundClass(QStringLiteral("/sys/class/sound"));
    const auto cards =
        soundClass.entryList({QStringLiteral("card*")}, QDir::Dirs | QDir::NoDotAndDotDot);
    for (const auto &card : cards) {
        // /sys/class/sound/cardN/device resolves to the PCI device that owns it.
        const QFileInfo owner(soundClass.filePath(card + QStringLiteral("/device")));
        if (!owner.exists() || owner.canonicalFilePath().isEmpty()) {
            continue;
        }
        if (QFileInfo(owner.canonicalFilePath()).fileName() != pciAddress) {
            continue;
        }
        const auto index = QStringView(card).sliced(4).toString();
        bool ok = false;
        const auto number = index.toInt(&ok);
        if (ok) {
            return QStringLiteral("hw:%1,0").arg(number);
        }
    }
    return {};
}

} // namespace quadcap::device
