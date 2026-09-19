#include "ui/AppController.h"

#include "device/DeviceDiscovery.h"
#include "device/SystemCheck.h"
#include "device/DeviceTypes.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QStorageInfo>

AppController::AppController(QObject *parent)
    : QObject(parent)
{
    outputDirectory_ = QDir(QStandardPaths::writableLocation(QStandardPaths::MoviesLocation))
                           .filePath(QStringLiteral("quadcap"));
    QDir().mkpath(outputDirectory_);

    QSettings settings;
    flashbackMinutes_ = qBound(1, settings.value(QStringLiteral("flashbackMinutes"), 10).toInt(), 30);
    /*
     * Internal by default, which is also the driver's own default, so nothing is written and no HPD
     * bounce happens at startup.
     *
     * The control is kept because other boards may honour it, but on the 4K60 Pro MK.2 it was
     * measured to do nothing at all: internal, display and merged each leave the card presenting a
     * byte-identical EDID naming itself "4K60ProMK2VRR" and advertising 2160p60 with PQ. A console
     * therefore picks 4K60 HDR no matter what this is set to, and a passthrough display that cannot
     * sync to that stays black. Constraining the console means writing a narrower EDID to the card
     * (see packaging/edid/) or changing the console's own output settings.
     */
    edidSource_ = static_cast<quadcap::device::EdidSource>(
        qBound(0, settings.value(QStringLiteral("edidSource"),
                          static_cast<int>(quadcap::device::EdidSource::Internal))
                      .toInt(),
            2));

    gameGainDb_ =
        qBound(-40.0, settings.value(QStringLiteral("audio/gameGainDb"), 0.0).toDouble(), 12.0);
    micGainDb_ =
        qBound(-40.0, settings.value(QStringLiteral("audio/micGainDb"), 0.0).toDouble(), 12.0);
    gameMuted_ = settings.value(QStringLiteral("audio/gameMuted"), false).toBool();
    micMuted_ = settings.value(QStringLiteral("audio/micMuted"), false).toBool();

    connect(&pipeline_, &quadcap::pipeline::CapturePipeline::audioLevel,
        this, &AppController::noteAudioLevel);
    meterTimer_.setInterval(60);
    connect(&meterTimer_, &QTimer::timeout, this, [this] {
        gameLevel_ = qMax(0.0, gameLevel_ - 0.06);
        micLevel_ = qMax(0.0, micLevel_ - 0.06);
        emit audioLevelsChanged();
    });
    meterTimer_.start();

    elapsedTimer_.setInterval(250);
    connect(&elapsedTimer_, &QTimer::timeout, this, &AppController::updateElapsed);
    diskTimer_.setInterval(5000);
    connect(&diskTimer_, &QTimer::timeout, this, &AppController::updateDisk);
    bufferTimer_.setInterval(1000);
    connect(&bufferTimer_, &QTimer::timeout, this, &AppController::flashbackBufferedChanged);

    // splitmuxsink's fragment-closed message is the only reliable "safe to read" signal.
    connect(&pipeline_, &quadcap::pipeline::CapturePipeline::segmentClosed,
        &flashback_, &quadcap::flashback::FlashbackRing::noteSegmentClosed);

    connect(&device_, &quadcap::device::CaptureDevice::statusChanged,
        this, &AppController::applyStatus);
    connect(&pipeline_, &quadcap::pipeline::CapturePipeline::errorOccurred,
        this, &AppController::setError);
    connect(&pipeline_, &quadcap::pipeline::CapturePipeline::warningOccurred,
        this, [this](const QString &warning) { setError(QStringLiteral("Pipeline warning: %1").arg(warning)); });
    connect(&pipeline_, &quadcap::pipeline::CapturePipeline::recordingChanged,
        this, [this](bool active, const QString &) {
            if (active) {
                recordingStartedMs_ = QDateTime::currentMSecsSinceEpoch();
                elapsedTimer_.start();
            } else {
                recordingStartedMs_ = 0;
                elapsedTimer_.stop();
            }
            emit recordingChanged();
            emit elapsedChanged();
        });
}

AppController::~AppController() = default;

QString AppController::deviceState() const
{
    return quadcap::device::stateName(status_.state);
}

QString AppController::statusText() const
{
    using quadcap::device::DeviceState;
    switch (status_.state) {
    case DeviceState::NoCard:
        return QStringLiteral("Capture card not found");
    case DeviceState::NoDriver:
        return QStringLiteral("Driver unavailable");
    case DeviceState::NoSignal:
        return QStringLiteral("Waiting for HDMI signal");
    case DeviceState::Locked:
        return QStringLiteral("Signal locked");
    case DeviceState::Error:
        return QStringLiteral("Device error");
    }
    return QStringLiteral("Device error");
}

QString AppController::signalText() const
{
    return status_.mode.isValid() ? status_.mode.toString() : QStringLiteral("No active mode");
}

QString AppController::deviceName() const
{
    return status_.cardName.isEmpty() ? QStringLiteral("4K capture") : status_.cardName;
}

QString AppController::statusColor() const
{
    using quadcap::device::DeviceState;
    switch (status_.state) {
    case DeviceState::Locked:
        return QStringLiteral("#49d17d");
    case DeviceState::NoSignal:
        return QStringLiteral("#f2b84b");
    case DeviceState::NoCard:
    case DeviceState::NoDriver:
    case DeviceState::Error:
        return QStringLiteral("#f06464");
    }
    return QStringLiteral("#f06464");
}

QString AppController::lastError() const
{
    return lastError_;
}

bool AppController::recording() const
{
    return pipeline_.isRecording();
}

bool AppController::capturing() const
{
    return pipeline_.isRunning();
}

QString AppController::elapsedText() const
{
    if (recordingStartedMs_ == 0) {
        return QStringLiteral("00:00:00");
    }
    const auto seconds = (QDateTime::currentMSecsSinceEpoch() - recordingStartedMs_) / 1000;
    return QStringLiteral("%1:%2:%3")
        .arg(seconds / 3600, 2, 10, QLatin1Char('0'))
        .arg((seconds / 60) % 60, 2, 10, QLatin1Char('0'))
        .arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

QString AppController::diskText() const
{
    const QStorageInfo storage(outputDirectory_);
    const auto gib = static_cast<double>(storage.bytesAvailable()) / (1024.0 * 1024.0 * 1024.0);
    return QStringLiteral("%1 GB free").arg(gib, 0, 'f', 1);
}

QString AppController::outputDirectory() const
{
    return outputDirectory_;
}

int AppController::flashbackMinutes() const
{
    return flashbackMinutes_;
}

QString AppController::flashbackBufferedText() const
{
    const auto seconds = flashback_.availableSeconds();
    return QStringLiteral("%1:%2 buffered")
        .arg(seconds / 60, 2, 10, QLatin1Char('0'))
        .arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

bool AppController::flashbackReady() const
{
    return flashback_.availableSeconds() > 0;
}

QString AppController::lastSavedText() const
{
    return lastSavedText_;
}

bool AppController::setupRequired() const
{
    return !setup_.ready();
}

QString AppController::setupHeadline() const
{
    return setup_.headline;
}

QStringList AppController::setupSteps() const
{
    return setup_.steps;
}

QString AppController::setupCommand() const
{
    return setup_.command;
}

bool AppController::secureBootEnabled() const
{
    return setup_.secureBootEnabled;
}

QString AppController::audioSummary() const
{
    const auto tracks = pipeline_.audioTrackNames();
    if (tracks.isEmpty()) {
        return QStringLiteral("No audio");
    }
    return tracks.join(QStringLiteral(" + "));
}

QString AppController::audioNotice() const
{
    return pipeline_.audioNotice();
}

double AppController::gameGainDb() const { return gameGainDb_; }
double AppController::micGainDb() const { return micGainDb_; }
bool AppController::gameMuted() const { return gameMuted_; }
bool AppController::micMuted() const { return micMuted_; }
double AppController::gameLevel() const { return gameLevel_; }
double AppController::micLevel() const { return micLevel_; }

void AppController::setGameGainDb(const double decibels)
{
    const auto bounded = qBound(-40.0, decibels, 12.0);
    if (qFuzzyCompare(bounded, gameGainDb_)) {
        return;
    }
    gameGainDb_ = bounded;
    QSettings().setValue(QStringLiteral("audio/gameGainDb"), bounded);
    pipeline_.setAudioGainDb(QStringLiteral("game"), bounded);
    emit audioMixChanged();
}

void AppController::setMicGainDb(const double decibels)
{
    const auto bounded = qBound(-40.0, decibels, 12.0);
    if (qFuzzyCompare(bounded, micGainDb_)) {
        return;
    }
    micGainDb_ = bounded;
    QSettings().setValue(QStringLiteral("audio/micGainDb"), bounded);
    pipeline_.setAudioGainDb(QStringLiteral("mic"), bounded);
    emit audioMixChanged();
}

void AppController::setGameMuted(const bool muted)
{
    if (muted == gameMuted_) {
        return;
    }
    gameMuted_ = muted;
    QSettings().setValue(QStringLiteral("audio/gameMuted"), muted);
    pipeline_.setAudioMuted(QStringLiteral("game"), muted);
    emit audioMixChanged();
}

void AppController::setMicMuted(const bool muted)
{
    if (muted == micMuted_) {
        return;
    }
    micMuted_ = muted;
    QSettings().setValue(QStringLiteral("audio/micMuted"), muted);
    pipeline_.setAudioMuted(QStringLiteral("mic"), muted);
    emit audioMixChanged();
}

void AppController::applyAudioMix()
{
    pipeline_.setAudioGainDb(QStringLiteral("game"), gameGainDb_);
    pipeline_.setAudioGainDb(QStringLiteral("mic"), micGainDb_);
    pipeline_.setAudioMuted(QStringLiteral("game"), gameMuted_);
    pipeline_.setAudioMuted(QStringLiteral("mic"), micMuted_);
}

void AppController::noteAudioLevel(const QString &source, const double rmsDb, double)
{
    const double position = qBound(0.0, (rmsDb + 60.0) / 60.0, 1.0);
    if (source == QLatin1String("game")) {
        gameLevel_ = qMax(gameLevel_, position);
    } else if (source == QLatin1String("mic")) {
        micLevel_ = qMax(micLevel_, position);
    }
}

void AppController::initialize(QObject *previewItem)
{
    if (initialized_) {
        return;
    }
    initialized_ = true;
    pipeline_.setPreviewItem(previewItem);
    refreshDevice();
    updateDisk();
    diskTimer_.start();
    bufferTimer_.start();
}

void AppController::saveFlashback()
{
    const auto stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss"));
    const auto path = QDir(outputDirectory_)
                          .filePath(QStringLiteral("flashback_%1.mkv").arg(stamp));

    int seconds = 0;
    QString error;
    if (!flashback_.save(flashbackMinutes_, path, &seconds, &error)) {
        setError(error);
        return;
    }

    lastSavedText_ = QStringLiteral("Saved %1:%2 to %3")
                         .arg(seconds / 60, 2, 10, QLatin1Char('0'))
                         .arg(seconds % 60, 2, 10, QLatin1Char('0'))
                         .arg(QFileInfo(path).fileName());
    emit lastSavedTextChanged();
}

void AppController::toggleRecording()
{
    if (!pipeline_.isRunning()) {
        setError(QStringLiteral("Capture pipeline is not running"));
        return;
    }
    if (pipeline_.isRecording()) {
        pipeline_.stopRecording();
        return;
    }
    QString error;
    if (!pipeline_.startRecording(newRecordingPath(), &error)) {
        setError(error);
    }
}

void AppController::refreshDevice()
{
    using quadcap::device::DeviceState;

    pipeline_.stop();
    emit capturingChanged();

    // Cheap sysfs reads; re-run on every refresh so the guide reflects what the machine looks like
    // right now, including a driver that was installed while the app was open.
    setup_ = quadcap::device::inspectSystem();
    emit setupChanged();

    // The probe result must not auto-start anything: the EDID source has not been applied yet.
    suspendAutoStart_ = true;
    applyStatus(device_.probe());
    suspendAutoStart_ = false;

    if (status_.state == DeviceState::NoCard || status_.state == DeviceState::NoDriver
        || status_.state == DeviceState::Error) {
        if (!status_.error.isEmpty()) {
            setError(status_.error);
        }
        return;
    }

    // No signal is not a failure — monitoring below picks it up when the console comes on.
    const bool bounced = status_.state == DeviceState::Locked && applyEdidSource();

    QString monitorError;
    if (!device_.startMonitoring(&monitorError)) {
        setError(monitorError);
    }

    /*
     * Changing the EDID bounces HPD, so the console drops its output and renegotiates — how long
     * that takes is the console's business, not ours, and a fixed delay guesses wrong. The
     * source-change event starts the pipeline the moment the signal is back. The same path covers
     * the console being switched on after the app has already started.
     */
    if (!bounced && status_.state == DeviceState::Locked) {
        startPipeline();
    }
}

bool AppController::applyEdidSource()
{
    auto current = quadcap::device::EdidSource::Internal;
    QString error;
    if (!device_.edidSource(&current, &error)) {
        return false; // Driver without the control; nothing to do and nothing worth reporting.
    }
    if (current == edidSource_) {
        return false;
    }
    /*
     * Only ever correct the driver's own default. sc0710 forces Internal at every probe, and
     * Internal is what makes a console advertise modes the passthrough display cannot show. But if
     * the card is on anything else, that is a deliberate choice someone made — overriding it would
     * bounce HPD and drop a working picture every time the app starts.
     */
    if (current != quadcap::device::EdidSource::Internal) {
        return false;
    }
    if (!device_.setEdidSource(edidSource_, &error)) {
        setError(error);
        return false;
    }
    return true;
}

void AppController::startPipeline()
{
    quadcap::pipeline::PipelineConfig config;
    config.devicePath = status_.deviceNode;
    config.ringDirectory = QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
                               .filePath(QStringLiteral("ring"));
    config.ringMinutes = flashbackMinutes_;
    config.enablePreview = true;
    config.enableAudioMonitoring = true;

    /*
     * Size the canonical output from the signal we are locked to, capped at 4K.
     *
     * It stays fixed for the life of the pipeline, so a mid-recording mode change still scales into
     * one continuous file — but starting a 1080p console at 3840x2160 would upscale every frame and
     * spend 4x the bitrate on detail that does not exist.
     */
    if (status_.mode.isValid()) {
        config.width = qBound(640, static_cast<int>(status_.mode.width), 3840);
        config.height = qBound(360, static_cast<int>(status_.mode.height), 2160);
    }
    if (status_.mode.hasFrameRate()) {
        config.frameRate = qBound(24, qRound(status_.mode.framesPerSecond()), 240);
    }

    // Found through sysfs by PCI address: the ALSA card index moves when USB audio comes and goes.
    config.gameAudioDevice =
        quadcap::device::DeviceDiscovery::alsaDeviceForPci(status_.pciAddress);

    // The pipeline starts a fresh ring, so anything recorded about the previous one is stale.
    flashback_.reset();
    flashback_.configure(config.ringDirectory, config.segmentSeconds, config.hardwareEncoder);
    emit flashbackBufferedChanged();

    QString pipelineError;
    if (!pipeline_.start(config, &pipelineError)) {
        setError(pipelineError);
    }
    applyAudioMix();
    emit capturingChanged();
}

void AppController::setFlashbackMinutes(const int minutes)
{
    const auto bounded = qBound(1, minutes, 30);
    if (bounded == flashbackMinutes_) {
        return;
    }
    flashbackMinutes_ = bounded;
    QSettings().setValue(QStringLiteral("flashbackMinutes"), bounded);
    emit flashbackMinutesChanged();
}

void AppController::applyStatus(const quadcap::device::DeviceStatus &status)
{
    status_ = status;
    emit deviceStatusChanged();

    // The signal locking is the cue to start capturing, wherever it came from: an EDID bounce
    // settling, the console being switched on, or a resolution change on the console's side.
    if (initialized_ && !suspendAutoStart_ && !pipeline_.isRunning()
        && status_.state == quadcap::device::DeviceState::Locked) {
        startPipeline();
    }
}

void AppController::setError(const QString &error)
{
    if (error == lastError_) {
        return;
    }
    lastError_ = error;
    // The banner is easy to miss and invisible when launched from a terminal.
    if (!error.isEmpty()) {
        qWarning("quadcap: %s", qPrintable(error));
    }
    emit lastErrorChanged();
}

void AppController::updateElapsed()
{
    emit elapsedChanged();
}

void AppController::updateDisk()
{
    emit diskChanged();
}

QString AppController::newRecordingPath() const
{
    const auto stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss"));
    return QDir(outputDirectory_).filePath(QStringLiteral("quadcap_%1.mkv").arg(stamp));
}
