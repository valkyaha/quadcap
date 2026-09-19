#pragma once

#include <QString>

namespace quadcap::obs {

//! What quadcap offers OBS, and where each part comes from.
struct SceneSources {
    //! v4l2loopback node carrying the picture, e.g. "/dev/video10".
    QString videoDevice;
    //! PipeWire sink names, without the ".monitor" suffix OBS reads them through.
    QString gameSink;
    QString micSink;
};

/*!
 * Writes an OBS scene collection wired to quadcap's outputs.
 *
 * Adding three sources by hand means knowing that a v4l2loopback node is a camera, that a null
 * sink is read through its monitor, and which of several similarly named devices is the right one.
 * Generating the collection removes all of that, and it is generated rather than shipped as a file
 * because the device paths are only known on the machine it runs on.
 */
class ObsScene final {
  public:
    //! Where OBS keeps its scene collections, so a written file simply appears in its menu.
    [[nodiscard]] static QString defaultCollectionPath();

    /*!
     * True when an OBS process is running.
     *
     * OBS holds its scene collections in memory and writes them out when it exits, so a file
     * dropped in underneath a running instance is silently discarded. Worth refusing rather than
     * appearing to succeed.
     */
    [[nodiscard]] static bool obsIsRunning();

    //! Writes the collection to \a path. False on failure, with \a error set.
    [[nodiscard]] static bool write(const SceneSources &sources, const QString &path,
                                    QString *error = nullptr);
};

} // namespace quadcap::obs
