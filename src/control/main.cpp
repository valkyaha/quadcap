#include "device/CaptureDevice.h"
#include "device/DeviceDiscovery.h"
#include "device/DeviceTypes.h"
#include "device/SystemCheck.h"
#include "obs/ObsScene.h"
#include "pipeline/CapturePipeline.h"
#include "pipeline/ObsVideoOutput.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>

using quadcap::device::CaptureDevice;
using quadcap::device::DeviceState;
using quadcap::device::DeviceStatus;

namespace {

QJsonObject statusJson(const DeviceStatus &status) {
    QJsonObject result{
        {QStringLiteral("state"), quadcap::device::stateName(status.state)},
        {QStringLiteral("device"), status.deviceNode},
        {QStringLiteral("pciAddress"), status.pciAddress},
        {QStringLiteral("card"), status.cardName},
    };
    if (status.mode.isValid()) {
        result.insert(QStringLiteral("mode"),
                      QJsonObject{
                          {QStringLiteral("width"), static_cast<qint64>(status.mode.width)},
                          {QStringLiteral("height"), static_cast<qint64>(status.mode.height)},
                          {QStringLiteral("fps"), status.mode.framesPerSecond()},
                          {QStringLiteral("interlaced"), status.mode.interlaced},
                      });
    }
    if (!status.error.isEmpty()) {
        result.insert(QStringLiteral("error"), status.error);
    }
    return result;
}

void printStatus(const DeviceStatus &status, bool json) {
    QTextStream out(stdout);
    if (json) {
        out << QJsonDocument(statusJson(status)).toJson(QJsonDocument::Compact) << Qt::endl;
        return;
    }
    out << "state: " << quadcap::device::stateName(status.state) << '\n';
    if (!status.cardName.isEmpty()) {
        out << "card: " << status.cardName << '\n';
    }
    if (!status.pciAddress.isEmpty()) {
        out << "pci: " << status.pciAddress << '\n';
    }
    if (!status.deviceNode.isEmpty()) {
        out << "device: " << status.deviceNode << '\n';
    }
    if (status.mode.isValid()) {
        out << "signal: " << status.mode.toString() << '\n';
    }
    if (!status.error.isEmpty()) {
        out << "error: " << status.error << '\n';
    }
    out.flush();
}

/*!
 * Records for a fixed number of seconds with no window and no preview.
 *
 * This is the headless counterpart to pressing Record in the UI: same pipeline, same encoder, same
 * ring, so it can verify capture over ssh or in a soak run without a display attached.
 */
int recordForSeconds(const DeviceStatus &status, const QString &path, const int seconds,
                     const int width, const int height, const int frameRate,
                     const bool hardwareEncoder, const bool obsOutput) {
    QTextStream out(stdout);
    QTextStream err(stderr);

    if (status.state != DeviceState::Locked) {
        err << "no locked signal: " << quadcap::device::stateName(status.state) << Qt::endl;
        return 1;
    }

    quadcap::pipeline::CapturePipeline pipeline;
    quadcap::pipeline::PipelineConfig config;
    config.devicePath = status.deviceNode;
    config.ringDirectory = QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
                               .filePath(QStringLiteral("ring-headless"));
    config.width = width;
    config.height = height;
    config.frameRate = frameRate;
    config.hardwareEncoder = hardwareEncoder;
    config.enablePreview = false;
    config.gameAudioDevice = quadcap::device::DeviceDiscovery::alsaDeviceForPci(status.pciAddress);
    quadcap::pipeline::ObsVideoOutput obsVideo;
    config.enableObsOutput = obsOutput;
    if (obsOutput) {
        const auto node = quadcap::device::DeviceDiscovery::obsLoopbackDevice();
        QString obsError;
        if (!obsVideo.start(node, 1920, 1080, 60, &obsError)) {
            err << "OBS video output unavailable: " << obsError << Qt::endl;
        }
        config.obsOutput = &obsVideo;
        config.obsGameSink = QStringLiteral("quadcap-game");
        config.obsMicSink = QStringLiteral("quadcap-mic");
    }

    QString failure;
    bool failed = false;
    QObject::connect(&pipeline, &quadcap::pipeline::CapturePipeline::errorOccurred,
                     [&failure, &failed](const QString &message) {
                         if (!failed) {
                             failure = message;
                             failed = true;
                         }
                     });

    QString error;
    if (!pipeline.start(config, &error)) {
        err << error << Qt::endl;
        return 2;
    }
    if (!pipeline.startRecording(path, &error)) {
        err << error << Qt::endl;
        pipeline.stop();
        return 2;
    }

    out << "recording " << status.mode.toString() << " -> " << width << "x" << height << "@"
        << frameRate << " for " << seconds << "s" << Qt::endl;

    QEventLoop loop;
    QElapsedTimer elapsed;
    elapsed.start();
    QTimer ticker;
    ticker.setInterval(1000);
    QObject::connect(&ticker, &QTimer::timeout, [&] {
        const auto done = static_cast<int>(elapsed.elapsed() / 1000);
        out << "\r  " << done << "/" << seconds << "s  " << (QFileInfo(path).size() / (1024 * 1024))
            << " MB" << Qt::flush;
        if (failed || done >= seconds) {
            loop.quit();
        }
    });
    ticker.start();
    loop.exec();
    out << Qt::endl;

    pipeline.stopRecording();
    // Let the muxer finish writing the header before the pipeline goes away.
    QTimer::singleShot(1500, &loop, &QEventLoop::quit);
    loop.exec();
    pipeline.stop();

    if (failed) {
        err << failure << Qt::endl;
        return 3;
    }

    const QFileInfo written(path);
    if (!written.exists() || written.size() == 0) {
        err << "no data was written to " << path << Qt::endl;
        return 3;
    }
    out << "wrote " << written.size() << " bytes to " << written.absoluteFilePath() << Qt::endl;
    return 0;
}

} // namespace

/*!
 * quadcapd is the headless half of quadcap: everything the window can tell you about the card,
 * available over SSH and usable from a script.
 *
 * The options fall into four groups. --status, --watch, and --json report what the card sees, and
 * are the ones worth reaching for when a console shows no picture. --get-edid, --set-edid, and
 * --edid-source control what the card advertises to the console, which is how a source is talked
 * out of a mode the display chain cannot carry. --setup reports what is stopping capture on this
 * machine, a driver that is absent, unloaded, or unsigned. --record captures to a file, shaped by
 * --seconds, --width, --height, --fps, and --software.
 *
 * Each group returns as soon as it has done its work, so the ordering below is what decides which
 * option wins when several are passed together.
 */
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("quadcapd"));
    QCoreApplication::setApplicationVersion(QStringLiteral(QUADCAP_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Inspect and monitor a quadcap capture device"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({{QStringLiteral("s"), QStringLiteral("status")},
                      QStringLiteral("Print device status and exit")});
    parser.addOption({{QStringLiteral("w"), QStringLiteral("watch")},
                      QStringLiteral("Print status and monitor source changes")});
    parser.addOption({QStringLiteral("json"), QStringLiteral("Emit newline-delimited JSON")});
    parser.addOption({QStringLiteral("get-edid"),
                      QStringLiteral("Write the current EDID to a file"), QStringLiteral("file")});
    parser.addOption({QStringLiteral("set-edid"), QStringLiteral("Apply an EDID from a file"),
                      QStringLiteral("file")});
    parser.addOption({QStringLiteral("setup"),
                      QStringLiteral("Report what is blocking capture on this machine")});
    parser.addOption({QStringLiteral("edid-source"),
                      QStringLiteral("Advertise internal, display, or merged EDID to the source"),
                      QStringLiteral("mode")});
    parser.addOption({QStringLiteral("record"), QStringLiteral("Record headlessly to a file"),
                      QStringLiteral("file")});
    parser.addOption({QStringLiteral("seconds"), QStringLiteral("Recording length (default 15)"),
                      QStringLiteral("n"), QStringLiteral("15")});
    parser.addOption({QStringLiteral("width"),
                      QStringLiteral("Canonical output width (default 3840)"), QStringLiteral("px"),
                      QStringLiteral("3840")});
    parser.addOption({QStringLiteral("height"),
                      QStringLiteral("Canonical output height (default 2160)"),
                      QStringLiteral("px"), QStringLiteral("2160")});
    parser.addOption({QStringLiteral("fps"),
                      QStringLiteral("Canonical output frame rate (default 60)"),
                      QStringLiteral("n"), QStringLiteral("60")});
    parser.addOption(
        {QStringLiteral("software"), QStringLiteral("Use the software encoder and CPU scaling")});
    parser.addOption(
        {QStringLiteral("obs"), QStringLiteral("Also send video and each audio source to OBS")});
    parser.addOption({QStringLiteral("write-obs-scene"),
                      QStringLiteral("Write an OBS scene collection wired to quadcap's outputs"),
                      QStringLiteral("file"), quadcap::obs::ObsScene::defaultCollectionPath()});
    parser.process(app);

    // Probed once up front: every branch below needs to know whether a card is present, and each
    // probe walks sysfs and opens the V4L2 node.
    CaptureDevice device;
    const auto initial = device.probe();
    const bool json = parser.isSet(QStringLiteral("json"));

    if (parser.isSet(QStringLiteral("get-edid"))) {
        QString error;
        const auto bytes = device.readEdid(&error);
        QFile output(parser.value(QStringLiteral("get-edid")));
        if (bytes.isEmpty() || !output.open(QIODevice::WriteOnly) ||
            output.write(bytes) != bytes.size()) {
            QTextStream(stderr) << (error.isEmpty() ? output.errorString() : error) << Qt::endl;
            return 2;
        }
        return 0;
    }

    if (parser.isSet(QStringLiteral("set-edid"))) {
        QFile input(parser.value(QStringLiteral("set-edid")));
        if (!input.open(QIODevice::ReadOnly)) {
            QTextStream(stderr) << input.errorString() << Qt::endl;
            return 2;
        }
        QString error;
        if (!device.writeEdid(input.readAll(), &error)) {
            QTextStream(stderr) << error << Qt::endl;
            return 2;
        }
        return 0;
    }

    if (parser.isSet(QStringLiteral("write-obs-scene"))) {
        QTextStream out(stdout);
        QTextStream err(stderr);
        if (quadcap::obs::ObsScene::obsIsRunning()) {
            err << "OBS is running; it would overwrite this on exit. Close it first." << Qt::endl;
            return 2;
        }
        quadcap::obs::SceneSources sources;
        sources.videoDevice = quadcap::device::DeviceDiscovery::obsLoopbackDevice();
        if (quadcap::pipeline::CapturePipeline::audioSinkExists(QStringLiteral("quadcap-game"))) {
            sources.gameSink = QStringLiteral("quadcap-game");
        }
        if (quadcap::pipeline::CapturePipeline::audioSinkExists(QStringLiteral("quadcap-mic"))) {
            sources.micSink = QStringLiteral("quadcap-mic");
        }
        const auto path = parser.value(QStringLiteral("write-obs-scene"));
        QString failure;
        if (!quadcap::obs::ObsScene::write(sources, path, &failure)) {
            err << failure << Qt::endl;
            return 2;
        }
        out << "wrote " << path << Qt::endl;
        return 0;
    }

    if (parser.isSet(QStringLiteral("setup"))) {
        const auto setup = quadcap::device::inspectSystem();
        QTextStream out(stdout);
        out << "issue: " << quadcap::device::setupIssueName(setup.issue) << '\n'
            << "card: " << (setup.cardPresent ? "present" : "absent") << '\n'
            << "module: " << (setup.moduleLoaded ? "loaded" : "not loaded") << '\n'
            << "dkms: " << (setup.dkmsRegistered ? "registered" : "not registered") << '\n'
            << "secure boot: " << (setup.secureBootEnabled ? "enabled" : "disabled") << '\n';
        if (!setup.ready()) {
            out << '\n' << setup.headline << '\n';
            for (int i = 0; i < setup.steps.size(); ++i) {
                out << "  " << (i + 1) << ". " << setup.steps.at(i) << '\n';
            }
            if (!setup.command.isEmpty()) {
                out << "\n  $ " << setup.command << '\n';
            }
        }
        out.flush();
        return setup.ready() ? 0 : 1;
    }

    if (parser.isSet(QStringLiteral("edid-source"))) {
        const auto requested = parser.value(QStringLiteral("edid-source")).trimmed().toLower();
        static const QMap<QString, quadcap::device::EdidSource> modes{
            {QStringLiteral("internal"), quadcap::device::EdidSource::Internal},
            {QStringLiteral("display"), quadcap::device::EdidSource::Display},
            {QStringLiteral("merged"), quadcap::device::EdidSource::Merged},
        };
        if (!modes.contains(requested)) {
            QTextStream(stderr) << "edid-source must be internal, display, or merged" << Qt::endl;
            return 2;
        }
        QString error;
        if (!device.setEdidSource(modes.value(requested), &error)) {
            QTextStream(stderr) << error << Qt::endl;
            return 2;
        }
        // The source renegotiates after the HPD bounce, so the old timings linger for a moment.
        QTextStream(stdout) << "EDID source set to " << requested << Qt::endl;
        return 0;
    }

    if (parser.isSet(QStringLiteral("record"))) {
        printStatus(initial, json);
        return recordForSeconds(initial, parser.value(QStringLiteral("record")),
                                parser.value(QStringLiteral("seconds")).toInt(),
                                parser.value(QStringLiteral("width")).toInt(),
                                parser.value(QStringLiteral("height")).toInt(),
                                parser.value(QStringLiteral("fps")).toInt(),
                                !parser.isSet(QStringLiteral("software")),
                                parser.isSet(QStringLiteral("obs")));
    }

    printStatus(initial, json);
    if (!parser.isSet(QStringLiteral("watch"))) {
        return initial.state == DeviceState::Locked ? 0 : 1;
    }

    QString error;
    if (!device.startMonitoring(&error)) {
        QTextStream(stderr) << error << Qt::endl;
        return 2;
    }
    QObject::connect(&device, &CaptureDevice::statusChanged, &app,
                     [json](const DeviceStatus &status) { printStatus(status, json); });
    return app.exec();
}
