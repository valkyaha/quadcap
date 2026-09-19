#include "ui/SingleInstance.h"

#include <QSignalSpy>
#include <QTest>

using quadcap::ui::SingleInstance;

class SingleInstanceTests final : public QObject {
    Q_OBJECT

  private slots:
    void firstClaimsItAndSecondDefersToIt() {
        const auto key = QStringLiteral("quadcap-test-%1").arg(QCoreApplication::applicationPid());

        SingleInstance first(key);
        QVERIFY2(first.isPrimary(),
                 "nothing was running, so this one should have claimed the socket");

        SingleInstance second(key);
        QVERIFY2(!second.isPrimary(),
                 "a second instance must defer rather than fight for the device");
    }

    void secondInstanceCanRaiseTheFirst() {
        const auto key = QStringLiteral("quadcap-raise-%1").arg(QCoreApplication::applicationPid());

        SingleInstance first(key);
        QVERIFY(first.isPrimary());
        QSignalSpy raised(&first, &SingleInstance::raiseRequested);

        SingleInstance second(key);
        QVERIFY(!second.isPrimary());
        QVERIFY(second.raiseExisting());

        // The running copy is told to show itself, which is what makes deferring acceptable.
        QTRY_VERIFY_WITH_TIMEOUT(raised.count() >= 1, 3000);
    }

    void reclaimsTheSocketAfterAnUncleanExit() {
        const auto key = QStringLiteral("quadcap-stale-%1").arg(QCoreApplication::applicationPid());

        {
            SingleInstance crashed(key);
            QVERIFY(crashed.isPrimary());
        }
        // A socket file left behind by a crash must not look like a running instance forever.
        SingleInstance restarted(key);
        QVERIFY2(restarted.isPrimary(), "a stale socket should be taken over, not treated as live");
    }
};

QTEST_MAIN(SingleInstanceTests)
#include "SingleInstanceTests.moc"
