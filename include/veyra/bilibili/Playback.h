#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QString>
#include <QUrl>

namespace veyra::bilibili {
// Independently implemented from the public web API and ISO BMFF/HLS formats.
// See docs/BILIBILI_PLAYBACK_2026-10-25.md for reference provenance.
struct Link {
    enum Kind { Invalid, Video, Episode, Season, Live, Short } kind = Invalid;
    QString id;
    int page = 1;
    QUrl url;
};
Link parseLink(QString text);
bool mediaUrlAllowed(const QUrl& url);
QByteArray userAgent();
QString signedQuery(QMap<QString, QString> params, const QString& imageKey,
                    const QString& subKey, qint64 timestamp);

struct ByteRange {
    quint64 start = 0, size = 0;
    bool valid() const { return size > 0; }
};
ByteRange parseRange(const QString& text);
struct Segment { quint64 offset = 0, size = 0; double seconds = 0; };
struct Index { QList<Segment> segments; QString error; };
Index parseSidx(const QByteArray& bytes, quint64 fileOffset);
QByteArray mediaPlaylist(const Index& index, ByteRange initialization, const QByteArray& uri);

struct Track {
    QUrl url;
    QList<QUrl> backups;
    ByteRange initialization, index;
    int quality = 0;
    qint64 bandwidth = 0;
    QString codecs;
    int width = 0, height = 0;
};
Track parseTrack(const QJsonObject& object);
QString qualityLabel(int id);
}
