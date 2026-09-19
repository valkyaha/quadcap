#include "obs/ObsScene.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

namespace quadcap::obs {
namespace {

QString newUuid() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

/*
 * Fields every source carries. OBS fills in what is missing, but an incomplete source is a source
 * whose defaults depend on the version reading it, and a scene collection is meant to survive
 * being opened by a newer OBS than the one it was written for.
 */
QJsonObject sourceSkeleton(const QString &name, const QString &id, const QString &uuid,
                           const QJsonObject &settings, const int mixers) {
    return QJsonObject{
        {QStringLiteral("name"), name},
        {QStringLiteral("uuid"), uuid},
        {QStringLiteral("id"), id},
        {QStringLiteral("versioned_id"), id},
        {QStringLiteral("settings"), settings},
        {QStringLiteral("mixers"), mixers},
        {QStringLiteral("sync"), 0},
        {QStringLiteral("flags"), 0},
        {QStringLiteral("volume"), 1.0},
        {QStringLiteral("balance"), 0.5},
        {QStringLiteral("enabled"), true},
        {QStringLiteral("muted"), false},
        {QStringLiteral("push-to-mute"), false},
        {QStringLiteral("push-to-mute-delay"), 0},
        {QStringLiteral("push-to-talk"), false},
        {QStringLiteral("push-to-talk-delay"), 0},
        {QStringLiteral("hotkeys"), QJsonObject{}},
        {QStringLiteral("deinterlace_mode"), 0},
        {QStringLiteral("deinterlace_field_order"), 0},
        {QStringLiteral("monitoring_type"), 0},
        {QStringLiteral("private_settings"), QJsonObject{}},
    };
}

QJsonObject sceneItem(const QString &name, const QString &uuid, const int id) {
    return QJsonObject{
        {QStringLiteral("name"), name},
        {QStringLiteral("source_uuid"), uuid},
        {QStringLiteral("visible"), true},
        {QStringLiteral("locked"), false},
        {QStringLiteral("rot"), 0.0},
        // Top-left, which is what makes a full-frame source line up with the canvas origin.
        {QStringLiteral("align"), 5},
        {QStringLiteral("bounds_type"), 0},
        {QStringLiteral("bounds_align"), 0},
        {QStringLiteral("bounds_crop"), false},
        {QStringLiteral("crop_left"), 0},
        {QStringLiteral("crop_top"), 0},
        {QStringLiteral("crop_right"), 0},
        {QStringLiteral("crop_bottom"), 0},
        {QStringLiteral("id"), id},
        {QStringLiteral("group_item_backup"), false},
        {QStringLiteral("pos"),
         QJsonObject{{QStringLiteral("x"), 0.0}, {QStringLiteral("y"), 0.0}}},
        {QStringLiteral("scale"),
         QJsonObject{{QStringLiteral("x"), 1.0}, {QStringLiteral("y"), 1.0}}},
        {QStringLiteral("bounds"),
         QJsonObject{{QStringLiteral("x"), 0.0}, {QStringLiteral("y"), 0.0}}},
        {QStringLiteral("scale_filter"), QStringLiteral("disable")},
        {QStringLiteral("blend_method"), QStringLiteral("default")},
        {QStringLiteral("blend_type"), QStringLiteral("normal")},
        {QStringLiteral("show_transition"), QJsonObject{{QStringLiteral("duration"), 0}}},
        {QStringLiteral("hide_transition"), QJsonObject{{QStringLiteral("duration"), 0}}},
        {QStringLiteral("private_settings"), QJsonObject{}},
    };
}

} // namespace

QString ObsScene::defaultCollectionPath() {
    const auto config = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    return QDir(config).filePath(QStringLiteral("obs-studio/basic/scenes/quadcap.json"));
}

bool ObsScene::obsIsRunning() {
    const QDir proc(QStringLiteral("/proc"));
    for (const auto &entry : proc.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        bool numeric = false;
        entry.toInt(&numeric);
        if (!numeric) {
            continue;
        }
        QFile comm(proc.filePath(entry) + QStringLiteral("/comm"));
        if (!comm.open(QIODevice::ReadOnly)) {
            continue; // A process that ended, or one belonging to another user.
        }
        if (QString::fromLatin1(comm.readAll()).trimmed() == QStringLiteral("obs")) {
            return true;
        }
    }
    return false;
}

bool ObsScene::write(const SceneSources &sources, const QString &path, QString *error) {
    if (sources.videoDevice.isEmpty() && sources.gameSink.isEmpty() && sources.micSink.isEmpty()) {
        if (error) {
            *error = QStringLiteral("Nothing to put in the scene; run the installer first");
        }
        return false;
    }

    QJsonArray sourceList;
    QJsonArray items;
    int itemId = 1;

    const auto addSource = [&](const QString &name, const QString &id, const QJsonObject &settings,
                               const int mixers) {
        const auto uuid = newUuid();
        sourceList.append(sourceSkeleton(name, id, uuid, settings, mixers));
        // Prepended so the picture ends up beneath nothing and the audio sources sit above it in
        // the list, which is the order they are read in rather than stacked in.
        items.prepend(sceneItem(name, uuid, itemId++));
    };

    if (!sources.videoDevice.isEmpty()) {
        // Only the device and the input index. OBS detects format, resolution and frame rate
        // from the node itself, which is what lets the scene keep working when the console
        // changes mode; pinning them here would freeze the source at one geometry.
        //
        // input has to be named even though a loopback has no input selector, because OBS
        // defaults it to -1 and then fails on VIDIOC_S_INPUT: "Unable to set input -1".
        addSource(QStringLiteral("quadcap video"), QStringLiteral("v4l2_input"),
                  QJsonObject{
                      {QStringLiteral("device_id"), sources.videoDevice},
                      {QStringLiteral("input"), 0},
                  },
                  0);
    }
    // OBS reads a sink through its monitor source, which is the sink name with ".monitor" on the
    // end. mixers 255 puts each one on every recording track, so they stay separable afterwards.
    if (!sources.gameSink.isEmpty()) {
        addSource(QStringLiteral("quadcap game audio"), QStringLiteral("pulse_output_capture"),
                  QJsonObject{
                      {QStringLiteral("device_id"), sources.gameSink + QStringLiteral(".monitor")}},
                  255);
    }
    if (!sources.micSink.isEmpty()) {
        addSource(QStringLiteral("quadcap microphone"), QStringLiteral("pulse_output_capture"),
                  QJsonObject{
                      {QStringLiteral("device_id"), sources.micSink + QStringLiteral(".monitor")}},
                  255);
    }

    const auto sceneName = QStringLiteral("quadcap");
    sourceList.append(sourceSkeleton(sceneName, QStringLiteral("scene"), newUuid(),
                                     QJsonObject{
                                         {QStringLiteral("custom_size"), false},
                                         {QStringLiteral("id_counter"), itemId},
                                         {QStringLiteral("items"), items},
                                     },
                                     0));

    const QJsonObject collection{
        {QStringLiteral("name"), sceneName},
        {QStringLiteral("current_scene"), sceneName},
        {QStringLiteral("current_program_scene"), sceneName},
        {QStringLiteral("scene_order"),
         QJsonArray{QJsonObject{{QStringLiteral("name"), sceneName}}}},
        {QStringLiteral("sources"), sourceList},
        {QStringLiteral("groups"), QJsonArray{}},
        {QStringLiteral("transitions"), QJsonArray{}},
        {QStringLiteral("saved_projectors"), QJsonArray{}},
        {QStringLiteral("canvases"), QJsonArray{}},
        {QStringLiteral("quick_transitions"),
         QJsonArray{
             QJsonObject{{QStringLiteral("name"), QStringLiteral("Cut")},
                         {QStringLiteral("duration"), 300},
                         {QStringLiteral("hotkeys"), QJsonArray{}},
                         {QStringLiteral("id"), 1},
                         {QStringLiteral("fade_to_black"), false}},
             QJsonObject{{QStringLiteral("name"), QStringLiteral("Fade")},
                         {QStringLiteral("duration"), 300},
                         {QStringLiteral("hotkeys"), QJsonArray{}},
                         {QStringLiteral("id"), 2},
                         {QStringLiteral("fade_to_black"), false}},
         }},
        {QStringLiteral("current_transition"), QStringLiteral("Fade")},
        {QStringLiteral("transition_duration"), 300},
        {QStringLiteral("preview_locked"), false},
        {QStringLiteral("scaling_enabled"), false},
        {QStringLiteral("scaling_level"), 0},
        {QStringLiteral("scaling_off_x"), 0.0},
        {QStringLiteral("scaling_off_y"), 0.0},
        {QStringLiteral("virtual-camera"), QJsonObject{{QStringLiteral("type2"), 3}}},
        {QStringLiteral("modules"), QJsonObject{}},
        {QStringLiteral("version"), 2},
    };

    const QFileInfo target(path);
    if (!QDir().mkpath(target.absolutePath())) {
        if (error) {
            *error = QStringLiteral("Could not create %1").arg(target.absolutePath());
        }
        return false;
    }

    // Written atomically: a collection half on disk is one OBS refuses to open, and it would be
    // sitting next to the user's real ones.
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) {
            *error = QStringLiteral("Could not write %1: %2").arg(path, file.errorString());
        }
        return false;
    }
    file.write(QJsonDocument(collection).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        if (error) {
            *error = QStringLiteral("Could not save %1: %2").arg(path, file.errorString());
        }
        return false;
    }
    return true;
}

} // namespace quadcap::obs
