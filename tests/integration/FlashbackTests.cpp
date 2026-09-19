#include "flashback/FlashbackRing.h"
#include "pipeline/CapturePipeline.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <gst/gst.h>

using quadcap::flashback::FlashbackRing;
using quadcap::pipeline::CapturePipeline;
using quadcap::pipeline::PipelineConfig;

namespace {

void writeSegment(const QDir &dir, const int index, const qint64 bytes)
{
    QFile file(dir.filePath(QStringLiteral("segment-%1.mkv").arg(index, 6, 10, QLatin1Char('0'))));
    QVERIFY(file.open(QIODevice::WriteOnly));
    if (bytes > 0) {
        file.write(QByteArray(static_cast<int>(bytes), 'x'));
    }
    file.close();
}

//! Counts the audio tracks a file exposes, by watching matroskademux advertise its pads.
int audioTrackCount(const QString &path)
{
    gst_init(nullptr, nullptr);
    GstElement *pipeline = gst_pipeline_new("count");
    GstElement *source = gst_element_factory_make("filesrc", "src");
    GstElement *demux = gst_element_factory_make("matroskademux", "demux");
    if (!pipeline || !source || !demux) {
        gst_clear_object(&pipeline);
        gst_clear_object(&source);
        gst_clear_object(&demux);
        return -1;
    }

    const auto location = QFile::encodeName(path);
    g_object_set(source, "location", location.constData(), nullptr);
    gst_bin_add_many(GST_BIN(pipeline), source, demux, nullptr);
    gst_element_link(source, demux);

    auto *tracks = new int(0);
    g_signal_connect(demux, "pad-added", G_CALLBACK(+[](GstElement *, GstPad *pad, gpointer data) {
        gchar *name = gst_pad_get_name(pad);
        if (name && g_str_has_prefix(name, "audio")) {
            ++*static_cast<int *>(data);
        }
        g_free(name);
        // Nothing consumes these pads; a fakesink per pad would only slow the count down.
        GstElement *sink = gst_element_factory_make("fakesink", nullptr);
        GstElement *parent = GST_ELEMENT(gst_pad_get_parent_element(pad));
        if (sink && parent) {
            gst_bin_add(GST_BIN(GST_ELEMENT_PARENT(parent)), sink);
            gst_element_sync_state_with_parent(sink);
            GstPad *sinkPad = gst_element_get_static_pad(sink, "sink");
            gst_pad_link(pad, sinkPad);
            gst_object_unref(sinkPad);
        }
        gst_clear_object(&parent);
    }), tracks);

    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    GstBus *bus = gst_element_get_bus(pipeline);
    GstMessage *message = gst_bus_timed_pop_filtered(bus, 20 * GST_SECOND,
        static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
    if (message) {
        gst_message_unref(message);
    }
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);

    const int result = *tracks;
    delete tracks;
    return result;
}

//! Plays a file through matroskademux and reports whether it reaches EOS without error.
bool isReadableMatroska(const QString &path)
{
    gst_init(nullptr, nullptr);
    const auto description = QStringLiteral("filesrc location=\"%1\" ! matroskademux ! fakesink")
                                 .arg(path)
                                 .toUtf8();
    GError *parseError = nullptr;
    GstElement *pipeline = gst_parse_launch(description.constData(), &parseError);
    if (!pipeline || parseError) {
        g_clear_error(&parseError);
        gst_clear_object(&pipeline);
        return false;
    }

    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    GstBus *bus = gst_element_get_bus(pipeline);
    GstMessage *message = gst_bus_timed_pop_filtered(bus, 20 * GST_SECOND,
        static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
    const bool ok = message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS;
    if (message) {
        gst_message_unref(message);
    }
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    return ok;
}

} // namespace

class FlashbackTests final : public QObject {
    Q_OBJECT

private slots:
    void parsesSegmentIndices()
    {
        QCOMPARE(FlashbackRing::indexOf(QStringLiteral("segment-000042.mkv")), 42);
        QCOMPARE(FlashbackRing::indexOf(QStringLiteral("segment-000000.mkv")), 0);
        QCOMPARE(FlashbackRing::indexOf(QStringLiteral("capture.mkv")), -1);
        QCOMPARE(FlashbackRing::indexOf(QStringLiteral("segment-abc.mkv")), -1);
    }

    void distrustsTheUnsettledTailWhenScanningBlind()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QDir ring(temporary.path());
        for (int i = 0; i < 5; ++i) {
            writeSegment(ring, i, 2048);
        }

        FlashbackRing flashback;
        flashback.configure(ring.absolutePath(), 2, false);

        // splitmuxsink opens fragment N+1 before it finishes flushing N, so with no fragment-closed
        // messages to go on, the newest two files are off limits.
        const auto segments = flashback.finalizedSegments();
        QCOMPARE(segments.size(), 3);
        QCOMPARE(segments.constFirst().index, 0);
        QCOMPARE(segments.constLast().index, 2);
        QCOMPARE(flashback.availableSeconds(), 6);
    }

    void prefersReportedFragmentsOverTheDirectoryListing()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QDir ring(temporary.path());
        for (int i = 0; i < 5; ++i) {
            writeSegment(ring, i, 2048);
        }

        FlashbackRing flashback;
        flashback.configure(ring.absolutePath(), 2, false);
        for (int i = 0; i < 4; ++i) {
            flashback.noteSegmentClosed(ring.filePath(QStringLiteral("segment-%1.mkv")
                                                          .arg(i, 6, 10, QLatin1Char('0'))),
                1'000'000'000);
        }

        // The sink declared four closed, so the conservative scan no longer applies, and the
        // reported durations are used verbatim rather than the nominal segment length.
        const auto segments = flashback.finalizedSegments();
        QCOMPARE(segments.size(), 4);
        QCOMPARE(segments.constLast().index, 3);
        QCOMPARE(flashback.availableSeconds(), 4);

        flashback.reset();
        QCOMPARE(flashback.finalizedSegments().size(), 3);
    }

    void forgetsFragmentsTheRingHasRecycled()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QDir ring(temporary.path());

        FlashbackRing flashback;
        flashback.configure(ring.absolutePath(), 1, false);
        for (int i = 0; i < 4; ++i) {
            const auto name = QStringLiteral("segment-%1.mkv").arg(i, 6, 10, QLatin1Char('0'));
            writeSegment(ring, i, 2048);
            flashback.noteSegmentClosed(ring.filePath(name), 1'000'000'000);
        }
        QCOMPARE(flashback.finalizedSegments().size(), 4);

        // max-files deleted the oldest fragment out from under us.
        QVERIFY(QFile::remove(ring.filePath(QStringLiteral("segment-000000.mkv"))));

        const auto segments = flashback.finalizedSegments();
        QCOMPARE(segments.size(), 3);
        QCOMPARE(segments.constFirst().index, 1);
    }

    void ordersSegmentsNumericallyNotLexically()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QDir ring(temporary.path());
        for (const int index : {7, 8, 9, 10, 11, 12}) {
            writeSegment(ring, index, 1024);
        }

        FlashbackRing flashback;
        flashback.configure(ring.absolutePath(), 1, false);
        const auto segments = flashback.finalizedSegments();

        QCOMPARE(segments.size(), 4);
        QCOMPARE(segments.constFirst().index, 7);
        QCOMPARE(segments.constLast().index, 10);
    }

    void reportsAnEmptyBufferInsteadOfWritingNothing()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());

        FlashbackRing flashback;
        flashback.configure(temporary.filePath(QStringLiteral("ring")), 2, false);

        QString error;
        QVERIFY(!flashback.save(1, temporary.filePath(QStringLiteral("out.mkv")), nullptr, &error));
        QVERIFY(!error.isEmpty());
    }

    void savesFromARingLeftBehindByAStoppedPipeline()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const auto ringDirectory = temporary.filePath(QStringLiteral("ring"));

        CapturePipeline pipeline;
        PipelineConfig config;
        config.testSource = true;
        config.hardwareEncoder = false;
        config.width = 320;
        config.height = 180;
        config.frameRate = 30;
        config.segmentSeconds = 1;
        config.ringMinutes = 1;
        config.ringDirectory = ringDirectory;

        QString error;
        QVERIFY2(pipeline.start(config, &error), qPrintable(error));
        QTest::qWait(5000);
        pipeline.stop();

        // Nothing is writing any more, so every listed segment is complete.
        FlashbackRing flashback;
        flashback.configure(ringDirectory, config.segmentSeconds, config.hardwareEncoder);
        QVERIFY(flashback.finalizedSegments().size() >= 2);

        const auto output = temporary.filePath(QStringLiteral("stale.mkv"));
        QVERIFY2(flashback.save(1, output, nullptr, &error), qPrintable(error));
        QVERIFY2(isReadableMatroska(output), "saved flashback did not demux cleanly");
    }

    void savesTheBufferedTailAsOnePlayableFile()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const auto ringDirectory = temporary.filePath(QStringLiteral("ring"));

        CapturePipeline pipeline;
        PipelineConfig config;
        config.testSource = true;
        config.hardwareEncoder = false;
        config.width = 320;
        config.height = 180;
        config.frameRate = 30;
        config.segmentSeconds = 1;
        config.ringMinutes = 1;
        config.ringDirectory = ringDirectory;
        // testSource swaps in tone generators, so this exercises the three-track layout.
        config.gameAudioDevice = QStringLiteral("test");
        config.captureMic = true;

        FlashbackRing flashback;
        flashback.configure(ringDirectory, config.segmentSeconds, config.hardwareEncoder);
        connect(&pipeline, &CapturePipeline::segmentClosed,
            &flashback, &FlashbackRing::noteSegmentClosed);

        QString error;
        QVERIFY2(pipeline.start(config, &error), qPrintable(error));
        QTRY_VERIFY_WITH_TIMEOUT(flashback.finalizedSegments().size() >= 3, 15000);

        const auto expectedSegments = flashback.finalizedSegments().size();
        const auto output = temporary.filePath(QStringLiteral("flashback.mkv"));
        int savedSeconds = 0;

        // Saving must not require stopping capture.
        QVERIFY2(flashback.save(1, output, &savedSeconds, &error), qPrintable(error));
        QVERIFY(pipeline.isRunning());
        pipeline.stop();

        QCOMPARE(savedSeconds, static_cast<int>(expectedSegments) * config.segmentSeconds);
        QVERIFY(QFileInfo(output).size() > 1024);
        QVERIFY2(isReadableMatroska(output), "saved flashback did not demux cleanly");
        // A flashback that drops the audio is half a recording.
        QCOMPARE(audioTrackCount(output), 3);

        // The staging hard-links must not survive the save.
        QVERIFY(QDir(ringDirectory).entryList({QStringLiteral(".save-*")}, QDir::Dirs).isEmpty());
    }
};

QTEST_GUILESS_MAIN(FlashbackTests)
#include "FlashbackTests.moc"
