#include "flashback/FlashbackRing.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

#include <gst/gst.h>

#include <memory>
#include <mutex>
#include <unistd.h>

namespace quadcap::flashback {
namespace {

void initializeGStreamer()
{
    static std::once_flag once;
    std::call_once(once, [] { gst_init(nullptr, nullptr); });
}

QString gstErrorMessage(GError *error, const gchar *debug)
{
    QString result = error ? QString::fromUtf8(error->message) : QStringLiteral("Unknown GStreamer error");
    if (debug && *debug) {
        result += QStringLiteral(" (%1)").arg(QString::fromUtf8(debug));
    }
    return result;
}

//! Everything the pad-added handler needs to attach a newly advertised stream.
struct SaveTargets {
    GstElement *pipeline = nullptr;
    GstElement *parser = nullptr;
    GstElement *mux = nullptr;
};

/*!
 * splitmuxsrc advertises its streams only once it has opened the first fragment, so everything is
 * linked as it turns up rather than at construction time.
 *
 * Video goes through the parser; each audio track is queued straight into its own muxer pad, which
 * is what keeps a saved flashback from being a silent film.
 */
void linkSourcePad(GstElement *, GstPad *pad, gpointer userData)
{
    const auto *targets = static_cast<SaveTargets *>(userData);
    gchar *rawName = gst_pad_get_name(pad);
    const QString name = QString::fromUtf8(rawName ? rawName : "");
    g_free(rawName);

    if (name.startsWith(QLatin1String("video"))) {
        GstPad *sink = gst_element_get_static_pad(targets->parser, "sink");
        if (sink) {
            if (!gst_pad_is_linked(sink)) {
                gst_pad_link(pad, sink);
            }
            gst_object_unref(sink);
        }
        return;
    }

    if (!name.startsWith(QLatin1String("audio"))) {
        return;
    }

    const auto queueName = QStringLiteral("save-%1-queue").arg(name).toLatin1();
    GstElement *queue = gst_element_factory_make("queue", queueName.constData());
    if (!queue) {
        return;
    }
    gst_bin_add(GST_BIN(targets->pipeline), queue);
    gst_element_sync_state_with_parent(queue);

    GstPad *queueSink = gst_element_get_static_pad(queue, "sink");
    GstPad *queueSrc = gst_element_get_static_pad(queue, "src");
    GstPad *muxPad = gst_element_request_pad_simple(targets->mux, "audio_%u");
    if (queueSink && queueSrc && muxPad && gst_pad_link(pad, queueSink) == GST_PAD_LINK_OK) {
        gst_pad_link(queueSrc, muxPad);
    }
    gst_clear_object(&queueSink);
    gst_clear_object(&queueSrc);
    gst_clear_object(&muxPad);
}

} // namespace

FlashbackRing::FlashbackRing(QObject *parent)
    : QObject(parent)
{
    initializeGStreamer();
}

void FlashbackRing::configure(const QString &ringDirectory, const int segmentSeconds,
    const bool hardwareEncoder)
{
    ringDirectory_ = ringDirectory;
    segmentSeconds_ = qMax(1, segmentSeconds);
    hardwareEncoder_ = hardwareEncoder;
}

QString FlashbackRing::ringDirectory() const
{
    return ringDirectory_;
}

int FlashbackRing::segmentSeconds() const
{
    return segmentSeconds_;
}

int FlashbackRing::indexOf(const QString &fileName)
{
    static const QRegularExpression pattern(QStringLiteral("^segment-(\\d+)\\.mkv$"));
    const auto match = pattern.match(fileName);
    if (!match.hasMatch()) {
        return -1;
    }
    bool ok = false;
    const auto value = match.captured(1).toInt(&ok);
    return ok ? value : -1;
}

void FlashbackRing::noteSegmentClosed(const QString &path, const qint64 durationNs)
{
    const QFileInfo info(path);
    const auto index = indexOf(info.fileName());
    if (index < 0) {
        return;
    }
    const auto known = std::find_if(closed_.begin(), closed_.end(),
        [index](const Segment &segment) { return segment.index == index; });
    if (known != closed_.end()) {
        known->durationNs = durationNs;
        return;
    }
    closed_.append(Segment {info.absoluteFilePath(), index, info.size(), durationNs});
    std::sort(closed_.begin(), closed_.end(),
        [](const Segment &lhs, const Segment &rhs) { return lhs.index < rhs.index; });
}

void FlashbackRing::reset()
{
    closed_.clear();
}

QVector<Segment> FlashbackRing::scanDirectory() const
{
    const QDir ring(ringDirectory_);
    QVector<Segment> segments;
    const auto entries = ring.entryInfoList({QString::fromLatin1(SegmentPattern)}, QDir::Files);
    segments.reserve(entries.size());
    for (const auto &entry : entries) {
        const auto index = indexOf(entry.fileName());
        if (index < 0) {
            continue;
        }
        segments.append(Segment {entry.absoluteFilePath(), index, entry.size(), 0});
    }
    std::sort(segments.begin(), segments.end(),
        [](const Segment &lhs, const Segment &rhs) { return lhs.index < rhs.index; });
    return segments;
}

QVector<Segment> FlashbackRing::finalizedSegments() const
{
    if (ringDirectory_.isEmpty()) {
        return {};
    }

    if (!closed_.isEmpty()) {
        // splitmuxsink told us these are closed; keep the ones its max-files has not recycled.
        QVector<Segment> live;
        live.reserve(closed_.size());
        for (const auto &segment : closed_) {
            const QFileInfo info(segment.path);
            if (info.exists() && info.size() > 0) {
                live.append(Segment {segment.path, segment.index, info.size(), segment.durationNs});
            }
        }
        return live;
    }

    auto segments = scanDirectory();
    /*
     * No fragment-closed messages yet, so trust the listing only up to the unsettled tail:
     * splitmuxsink opens fragment N+1 while fragment N is still being flushed, which leaves two
     * files at the end that a reader must not touch.
     */
    segments.resize(qMax<qsizetype>(0, segments.size() - UnsettledTail));
    while (!segments.isEmpty() && segments.constLast().sizeBytes == 0) {
        segments.removeLast();
    }
    return segments;
}

int FlashbackRing::availableSeconds() const
{
    const auto segments = finalizedSegments();
    qint64 totalNs = 0;
    for (const auto &segment : segments) {
        if (segment.durationNs <= 0) {
            return static_cast<int>(segments.size()) * segmentSeconds_;
        }
        totalNs += segment.durationNs;
    }
    return static_cast<int>(totalNs / 1'000'000'000);
}

bool FlashbackRing::save(const int minutes, const QString &outputPath, int *savedSeconds,
    QString *error)
{
    if (ringDirectory_.isEmpty()) {
        if (error) {
            *error = QStringLiteral("No ring directory is configured");
        }
        return false;
    }
    if (minutes <= 0) {
        if (error) {
            *error = QStringLiteral("Flashback duration must be at least one minute");
        }
        return false;
    }

    const auto available = finalizedSegments();
    if (available.isEmpty()) {
        if (error) {
            *error = QStringLiteral("The flashback buffer is still filling; nothing to save yet");
        }
        return false;
    }

    // Walk back from the newest segment until the requested window is covered.
    const qint64 windowNs = static_cast<qint64>(minutes) * 60 * 1'000'000'000;
    const qint64 fallbackNs = static_cast<qint64>(segmentSeconds_) * 1'000'000'000;
    qint64 coveredNs = 0;
    qsizetype first = available.size();
    while (first > 0 && coveredNs < windowNs) {
        --first;
        const auto &segment = available.at(first);
        coveredNs += segment.durationNs > 0 ? segment.durationNs : fallbackNs;
    }
    const auto selected = available.mid(first);

    const QFileInfo output(outputPath);
    if (!QDir().mkpath(output.absolutePath())) {
        if (error) {
            *error = QStringLiteral("Could not create output directory %1").arg(output.absolutePath());
        }
        return false;
    }

    /*
     * The ring keeps rolling while we read it, and splitmuxsink deletes its oldest file once
     * max-files is reached. Hard-linking the selection into a staging directory pins those inodes
     * for the duration of the remux: it costs no space and no copy, it cannot race, and it leaves
     * the staging glob containing exactly the fragments we mean to stitch.
     */
    const auto staging = QDir(ringDirectory_)
                             .filePath(QStringLiteral(".save-%1")
                                           .arg(QDateTime::currentMSecsSinceEpoch()));
    if (!QDir().mkpath(staging)) {
        if (error) {
            *error = QStringLiteral("Could not create staging directory %1").arg(staging);
        }
        return false;
    }

    QVector<Segment> pinned;
    pinned.reserve(selected.size());
    for (const auto &segment : selected) {
        const auto linkPath = QDir(staging).filePath(QFileInfo(segment.path).fileName());
        const auto source = QFile::encodeName(segment.path);
        const auto target = QFile::encodeName(linkPath);
        if (::link(source.constData(), target.constData()) != 0) {
            // The ring recycled this file between listing and pinning; the rest is still usable.
            continue;
        }
        pinned.append(Segment {linkPath, segment.index, segment.sizeBytes, segment.durationNs});
    }

    const auto cleanup = [&staging] { QDir(staging).removeRecursively(); };

    if (pinned.isEmpty()) {
        cleanup();
        if (error) {
            *error = QStringLiteral("The ring recycled every selected segment before it could be read");
        }
        return false;
    }

    if (!concatenate(staging, output.absoluteFilePath(), error)) {
        cleanup();
        return false;
    }
    cleanup();

    qint64 savedNs = 0;
    for (const auto &segment : pinned) {
        savedNs += segment.durationNs > 0 ? segment.durationNs : fallbackNs;
    }
    const auto seconds = static_cast<int>(savedNs / 1'000'000'000);
    if (savedSeconds) {
        *savedSeconds = seconds;
    }
    emit saved(output.absoluteFilePath(), seconds);
    return true;
}

bool FlashbackRing::concatenate(const QString &stagingDirectory, const QString &outputPath,
    QString *error) const
{
    GstElement *pipeline = gst_pipeline_new("flashback-save");
    GstElement *source = gst_element_factory_make("splitmuxsrc", "source");
    GstElement *parser = gst_element_factory_make(hardwareEncoder_ ? "h265parse" : "h264parse", "parser");
    GstElement *mux = gst_element_factory_make("matroskamux", "mux");
    GstElement *sink = gst_element_factory_make("filesink", "sink");

    if (!pipeline || !source || !parser || !mux || !sink) {
        if (error) {
            *error = QStringLiteral("Required GStreamer elements for flashback remuxing are unavailable");
        }
        gst_clear_object(&pipeline);
        gst_clear_object(&source);
        gst_clear_object(&parser);
        gst_clear_object(&mux);
        gst_clear_object(&sink);
        return false;
    }

    // The staging directory holds exactly the pinned fragments, so the glob needs no further
    // filtering and splitmuxsrc restores their order and timestamps for us.
    const auto glob = QFile::encodeName(QDir(stagingDirectory).filePath(QString::fromLatin1(SegmentPattern)));
    g_object_set(source, "location", glob.constData(), nullptr);
    const auto pathBytes = QFile::encodeName(outputPath);
    g_object_set(sink, "location", pathBytes.constData(), nullptr);
    g_object_set(mux, "writing-app", "quadcap", nullptr);

    gst_bin_add_many(GST_BIN(pipeline), source, parser, mux, sink, nullptr);
    if (!gst_element_link_many(parser, mux, sink, nullptr)) {
        if (error) {
            *error = QStringLiteral("Could not link the flashback remuxing chain");
        }
        gst_object_unref(pipeline);
        return false;
    }
    // Outlives the pipeline below, and is freed once the remux has finished.
    auto targets = std::make_unique<SaveTargets>(SaveTargets {pipeline, parser, mux});
    g_signal_connect(source, "pad-added", G_CALLBACK(linkSourcePad), targets.get());

    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        if (error) {
            *error = QStringLiteral("GStreamer refused to start the flashback remux");
        }
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        return false;
    }

    GstBus *bus = gst_element_get_bus(pipeline);
    bool ok = false;
    QString failure;
    GstMessage *message = gst_bus_timed_pop_filtered(bus, 60 * GST_SECOND,
        static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
    if (!message) {
        failure = QStringLiteral("Timed out while remuxing the flashback buffer");
    } else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) {
        ok = true;
    } else {
        GError *gerror = nullptr;
        gchar *debug = nullptr;
        gst_message_parse_error(message, &gerror, &debug);
        failure = gstErrorMessage(gerror, debug);
        g_clear_error(&gerror);
        g_free(debug);
    }
    if (message) {
        gst_message_unref(message);
    }
    gst_object_unref(bus);

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);

    if (!ok && error) {
        *error = failure;
    }
    return ok;
}

} // namespace quadcap::flashback
