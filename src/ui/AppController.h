#pragma once

#include "device/CaptureDevice.h"
#include "device/SystemCheck.h"
#include "flashback/FlashbackRing.h"
#include "pipeline/CapturePipeline.h"
#include "pipeline/ObsAudioOutput.h"
#include "pipeline/ObsVideoOutput.h"

#include <QObject>
#include <QTimer>

class AppController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString deviceState READ deviceState NOTIFY deviceStatusChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY deviceStatusChanged)
    Q_PROPERTY(QString signalText READ signalText NOTIFY deviceStatusChanged)
    Q_PROPERTY(QString deviceName READ deviceName NOTIFY deviceStatusChanged)
    Q_PROPERTY(QString statusColor READ statusColor NOTIFY deviceStatusChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(bool recording READ recording NOTIFY recordingChanged)
    Q_PROPERTY(bool capturing READ capturing NOTIFY capturingChanged)
    Q_PROPERTY(QString elapsedText READ elapsedText NOTIFY elapsedChanged)
    Q_PROPERTY(QString diskText READ diskText NOTIFY diskChanged)
    Q_PROPERTY(QString outputDirectory READ outputDirectory CONSTANT)
    Q_PROPERTY(int flashbackMinutes READ flashbackMinutes WRITE setFlashbackMinutes NOTIFY
                   flashbackMinutesChanged)
    Q_PROPERTY(
        QString flashbackBufferedText READ flashbackBufferedText NOTIFY flashbackBufferedChanged)
    Q_PROPERTY(bool flashbackReady READ flashbackReady NOTIFY flashbackBufferedChanged)
    Q_PROPERTY(QString lastSavedText READ lastSavedText NOTIFY lastSavedTextChanged)
    Q_PROPERTY(bool setupRequired READ setupRequired NOTIFY setupChanged)
    Q_PROPERTY(QString setupHeadline READ setupHeadline NOTIFY setupChanged)
    Q_PROPERTY(QStringList setupSteps READ setupSteps NOTIFY setupChanged)
    Q_PROPERTY(QString setupCommand READ setupCommand NOTIFY setupChanged)
    Q_PROPERTY(bool secureBootEnabled READ secureBootEnabled NOTIFY setupChanged)
    Q_PROPERTY(QString audioSummary READ audioSummary NOTIFY capturingChanged)
    Q_PROPERTY(QString audioNotice READ audioNotice NOTIFY capturingChanged)
    Q_PROPERTY(double gameGainDb READ gameGainDb WRITE setGameGainDb NOTIFY audioMixChanged)
    Q_PROPERTY(double micGainDb READ micGainDb WRITE setMicGainDb NOTIFY audioMixChanged)
    Q_PROPERTY(bool gameMuted READ gameMuted WRITE setGameMuted NOTIFY audioMixChanged)
    Q_PROPERTY(bool micMuted READ micMuted WRITE setMicMuted NOTIFY audioMixChanged)
    Q_PROPERTY(bool obsEnabled READ obsEnabled WRITE setObsEnabled NOTIFY obsChanged)
    Q_PROPERTY(QString obsStatus READ obsStatus NOTIFY obsChanged)
    Q_PROPERTY(QString obsNotice READ obsNotice NOTIFY obsChanged)
    Q_PROPERTY(double gameLevel READ gameLevel NOTIFY audioLevelsChanged)
    Q_PROPERTY(double micLevel READ micLevel NOTIFY audioLevelsChanged)

  public:
    explicit AppController(QObject *parent = nullptr);
    ~AppController() override;

    [[nodiscard]] QString deviceState() const;
    [[nodiscard]] QString statusText() const;
    [[nodiscard]] QString signalText() const;
    [[nodiscard]] QString deviceName() const;
    [[nodiscard]] QString statusColor() const;
    [[nodiscard]] QString lastError() const;
    [[nodiscard]] bool recording() const;

    //! True while the capture pipeline is live, which is what decides whether a picture is on
    //! screen.
    [[nodiscard]] bool capturing() const;
    [[nodiscard]] QString elapsedText() const;
    [[nodiscard]] QString diskText() const;
    [[nodiscard]] QString outputDirectory() const;
    [[nodiscard]] int flashbackMinutes() const;
    [[nodiscard]] QString flashbackBufferedText() const;
    [[nodiscard]] bool flashbackReady() const;
    [[nodiscard]] QString lastSavedText() const;
    [[nodiscard]] bool setupRequired() const;
    [[nodiscard]] QString setupHeadline() const;
    [[nodiscard]] QStringList setupSteps() const;
    [[nodiscard]] QString setupCommand() const;
    [[nodiscard]] bool secureBootEnabled() const;

    //! Which audio tracks are being recorded, e.g. "mix + game + mic".
    [[nodiscard]] QString audioSummary() const;

    //! Why a source is missing, so a silent track is never a silent surprise.
    [[nodiscard]] QString audioNotice() const;

    [[nodiscard]] double gameGainDb() const;
    [[nodiscard]] double micGainDb() const;
    [[nodiscard]] bool gameMuted() const;
    [[nodiscard]] bool micMuted() const;

    [[nodiscard]] bool obsEnabled() const;

    //! What OBS is being sent right now, e.g. "video + game + mic".
    [[nodiscard]] QString obsStatus() const;

    //! Why part of the OBS route is not running, so a missing source in OBS has an explanation.
    [[nodiscard]] QString obsNotice() const;

    void setObsEnabled(bool enabled);

    [[nodiscard]] double gameLevel() const;
    [[nodiscard]] double micLevel() const;

    void setGameGainDb(double decibels);
    void setMicGainDb(double decibels);
    void setGameMuted(bool muted);
    void setMicMuted(bool muted);

    Q_INVOKABLE void initialize(QObject *previewItem);
    Q_INVOKABLE void toggleRecording();
    Q_INVOKABLE void refreshDevice();

    //! Writes the buffered tail to the output directory without interrupting capture.
    Q_INVOKABLE void saveFlashback();

    /*!
     * Writes an OBS scene collection wired to quadcap's outputs.
     *
     * Returns a sentence for the UI, because every outcome here is something the person needs to
     * read: it worked, OBS is running and would discard it, or the outputs do not exist yet.
     */
    Q_INVOKABLE QString installObsScene();

    void setFlashbackMinutes(int minutes);

  signals:
    void deviceStatusChanged();
    void lastErrorChanged();
    void recordingChanged();
    void capturingChanged();
    void elapsedChanged();
    void diskChanged();
    void flashbackMinutesChanged();
    void flashbackBufferedChanged();
    void lastSavedTextChanged();
    void setupChanged();
    void audioMixChanged();
    void audioLevelsChanged();
    void obsChanged();

  private:
    void applyStatus(const quadcap::device::DeviceStatus &status);

    /*!
     * Applies the configured EDID source if the card is not already on it.
     *
     * Returns true when it actually changed, which means HPD was bounced and the console is
     * renegotiating — the caller has to let the signal settle before building a pipeline on it.
     */
    [[nodiscard]] bool applyEdidSource();
    void startPipeline();
    void applyAudioMix();

    /*!
     * Brings the loopback writer up or down to match the switch.
     *
     * Kept out of startPipeline because it must not follow the signal: the whole point is that the
     * camera stays in OBS while the console is asleep.
     */
    void applyObsOutput();
    void noteAudioLevel(const QString &source, double rmsDb, double peakDb);
    void setError(const QString &error);
    void updateElapsed();
    void updateDisk();
    [[nodiscard]] QString newRecordingPath() const;

    quadcap::device::CaptureDevice device_;
    quadcap::pipeline::CapturePipeline pipeline_;
    //! Holds the loopback node open for as long as OBS output is on, across pipeline rebuilds.
    quadcap::pipeline::ObsVideoOutput obsOutput_;
    //! One per source, so OBS receives game and microphone as inputs it can treat separately.
    quadcap::pipeline::ObsAudioOutput obsGameOutput_;
    quadcap::pipeline::ObsAudioOutput obsMicOutput_;
    quadcap::flashback::FlashbackRing flashback_;
    quadcap::device::DeviceStatus status_;
    quadcap::device::SetupStatus setup_;
    QString lastError_;
    QString lastSavedText_;
    QString outputDirectory_;
    QTimer elapsedTimer_;
    QTimer diskTimer_;
    QTimer bufferTimer_;
    QTimer meterTimer_;
    double gameGainDb_ = 0.0;
    double micGainDb_ = 0.0;
    bool obsEnabled_ = false;
    bool gameMuted_ = false;
    bool micMuted_ = false;
    double gameLevel_ = 0.0;
    double micLevel_ = 0.0;
    qint64 recordingStartedMs_ = 0;
    int flashbackMinutes_ = 10;
    quadcap::device::EdidSource edidSource_ = quadcap::device::EdidSource::Internal;
    //! Suppresses the lock-triggered auto-start while refreshDevice is still setting things up.
    bool suspendAutoStart_ = false;
    bool initialized_ = false;
};
