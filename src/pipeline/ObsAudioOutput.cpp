#include "pipeline/ObsAudioOutput.h"

#include <gst/app/gstappsrc.h>

#include <mutex>

namespace quadcap::pipeline {
namespace {

void initializeGStreamer() {
    static std::once_flag once;
    std::call_once(once, [] { gst_init(nullptr, nullptr); });
}

} // namespace

ObsAudioOutput::ObsAudioOutput(QObject *parent) : QObject(parent) {}

ObsAudioOutput::~ObsAudioOutput() {
    stop();
}

bool ObsAudioOutput::isRunning() const {
    QMutexLocker locker(&mutex_);
    return pipeline_ != nullptr;
}

QString ObsAudioOutput::requiredCaps() {
    return QStringLiteral("audio/x-raw,format=S16LE,rate=48000,channels=2,layout=interleaved");
}

bool ObsAudioOutput::start(const QString &sink, QString *error) {
    initializeGStreamer();
    stop();

    if (sink.isEmpty()) {
        if (error) {
            *error = QStringLiteral("No sink to write to");
        }
        return false;
    }
    sink_ = sink;

    GstElement *pipeline = gst_pipeline_new(nullptr);
    GstElement *source = gst_element_factory_make("appsrc", "obs-audio-appsrc");
    GstElement *convert = gst_element_factory_make("audioconvert", "obs-audio-convert");
    GstElement *output = gst_element_factory_make("pulsesink", "obs-audio-out");
    if (!pipeline || !source || !convert || !output) {
        gst_clear_object(&pipeline);
        gst_clear_object(&source);
        gst_clear_object(&convert);
        gst_clear_object(&output);
        if (error) {
            *error = QStringLiteral("Could not build the OBS audio output");
        }
        return false;
    }

    GstCaps *caps = gst_caps_from_string(requiredCaps().toLatin1().constData());
    g_object_set(source, "caps", caps, "is-live", TRUE, "format", GST_FORMAT_TIME, "do-timestamp",
                 TRUE, "block", FALSE, nullptr);
    gst_caps_unref(caps);

    const auto device = sink_.toUtf8();
    // async FALSE so the sink never holds a state change waiting to preroll, and the capture
    // clock stays master rather than the sound server's.
    g_object_set(output, "device", device.constData(), "async", FALSE, nullptr);
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(output), "provide-clock")) {
        g_object_set(output, "provide-clock", FALSE, nullptr);
    }

    gst_bin_add_many(GST_BIN(pipeline), source, convert, output, nullptr);
    if (!gst_element_link_many(source, convert, output, nullptr)) {
        gst_object_unref(pipeline);
        if (error) {
            *error = QStringLiteral("Could not link the OBS audio output");
        }
        return false;
    }

    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        if (error) {
            *error = QStringLiteral("OBS sink %1 could not be opened").arg(sink_);
        }
        return false;
    }

    QMutexLocker locker(&mutex_);
    pipeline_ = pipeline;
    source_ = source;
    return true;
}

void ObsAudioOutput::stop() {
    GstElement *pipeline = nullptr;
    {
        QMutexLocker locker(&mutex_);
        pipeline = pipeline_;
        pipeline_ = nullptr;
        source_ = nullptr;
    }
    if (!pipeline) {
        return;
    }
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
}

void ObsAudioOutput::submitBuffer(GstBuffer *buffer) {
    if (!buffer) {
        return;
    }
    QMutexLocker locker(&mutex_);
    if (!source_) {
        return;
    }
    GstBuffer *copy = gst_buffer_copy(buffer);
    GST_BUFFER_PTS(copy) = GST_CLOCK_TIME_NONE;
    GST_BUFFER_DTS(copy) = GST_CLOCK_TIME_NONE;
    gst_app_src_push_buffer(GST_APP_SRC(source_), copy); // Takes ownership.
}

} // namespace quadcap::pipeline
