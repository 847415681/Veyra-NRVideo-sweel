#include "veyra/bilibili/StreamServer.h"
#include "veyra/bilibili/Playback.h"
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUuid>

namespace veyra::bilibili {
namespace {
constexpr qint64 bufferLimit = 256 * 1024;
void response(QTcpSocket* socket, int code, const QByteArray& body = {}, const QByteArray& type = "text/plain", bool head = false) {
    socket->write("HTTP/1.1 " + QByteArray::number(code) + " Response\r\nConnection: close\r\nCache-Control: no-store\r\nContent-Type: " + type + "\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n");
    if (!head) socket->write(body);
    socket->disconnectFromHost();
}
class Transfer final : public QObject {
public:
    Transfer(QTcpSocket* socket, QNetworkAccessManager* manager, QList<QUrl> urls, QByteArray range, bool head)
        : QObject(socket), socket_(socket), manager_(manager), urls_(std::move(urls)), range_(std::move(range)), head_(head) {
        connect(socket_, &QTcpSocket::bytesWritten, this, [this] { pump(); });
        connect(socket_, &QTcpSocket::disconnected, this, [this] { if (reply_) reply_->abort(); });
        start();
    }
    ~Transfer() override { if (reply_) { reply_->disconnect(this); reply_->abort(); reply_->deleteLater(); } }
private:
    void start() {
        if (urls_.isEmpty()) { response(socket_, 502); return; }
        const auto url = urls_.takeFirst();
        if (!mediaUrlAllowed(url)) { response(socket_, 502); return; }
        QNetworkRequest request(url);
        request.setRawHeader("User-Agent", userAgent());
        request.setRawHeader("Referer", "https://www.bilibili.com/");
        request.setRawHeader("Accept-Encoding", "identity");
        if (!range_.isEmpty()) request.setRawHeader("Range", range_);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        request.setTransferTimeout(20000);
        reply_ = head_ ? manager_->head(request) : manager_->get(request);
        reply_->setReadBufferSize(bufferLimit);
        connect(reply_, &QNetworkReply::readyRead, this, [this] { pump(); });
        connect(reply_, &QNetworkReply::finished, this, [this] {
            if (!reply_) return;
            const int code = reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QUrl redirect = reply_->url().resolved(reply_->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl());
            if (!headers_ && code >= 300 && code < 400 && ++redirects_ <= 3 && mediaUrlAllowed(redirect)) {
                urls_.prepend(redirect); retry(); return;
            }
            if (!headers_ && code != 200 && code != 206 && !urls_.isEmpty()) { retry(); return; }
            pump();
            if (!headers_ && socket_->state() == QAbstractSocket::ConnectedState) response(socket_, code == 416 ? 416 : 502);
        });
    }
    void retry() {
        reply_->disconnect(this); reply_->deleteLater(); reply_ = nullptr; start();
    }
    void pump() {
        if (!reply_ || socket_->state() != QAbstractSocket::ConnectedState) return;
        const int code = reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (code != 200 && code != 206) return;
        if (!headers_) {
            QByteArray headers = "HTTP/1.1 " + QByteArray::number(code) + " OK\r\nConnection: close\r\nCache-Control: no-store\r\n";
            for (const auto* name : {"Content-Type", "Content-Length", "Content-Range", "Accept-Ranges"}) {
                const auto value = reply_->rawHeader(name);
                if (!value.isEmpty() && !value.contains('\r') && !value.contains('\n')) headers += QByteArray(name) + ": " + value + "\r\n";
            }
            socket_->write(headers + "\r\n"); headers_ = true;
        }
        while (!head_ && reply_->bytesAvailable() > 0 && socket_->bytesToWrite() < bufferLimit)
            socket_->write(reply_->read(std::min(bufferLimit - socket_->bytesToWrite(), reply_->bytesAvailable())));
        if (reply_->isFinished() && (head_ || reply_->bytesAvailable() == 0)) socket_->disconnectFromHost();
    }
    QTcpSocket* socket_;
    QNetworkAccessManager* manager_;
    QList<QUrl> urls_;
    QByteArray range_;
    QPointer<QNetworkReply> reply_;
    bool head_ = false, headers_ = false;
    int redirects_ = 0;
};
class Gateway final : public QObject {
public:
    QTcpServer* server = nullptr;
    QNetworkAccessManager* network = nullptr;
    QMap<QString, QByteArray> documents;
    QMap<QString, QList<QUrl>> resources;
    QList<QPointer<QTcpSocket>> clients;
    QString prefix;
    void initialize() {
        server = new QTcpServer(this);
        server->setProxy(QNetworkProxy::NoProxy);
        network = new QNetworkAccessManager(this);
        connect(server, &QTcpServer::newConnection, this, [this] {
            while (auto* socket = server->nextPendingConnection()) {
                if (clients.size() >= 24) { socket->abort(); socket->deleteLater(); continue; }
                clients.append(socket); socket->setReadBufferSize(8192);
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QObject::destroyed, this, [this, socket] {
                    for (qsizetype i = clients.size(); i-- > 0;) if (!clients[i] || clients[i] == socket) clients.removeAt(i);
                });
                QTimer::singleShot(10000, socket, [socket] { if (!socket->property("accepted").toBool()) socket->abort(); });
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] { accept(socket); });
            }
        });
        server->listen(QHostAddress::LocalHost, 0);
    }
    void clear() {
        const auto old = clients;
        for (const auto& socket : old) if (socket) { socket->abort(); socket->deleteLater(); }
        clients.clear(); documents.clear(); resources.clear(); prefix.clear();
    }
    void accept(QTcpSocket* socket) {
        if (socket->property("accepted").toBool()) return;
        QByteArray request = socket->property("request").toByteArray() + socket->readAll();
        if (request.size() >= 8192) { response(socket, 431); return; }
        if (!request.contains("\r\n\r\n")) { socket->setProperty("request", request); return; }
        socket->setProperty("accepted", true);
        const auto lines = request.split('\n'), first = lines.value(0).trimmed().split(' ');
        if (first.size() != 3 || (first[0] != "GET" && first[0] != "HEAD")) { response(socket, 405); return; }
        QByteArray host, origin, range;
        for (const auto& line : lines) {
            const auto colon = line.indexOf(':');
            if (colon < 0) continue;
            const auto key = line.left(colon).trimmed().toLower(), value = line.mid(colon + 1).trimmed();
            if (key == "host") host = value;
            if (key == "origin") origin = value;
            if (key == "range") range = value;
        }
        if (host != "127.0.0.1:" + QByteArray::number(server->serverPort()) || !origin.isEmpty() || !socket->peerAddress().isLoopback()) { response(socket, 403); return; }
        const QString path = QString::fromLatin1(first[1]);
        if (prefix.isEmpty() || !path.startsWith(prefix)) { response(socket, 404); return; }
        const auto key = path.mid(prefix.size());
        const bool head = first[0] == "HEAD";
        if (documents.contains(key)) { response(socket, 200, documents[key], "application/vnd.apple.mpegurl", head); return; }
        // Fragment identity is only for the local HLS reader. The CDN receives
        // the original allowlisted URL and the requested byte range unchanged.
        const auto resourceKey = key.section(QLatin1Char('?'), 0, 0);
        if (!resources.contains(resourceKey)) { response(socket, 404); return; }
        if (!range.isEmpty() && !QRegularExpression("^bytes=[0-9]+-[0-9]*$").match(QString::fromLatin1(range)).hasMatch()) { response(socket, 416); return; }
        new Transfer(socket, network, resources[resourceKey], range, head);
    }
};
}
StreamServer::StreamServer() = default;
void StreamServer::ensureWorker() {
    if (worker_) return;
    auto* gateway = new Gateway;
    worker_ = gateway; gateway->moveToThread(&thread_); thread_.start();
    QMetaObject::invokeMethod(gateway, [gateway] { gateway->initialize(); }, Qt::BlockingQueuedConnection);
}
StreamServer::~StreamServer() {
    if (!worker_) return;
    QMetaObject::invokeMethod(worker_, [this] { delete worker_; worker_ = nullptr; }, Qt::BlockingQueuedConnection);
    thread_.quit(); thread_.wait();
}
QUrl StreamServer::install(const QMap<QString, QByteArray>& documents, const QMap<QString, QList<QUrl>>& resources, const QString& entry) {
    ensureWorker();
    QUrl url;
    QMetaObject::invokeMethod(worker_, [&, this] {
        auto* gateway = static_cast<Gateway*>(worker_); gateway->clear();
        if (!gateway->server->isListening()) return;
        gateway->documents = documents; gateway->resources = resources;
        gateway->prefix = '/' + QUuid::createUuid().toString(QUuid::WithoutBraces) + '/';
        url = QUrl("http://127.0.0.1:" + QString::number(gateway->server->serverPort()) + gateway->prefix + entry);
    }, Qt::BlockingQueuedConnection);
    return url;
}
void StreamServer::clear() {
    if (!worker_) return;
    QMetaObject::invokeMethod(worker_, [this] { static_cast<Gateway*>(worker_)->clear(); }, Qt::BlockingQueuedConnection);
}
}
