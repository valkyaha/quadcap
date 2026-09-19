#pragma once

#include <QObject>
#include <QString>
#include <QVector>

namespace quadcap::flashback {

struct Segment {
    QString path;
    int index = 0;
    qint64 sizeBytes = 0;
    //! Exact fragment duration when splitmuxsink reported it; 0 when only the file was observed.
    qint64 durationNs = 0;

    friend bool operator==(const Segment &, const Segment &) = default;
};

/*!
 * Reads back the keyframe-aligned segments that the capture pipeline's splitmuxsink leaves in the
 * ring directory, and stitches a chosen tail of them into one playable file.
 *
 * Saving is a remux: the encoded stream is copied, never re-encoded, so the cost is proportional to
 * the bytes on disk rather than the duration of the buffer.
 *
 * Completeness is taken from splitmuxsink's own `splitmuxsink-fragment-closed` messages, forwarded
 * through \ref noteSegmentClosed. A file appearing on disk is not sufficient: splitmuxsink opens
 * fragment N+1 before it has finished flushing fragment N, so a directory listing alone will offer
 * up a half-written file whose header has no tracks yet.
 */
class FlashbackRing final : public QObject {
    Q_OBJECT

public:
    static constexpr auto SegmentPattern = "segment-*.mkv";

    /*!
     * Fragments trailing the newest one that a bare directory scan must distrust, used only when no
     * fragment-closed messages have been seen. Covers the open fragment and the one still being
     * flushed behind it.
     */
    static constexpr int UnsettledTail = 2;

    explicit FlashbackRing(QObject *parent = nullptr);

    FlashbackRing(const FlashbackRing &) = delete;
    FlashbackRing &operator=(const FlashbackRing &) = delete;

    void configure(const QString &ringDirectory, int segmentSeconds, bool hardwareEncoder);

    [[nodiscard]] QString ringDirectory() const;
    [[nodiscard]] int segmentSeconds() const;

    /*!
     * Segments that are safe to read, oldest first.
     *
     * Once any fragment-closed message has arrived this is exactly the set splitmuxsink declared
     * closed and that still exists on disk. Before that — a ring left over from a previous run —
     * it falls back to a directory scan minus \ref UnsettledTail.
     */
    [[nodiscard]] QVector<Segment> finalizedSegments() const;

    //! Wall-clock duration currently retrievable, exact when fragment durations are known.
    [[nodiscard]] int availableSeconds() const;

    /*!
     * Writes the most recent \a minutes of buffered video to \a outputPath.
     *
     * Fewer segments than requested is not an error; whatever the ring holds is saved and the
     * covered duration is returned through \a savedSeconds.
     */
    [[nodiscard]] bool save(int minutes, const QString &outputPath, int *savedSeconds = nullptr,
        QString *error = nullptr);

    //! Parses the numeric index out of a `segment-%06d.mkv` name; -1 when the name does not match.
    [[nodiscard]] static int indexOf(const QString &fileName);

public slots:
    //! Records a fragment splitmuxsink has finished writing. \a durationNs may be 0 if unreported.
    void noteSegmentClosed(const QString &path, qint64 durationNs);

    //! Forgets every recorded fragment, for when the pipeline restarts onto a fresh ring.
    void reset();

signals:
    void saved(const QString &path, int seconds);

private:
    [[nodiscard]] QVector<Segment> scanDirectory() const;

    //! Stitches every fragment in \a stagingDirectory into \a outputPath, copying the encoded stream.
    [[nodiscard]] bool concatenate(const QString &stagingDirectory, const QString &outputPath,
        QString *error) const;

    QString ringDirectory_;
    int segmentSeconds_ = 2;
    bool hardwareEncoder_ = true;
    QVector<Segment> closed_;
};

} // namespace quadcap::flashback
