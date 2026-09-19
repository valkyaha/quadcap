#include "pipeline/ObsVideoOutput.h"

#include <QFile>

#include <mutex>

#include <gst/app/gstappsrc.h>

namespace quadcap::pipeline {
namespace {

//! Two pixels of limited-range black in YUY2: Y=16 for each, U and V centred at 128.
constexpr unsigned char BlackY = 16;
constexpr unsigned char BlackChroma = 128;

/*!
 * How long without a captured frame before black is written instead, in thousandths of a frame
 * period.
 *
 * OBS gives up on a v4l2 device after five frame periods and logs "select timed out", so the gap
 * has to be filled well inside that. Two and a half periods leaves room for ordinary jitter in the
 * arrival of live frames without mistaking it for a stall and cutting black into moving video.
 */
constexpr qint64 IdleFramePeriodsMilli = 2500;

/*
 * This can be the first thing in the process to touch GStreamer: the output is brought up when the
 * switch is on, which happens before any capture pipeline is built when there is no signal yet.
 */
void initializeGStreamer() {
    static std::once_flag once;
    std::call_once(once, [] { gst_init(nullptr, nullptr); });
}

} // namespace

ObsVideoOutput::ObsVideoOutput(QObject *parent) : QObject(parent) {
    connect(&idleTimer_, &QTimer::timeout, this, &ObsVideoOutput::writeBlackIfIdle);
}

ObsVideoOutput::~ObsVideoOutput() {
    stop();
}

bool ObsVideoOutput::isRunning() const {
    QMutexLocker locker(&mutex_);
    return pipeline_ != nullptr;
}

QString ObsVideoOutput::device() const {
    return device_;
}
int ObsVideoOutput::width() const {
    return width_;
}
int ObsVideoOutput::height() const {
    return height_;
}
int ObsVideoOutput::frameRate() const {
    return frameRate_;
}

QString ObsVideoOutput::requiredCaps() const {
    // YUY2 rather than the cheaper NV12: OBS reads v4l2 through libv4l2, which refuses NV12 from a
    // loopback node and fails the source with "Selected video format not supported".
    return QStringLiteral("video/x-raw,format=YUY2,width=%1,height=%2,framerate=%3/1")
        .arg(width_)
        .arg(height_)
        .arg(frameRate_);
}

GstBuffer *ObsVideoOutput::makeBlackFrame() const {
    const gsize size = static_cast<gsize>(width_) * height_ * 2; // YUY2 is 16 bits per pixel.
    GstBuffer *buffer = gst_buffer_new_allocate(nullptr, size, nullptr);
    if (!buffer) {
        return nullptr;
    }
    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
        gst_buffer_unref(buffer);
        return nullptr;
    }
    for (gsize i = 0; i + 1 < map.size; i += 2) {
        map.data[i] = BlackY;
        map.data[i + 1] = BlackChroma;
    }
    gst_buffer_unmap(buffer, &map);
    return buffer;
}

bool ObsVideoOutput::start(const QString &device, const int width, const int height,
                           const int frameRate, QString *error) {
    initializeGStreamer();
    stop();

    if (device.isEmpty() || width <= 0 || height <= 0 || frameRate <= 0) {
        if (error) {
            *error = QStringLiteral("No loopback device to write to");
        }
        return false;
    }

    device_ = device;
    width_ = width;
    height_ = height;
    frameRate_ = frameRate;

    GstElement *pipeline = gst_pipeline_new("obs-output");
    GstElement *source = gst_element_factory_make("appsrc", "obs-appsrc");
    GstElement *convert = gst_element_factory_make("videoconvert", "obs-output-convert");
    GstElement *sink = gst_element_factory_make("v4l2sink", "obs-output-sink");
    if (!pipeline || !source || !convert || !sink) {
        gst_clear_object(&pipeline);
        gst_clear_object(&source);
        gst_clear_object(&convert);
        gst_clear_object(&sink);
        if (error) {
            *error = QStringLiteral("Could not build the OBS output");
        }
        return false;
    }

    GstCaps *caps = gst_caps_from_string(requiredCaps().toLatin1().constData());
    // Live, and timestamped as it goes: the frames come from two places at irregular intervals and
    // nothing upstream of here carries a clock worth preserving.
    g_object_set(source, "caps", caps, "is-live", TRUE, "format", GST_FORMAT_TIME, "do-timestamp",
                 TRUE, "block", FALSE, "max-bytes", static_cast<guint64>(0), nullptr);
    gst_caps_unref(caps);

    const auto path = QFile::encodeName(device_);
    g_object_set(sink, "device", path.constData(), "sync", FALSE, nullptr);
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(sink), "provide-clock")) {
        g_object_set(sink, "provide-clock", FALSE, nullptr);
    }

    gst_bin_add_many(GST_BIN(pipeline), source, convert, sink, nullptr);
    if (!gst_element_link_many(source, convert, sink, nullptr)) {
        gst_object_unref(pipeline);
        if (error) {
            *error = QStringLiteral("Could not link the OBS output");
        }
        return false;
    }

    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        if (error) {
            *error = QStringLiteral("%1 could not be opened for writing").arg(device_);
        }
        return false;
    }

    {
        QMutexLocker locker(&mutex_);
        pipeline_ = pipeline;
        source_ = source;
        sinceLastFrame_.start();
    }

    // Half a frame period, so a gap is noticed and filled before the device starves.
    idleTimer_.start(qMax(10, 1000 / (frameRate_ * 2)));
    writeBlackIfIdle();
    return true;
}

void ObsVideoOutput::stop() {
    idleTimer_.stop();

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

void ObsVideoOutput::submitFrame(GstBuffer *buffer) {
    if (!buffer) {
        return;
    }
    QMutexLocker locker(&mutex_);
    if (!source_) {
        return; // Stopped while a capture pipeline was still running; nothing to do.
    }
    sinceLastFrame_.restart();
    // Timestamps are applied by appsrc, and a buffer arriving from a tee carries the capture
    // clock's, which would fight it.
    GstBuffer *copy = gst_buffer_copy(buffer);
    GST_BUFFER_PTS(copy) = GST_CLOCK_TIME_NONE;
    GST_BUFFER_DTS(copy) = GST_CLOCK_TIME_NONE;
    gst_app_src_push_buffer(GST_APP_SRC(source_), copy); // Takes ownership.
}

void ObsVideoOutput::writeBlackIfIdle() {
    GstElement *source = nullptr;
    {
        QMutexLocker locker(&mutex_);
        const qint64 idleAfterMs = qMax<qint64>(8, IdleFramePeriodsMilli / qMax(1, frameRate_));
        if (!source_ || sinceLastFrame_.elapsed() < idleAfterMs) {
            return;
        }
        source = GST_ELEMENT(gst_object_ref(source_));
    }

    GstBuffer *black = makeBlackFrame();
    if (!black) {
        gst_object_unref(source);
        return;
    }
    gst_app_src_push_buffer(GST_APP_SRC(source), black); // Takes ownership.
    gst_object_unref(source);
}

} // namespace quadcap::pipeline
