#pragma once
#include <QObject>
#include <QMap>
#include <QThread>
#include <QUrl>

namespace veyra::bilibili {
// A bounded, loopback-only transport adapter. No decoding, transcoding or disk cache.
class StreamServer final {
public:
    StreamServer();
    ~StreamServer();
    QUrl install(const QMap<QString, QByteArray>& documents,
                 const QMap<QString, QList<QUrl>>& resources, const QString& entry);
    QUrl installHls(const QList<QUrl>& urls);
    void clear();
private:
    void ensureWorker();
    QThread thread_;
    QObject* worker_ = nullptr;
};
}
