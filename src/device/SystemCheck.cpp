#include "device/SystemCheck.h"

#include "device/DeviceDiscovery.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <unistd.h>

namespace quadcap::device {
namespace {

constexpr auto InstallCommand = "sudo ./packaging/install.sh";

bool moduleLoaded() {
    return QFileInfo::exists(
        QStringLiteral("/sys/module/%1").arg(QLatin1String(DeviceDiscovery::DriverName)));
}

bool dkmsRegistered() {
    // dkms keeps one directory per registered package; no need to shell out to read that.
    return QFileInfo::exists(
        QStringLiteral("/var/lib/dkms/%1").arg(QLatin1String(DeviceDiscovery::DriverName)));
}

/*!
 * Reads the firmware's SecureBoot variable directly.
 *
 * The efivars blob is a 4-byte attribute header followed by the value, so the last byte is the
 * flag. Absent variable means a machine booted without UEFI Secure Boot at all.
 */
bool secureBootEnabled() {
    const QDir efivars(QStringLiteral("/sys/firmware/efi/efivars"));
    const auto matches = efivars.entryList({QStringLiteral("SecureBoot-*")}, QDir::Files);
    if (matches.isEmpty()) {
        return false;
    }
    QFile variable(efivars.filePath(matches.constFirst()));
    if (!variable.open(QIODevice::ReadOnly)) {
        return false;
    }
    const auto bytes = variable.read(5);
    return bytes.size() == 5 && bytes.at(4) != 0;
}

} // namespace

QString setupIssueName(const SetupIssue issue) {
    switch (issue) {
    case SetupIssue::Ready:
        return QStringLiteral("ready");
    case SetupIssue::NoCard:
        return QStringLiteral("no-card");
    case SetupIssue::DriverNotInstalled:
        return QStringLiteral("driver-not-installed");
    case SetupIssue::DriverNotLoaded:
        return QStringLiteral("driver-not-loaded");
    case SetupIssue::AwaitingMokEnrollment:
        return QStringLiteral("awaiting-mok-enrollment");
    case SetupIssue::NodeUnreadable:
        return QStringLiteral("node-unreadable");
    }
    return QStringLiteral("unknown");
}

SetupStatus inspectSystem() {
    SetupStatus status;
    status.cardPresent = DeviceDiscovery::cardPresent(&status.pciAddress);
    status.moduleLoaded = moduleLoaded();
    status.dkmsRegistered = dkmsRegistered();
    status.secureBootEnabled = secureBootEnabled();

    const auto devices = DeviceDiscovery::videoDevices();
    status.videoNodePresent = !devices.isEmpty();
    if (status.videoNodePresent) {
        const auto node = QFile::encodeName(devices.constFirst().node);
        status.videoNodeReadable = ::access(node.constData(), R_OK | W_OK) == 0;
    }

    if (!status.cardPresent) {
        status.issue = SetupIssue::NoCard;
        status.headline = QStringLiteral("No supported capture card found");
        status.steps = {
            QStringLiteral("Check that the card is seated in a PCIe slot and that the machine sees "
                           "it: lspci -nn | grep 12ab:0710"),
            QStringLiteral("A slot wired narrower than x4 cannot carry 4K60, so prefer a x4 or "
                           "wider slot."),
        };
        return status;
    }

    if (status.videoNodePresent && status.moduleLoaded) {
        if (!status.videoNodeReadable) {
            status.issue = SetupIssue::NodeUnreadable;
            status.headline = QStringLiteral("The capture device is not readable by this user");
            status.steps = {
                QStringLiteral("Add yourself to the video group, then log out and back in."),
            };
            status.command = QStringLiteral("sudo usermod -aG video $USER");
            return status;
        }
        status.issue = SetupIssue::Ready;
        status.headline = QStringLiteral("Ready");
        return status;
    }

    if (!status.dkmsRegistered) {
        status.issue = SetupIssue::DriverNotInstalled;
        status.headline = QStringLiteral("The sc0710 driver is not installed");
        status.steps = {
            QStringLiteral("This card has no in-tree driver, so one has to be built for your "
                           "kernel. The installer registers it with DKMS, which rebuilds it "
                           "automatically whenever the kernel updates."),
            QStringLiteral("Run the installer from the project directory."),
        };
        if (status.secureBootEnabled) {
            status.steps.append(QStringLiteral(
                "Secure Boot is on, so the module must be signed by a key your "
                "firmware trusts. If the installer has to enroll one it will ask you "
                "to choose a password, then you must reboot and complete enrollment "
                "in the blue MOK Manager screen using that password."));
        }
        status.command = QString::fromLatin1(InstallCommand);
        return status;
    }

    if (status.secureBootEnabled) {
        /*
         * DKMS has built it but the kernel is not running it. Under Secure Boot the overwhelmingly
         * common reason is an unenrolled key: the module exists and is signed, and the kernel
         * refuses it with "Key was rejected by service" until the enrollment reboot happens.
         */
        status.issue = SetupIssue::AwaitingMokEnrollment;
        status.headline = QStringLiteral("The driver is built but the kernel will not load it");
        status.steps = {
            QStringLiteral(
                "Under Secure Boot this almost always means the signing key has not been "
                "enrolled yet."),
            QStringLiteral("Enroll it, choose a password when asked, then reboot and complete "
                           "enrollment in the blue MOK Manager screen."),
            QStringLiteral("If it was already enrolled, load the module and check dmesg for the "
                           "real reason."),
        };
        status.command = QStringLiteral("sudo update-secureboot-policy --enroll-key");
        return status;
    }

    status.issue = SetupIssue::DriverNotLoaded;
    status.headline = QStringLiteral("The driver is installed but not loaded");
    status.steps = {
        QStringLiteral("Load it now. The installer also arranges for it to load at every boot."),
    };
    status.command = QStringLiteral("sudo modprobe sc0710");
    return status;
}

} // namespace quadcap::device
