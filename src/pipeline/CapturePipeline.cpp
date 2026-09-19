#include "pipeline/CapturePipeline.h"

#include <QDir>
#include <QFileInfo>
#include <QMetaObject>

#include <gst/app/gstappsink.h>
#include <gst/video/video-event.h>

#include <cmath>
#include <limits>
#include <mutex>

namespace quadcap::pipeline {
namespace {

void initializeGStreamer() {
    static std::once_flag once;
    std::call_once(once, [] { gst_init(nullptr, nullptr); });
}

QString gstErrorMessage(GError *error, const gchar *debug) {
    QString result =
        error ? QString::fromUtf8(error->message) : QStringLiteral("Unknown GStreamer error");
    if (debug && *debug) {
        result += QStringLiteral(" (%1)").arg(QString::fromUtf8(debug));
    }
    return result;
}

bool linkAll(std::initializer_list<GstElement *> elements) {
    if (elements.size() < 2) {
        return true;
    }
    auto current = elements.begin();
    auto next = current;
    ++next;
    while (next != elements.end()) {
        if (!gst_element_link(*current, *next)) {
            return false;
        }
        ++current;
        ++next;
    }
    return true;
}

double loudestChannel(const GstStructure *structure, const char *field) {
    const GValue *channels = gst_structure_get_value(structure, field);
    if (!channels) {
        return -std::numeric_limits<double>::infinity();
    }

    guint count = 0;
    const GValue *(*at)(const GValue *, guint) = nullptr;
    if (GST_VALUE_HOLDS_ARRAY(channels)) {
        count = gst_value_array_get_size(channels);
        at = gst_value_array_get_value;
    } else if (GST_VALUE_HOLDS_LIST(channels)) {
        count = gst_value_list_get_size(channels);
        at = gst_value_list_get_value;
    } else {
        return -std::numeric_limits<double>::infinity();
    }

    double loudest = -std::numeric_limits<double>::infinity();
    for (guint i = 0; i < count; ++i) {
        const GValue *entry = at(channels, i);
        if (entry && G_VALUE_HOLDS_DOUBLE(entry)) {
            loudest = qMax(loudest, g_value_get_double(entry));
        }
    }
    return loudest;
}

} // namespace

CapturePipeline::CapturePipeline(QObject *parent) : QObject(parent) {
    initializeGStreamer();
    busTimer_.setInterval(50);
    connect(&busTimer_, &QTimer::timeout, this, &CapturePipeline::pollBus);
}

CapturePipeline::~CapturePipeline() {
    stop();
}

bool CapturePipeline::start(const PipelineConfig &config, QString *error) {
    if (pipeline_) {
        if (error) {
            *error = QStringLiteral("Pipeline is already running");
        }
        return false;
    }
    config_ = config;
    if (!build(error)) {
        destroyPipeline();
        return false;
    }

    if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE) {
        busTimer_.start();
        return true;
    }

    /*
     * Starting failed. Audio sources are probed before being wired in, so most device problems are
     * already handled — but a device can be taken between that probe and this state change, and an
     * audio element can fail for reasons a probe does not reach. Rather than hand back nothing,
     * rebuild without audio and try once more: a recording with no sound is recoverable, a missing
     * recording is not.
     */
    destroyPipeline();
    if (!config_.enableAudio) {
        if (error) {
            *error = QStringLiteral("GStreamer refused to start the pipeline");
        }
        return false;
    }

    config_.enableAudio = false;
    if (!build(error)) {
        destroyPipeline();
        return false;
    }
    if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        if (error) {
            *error =
                QStringLiteral("GStreamer refused to start the pipeline, with and without audio");
        }
        destroyPipeline();
        return false;
    }

    audioNotice_ =
        QStringLiteral("Recording without audio: the sound devices could not be started");
    emit warningOccurred(audioNotice_);
    busTimer_.start();
    return true;
}

void CapturePipeline::stop() {
    busTimer_.stop();
    if (!pipeline_) {
        return;
    }
    // Only a recording needs the graph drained before it goes away; the ring's own tail is
    // already treated as unsettled by the reader. Waiting seconds for an EOS that nothing is
    // waiting on would just be a frozen window.
    const bool wasRecording = recordQueue_ != nullptr;
    if (recordQueue_) {
        if (recordTeePad_) {
            GstPad *queueSink = gst_element_get_static_pad(recordQueue_, "sink");
            if (queueSink) {
                gst_pad_unlink(recordTeePad_, queueSink);
                gst_pad_send_event(queueSink, gst_event_new_eos());
                gst_object_unref(queueSink);
            }
        }
        destroyRecordingBranch();
    }
    gst_element_send_event(pipeline_, gst_event_new_eos());
    GstBus *bus = gst_element_get_bus(pipeline_);
    GstMessage *terminal = gst_bus_timed_pop_filtered(
        bus, wasRecording ? 5 * GST_SECOND : 500 * GST_MSECOND,
        static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    gst_clear_message(&terminal);
    gst_object_unref(bus);
    destroyPipeline();
}

/*!
 * Attaches a recording branch to the running graph without interrupting it.
 *
 * Recording reuses the encoder that is already feeding the flashback ring rather than starting a
 * second one, because encoding 4K60 twice is not something the card and the GPU have headroom for.
 * The branch is therefore built live and hung off the encoded tee: a queue, a muxer, and a sink,
 * plus one queue per audio track bound to a mux audio pad, all of it added to a pipeline that is
 * already PLAYING and then synced to its state.
 *
 * Every failure path calls destroyRecordingBranch before returning, since a half-linked branch left
 * on a live pipeline would stall the ring the preview depends on.
 *
 * The last step asks the encoder for a keyframe. Without it the file opens on a P-frame referring
 * to a picture that was never written, and the first second is unplayable.
 */
bool CapturePipeline::startRecording(const QString &path, QString *error) {
    if (!pipeline_ || !encodedTee_) {
        if (error) {
            *error = QStringLiteral("Pipeline is not running");
        }
        return false;
    }
    if (recordQueue_) {
        if (error) {
            *error = QStringLiteral("A recording is already active");
        }
        return false;
    }

    const QFileInfo output(path);
    if (!QDir().mkpath(output.absolutePath())) {
        if (error) {
            *error =
                QStringLiteral("Could not create output directory %1").arg(output.absolutePath());
        }
        return false;
    }

    recordQueue_ = make("queue", "record-queue", error);
    recordMux_ = make("matroskamux", "record-mux", error);
    recordSink_ = make("filesink", "record-sink", error);
    if (!recordQueue_ || !recordMux_ || !recordSink_) {
        destroyRecordingBranch();
        return false;
    }
    const auto pathBytes = QFile::encodeName(output.absoluteFilePath());
    g_object_set(recordSink_, "location", pathBytes.constData(), "sync", FALSE, nullptr);
    g_object_set(recordMux_, "writing-app", "quadcap", nullptr);

    gst_bin_add_many(GST_BIN(pipeline_), recordQueue_, recordMux_, recordSink_, nullptr);
    if (!linkAll({recordQueue_, recordMux_, recordSink_})) {
        if (error) {
            *error = QStringLiteral("Could not link the recording branch");
        }
        destroyRecordingBranch();
        return false;
    }

    recordTeePad_ = gst_element_request_pad_simple(encodedTee_, "src_%u");
    // Named for the branch it belongs to: the audio loop below binds a pad of its own per track,
    // and two things called queueSink in one function is a good way to unref the wrong one.
    GstPad *videoQueueSink = gst_element_get_static_pad(recordQueue_, "sink");
    const bool linked = recordTeePad_ && videoQueueSink &&
                        gst_pad_link(recordTeePad_, videoQueueSink) == GST_PAD_LINK_OK;
    if (videoQueueSink) {
        gst_object_unref(videoQueueSink);
    }
    if (!linked) {
        if (error) {
            *error = QStringLiteral("Could not attach the recording branch to the encoded stream");
        }
        destroyRecordingBranch();
        return false;
    }

    // Attach every audio track the pipeline is carrying as its own track in this file.
    for (qsizetype i = 0; i < audioTracks_.size(); ++i) {
        const auto queueName = QStringLiteral("record-audio-%1").arg(i).toLatin1();
        GstElement *queue = gst_element_factory_make("queue", queueName.constData());
        if (!queue) {
            destroyRecordingBranch();
            if (error) {
                *error = QStringLiteral("Could not create the audio recording branch");
            }
            return false;
        }
        gst_bin_add(GST_BIN(pipeline_), queue);
        recordAudioQueues_.append(queue);

        GstPad *muxPad = gst_element_request_pad_simple(recordMux_, "audio_%u");
        GstPad *queueSink = gst_element_get_static_pad(queue, "sink");
        GstPad *queueSrc = gst_element_get_static_pad(queue, "src");
        GstPad *teePad = gst_element_request_pad_simple(audioTracks_.at(i).tee, "src_%u");
        const bool audioLinked = muxPad && queueSink && queueSrc && teePad &&
                                 gst_pad_link(teePad, queueSink) == GST_PAD_LINK_OK &&
                                 gst_pad_link(queueSrc, muxPad) == GST_PAD_LINK_OK;
        gst_clear_object(&queueSink);
        gst_clear_object(&queueSrc);
        gst_clear_object(&muxPad);
        if (teePad) {
            recordAudioTeePads_.append(teePad);
        }
        if (!audioLinked) {
            destroyRecordingBranch();
            if (error) {
                *error =
                    QStringLiteral("Could not attach audio track %1").arg(audioTracks_.at(i).name);
            }
            return false;
        }
        gst_element_sync_state_with_parent(queue);
    }

    GstPad *sinkPad = gst_element_get_static_pad(recordSink_, "sink");
    gst_pad_add_probe(sinkPad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
                      &CapturePipeline::observeRecordingEos, this, nullptr);
    gst_object_unref(sinkPad);

    gst_element_sync_state_with_parent(recordQueue_);
    gst_element_sync_state_with_parent(recordMux_);
    gst_element_sync_state_with_parent(recordSink_);

    gst_element_send_event(
        encoder_, gst_video_event_new_upstream_force_key_unit(GST_CLOCK_TIME_NONE, TRUE, 0));

    recordingPath_ = output.absoluteFilePath();
    recordingStopping_ = false;
    emit recordingChanged(true, recordingPath_);
    return true;
}

void CapturePipeline::stopRecording() {
    if (!recordTeePad_ || recordingStopping_) {
        return;
    }
    recordingStopping_ = true;
    gst_pad_add_probe(recordTeePad_, GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM,
                      &CapturePipeline::blockRecordingPad, this, nullptr);
}

void CapturePipeline::setPreviewItem(QObject *item) {
    previewItem_ = item;
    if (previewSink_ && g_object_class_find_property(G_OBJECT_GET_CLASS(previewSink_), "widget")) {
        g_object_set(previewSink_, "widget", item, nullptr);
    }
}

bool CapturePipeline::isRunning() const {
    return pipeline_ != nullptr;
}

bool CapturePipeline::isRecording() const {
    return recordQueue_ != nullptr;
}

QString CapturePipeline::currentRecording() const {
    return recordingPath_;
}

void CapturePipeline::pollBus() {
    if (!pipeline_ || tearingDown_) {
        return; // Already failed; the graph goes away on the next turn of the event loop.
    }
    GstBus *bus = gst_element_get_bus(pipeline_);
    while (GstMessage *message = gst_bus_pop(bus)) {
        switch (GST_MESSAGE_TYPE(message)) {
        case GST_MESSAGE_ERROR: {
            GError *error = nullptr;
            gchar *debug = nullptr;
            gst_message_parse_error(message, &error, &debug);
            const auto text = gstErrorMessage(error, debug);
            g_clear_error(&error);
            g_free(debug);
            gst_message_unref(message);
            gst_object_unref(bus);

            /*
             * A pipeline that has posted an error is finished, and leaving it allocated does real
             * damage: it holds the capture device open, so the next attempt to start finds the
             * card busy and blames another application, and after a few failures the process holds
             * several descriptors on a node it can no longer use. isRunning() also keeps answering
             * true for a graph that will never produce another frame.
             *
             * The teardown is deferred to the next turn of the event loop rather than done here.
             * Changing a graph's state from inside its own bus handler deadlocks: pipewiresrc
             * takes the PipeWire thread-loop lock on the way to NULL, and that loop is waiting on
             * the main loop this handler is blocking.
             */
            tearingDown_ = true;
            QMetaObject::invokeMethod(
                this,
                [this] {
                    tearingDown_ = false;
                    destroyPipeline();
                },
                Qt::QueuedConnection);
            emit errorOccurred(text);
            return;
        }
        case GST_MESSAGE_WARNING: {
            GError *error = nullptr;
            gchar *debug = nullptr;
            gst_message_parse_warning(message, &error, &debug);
            emit warningOccurred(gstErrorMessage(error, debug));
            g_clear_error(&error);
            g_free(debug);
            break;
        }
        case GST_MESSAGE_ELEMENT: {
            const GstStructure *structure = gst_message_get_structure(message);
            if (structure && gst_structure_has_name(structure, "level")) {
                gchar *elementName = gst_object_get_name(GST_MESSAGE_SRC(message));
                const QString name = QString::fromUtf8(elementName ? elementName : "");
                g_free(elementName);
                const auto source = name.section(QLatin1Char('-'), 0, 0);
                if (!source.isEmpty()) {
                    emit audioLevel(source, loudestChannel(structure, "rms"),
                                    loudestChannel(structure, "peak"));
                }
                break;
            }
            if (structure && gst_structure_has_name(structure, "splitmuxsink-fragment-closed")) {
                const gchar *location = gst_structure_get_string(structure, "location");
                guint64 duration = 0;
                gst_structure_get_uint64(structure, "fragment-duration", &duration);
                if (location) {
                    emit segmentClosed(QString::fromUtf8(location), static_cast<qint64>(duration));
                }
            }
            break;
        }
        default:
            break;
        }
        gst_message_unref(message);
    }
    gst_object_unref(bus);
}

void CapturePipeline::finalizeRecording() {
    if (!recordQueue_) {
        return;
    }
    const auto completedPath = recordingPath_;
    destroyRecordingBranch();
    recordingPath_.clear();
    recordingStopping_ = false;
    emit recordingChanged(false, completedPath);
}

bool CapturePipeline::build(QString *error) {
    if (config_.width <= 0 || config_.height <= 0 || config_.frameRate <= 0 ||
        config_.segmentSeconds <= 0 || config_.ringMinutes <= 0) {
        if (error) {
            *error = QStringLiteral(
                "Pipeline dimensions, rate, segment length, and ring duration must be positive");
        }
        return false;
    }
    if (!config_.testSource && config_.devicePath.isEmpty()) {
        if (error) {
            *error = QStringLiteral("A V4L2 device path is required");
        }
        return false;
    }

    pipeline_ = gst_pipeline_new("quadcap-pipeline");

    /*
     * Colour conversion and scaling run on the GPU whenever NVENC is in play.
     *
     * Measured here at 4096x2160: `videoconvert ! videoscale ! videorate` on the CPU sustains
     * roughly 0.5x realtime and starves the encoder outright — the ring fills with zero-byte
     * fragments. Uploading once and letting GL unpack YUY2, then CUDA scale into NV12, is about
     * 2.5x faster and clears 4K60. cudaconvertscale cannot accept packed YUY2, which is why the
     * unpack happens in GL before the CUDA hand-off rather than in one step.
     */
    const bool gpuProcessing = config_.hardwareEncoder;

    auto *source = make(config_.testSource ? "videotestsrc" : "v4l2src", "video-source", error);
    auto *sourceCaps = make("capsfilter", "source-caps", error);
    auto *rawTee = make("tee", "raw-tee", error);
    auto *previewQueue = make("queue", "preview-queue", error);
    auto *encodeQueue = make("queue", "encode-queue", error);
    encoder_ = make(config_.hardwareEncoder ? "nvh265enc" : "x264enc", "video-encoder", error);
    auto *parser = make(config_.hardwareEncoder ? "h265parse" : "h264parse", "video-parser", error);
    encodedTee_ = make("tee", "encoded-tee", error);
    auto *ringQueue = make("queue", "ring-queue", error);
    auto *ringSink = make("splitmuxsink", "segment-ring", error);

    if (!source || !sourceCaps || !rawTee || !previewQueue || !encodeQueue || !encoder_ ||
        !parser || !encodedTee_ || !ringQueue || !ringSink) {
        return false;
    }
    gst_bin_add_many(GST_BIN(pipeline_), source, sourceCaps, rawTee, previewQueue, encodeQueue,
                     encoder_, parser, encodedTee_, ringQueue, ringSink, nullptr);

    if (config_.testSource) {
        g_object_set(source, "is-live", TRUE, "pattern", 18, nullptr);
    } else {
        /*
         * io-mode 2 is mmap, deliberately not 4 (dmabuf).
         *
         * v4l2src prefers DMABuf with this driver and negotiates
         * video/x-raw(memory:DMABuf), drm-format=YUYV. glupload imports those buffers as textures
         * and every frame comes out black — the capture is fine, the GL import is not. Recordings
         * still look structurally perfect (right geometry, 60 fps, no drops), so this fails
         * silently and is only visible by actually looking at a frame.
         */
        const auto path = QFile::encodeName(config_.devicePath);
        g_object_set(source, "device", path.constData(), "io-mode", 2, nullptr);
    }

    // Plain video/x-raw carries no memory feature, which keeps DMABuf out of the graph even if
    // v4l2src would rather negotiate it.
    GstCaps *systemMemory = gst_caps_from_string("video/x-raw");
    g_object_set(sourceCaps, "caps", systemMemory, nullptr);
    gst_caps_unref(systemMemory);

    const auto keyInterval = static_cast<guint>(config_.frameRate * config_.segmentSeconds);

    /*
     * Scale the bitrate with the frame area rather than pinning 60 Mbps to every resolution.
     * 60 Mbps is the 4K60 figure; spending it on a 1080p source just stores noise.
     */
    const auto referencePixels = 3840.0 * 2160.0;
    const auto pixels = static_cast<double>(config_.width) * config_.height;
    const auto rateScale = qBound(0.12, pixels / referencePixels, 1.0);
    const auto targetKbps = static_cast<guint>(qRound(60000.0 * rateScale));
    const auto ceilingKbps = static_cast<guint>(qRound(90000.0 * rateScale));

    if (config_.hardwareEncoder) {
        g_object_set(encoder_, "rc-mode", 2, "const-quality", 22.0, "bitrate", targetKbps,
                     "max-bitrate", ceilingKbps, "gop-size", static_cast<gint>(keyInterval), "aud",
                     TRUE, nullptr);
    } else {
        g_object_set(encoder_, "tune", 0x00000004, "speed-preset", 1, "bitrate", 4000u,
                     "key-int-max", keyInterval, nullptr);
    }

    // Preview must never be able to stall the encoder, so it drops rather than blocks.
    g_object_set(previewQueue, "leaky", 2, "max-size-buffers", 2u, "max-size-bytes", 0u,
                 "max-size-time", static_cast<guint64>(0), nullptr);

    if (config_.enablePreview) {
        previewSink_ = make("qml6glsink", "preview-sink", error);
    } else {
        previewSink_ = make("fakesink", "preview-sink", error);
    }
    if (!previewSink_) {
        return false;
    }
    gst_bin_add(GST_BIN(pipeline_), previewSink_);
    /*
     * Never let the sink drop or pace on timestamps.
     *
     * sc0710 advertises a bogus 1/1 framerate (it delivers ~60), so every buffer looks
     * catastrophically late to GstBaseSink's QoS and it throws nearly all of them away — the
     * preview then shows one stale frame instead of live video. Buffer PTS are real, so recording
     * is unaffected; this only concerns what reaches the screen. A monitoring preview should show
     * the newest frame and never stall the encoder, which is what the leaky queue ahead of it and
     * these three settings together buy.
     */
    g_object_set(previewSink_, "sync", FALSE, "qos", FALSE, "max-lateness", G_GINT64_CONSTANT(-1),
                 nullptr);
    if (config_.enablePreview) {
        g_object_set(previewSink_, "widget", previewItem_, nullptr);
    }

    // Configure the ring before anything is linked to it: splitmuxsink hands out pads that
    // proxy its muxer, so requesting one before the muxer is set binds it to the default
    // mp4mux and the audio pad then refuses raw PCM with GST_PAD_LINK_NOFORMAT.
    const auto ring = QFileInfo(config_.ringDirectory).absoluteFilePath();
    if (!QDir().mkpath(ring)) {
        if (error) {
            *error = QStringLiteral("Could not create ring directory %1").arg(ring);
        }
        return false;
    }
    const auto location =
        QFile::encodeName(QDir(ring).filePath(QStringLiteral("segment-%06d.mkv")));
    const auto maxFiles =
        static_cast<guint>(qMax(1, config_.ringMinutes * 60 / config_.segmentSeconds));
    GstElement *ringMux = gst_element_factory_make("matroskamux", "ring-mux");
    if (!ringMux) {
        if (error) {
            *error = QStringLiteral("Required GStreamer element 'matroskamux' is unavailable");
        }
        return false;
    }
    g_object_set(ringSink, "location", location.constData(), "muxer", ringMux, "max-size-time",
                 static_cast<guint64>(config_.segmentSeconds) * GST_SECOND, "max-files", maxFiles,
                 "async-finalize", FALSE, nullptr);

    if (gpuProcessing) {
        auto *glUpload = make("glupload", "source-upload", error);
        auto *glConvert = make("glcolorconvert", "source-convert", error);
        auto *glCaps = make("capsfilter", "gl-caps", error);
        auto *cudaUpload = make("cudaupload", "encode-cuda-upload", error);
        auto *cudaScale = make("cudaconvertscale", "encode-cuda-scale", error);
        auto *cudaCaps = make("capsfilter", "canonical-caps", error);
        auto *rate = make("videorate", "encode-rate", error);
        auto *rateCaps = make("capsfilter", "canonical-rate", error);
        if (!glUpload || !glConvert || !glCaps || !cudaUpload || !cudaScale || !cudaCaps || !rate ||
            !rateCaps) {
            return false;
        }
        gst_bin_add_many(GST_BIN(pipeline_), glUpload, glConvert, glCaps, cudaUpload, cudaScale,
                         cudaCaps, rate, rateCaps, nullptr);

        GstCaps *rgba = gst_caps_from_string("video/x-raw(memory:GLMemory),format=RGBA");
        g_object_set(glCaps, "caps", rgba, nullptr);
        gst_caps_unref(rgba);

        const auto scaled =
            QStringLiteral("video/x-raw(memory:CUDAMemory),format=NV12,width=%1,height=%2")
                .arg(config_.width)
                .arg(config_.height)
                .toLatin1();
        GstCaps *nv12 = gst_caps_from_string(scaled.constData());
        g_object_set(cudaCaps, "caps", nv12, nullptr);
        gst_caps_unref(nv12);

        const auto paced = QStringLiteral("video/x-raw(memory:CUDAMemory),framerate=%1/1")
                               .arg(config_.frameRate)
                               .toLatin1();
        GstCaps *rateFilter = gst_caps_from_string(paced.constData());
        g_object_set(rateCaps, "caps", rateFilter, nullptr);
        gst_caps_unref(rateFilter);

        if (!linkAll({source, sourceCaps, glUpload, glConvert, glCaps, rawTee}) ||
            !linkAll({rawTee, previewQueue, previewSink_}) ||
            !linkAll({rawTee, encodeQueue, cudaUpload, cudaScale, cudaCaps, rate, rateCaps,
                      encoder_, parser, encodedTee_}) ||
            !linkAll({encodedTee_, ringQueue, ringSink})) {
            if (error) {
                *error = QStringLiteral("Could not link the GPU capture graph");
            }
            return false;
        }
    } else {
        auto *convert = make("videoconvert", "source-convert", error);
        auto *scale = make("videoscale", "source-scale", error);
        auto *rate = make("videorate", "source-rate", error);
        auto *capsFilter = make("capsfilter", "canonical-caps", error);
        auto *encodeConvert = make("videoconvert", "encode-convert", error);
        auto *encodeCaps = make("capsfilter", "encode-caps", error);
        if (!convert || !scale || !rate || !capsFilter || !encodeConvert || !encodeCaps) {
            return false;
        }
        gst_bin_add_many(GST_BIN(pipeline_), convert, scale, rate, capsFilter, encodeConvert,
                         encodeCaps, nullptr);

        GstCaps *canonicalCaps = gst_caps_new_simple(
            "video/x-raw", "width", G_TYPE_INT, config_.width, "height", G_TYPE_INT, config_.height,
            "framerate", GST_TYPE_FRACTION, config_.frameRate, 1, nullptr);
        g_object_set(capsFilter, "caps", canonicalCaps, nullptr);
        gst_caps_unref(canonicalCaps);

        GstCaps *encoderCaps =
            gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "NV12", nullptr);
        g_object_set(encodeCaps, "caps", encoderCaps, nullptr);
        gst_caps_unref(encoderCaps);

        bool previewLinked = false;
        if (config_.enablePreview) {
            auto *glUpload = make("glupload", "preview-upload", error);
            auto *glConvert = make("glcolorconvert", "preview-convert", error);
            if (!glUpload || !glConvert) {
                return false;
            }
            gst_bin_add_many(GST_BIN(pipeline_), glUpload, glConvert, nullptr);
            previewLinked = linkAll({previewQueue, glUpload, glConvert, previewSink_});
        } else {
            previewLinked = linkAll({previewQueue, previewSink_});
        }

        if (!previewLinked ||
            !linkAll({source, sourceCaps, convert, scale, rate, capsFilter, rawTee}) ||
            !linkAll({rawTee, previewQueue}) ||
            !linkAll(
                {rawTee, encodeQueue, encodeConvert, encodeCaps, encoder_, parser, encodedTee_}) ||
            !linkAll({encodedTee_, ringQueue, ringSink})) {
            if (error) {
                *error = QStringLiteral("Could not link the capture graph");
            }
            return false;
        }
    }

    if (!buildAudio(ringSink, error)) {
        if (error && error->isEmpty()) {
            *error = QStringLiteral("Could not link the audio graph");
        }
        return false;
    }

    // Built last, off the same raw tee the preview and the encoder use, and never fatal: OBS not
    // being reachable is not a reason to stop capturing.
    if (config_.enableObsOutput) {
        buildObsVideo(rawTee);
    }
    return true;
}

bool CapturePipeline::canOpenSource(GstElement *element) {
    if (!element) {
        return false;
    }
    // READY is where a capture element actually claims its device, so it is the cheapest point at
    // which "busy" or "missing" becomes visible, and the element is returned to NULL either way so
    // the caller is free to wire it in or throw it away.
    //
    // Tried twice because a sound device released moments ago by the graph being replaced can
    // still refuse the next open, which left a rebuilt pipeline silent until something else
    // prompted another attempt.
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (gst_element_set_state(element, GST_STATE_READY) != GST_STATE_CHANGE_FAILURE) {
            gst_element_set_state(element, GST_STATE_NULL);
            return true;
        }
        gst_element_set_state(element, GST_STATE_NULL);
        g_usleep(150 * 1000);
    }
    return false;
}

QString CapturePipeline::audioNotice() const {
    return audioNotice_;
}

QString CapturePipeline::obsNotice() const {
    return obsNotice_;
}

bool CapturePipeline::obsActive() const {
    return obsVideoLive_ || obsAudioLive_ > 0;
}

void CapturePipeline::noteObsProblem(const QString &note) {
    obsNotice_ = obsNotice_.isEmpty() ? note : obsNotice_ + QStringLiteral("; ") + note;
}

/*
 * The OBS branches all start with a leaky queue. OBS is a separate process that can be paused,
 * dragged around, or simply slow, and without a leak that backpressure would travel up the tee and
 * stall the recording and the preview with it. Dropping frames on the route to OBS is the correct
 * trade: the recording is the artefact that matters.
 */
namespace {
GstElement *makeLeakyQueue(const char *name) {
    GstElement *queue = gst_element_factory_make("queue", name);
    if (queue) {
        g_object_set(queue, "leaky", 2 /* downstream */, "max-size-buffers", 4u, "max-size-bytes",
                     0u, "max-size-time", G_GUINT64_CONSTANT(0), nullptr);
    }
    return queue;
}
} // namespace

bool CapturePipeline::audioSinkExists(const QString &name) {
    if (name.isEmpty()) {
        return false;
    }
    initializeGStreamer();
    GstDeviceMonitor *monitor = gst_device_monitor_new();
    gst_device_monitor_add_filter(monitor, "Audio/Sink", nullptr);
    if (!gst_device_monitor_start(monitor)) {
        gst_object_unref(monitor);
        return false;
    }

    GList *devices = gst_device_monitor_get_devices(monitor);
    bool found = false;
    for (GList *it = devices; it != nullptr && !found; it = it->next) {
        GstStructure *properties = gst_device_get_properties(GST_DEVICE_CAST(it->data));
        if (!properties) {
            continue;
        }
        // PipeWire publishes node.name; the PulseAudio provider uses device.name for the same
        // thing, and pulsesink's device property accepts either spelling.
        for (const char *key : {"node.name", "device.name"}) {
            const gchar *value = gst_structure_get_string(properties, key);
            if (value && name == QString::fromUtf8(value)) {
                found = true;
                break;
            }
        }
        gst_structure_free(properties);
    }
    g_list_free_full(devices, gst_object_unref);
    gst_device_monitor_stop(monitor);
    gst_object_unref(monitor);
    return found;
}

void CapturePipeline::buildObsVideo(GstElement *rawTee) {
    if (!config_.obsOutput || !config_.obsOutput->isRunning()) {
        noteObsProblem(
            QStringLiteral("No v4l2loopback device for video; install v4l2loopback-dkms"));
        return;
    }

    ObsVideoOutput *output = config_.obsOutput;
    GstElement *queue = makeLeakyQueue("obs-video-queue");
    GstElement *convert = gst_element_factory_make("videoconvert", "obs-convert");
    GstElement *scale = gst_element_factory_make("videoscale", "obs-scale");
    GstElement *rate = gst_element_factory_make("videorate", "obs-rate");
    GstElement *caps = gst_element_factory_make("capsfilter", "obs-caps");
    GstElement *sink = gst_element_factory_make("appsink", "obs-video-sink");
    if (!queue || !convert || !scale || !rate || !caps || !sink) {
        gst_clear_object(&queue);
        gst_clear_object(&convert);
        gst_clear_object(&scale);
        gst_clear_object(&rate);
        gst_clear_object(&caps);
        gst_clear_object(&sink);
        noteObsProblem(QStringLiteral("Could not build the OBS video branch"));
        return;
    }

    // Scaled and paced into the geometry the output already opened the node with. The node's
    // format must not change underneath OBS, so capture is fitted to it rather than the reverse.
    GstCaps *wanted = gst_caps_from_string(output->requiredCaps().toLatin1().constData());
    g_object_set(caps, "caps", wanted, nullptr);
    gst_caps_unref(wanted);

    g_object_set(sink, "emit-signals", FALSE, "sync", FALSE, "max-buffers", 2u, "drop", TRUE,
                 nullptr);

    gst_bin_add_many(GST_BIN(pipeline_), queue, convert, scale, rate, caps, sink, nullptr);

    bool linked = false;
    if (config_.hardwareEncoder && !config_.testSource) {
        // The raw tee carries GL memory on the GPU path, so the frame comes back to system memory
        // before the colour conversion rather than being converted twice.
        GstElement *download = gst_element_factory_make("gldownload", "obs-download");
        if (download) {
            gst_bin_add(GST_BIN(pipeline_), download);
            linked = linkAll({rawTee, queue, download, convert, scale, rate, caps, sink});
        }
    } else {
        linked = linkAll({rawTee, queue, convert, scale, rate, caps, sink});
    }

    if (!linked) {
        noteObsProblem(QStringLiteral("Could not link the OBS video branch"));
        return;
    }

    GstAppSinkCallbacks callbacks{};
    callbacks.new_sample = [](GstAppSink *appsink, gpointer userData) -> GstFlowReturn {
        GstSample *sample = gst_app_sink_pull_sample(appsink);
        if (!sample) {
            return GST_FLOW_OK;
        }
        if (GstBuffer *buffer = gst_sample_get_buffer(sample)) {
            static_cast<ObsVideoOutput *>(userData)->submitFrame(buffer);
        }
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    };
    gst_app_sink_set_callbacks(GST_APP_SINK(sink), &callbacks, output, nullptr);
    obsVideoLive_ = true;
}

void CapturePipeline::attachObsAudio(GstElement *tee, ObsAudioOutput *output, const char *label) {
    if (!tee || !output) {
        return;
    }
    if (!output->isRunning()) {
        noteObsProblem(QStringLiteral("OBS %1 sink is not available").arg(QLatin1String(label)));
        return;
    }

    const auto named = [label](const char *suffix) {
        return QStringLiteral("obs-%1-%2")
            .arg(QLatin1String(label), QLatin1String(suffix))
            .toLatin1();
    };
    GstElement *queue = makeLeakyQueue(named("queue").constData());
    GstElement *convert = gst_element_factory_make("audioconvert", named("convert").constData());
    GstElement *resample = gst_element_factory_make("audioresample", named("resample").constData());
    GstElement *caps = gst_element_factory_make("capsfilter", named("caps").constData());
    GstElement *sink = gst_element_factory_make("appsink", named("sink").constData());
    if (!queue || !convert || !resample || !caps || !sink) {
        gst_clear_object(&queue);
        gst_clear_object(&convert);
        gst_clear_object(&resample);
        gst_clear_object(&caps);
        gst_clear_object(&sink);
        noteObsProblem(
            QStringLiteral("Could not build the OBS %1 audio branch").arg(QLatin1String(label)));
        return;
    }

    GstCaps *wanted = gst_caps_from_string(ObsAudioOutput::requiredCaps().toLatin1().constData());
    g_object_set(caps, "caps", wanted, nullptr);
    gst_caps_unref(wanted);
    g_object_set(sink, "emit-signals", FALSE, "sync", FALSE, "max-buffers", 4u, "drop", TRUE,
                 nullptr);

    gst_bin_add_many(GST_BIN(pipeline_), queue, convert, resample, caps, sink, nullptr);
    if (!linkAll({tee, queue, convert, resample, caps, sink})) {
        noteObsProblem(
            QStringLiteral("Could not link the OBS %1 audio branch").arg(QLatin1String(label)));
        return;
    }

    GstAppSinkCallbacks callbacks{};
    callbacks.new_sample = [](GstAppSink *appsink, gpointer userData) -> GstFlowReturn {
        GstSample *sample = gst_app_sink_pull_sample(appsink);
        if (!sample) {
            return GST_FLOW_OK;
        }
        if (GstBuffer *buffer = gst_sample_get_buffer(sample)) {
            static_cast<ObsAudioOutput *>(userData)->submitBuffer(buffer);
        }
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    };
    gst_app_sink_set_callbacks(GST_APP_SINK(sink), &callbacks, output, nullptr);
    ++obsAudioLive_;
}

void CapturePipeline::setAudioGainDb(const QString &source, const double decibels) {
    pendingGainDb_.insert(source, decibels);
    const double linear = decibels <= -40.0 ? 0.0 : std::pow(10.0, decibels / 20.0);
    const double volume = qBound(0.0, linear, 10.0);
    if (GstElement *gain = mixGain_.value(source)) {
        g_object_set(gain, "volume", volume, nullptr);
    }
    if (source == QLatin1String("game") && monitorGain_) {
        g_object_set(monitorGain_, "volume", volume, nullptr);
    }
}

void CapturePipeline::setAudioMuted(const QString &source, const bool muted) {
    pendingMuted_.insert(source, muted);
    if (GstElement *gain = mixGain_.value(source)) {
        g_object_set(gain, "mute", muted ? TRUE : FALSE, nullptr);
    }
    if (source == QLatin1String("game") && monitorGain_) {
        g_object_set(monitorGain_, "mute", muted ? TRUE : FALSE, nullptr);
    }
}

QStringList CapturePipeline::audioTrackNames() const {
    QStringList names;
    names.reserve(audioTracks_.size());
    for (const auto &track : audioTracks_) {
        names.append(track.name);
    }
    return names;
}

GstElement *CapturePipeline::buildAudioSource(GstElement *source, const char *label) {
    const auto named = [label](const char *suffix) {
        return QStringLiteral("%1-%2")
            .arg(QString::fromLatin1(label), QString::fromLatin1(suffix))
            .toLatin1();
    };

    GstElement *convert = gst_element_factory_make("audioconvert", named("convert").constData());
    GstElement *resample = gst_element_factory_make("audioresample", named("resample").constData());
    GstElement *rate = gst_element_factory_make("audiorate", named("rate").constData());
    GstElement *caps = gst_element_factory_make("capsfilter", named("caps").constData());
    GstElement *meter = gst_element_factory_make("level", named("level").constData());
    GstElement *tee = gst_element_factory_make("tee", named("tee").constData());
    if (!convert || !resample || !rate || !caps || !meter || !tee) {
        gst_clear_object(&convert);
        gst_clear_object(&resample);
        gst_clear_object(&rate);
        gst_clear_object(&caps);
        gst_clear_object(&meter);
        gst_clear_object(&tee);
        return nullptr;
    }

    /*
     * Every source is forced to the same S16LE 48 kHz stereo shape before it is mixed or muxed.
     *
     * The card and the microphone are independent clock domains and drift against each other by
     * tens of parts per million — seconds an hour, which is the usual reason home-made capture
     * tools end up out of sync. audioresample absorbs the rate difference and audiorate fills or
     * drops samples so the output timeline stays continuous against the pipeline clock.
     */
    GstCaps *shape =
        gst_caps_from_string("audio/x-raw,format=S16LE,rate=48000,channels=2,layout=interleaved");
    g_object_set(caps, "caps", shape, nullptr);
    gst_caps_unref(shape);
    g_object_set(rate, "tolerance", static_cast<guint64>(40 * GST_MSECOND), nullptr);

    g_object_set(meter, "post-messages", TRUE, "interval", static_cast<guint64>(80 * GST_MSECOND),
                 nullptr);

    gst_bin_add_many(GST_BIN(pipeline_), convert, resample, rate, caps, meter, tee, nullptr);
    if (!linkAll({source, convert, resample, rate, caps, meter, tee})) {
        return nullptr;
    }
    return tee;
}

bool CapturePipeline::buildAudio(GstElement *ringSink, QString *error) {
    audioTracks_.clear();
    audioNotice_.clear();
    obsNotice_.clear();
    obsVideoLive_ = false;
    obsAudioLive_ = 0;
    mixGain_.clear();
    monitorGain_ = nullptr;
    if (!config_.enableAudio) {
        return true;
    }

    GstElement *gameTee = nullptr;
    GstElement *micTee = nullptr;

    if (config_.captureGameAudio && !config_.gameAudioDevice.isEmpty()) {
        GstElement *source = gst_element_factory_make(
            config_.testSource ? "audiotestsrc" : "alsasrc", "game-audio-source");
        if (source) {
            if (config_.testSource) {
                g_object_set(source, "is-live", TRUE, "freq", 440.0, nullptr);
            } else {
                const auto device = QFile::encodeName(config_.gameAudioDevice);
                g_object_set(source, "device", device.constData(), "buffer-time",
                             static_cast<gint64>(40'000), "latency-time",
                             static_cast<gint64>(20'000), nullptr);
            }
            // The video capture clock is the master; audio must not drag the pipeline onto a
            // sound-card clock that runs at its own rate.
            if (g_object_class_find_property(G_OBJECT_GET_CLASS(source), "provide-clock")) {
                g_object_set(source, "provide-clock", FALSE, nullptr);
            }
            if (canOpenSource(source)) {
                gst_bin_add(GST_BIN(pipeline_), source);
                gameTee = buildAudioSource(source, "game");
            } else {
                gst_object_unref(source);
                audioNotice_ = QStringLiteral("Console audio unavailable (%1 could not be opened; "
                                              "another program may be using it)")
                                   .arg(config_.gameAudioDevice);
            }
        }
    }

    if (config_.captureMic) {
        GstElement *source = gst_element_factory_make(
            config_.testSource ? "audiotestsrc" : "pipewiresrc", "mic-audio-source");
        if (source) {
            if (config_.testSource) {
                g_object_set(source, "is-live", TRUE, "freq", 880.0, nullptr);
            } else if (!config_.micTarget.isEmpty()) {
                const auto target = config_.micTarget.toUtf8();
                g_object_set(source, "target-object", target.constData(), nullptr);
            }
            if (g_object_class_find_property(G_OBJECT_GET_CLASS(source), "provide-clock")) {
                g_object_set(source, "provide-clock", FALSE, nullptr);
            }
            if (canOpenSource(source)) {
                gst_bin_add(GST_BIN(pipeline_), source);
                micTee = buildAudioSource(source, "mic");
            } else {
                gst_object_unref(source);
                const auto note = QStringLiteral("Microphone unavailable");
                audioNotice_ =
                    audioNotice_.isEmpty() ? note : audioNotice_ + QStringLiteral("; ") + note;
            }
        }
    }

    // Sent per source rather than from the mix, which is the whole point: OBS gets game and
    // microphone as two inputs it can level and filter apart from each other.
    if (config_.enableObsOutput) {
        attachObsAudio(gameTee, config_.obsGameOutput, "game");
        attachObsAudio(micTee, config_.obsMicOutput, "mic");
    }

    if (!gameTee && !micTee) {
        return true; // No audio anywhere; video still records.
    }

    /*
     * Track order is deliberate. Track 0 is the mix so the file is usable with no editing at all;
     * the isolated sources follow for anyone who wants to rebalance or drop the mic in post.
     */
    if (gameTee && micTee) {
        GstElement *mixer = gst_element_factory_make("audiomixer", "audio-mixer");
        GstElement *mixConvert = gst_element_factory_make("audioconvert", "mix-convert");
        GstElement *mixTee = gst_element_factory_make("tee", "mix-tee");
        GstElement *gameToMix = gst_element_factory_make("queue", "game-to-mix");
        GstElement *micToMix = gst_element_factory_make("queue", "mic-to-mix");
        GstElement *gameGain = gst_element_factory_make("volume", "game-gain");
        GstElement *micGain = gst_element_factory_make("volume", "mic-gain");
        if (mixer && mixConvert && mixTee && gameToMix && micToMix && gameGain && micGain) {
            gst_bin_add_many(GST_BIN(pipeline_), mixer, mixConvert, mixTee, gameToMix, micToMix,
                             gameGain, micGain, nullptr);
            const bool mixed = linkAll({gameTee, gameToMix, gameGain, mixer}) &&
                               linkAll({micTee, micToMix, micGain, mixer}) &&
                               linkAll({mixer, mixConvert, mixTee});
            if (mixed) {
                mixGain_.insert(QStringLiteral("game"), gameGain);
                mixGain_.insert(QStringLiteral("mic"), micGain);
                audioTracks_.append({QStringLiteral("mix"), mixTee});
            }
        }
    }
    if (gameTee) {
        audioTracks_.append({QStringLiteral("game"), gameTee});
    }
    if (micTee) {
        audioTracks_.append({QStringLiteral("mic"), micTee});
    }

    if (config_.enableAudioMonitoring && gameTee) {
        GstElement *queue = gst_element_factory_make("queue", "game-monitor-queue");
        GstElement *gain = gst_element_factory_make("volume", "game-monitor-gain");
        GstElement *convert = gst_element_factory_make("audioconvert", "game-monitor-convert");
        GstElement *resample = gst_element_factory_make("audioresample", "game-monitor-resample");
        GstElement *sink = gst_element_factory_make(
            config_.testSource ? "fakesink" : "pipewiresink", "game-monitor-sink");
        if (queue && gain && convert && resample && sink) {
            g_object_set(queue, "max-size-time", static_cast<guint64>(50 * GST_MSECOND),
                         "max-size-buffers", 0u, "max-size-bytes", 0u, "leaky", 2, nullptr);
            if (g_object_class_find_property(G_OBJECT_GET_CLASS(sink), "sync")) {
                g_object_set(sink, "sync", FALSE, nullptr);
            }
            if (g_object_class_find_property(G_OBJECT_GET_CLASS(sink), "async")) {
                g_object_set(sink, "async", FALSE, nullptr);
            }
            gst_bin_add_many(GST_BIN(pipeline_), queue, gain, convert, resample, sink, nullptr);
            if (linkAll({gameTee, queue, gain, convert, resample, sink})) {
                monitorGain_ = gain;
            } else {
                gst_bin_remove_many(GST_BIN(pipeline_), queue, gain, convert, resample, sink,
                                    nullptr);
                if (audioNotice_.isEmpty()) {
                    audioNotice_ = QStringLiteral(
                        "Console audio is recording, but live monitoring could not start");
                }
            }
        } else {
            gst_clear_object(&queue);
            gst_clear_object(&gain);
            gst_clear_object(&convert);
            gst_clear_object(&resample);
            gst_clear_object(&sink);
            if (audioNotice_.isEmpty()) {
                audioNotice_ =
                    QStringLiteral("Console audio is recording, but no playback sink is available");
            }
        }
    }

    for (auto it = pendingGainDb_.constBegin(); it != pendingGainDb_.constEnd(); ++it) {
        setAudioGainDb(it.key(), it.value());
    }
    for (auto it = pendingMuted_.constBegin(); it != pendingMuted_.constEnd(); ++it) {
        setAudioMuted(it.key(), it.value());
    }

    // Give the ring the same tracks the recording gets, so a saved flashback is not a silent film.
    for (qsizetype i = 0; i < audioTracks_.size(); ++i) {
        const auto queueName = QStringLiteral("ring-audio-%1").arg(i).toLatin1();
        GstElement *queue = gst_element_factory_make("queue", queueName.constData());
        if (!queue) {
            if (error) {
                *error = QStringLiteral("Could not create the ring queue for audio track %1")
                             .arg(audioTracks_.at(i).name);
            }
            return false;
        }
        gst_bin_add(GST_BIN(pipeline_), queue);
        GstPad *ringPad = gst_element_request_pad_simple(ringSink, "audio_%u");
        if (!ringPad) {
            if (error) {
                *error = QStringLiteral("The segment ring refused an audio pad for track %1")
                             .arg(audioTracks_.at(i).name);
            }
            return false;
        }
        GstPad *queueSink = gst_element_get_static_pad(queue, "sink");
        GstPad *queueSrc = gst_element_get_static_pad(queue, "src");
        GstPad *teePad = gst_element_request_pad_simple(audioTracks_.at(i).tee, "src_%u");
        const auto fromTee =
            (teePad && queueSink) ? gst_pad_link(teePad, queueSink) : GST_PAD_LINK_REFUSED;
        const auto toRing = (queueSrc && fromTee == GST_PAD_LINK_OK)
                                ? gst_pad_link(queueSrc, ringPad)
                                : GST_PAD_LINK_REFUSED;
        gst_clear_object(&queueSink);
        gst_clear_object(&queueSrc);
        if (fromTee != GST_PAD_LINK_OK || toRing != GST_PAD_LINK_OK) {
            if (error) {
                *error =
                    QStringLiteral("Could not link audio track %1 into the ring (tee=%2 ring=%3)")
                        .arg(audioTracks_.at(i).name)
                        .arg(static_cast<int>(fromTee))
                        .arg(static_cast<int>(toRing));
            }
            return false;
        }
    }
    return true;
}

GstElement *CapturePipeline::make(const char *factory, const char *name, QString *error) const {
    GstElement *element = gst_element_factory_make(factory, name);
    if (!element && error && error->isEmpty()) {
        *error = QStringLiteral("Required GStreamer element '%1' is unavailable")
                     .arg(QString::fromLatin1(factory));
    }
    return element;
}

void CapturePipeline::destroyPipeline() {
    if (!pipeline_) {
        return;
    }
    /*
     * Detach the preview before anything is freed.
     *
     * qml6glsink and the QML video item hold each other, and the item is drawn on Qt's render
     * thread, which knows nothing about this one. Unreffing the pipeline frees the sink under a
     * renderer that is still pointing at it, and the crash that follows is a jump through whatever
     * happens to be in freed memory. Rebuilding the graph is exactly when this happens, which is
     * why Refresh could take the window with it.
     */
    if (previewSink_ && g_object_class_find_property(G_OBJECT_GET_CLASS(previewSink_), "widget")) {
        g_object_set(previewSink_, "widget", nullptr, nullptr);
    }
    gst_element_set_state(pipeline_, GST_STATE_NULL);
    gst_object_unref(pipeline_);
    pipeline_ = nullptr;
    encodedTee_ = nullptr;
    encoder_ = nullptr;
    previewSink_ = nullptr;
    recordQueue_ = nullptr;
    recordMux_ = nullptr;
    recordSink_ = nullptr;
    recordTeePad_ = nullptr;
    audioTracks_.clear();
    mixGain_.clear();
    monitorGain_ = nullptr;
    recordAudioQueues_.clear();
    recordAudioTeePads_.clear();
    recordingPath_.clear();
    recordingStopping_ = false;
}

void CapturePipeline::destroyRecordingBranch() {
    if (!pipeline_ || !recordQueue_) {
        return;
    }

    for (qsizetype i = 0; i < recordAudioQueues_.size(); ++i) {
        GstElement *queue = recordAudioQueues_.at(i);
        gst_element_set_state(queue, GST_STATE_NULL);
        if (i < recordAudioTeePads_.size()) {
            GstPad *teePad = recordAudioTeePads_.at(i);
            if (GstElement *tee = gst_pad_get_parent_element(teePad)) {
                gst_element_release_request_pad(tee, teePad);
                gst_object_unref(tee);
            }
            gst_object_unref(teePad);
        }
        gst_bin_remove(GST_BIN(pipeline_), queue);
    }
    recordAudioQueues_.clear();
    recordAudioTeePads_.clear();

    gst_element_set_state(recordQueue_, GST_STATE_NULL);
    gst_element_set_state(recordMux_, GST_STATE_NULL);
    gst_element_set_state(recordSink_, GST_STATE_NULL);
    if (recordTeePad_) {
        gst_element_release_request_pad(encodedTee_, recordTeePad_);
        gst_object_unref(recordTeePad_);
        recordTeePad_ = nullptr;
    }
    gst_bin_remove_many(GST_BIN(pipeline_), recordQueue_, recordMux_, recordSink_, nullptr);
    recordQueue_ = nullptr;
    recordMux_ = nullptr;
    recordSink_ = nullptr;
}

GstPadProbeReturn CapturePipeline::blockRecordingPad(GstPad *pad, GstPadProbeInfo *,
                                                     gpointer userData) {
    auto *self = static_cast<CapturePipeline *>(userData);
    if (!self->recordQueue_) {
        return GST_PAD_PROBE_REMOVE;
    }
    GstPad *queueSink = gst_element_get_static_pad(self->recordQueue_, "sink");
    gst_pad_unlink(pad, queueSink);
    gst_pad_send_event(queueSink, gst_event_new_eos());
    gst_object_unref(queueSink);

    // matroskamux only writes its final headers once every input has ended, so an audio track left
    // running would leave the file unplayable.
    for (qsizetype i = 0; i < self->recordAudioQueues_.size(); ++i) {
        GstElement *queue = self->recordAudioQueues_.at(i);
        GstPad *audioSink = gst_element_get_static_pad(queue, "sink");
        if (!audioSink) {
            continue;
        }
        if (i < self->recordAudioTeePads_.size()) {
            gst_pad_unlink(self->recordAudioTeePads_.at(i), audioSink);
        }
        gst_pad_send_event(audioSink, gst_event_new_eos());
        gst_object_unref(audioSink);
    }
    return GST_PAD_PROBE_REMOVE;
}

GstPadProbeReturn CapturePipeline::observeRecordingEos(GstPad *, GstPadProbeInfo *info,
                                                       gpointer userData) {
    if ((GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) == 0) {
        return GST_PAD_PROBE_OK;
    }
    GstEvent *event = GST_PAD_PROBE_INFO_EVENT(info);
    if (event && GST_EVENT_TYPE(event) == GST_EVENT_EOS) {
        auto *self = static_cast<CapturePipeline *>(userData);
        QMetaObject::invokeMethod(self, &CapturePipeline::finalizeRecording, Qt::QueuedConnection);
    }
    return GST_PAD_PROBE_OK;
}

} // namespace quadcap::pipeline
