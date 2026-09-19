#pragma once

#include <QObject>
#include <QString>
#include <QHash>
#include <QStringList>
#include <QVector>
#include <QTimer>

#include <gst/gst.h>

namespace quadcap::pipeline {

struct PipelineConfig {
    QString devicePath;
    QString ringDirectory;
    int width = 3840;
    int height = 2160;
    int frameRate = 60;
    int ringMinutes = 10;
    int segmentSeconds = 2;
    bool testSource = false;
    bool hardwareEncoder = true;
    bool enablePreview = false;

    bool enableAudio = true;
    bool enableAudioMonitoring = false;
    //! ALSA device carrying HDMI audio, e.g. "hw:CARD=MK2,DEV=0". Empty skips game audio.
    QString gameAudioDevice;
    //! PipeWire node for the microphone. Empty uses the default source.
    QString micTarget;
    bool captureGameAudio = true;
    bool captureMic = true;
};

/*!
 * One recorded audio track, in muxing order.
 *
 * Mix exists only when both sources are present — with a single source it would duplicate it for no
 * benefit, so the track list collapses to just that source.
 */
struct AudioTrack {
    QString name;
    GstElement *tee = nullptr;
};

class CapturePipeline final : public QObject {
    Q_OBJECT

public:
    explicit CapturePipeline(QObject *parent = nullptr);
    ~CapturePipeline() override;

    CapturePipeline(const CapturePipeline &) = delete;
    CapturePipeline &operator=(const CapturePipeline &) = delete;

    [[nodiscard]] bool start(const PipelineConfig &config, QString *error = nullptr);
    void stop();

    [[nodiscard]] bool startRecording(const QString &path, QString *error = nullptr);
    void stopRecording();

    void setPreviewItem(QObject *item);

    [[nodiscard]] bool isRunning() const;
    [[nodiscard]] bool isRecording() const;
    [[nodiscard]] QString currentRecording() const;

    //! Names of the audio tracks currently being muxed, in track order. Empty when audio is off.
    [[nodiscard]] QStringList audioTrackNames() const;

    //! Why an audio source was left out, for the UI to show. Empty when everything was captured.
    [[nodiscard]] QString audioNotice() const;

    void setAudioGainDb(const QString &source, double decibels);
    void setAudioMuted(const QString &source, bool muted);

    /*!
     * True when \a element reaches READY, which for a capture source means its device opened.
     *
     * A busy or absent sound device only reports itself when the element is activated. Probing it
     * before wiring it into the graph is what lets a missing microphone be left out rather than
     * taking the whole pipeline — and the video with it — down at PLAYING.
     */
    [[nodiscard]] static bool canOpenSource(GstElement *element);

signals:
    void errorOccurred(const QString &message);
    void warningOccurred(const QString &message);
    void recordingChanged(bool recording, const QString &path);

    /*!
     * A ring fragment splitmuxsink has finished writing and closed.
     *
     * This is the only trustworthy "safe to read" signal: the file appears on disk well before it
     * is complete, because the next fragment is opened while this one is still being flushed.
     */
    void segmentClosed(const QString &path, qint64 durationNs);

    void audioLevel(const QString &source, double rmsDb, double peakDb);

private slots:
    void pollBus();
    void finalizeRecording();

private:
    [[nodiscard]] bool build(QString *error);

    /*!
     * Builds the audio capture graph and attaches every track to the ring.
     *
     * Missing or unopenable sources are not fatal: whatever is present is captured and the rest is
     * left out, because losing the mic should never cost you the video.
     */
    [[nodiscard]] bool buildAudio(GstElement *ringSink, QString *error);

    //! One source chain: convert, resample, drift-correct, then tee. Null when it cannot be built.
    [[nodiscard]] GstElement *buildAudioSource(GstElement *source, const char *label);

    [[nodiscard]] GstElement *make(const char *factory, const char *name, QString *error) const;
    void destroyPipeline();
    void destroyRecordingBranch();

    static GstPadProbeReturn blockRecordingPad(GstPad *pad, GstPadProbeInfo *info, gpointer userData);
    static GstPadProbeReturn observeRecordingEos(GstPad *pad, GstPadProbeInfo *info, gpointer userData);

    PipelineConfig config_;
    QObject *previewItem_ = nullptr;
    GstElement *pipeline_ = nullptr;
    GstElement *encodedTee_ = nullptr;
    GstElement *encoder_ = nullptr;
    GstElement *previewSink_ = nullptr;
    GstElement *recordQueue_ = nullptr;
    GstElement *recordMux_ = nullptr;
    GstElement *recordSink_ = nullptr;
    GstPad *recordTeePad_ = nullptr;
    QVector<AudioTrack> audioTracks_;
    QString audioNotice_;
    QHash<QString, GstElement *> mixGain_;
    GstElement *monitorGain_ = nullptr;
    QHash<QString, double> pendingGainDb_;
    QHash<QString, bool> pendingMuted_;
    QVector<GstElement *> recordAudioQueues_;
    QVector<GstPad *> recordAudioTeePads_;
    QString recordingPath_;
    bool recordingStopping_ = false;
    QTimer busTimer_;
};

} // namespace quadcap::pipeline
