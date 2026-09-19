#pragma once

#include "device/CaptureDevice.h"
#include "device/SystemCheck.h"
#include "flashback/FlashbackRing.h"
#include "pipeline/CapturePipeline.h"

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
    Q_PROPERTY(int flashbackMinutes READ flashbackMinutes WRITE setFlashbackMinutes NOTIFY flashbackMinutesChanged)
    Q_PROPERTY(QString flashbackBufferedText READ flashbackBufferedText NOTIFY flashbackBufferedChanged)
    Q_PROPERTY(bool flashbackReady READ flashbackReady NOTIFY flashbackBufferedChanged)
    Q_PROPERTY(QString lastSavedText READ lastSavedText NOTIFY lastSavedTextChanged)
    Q_PROPERTY(bool setupRequired READ setupRequired NOTIFY setupChanged)
    Q_PROPERTY(QString setupHeadline READ setupHeadline NOTIFY setupChanged)
    Q_PROPERTY(QStringList setupSteps READ setupSteps NOTIFY setupChanged)
    Q_PROPERTY(QString setupCommand READ setupCommand NOTIFY setupChanged)
    Q_PROPERTY(bool secureBootEnabled READ secureBootEnabled NOTIFY setupChanged)
    Q_PROPERTY(QString audioSummary READ audioSummary NOTIFY capturingChanged)
    Q_PROPERTY(QString audioNotice READ audioNotice NOTIFY capturingChanged)

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

    //! True while the capture pipeline is live, which is what decides whether a picture is on screen.
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

    Q_INVOKABLE void initialize(QObject *previewItem);
    Q_INVOKABLE void toggleRecording();
    Q_INVOKABLE void refreshDevice();

    //! Writes the buffered tail to the output directory without interrupting capture.
    Q_INVOKABLE void saveFlashback();

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
    void setError(const QString &error);
    void updateElapsed();
    void updateDisk();
    [[nodiscard]] QString newRecordingPath() const;

    quadcap::device::CaptureDevice device_;
    quadcap::pipeline::CapturePipeline pipeline_;
    quadcap::flashback::FlashbackRing flashback_;
    quadcap::device::DeviceStatus status_;
    quadcap::device::SetupStatus setup_;
    QString lastError_;
    QString lastSavedText_;
    QString outputDirectory_;
    QTimer elapsedTimer_;
    QTimer diskTimer_;
    QTimer bufferTimer_;
    qint64 recordingStartedMs_ = 0;
    int flashbackMinutes_ = 10;
    quadcap::device::EdidSource edidSource_ = quadcap::device::EdidSource::Internal;
    //! Suppresses the lock-triggered auto-start while refreshDevice is still setting things up.
    bool suspendAutoStart_ = false;
    bool initialized_ = false;
};

