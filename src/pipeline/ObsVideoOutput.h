#pragma once

#include <QElapsedTimer>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QTimer>

#include <gst/gst.h>

namespace quadcap::pipeline {

/*!
 * Owns the v4l2loopback node OBS reads, for as long as OBS output is switched on.
 *
 * The node is configured with exclusive_caps, so it only presents itself as a camera while
 * something is writing to it. If quadcap were the writer directly, every console standby would
 * close the device, and OBS would drop the source and not retry: the scene comes back with a dead
 * camera that has to be toggled by hand.
 *
 * So the writer is this object rather than the capture graph. It outlives any one capture
 * pipeline, and when no frames are arriving it writes black instead, which keeps the device open
 * and the format unchanged. OBS sees a camera that is always there and briefly goes black, which
 * is what it does for a real camera too.
 */
class ObsVideoOutput final : public QObject {
    Q_OBJECT

  public:
    explicit ObsVideoOutput(QObject *parent = nullptr);
    ~ObsVideoOutput() override;

    ObsVideoOutput(const ObsVideoOutput &) = delete;
    ObsVideoOutput &operator=(const ObsVideoOutput &) = delete;

    /*!
     * Opens \a device and starts writing black.
     *
     * The geometry is fixed for the lifetime of the output and capture is scaled into it. That is
     * deliberate: a console changing resolution would otherwise change the camera's format
     * underneath OBS, which drops the source just as surely as the device disappearing.
     */
    [[nodiscard]] bool start(const QString &device, int width, int height, int frameRate,
                             QString *error = nullptr);
    void stop();

    [[nodiscard]] bool isRunning() const;
    [[nodiscard]] QString device() const;
    [[nodiscard]] int width() const;
    [[nodiscard]] int height() const;
    [[nodiscard]] int frameRate() const;

    //! Caps the capture branch must produce for its frames to be accepted here.
    [[nodiscard]] QString requiredCaps() const;

    /*!
     * Hands one captured frame to the node. Safe to call from a GStreamer streaming thread.
     *
     * Ignored when the output is stopped, so a capture pipeline outliving it by a few frames is
     * harmless rather than a crash.
     */
    void submitFrame(GstBuffer *buffer);

  private:
    void writeBlackIfIdle();
    [[nodiscard]] GstBuffer *makeBlackFrame() const;

    GstElement *pipeline_ = nullptr;
    GstElement *source_ = nullptr;
    QTimer idleTimer_;
    mutable QMutex mutex_;
    QElapsedTimer sinceLastFrame_;
    QString device_;
    int width_ = 0;
    int height_ = 0;
    int frameRate_ = 0;
};

} // namespace quadcap::pipeline
