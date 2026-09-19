#include "pipeline/CapturePipeline.h"

#include <QDir>
#include <QSignalSpy>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>

using quadcap::pipeline::CapturePipeline;
using quadcap::pipeline::PipelineConfig;

class PipelineTests final : public QObject {
    Q_OBJECT

private slots:
    void rejectsInvalidConfiguration()
    {
        CapturePipeline pipeline;
        QString error;
        PipelineConfig config;
        config.testSource = true;
        config.width = 0;
        QVERIFY(!pipeline.start(config, &error));
        QVERIFY(!error.isEmpty());
    }

    void splitsGameAndMicIntoSeparateTracks()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());

        CapturePipeline pipeline;
        QSignalSpy errors(&pipeline, &CapturePipeline::errorOccurred);
        PipelineConfig config;
        config.testSource = true;
        config.hardwareEncoder = false;
        config.width = 320;
        config.height = 180;
        config.frameRate = 30;
        config.segmentSeconds = 1;
        config.ringMinutes = 1;
        config.ringDirectory = temporary.filePath(QStringLiteral("ring"));
        // Any non-empty device stands in for the card here; testSource swaps in a tone generator.
        config.gameAudioDevice = QStringLiteral("test");
        config.captureMic = true;

        QString error;
        QVERIFY2(pipeline.start(config, &error), qPrintable(error));

        // Mix first so the file is usable untouched, then the isolated sources for post.
        QCOMPARE(pipeline.audioTrackNames(),
            (QStringList {QStringLiteral("mix"), QStringLiteral("game"), QStringLiteral("mic")}));

        const auto recording = temporary.filePath(QStringLiteral("tracks.mkv"));
        QVERIFY2(pipeline.startRecording(recording, &error), qPrintable(error));
        QTest::qWait(2500);
        pipeline.stopRecording();
        QTRY_VERIFY_WITH_TIMEOUT(!pipeline.isRecording(), 5000);
        pipeline.stop();

        QCOMPARE(errors.count(), 0);
        QVERIFY(QFileInfo(recording).size() > 1024);
    }

    void keepsCapturingWhenNoAudioSourceExists()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());

        CapturePipeline pipeline;
        PipelineConfig config;
        config.testSource = true;
        config.hardwareEncoder = false;
        config.width = 320;
        config.height = 180;
        config.frameRate = 30;
        config.segmentSeconds = 1;
        config.ringMinutes = 1;
        config.ringDirectory = temporary.filePath(QStringLiteral("ring"));
        config.enableAudio = false;

        QString error;
        QVERIFY2(pipeline.start(config, &error), qPrintable(error));
        QVERIFY(pipeline.audioTrackNames().isEmpty());
        QTest::qWait(1500);
        QVERIFY(pipeline.isRunning());
        pipeline.stop();
    }

    void sharesEncoderBetweenRingAndRecording()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());

        CapturePipeline pipeline;
        QSignalSpy errors(&pipeline, &CapturePipeline::errorOccurred);
        QSignalSpy recordings(&pipeline, &CapturePipeline::recordingChanged);
        PipelineConfig config;
        config.testSource = true;
        config.hardwareEncoder = false;
        config.width = 640;
        config.height = 360;
        config.frameRate = 30;
        config.segmentSeconds = 1;
        config.ringMinutes = 1;
        config.ringDirectory = temporary.filePath(QStringLiteral("ring"));

        QString error;
        QVERIFY2(pipeline.start(config, &error), qPrintable(error));
        const auto recording = temporary.filePath(QStringLiteral("capture.mkv"));
        QVERIFY2(pipeline.startRecording(recording, &error), qPrintable(error));
        QTest::qWait(2500);
        pipeline.stopRecording();
        QTRY_VERIFY_WITH_TIMEOUT(recordings.count() >= 2, 5000);
        QVERIFY(!pipeline.isRecording());
        pipeline.stop();

        QCOMPARE(errors.count(), 0);
        QVERIFY(QFileInfo(recording).size() > 1024);
        QVERIFY(QDir(config.ringDirectory).entryList({QStringLiteral("*.mkv")}, QDir::Files).size() >= 1);
        QCOMPARE(recordings.count(), 2);
    }
};

QTEST_GUILESS_MAIN(PipelineTests)
#include "PipelineTests.moc"
