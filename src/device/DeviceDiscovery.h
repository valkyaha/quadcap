#pragma once

#include <QString>
#include <QVector>

namespace quadcap::device {

struct DiscoveredDevice {
    QString node;
    QString cardName;
    QString driver;
    QString busInfo;
    QString pciAddress;
};

class DeviceDiscovery final {
public:
    static constexpr auto VendorId = "0x12ab";
    static constexpr auto DeviceId = "0x0710";
    static constexpr auto DriverName = "sc0710";

    [[nodiscard]] static bool cardPresent(QString *pciAddress = nullptr);
    [[nodiscard]] static QVector<DiscoveredDevice> videoDevices();
    [[nodiscard]] static QString pciAddressFromSysfs(const QString &videoNode);

    /*!
     * ALSA device string for the capture card's HDMI audio, e.g. "hw:3,0".
     *
     * Matched through sysfs by PCI address rather than by card name or index, both of which move
     * when USB audio devices come and go. Empty when the card exposes no ALSA node.
     */
    [[nodiscard]] static QString alsaDeviceForPci(const QString &pciAddress);
};

} // namespace quadcap::device

