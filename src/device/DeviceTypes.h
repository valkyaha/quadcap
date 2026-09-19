#pragma once

#include <QMetaType>
#include <QString>

namespace quadcap::device {

enum class DeviceState {
    NoCard,
    NoDriver,
    NoSignal,
    Locked,
    Error,
};

/*!
 * Which EDID the card presents to the console, which is what the console picks its output mode from.
 *
 * Internal advertises the card's own capabilities and ignores whatever is plugged into HDMI OUT, so
 * a console will happily choose 4K60 HDR that the passthrough display cannot show — it reports an
 * unsupported input and stays black. Merged has the MCU intersect the card's capabilities with the
 * display's, which is the right default for a passthrough rig.
 */
enum class EdidSource {
    Internal = 0,
    Display = 1,
    Merged = 2,
};

[[nodiscard]] QString edidSourceName(EdidSource source);

struct SignalMode {
    quint32 width = 0;
    quint32 height = 0;
    quint64 pixelClockHz = 0;
    quint32 totalWidth = 0;
    quint32 totalHeight = 0;
    bool interlaced = false;

    [[nodiscard]] double framesPerSecond() const;

    //! False when the driver reported no pixel clock, which its procedural timings do not carry.
    [[nodiscard]] bool hasFrameRate() const;

    [[nodiscard]] bool isValid() const;
    [[nodiscard]] QString toString() const;

    friend bool operator==(const SignalMode &, const SignalMode &) = default;
};

struct DeviceStatus {
    DeviceState state = DeviceState::NoCard;
    QString deviceNode;
    QString pciAddress;
    QString cardName;
    QString error;
    SignalMode mode;

    friend bool operator==(const DeviceStatus &, const DeviceStatus &) = default;
};

[[nodiscard]] QString stateName(DeviceState state);

} // namespace quadcap::device

Q_DECLARE_METATYPE(quadcap::device::DeviceStatus)

