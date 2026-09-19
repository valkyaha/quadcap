#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVector>

#include "pipeline/ObsAudioOutput.h"
#include "pipeline/ObsVideoOutput.h"

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

    /*!
     * Routing to OBS, which cannot open the capture card itself while quadcap holds it.
     *
     * Video goes to a v4l2loopback node OBS adds as a camera. Audio goes to one sink per source
     * rather than a single mixed one, so game and microphone arrive in OBS as separate inputs that
     * can be levelled, ducked and filtered independently.
     */
    bool enableObsOutput = false;
    /*!
     * Where captured frames go for OBS to read.
     *
     * Not a device path: the node is held open by an ObsVideoOutput that outlives any one capture
     * pipeline, so the camera does not disappear from OBS when the console sleeps. Null leaves the
     * video leg out.
     */
    ObsVideoOutput *obsOutput = nullptr;
    /*!
     * Where each audio source goes for OBS to read.
     *
     * Objects rather than sink names, for the same reason as the video: an audio sink acquires a
     * ring buffer on its way to PAUSED and that can block, which inside this graph meant blocking
     * the state change the UI thread is waiting on. Null leaves that source out.
     */
    ObsAudioOutput *obsGameOutput = nullptr;
    ObsAudioOutput *obsMicOutput = nullptr;
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

    //! Why the OBS route is not carrying everything. Empty when it is, or when it is switched off.
    [[nodiscard]] QString obsNotice() const;

    //! True when at least one OBS output is live.
    [[nodiscard]] bool obsActive() const;

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

    /*!
     * True when an audio sink with this exact name is present.
     *
     * Needed because pulsesink does not fail on a name that does not exist: it quietly falls back
     * to the default sink. Left unchecked, switching on the OBS route would send console audio to
     * the speakers instead, with OBS receiving nothing and no error to explain either half.
     */
    [[nodiscard]] static bool audioSinkExists(const QString &name);

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

    /*!
     * Hangs the OBS video branch off the raw tee.
     *
     * Never fatal. A missing loopback node means the module is not installed, which is a reason to
     * tell someone rather than to refuse to capture.
     */
    void buildObsVideo(GstElement *rawTee);

    //! Taps one audio source for its OBS output. Never fatal, for the same reason.
    void attachObsAudio(GstElement *tee, ObsAudioOutput *output, const char *label);

    //! Adds \a note to obsNotice_, keeping any note already there.
    void noteObsProblem(const QString &note);

    [[nodiscard]] GstElement *make(const char *factory, const char *name, QString *error) const;
    void destroyPipeline();
    void destroyRecordingBranch();

    static GstPadProbeReturn blockRecordingPad(GstPad *pad, GstPadProbeInfo *info,
                                               gpointer userData);
    static GstPadProbeReturn observeRecordingEos(GstPad *pad, GstPadProbeInfo *info,
                                                 gpointer userData);

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
    QString obsNotice_;
    bool obsVideoLive_ = false;
    int obsAudioLive_ = 0;
    QHash<QString, GstElement *> mixGain_;
    GstElement *monitorGain_ = nullptr;
    QHash<QString, double> pendingGainDb_;
    QHash<QString, bool> pendingMuted_;
    QVector<GstElement *> recordAudioQueues_;
    QVector<GstPad *> recordAudioTeePads_;
    QString recordingPath_;
    bool recordingStopping_ = false;
    //! Set between a fatal error and the deferred teardown, so the failure is reported once.
    bool tearingDown_ = false;
    QTimer busTimer_;
};

} // namespace quadcap::pipeline
