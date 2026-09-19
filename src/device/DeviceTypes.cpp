#include "device/DeviceTypes.h"

#include <QLocale>

namespace quadcap::device {

double SignalMode::framesPerSecond() const
{
    if (pixelClockHz == 0 || totalWidth == 0 || totalHeight == 0) {
        return 0.0;
    }

    const auto fieldRate = static_cast<double>(pixelClockHz)
        / static_cast<double>(static_cast<quint64>(totalWidth) * totalHeight);
    return interlaced ? fieldRate * 2.0 : fieldRate;
}

bool SignalMode::hasFrameRate() const
{
    return framesPerSecond() > 0.0;
}

bool SignalMode::isValid() const
{
    /*
     * Dimensions alone decide this. When sc0710 falls back to a procedural timing it reports the
     * active area from the card's registers but zeroes the pixel clock and every porch, so
     * demanding a frame rate here would call a perfectly good 3840x2160 signal "no signal".
     */
    return width != 0 && height != 0;
}

QString SignalMode::toString() const
{
    if (!isValid()) {
        return QStringLiteral("unknown mode");
    }

    const auto scan = interlaced ? QLatin1Char('i') : QLatin1Char('p');
    const auto geometry = QStringLiteral("%1x%2%3").arg(width).arg(height).arg(scan);
    if (!hasFrameRate()) {
        return geometry;
    }
    return geometry + QLocale::c().toString(framesPerSecond(), 'f', 2);
}

QString edidSourceName(const EdidSource source)
{
    switch (source) {
    case EdidSource::Internal:
        return QStringLiteral("internal");
    case EdidSource::Display:
        return QStringLiteral("display");
    case EdidSource::Merged:
        return QStringLiteral("merged");
    }
    return QStringLiteral("internal");
}

QString stateName(const DeviceState state)
{
    switch (state) {
    case DeviceState::NoCard:
        return QStringLiteral("no-card");
    case DeviceState::NoDriver:
        return QStringLiteral("no-driver");
    case DeviceState::NoSignal:
        return QStringLiteral("no-signal");
    case DeviceState::Locked:
        return QStringLiteral("locked");
    case DeviceState::Error:
        return QStringLiteral("error");
    }
    return QStringLiteral("error");
}

} // namespace quadcap::device

