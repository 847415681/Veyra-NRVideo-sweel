#pragma once
#include <QNetworkAccessManager>
#include <QNetworkCookieJar>
#include <QNetworkCookie>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QTimer>
#include <QtEndian>
#include <cstring>

class MockReply final : public QNetworkReply {
public:
    MockReply(const QNetworkRequest& request, QByteArray data, int status, int delay, QObject* parent)
        : QNetworkReply(parent), data_(std::move(data)) {
        setRequest(request); setUrl(request.url()); setOperation(QNetworkAccessManager::GetOperation);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
        setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        setRawHeader("Content-Range", "bytes 100-143/1024");
        if (status == 302) setAttribute(QNetworkRequest::RedirectionTargetAttribute, QUrl("https://www.bilibili.com/video/BV1xx411c7mD?p=2"));
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        QTimer::singleShot(delay, this, [this] { if (!isFinished()) { setFinished(true); emit readyRead(); emit finished(); } });
    }
    void abort() override { if (!isFinished()) { setError(OperationCanceledError, "cancelled"); setFinished(true); emit finished(); } }
    qint64 bytesAvailable() const override { return data_.size() - offset_ + QNetworkReply::bytesAvailable(); }
    bool isSequential() const override { return true; }
protected:
    qint64 readData(char* data, qint64 maximum) override {
        const qint64 size = std::min(maximum, qint64(data_.size()) - offset_);
        if (!size) return -1;
        std::memcpy(data, data_.constData() + offset_, size); offset_ += size; return size;
    }
private:
    QByteArray data_; qint64 offset_ = 0;
};
class MockNetwork final : public QNetworkAccessManager {
public:
    int delay = 0, playCode = 0, pollCount = 0;
    bool loggedIn = false, malformed = false, silent = false, drm = false, preview = false;
    QList<QNetworkRequest> requests;
    static QJsonObject track(int quality, bool audio = false) {
        return {{"id", quality}, {"baseUrl", audio ? "https://cdn.bilivideo.com/audio.m4s" : "https://cdn.bilivideo.com/video.m4s"},
            {"bandwidth", audio ? 128000 : 1200000}, {"codecs", audio ? "mp4a.40.2" : "avc1.64001f"},
            {"width", audio ? 0 : 1920}, {"height", audio ? 0 : 1080},
            {"SegmentBase", QJsonObject{{"Initialization", "0-99"}, {"indexRange", "100-143"}}}};
    }
protected:
    QNetworkReply* createRequest(Operation, const QNetworkRequest& request, QIODevice*) override {
        requests.append(request); const auto path = request.url().path(); QJsonObject root{{"code", 0}};
        if (malformed) return new MockReply(request, "not json", 200, delay, this);
        if (request.url().host() == "b23.tv") return new MockReply(request, {}, 302, delay, this);
        if (path == "/x/web-interface/view") root["data"] = QJsonDocument::fromJson(R"json({
            "title":"测试视频", "aid":170001, "bvid":"BV1xx411c7mD",
            "pages":[{"page":1,"part":"第一部分","cid":11},{"page":2,"part":"第二部分","cid":22}]
        })json").object();
        else if (path == "/x/web-interface/nav") root["data"] = QJsonObject{{"isLogin", loggedIn}, {"uname", loggedIn ? "测试账号" : ""},
            {"wbi_img", QJsonObject{{"img_url", "https://i0.hdslb.com/bfs/wbi/7cd084941338484aae1ad9425b84077c.png"}, {"sub_url", "https://i0.hdslb.com/bfs/wbi/4932caff0ff746eab6f01bf08b70ac45.png"}}}};
        else if (path.endsWith("playurl")) {
            root["code"] = playCode;
            root["data"] = QJsonObject{{"is_drm", drm}, {"is_preview", preview}, {"dash", QJsonObject{{"video", QJsonArray{track(80), track(64)}}, {"audio", silent ? QJsonArray{} : QJsonArray{track(30280, true)}}}}};
        } else if (path.endsWith("/generate")) root["data"] = QJsonObject{{"qrcode_key", "test-key"}, {"url", "https://account.bilibili.com/h5-app/passport/login/scan?qrcode_key=test-key"}};
        else if (path.endsWith("/poll")) {
            loggedIn = ++pollCount >= 2; root["data"] = QJsonObject{{"code", loggedIn ? 0 : 86090}};
            if (loggedIn) { QNetworkCookie cookie("SESSDATA", "mock-session-value"); cookie.setDomain(".bilibili.com"); cookie.setPath("/"); cookie.setSecure(true); cookieJar()->setCookiesFromUrl({cookie}, QUrl("https://passport.bilibili.com/")); }
        } else if (path.endsWith(".m4s")) {
            QByteArray index; auto u32 = [&](quint32 v) { char b[4]; qToBigEndian(v, b); index.append(b, 4); };
            u32(44); index += "sidx"; u32(0); u32(1); u32(1000); u32(0); u32(0); u32(1); u32(512); u32(5000); u32(0x90000000);
            return new MockReply(request, index, 206, delay, this);
        } else if (path.endsWith("/season")) root["result"] = QJsonDocument::fromJson(R"json({
            "title":"测试番剧", "episodes":[{"id":123,"cid":33,"aid":44,"title":"1","long_title":"第一集"}]
        })json").object();
        else if (path.endsWith("get_info")) root["data"] = QJsonObject{{"room_id", 123}, {"live_status", 1}, {"title", "测试直播"}};
        else if (path.endsWith("getRoomPlayInfo")) root["data"] = QJsonDocument::fromJson(R"json({
            "live_status":1,"playurl_info":{"playurl":{
                "g_qn_desc":[{"qn":10000,"desc":"原画"}],
                "stream":[{"format":[{"codec":[{"accept_qn":[10000],"base_url":"/live.flv",
                    "url_info":[{"host":"https://live.bilivideo.com","extra":"?test=1"}]}]}]}]
            }}
        })json").object();
        else root["code"] = -404;
        return new MockReply(request, QJsonDocument(root).toJson(QJsonDocument::Compact), 200, delay, this);
    }
};
