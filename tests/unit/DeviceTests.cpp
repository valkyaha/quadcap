#include "device/DeviceTypes.h"
#include "device/SystemCheck.h"

#include <QTest>

using quadcap::device::DeviceState;
using quadcap::device::SignalMode;

class DeviceTests final : public QObject {
    Q_OBJECT

  private slots:
    void calculatesProgressiveRate() {
        const SignalMode mode{
            .width = 3840,
            .height = 2160,
            .pixelClockHz = 594'000'000,
            .totalWidth = 4400,
            .totalHeight = 2250,
            .interlaced = false,
        };
        QCOMPARE(mode.framesPerSecond(), 60.0);
        QCOMPARE(mode.toString(), QStringLiteral("3840x2160p60.00"));
    }

    void acceptsAModeWithNoPixelClock() {
        // sc0710's procedural timings report the active area but zero the clock and every porch.
        const SignalMode mode{.width = 3840, .height = 2160};
        QVERIFY(mode.isValid());
        QVERIFY(!mode.hasFrameRate());
        QCOMPARE(mode.framesPerSecond(), 0.0);
        QCOMPARE(mode.toString(), QStringLiteral("3840x2160p"));
    }

    void rejectsAModeWithNoDimensions() {
        const SignalMode mode{};
        QVERIFY(!mode.isValid());
        QCOMPARE(mode.toString(), QStringLiteral("unknown mode"));
    }

    void namesEverySetupIssue() {
        using quadcap::device::SetupIssue;
        using quadcap::device::setupIssueName;
        QCOMPARE(setupIssueName(SetupIssue::Ready), QStringLiteral("ready"));
        QCOMPARE(setupIssueName(SetupIssue::NoCard), QStringLiteral("no-card"));
        QCOMPARE(setupIssueName(SetupIssue::DriverNotInstalled),
                 QStringLiteral("driver-not-installed"));
        QCOMPARE(setupIssueName(SetupIssue::DriverNotLoaded), QStringLiteral("driver-not-loaded"));
        QCOMPARE(setupIssueName(SetupIssue::AwaitingMokEnrollment),
                 QStringLiteral("awaiting-mok-enrollment"));
        QCOMPARE(setupIssueName(SetupIssue::NodeUnreadable), QStringLiteral("node-unreadable"));
    }

    void reportsActionableSetupAdvice() {
        // Environment-dependent by nature, so this asserts the contract rather than a verdict:
        // anything short of ready must say what is wrong, and never leave a dead end.
        const auto status = quadcap::device::inspectSystem();
        if (status.ready()) {
            QVERIFY(status.headline.isEmpty() || status.headline == QStringLiteral("Ready"));
            QVERIFY(status.command.isEmpty());
            return;
        }
        QVERIFY2(!status.headline.isEmpty(), "a blocked setup must name the problem");
        QVERIFY2(!status.steps.isEmpty(), "a blocked setup must say what to do");
        for (const auto &step : status.steps) {
            QVERIFY(!step.trimmed().isEmpty());
        }
    }

    void namesEveryState() {
        QCOMPARE(quadcap::device::stateName(DeviceState::NoCard), QStringLiteral("no-card"));
        QCOMPARE(quadcap::device::stateName(DeviceState::NoDriver), QStringLiteral("no-driver"));
        QCOMPARE(quadcap::device::stateName(DeviceState::NoSignal), QStringLiteral("no-signal"));
        QCOMPARE(quadcap::device::stateName(DeviceState::Locked), QStringLiteral("locked"));
        QCOMPARE(quadcap::device::stateName(DeviceState::Error), QStringLiteral("error"));
    }
};

QTEST_GUILESS_MAIN(DeviceTests)
#include "DeviceTests.moc"
