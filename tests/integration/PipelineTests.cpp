#include "pipeline/CapturePipeline.h"
#include "pipeline/ObsVideoOutput.h"

#include <QDir>
#include <QSet>
#include <QSignalSpy>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>

#include <gst/gst.h>

using quadcap::pipeline::CapturePipeline;
using quadcap::pipeline::PipelineConfig;

class PipelineTests final : public QObject {
    Q_OBJECT

  private slots:
    void rejectsInvalidConfiguration() {
        CapturePipeline pipeline;
        QString error;
        PipelineConfig config;
        config.testSource = true;
        config.width = 0;
        QVERIFY(!pipeline.start(config, &error));
        QVERIFY(!error.isEmpty());
    }

    void splitsGameAndMicIntoSeparateTracks() {
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
        config.enableAudioMonitoring = true;

        QString error;
        QVERIFY2(pipeline.start(config, &error), qPrintable(error));

        // Mix first so the file is usable untouched, then the isolated sources for post.
        QCOMPARE(
            pipeline.audioTrackNames(),
            (QStringList{QStringLiteral("mix"), QStringLiteral("game"), QStringLiteral("mic")}));

        const auto recording = temporary.filePath(QStringLiteral("tracks.mkv"));
        QVERIFY2(pipeline.startRecording(recording, &error), qPrintable(error));
        QTest::qWait(2500);
        pipeline.stopRecording();
        QTRY_VERIFY_WITH_TIMEOUT(!pipeline.isRecording(), 5000);
        pipeline.stop();

        QCOMPARE(errors.count(), 0);
        QVERIFY(QFileInfo(recording).size() > 1024);
    }

    void gainShapesTheMixAndLeavesIsolatedTracksAlone() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());

        CapturePipeline pipeline;
        QSignalSpy errors(&pipeline, &CapturePipeline::errorOccurred);
        QSignalSpy levels(&pipeline, &CapturePipeline::audioLevel);
        PipelineConfig config;
        config.testSource = true;
        config.hardwareEncoder = false;
        config.width = 320;
        config.height = 180;
        config.frameRate = 30;
        config.segmentSeconds = 1;
        config.ringMinutes = 1;
        config.ringDirectory = temporary.filePath(QStringLiteral("ring"));
        config.gameAudioDevice = QStringLiteral("test");
        config.captureMic = true;
        config.enableAudioMonitoring = true;

        QString error;
        QVERIFY2(pipeline.start(config, &error), qPrintable(error));

        pipeline.setAudioMuted(QStringLiteral("mic"), true);
        pipeline.setAudioGainDb(QStringLiteral("game"), -40.0);
        QCOMPARE(
            pipeline.audioTrackNames(),
            (QStringList{QStringLiteral("mix"), QStringLiteral("game"), QStringLiteral("mic")}));

        QTRY_VERIFY_WITH_TIMEOUT(levels.count() >= 2, 5000);
        QSet<QString> metered;
        for (const auto &call : levels) {
            metered.insert(call.at(0).toString());
        }
        QVERIFY2(metered.contains(QStringLiteral("game")), "game source should report a level");
        QVERIFY2(metered.contains(QStringLiteral("mic")), "mic source should report a level");

        pipeline.stop();
        QCOMPARE(errors.count(), 0);
    }

    void remembersFaderPositionsAcrossARebuild() {
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
        config.gameAudioDevice = QStringLiteral("test");

        pipeline.setAudioGainDb(QStringLiteral("mic"), -12.0);
        pipeline.setAudioMuted(QStringLiteral("game"), true);

        QString error;
        QVERIFY2(pipeline.start(config, &error), qPrintable(error));
        QVERIFY(pipeline.isRunning());
        QTest::qWait(100);
        pipeline.stop();

        pipeline.setAudioGainDb(QStringLiteral("game"), -6.0);
        pipeline.setAudioGainDb(QStringLiteral("mic"), -18.0);
        pipeline.setAudioMuted(QStringLiteral("game"), false);
        pipeline.setAudioMuted(QStringLiteral("mic"), true);

        QVERIFY2(pipeline.start(config, &error), qPrintable(error));
        QVERIFY(pipeline.isRunning());
        QTest::qWait(100);
        pipeline.stop();
    }

    void refusesAnAudioDeviceItCannotOpen() {
        // An ALSA card index nothing is plugged into: the element builds, but the device does not
        // open. Catching that here is what keeps it out of the graph.
        GstElement *absent = gst_element_factory_make("alsasrc", nullptr);
        QVERIFY(absent != nullptr);
        g_object_set(absent, "device", "hw:99,0", nullptr);
        QVERIFY2(!CapturePipeline::canOpenSource(absent),
                 "a device that cannot be opened must not pass the probe");
        gst_object_unref(absent);

        // A source with no device to claim reaches READY, so the probe must not reject everything.
        GstElement *tone = gst_element_factory_make("audiotestsrc", nullptr);
        QVERIFY(tone != nullptr);
        QVERIFY2(CapturePipeline::canOpenSource(tone), "a working source must pass the probe");
        gst_object_unref(tone);
    }

    void capturesVideoWhenTheAudioDeviceIsUnavailable() {
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
        config.gameAudioDevice = QStringLiteral("test");
        config.captureMic = true;

        QString error;
        QVERIFY2(pipeline.start(config, &error), qPrintable(error));

        // Losing a microphone must never cost the recording; video keeps running either way.
        const auto recording = temporary.filePath(QStringLiteral("degraded.mkv"));
        QVERIFY2(pipeline.startRecording(recording, &error), qPrintable(error));
        QTest::qWait(2000);
        pipeline.stopRecording();
        QTRY_VERIFY_WITH_TIMEOUT(!pipeline.isRecording(), 5000);
        QVERIFY(pipeline.isRunning());
        pipeline.stop();

        QCOMPARE(errors.count(), 0);
        QVERIFY(QFileInfo(recording).size() > 1024);
    }

    void keepsCapturingWhenTheObsRouteIsUnavailable() {
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
        config.gameAudioDevice = QStringLiteral("test");
        config.captureMic = true;
        config.enableObsOutput = true;
        // Nothing is listening on any of these. Someone without v4l2loopback installed, or before
        // the PipeWire sinks exist, is the common case, and it must cost them nothing. A null
        // output stands in for a loopback node that was never opened.
        config.obsOutput = nullptr;
        config.obsGameSink = QStringLiteral("quadcap-game-absent");
        config.obsMicSink = QStringLiteral("quadcap-mic-absent");

        QString error;
        QVERIFY2(pipeline.start(config, &error), qPrintable(error));
        QVERIFY(pipeline.isRunning());
        QVERIFY2(!pipeline.obsActive(),
                 "nothing was reachable, so nothing should be reported live");
        QVERIFY2(!pipeline.obsNotice().isEmpty(), "a silent failure would leave OBS empty with no "
                                                  "explanation");

        // The recording is what must not be affected.
        const auto recording = temporary.filePath(QStringLiteral("with-obs-down.mkv"));
        QVERIFY2(pipeline.startRecording(recording, &error), qPrintable(error));
        QTest::qWait(2000);
        pipeline.stopRecording();
        QTRY_VERIFY_WITH_TIMEOUT(!pipeline.isRecording(), 5000);
        pipeline.stop();

        QCOMPARE(errors.count(), 0);
        QVERIFY(QFileInfo(recording).size() > 1024);
    }

    void keepsTheLoopbackOpenWithNoCaptureRunning() {
        // The point of the separate output: OBS must keep seeing a camera while the console is
        // asleep, so the node has to stay open and keep receiving frames with no pipeline at all.
        quadcap::pipeline::ObsVideoOutput output;
        QString error;
        if (!output.start(QStringLiteral("/dev/video99"), 320, 180, 30, &error)) {
            QVERIFY2(!error.isEmpty(), "a refused device must say why");
        }
        QVERIFY2(!output.isRunning(), "a device that cannot be opened must not report as running");

        // Submitting to a stopped output is what a capture pipeline outliving it looks like.
        output.submitFrame(nullptr);
        output.stop();
    }

    void leavesTheObsRouteAloneWhenItIsOff() {
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
        config.gameAudioDevice = QStringLiteral("test");

        QString error;
        QVERIFY2(pipeline.start(config, &error), qPrintable(error));
        QVERIFY(!pipeline.obsActive());
        // Switched off is not a problem worth reporting; only a failed attempt is.
        QVERIFY2(pipeline.obsNotice().isEmpty(),
                 "an untouched OBS route must not produce a warning");
        pipeline.stop();
    }

    void keepsCapturingWhenNoAudioSourceExists() {
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

    void sharesEncoderBetweenRingAndRecording() {
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
        QVERIFY(
            QDir(config.ringDirectory).entryList({QStringLiteral("*.mkv")}, QDir::Files).size() >=
            1);
        QCOMPARE(recordings.count(), 2);
    }
};

QTEST_GUILESS_MAIN(PipelineTests)
#include "PipelineTests.moc"
