#pragma once

#include <QMutex>
#include <QObject>
#include <QString>

#include <gst/gst.h>

namespace quadcap::pipeline {

/*!
 * Writes one captured audio source to the PipeWire sink OBS reads it from.
 *
 * It runs its own pipeline rather than living in the capture graph, for the same reason the video
 * output does. An audio sink acquires a ring buffer when it goes to PAUSED, and that can block;
 * inside the capture graph it blocked the graph's state change, which quadcap performs on the UI
 * thread, and the window froze with no picture and no way to interact with it. Here a sink that
 * stalls stalls only itself.
 */
class ObsAudioOutput final : public QObject {
    Q_OBJECT

  public:
    explicit ObsAudioOutput(QObject *parent = nullptr);
    ~ObsAudioOutput() override;

    ObsAudioOutput(const ObsAudioOutput &) = delete;
    ObsAudioOutput &operator=(const ObsAudioOutput &) = delete;

    [[nodiscard]] bool start(const QString &sink, QString *error = nullptr);
    void stop();
    [[nodiscard]] bool isRunning() const;

    //! Caps the capture branch must produce for its buffers to be accepted here.
    [[nodiscard]] static QString requiredCaps();

    //! Hands one captured buffer over. Safe to call from a GStreamer streaming thread.
    void submitBuffer(GstBuffer *buffer);

  private:
    GstElement *pipeline_ = nullptr;
    GstElement *source_ = nullptr;
    mutable QMutex mutex_;
    QString sink_;
};

} // namespace quadcap::pipeline
