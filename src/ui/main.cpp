#include "ui/AppController.h"
#include "ui/SingleInstance.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTextStream>
#include <QTimer>

#include <gst/gst.h>

#include <memory>

namespace {

/*!
 * Makes the GstGLVideoItem QML type resolvable.
 *
 * gstreamer1.0-qt6 ships only libgstqml6.so — there is no QML module directory on the import path.
 * The type is registered as a side effect of loading that GStreamer plugin, and the plugin is not
 * loaded until something asks for one of its elements. Creating the sink here is what makes
 * `import org.freedesktop.gstreamer.Qt6GLVideoItem` resolve; without it the engine fails with
 * "module is not installed" before any pipeline has had a chance to run.
 */
bool registerVideoItemType()
{
    GstElement *probe = gst_element_factory_make("qml6glsink", nullptr);
    if (!probe) {
        return false;
    }
    gst_object_ref_sink(probe);
    gst_object_unref(probe);
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("quadcap"));
    QCoreApplication::setApplicationName(QStringLiteral("quadcap"));
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    /*
     * One copy per user. The card's ALSA node is exclusive, so a second instance cannot take the
     * audio and dies with an error that points nowhere near the real cause. Raise the window that
     * is already open instead of starting a copy that cannot work.
     */
    quadcap::ui::SingleInstance instance(QStringLiteral("quadcap"));
    if (!instance.isPrimary()) {
        instance.raiseExisting();
        QTextStream(stderr) << "quadcap is already running; raising the existing window.\n";
        return 0;
    }

    // qml6glsink wraps Qt's OpenGL context, so Qt Quick must not pick a different RHI backend.
    // This has to be set before the first window is created.
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);

    gst_init(&argc, &argv);
    if (!registerVideoItemType()) {
        QTextStream(stderr)
            << "qml6glsink is unavailable, so the preview cannot be shown. "
            << "Install gstreamer1.0-qt6.\n";
        return 1;
    }

    AppController controller;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("appController"), &controller);
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed,
        &app, [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.loadFromModule(QStringLiteral("Quadcap"), QStringLiteral("Main"));

    if (engine.rootObjects().isEmpty()) {
        return 1;
    }

    QObject *root = engine.rootObjects().front();
    QObject::connect(&instance, &quadcap::ui::SingleInstance::raiseRequested, root, [root] {
        if (auto *window = qobject_cast<QQuickWindow *>(root)) {
            window->show();
            window->raise();
            window->requestActivate();
        }
    });
    QObject *preview = root->findChild<QObject *>(QStringLiteral("previewSurface"));

    /*
     * Build the pipeline only once the scene graph has presented a frame.
     *
     * qml6glsink wraps the QQuickWindow's OpenGL context when it goes to READY, and that context
     * does not exist until the window has actually rendered. Starting on a queued timer instead
     * fails with "failed to retrieve wrapped context (NULL)" and the whole pipeline refuses to
     * start — silently, because the sink is only one element in it.
     */
    if (auto *window = qobject_cast<QQuickWindow *>(root)) {
        auto connection = std::make_shared<QMetaObject::Connection>();
        *connection = QObject::connect(
            window, &QQuickWindow::frameSwapped, &controller,
            [&controller, preview, connection] {
                QObject::disconnect(*connection);
                controller.initialize(preview);
            },
            Qt::QueuedConnection);
    } else {
        QTimer::singleShot(0, &controller, [&controller, preview] { controller.initialize(preview); });
    }

    return app.exec();
}

