#include "veyra/bilibili/Playback.h"
#include "veyra/bilibili/StreamServer.h"
#include "veyra/ui/BilibiliModel.h"
#ifndef Q_MOC_RUN
#include "MockNetwork.h"
#endif
#include <QtTest>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QtEndian>
#include <QTemporaryDir>
#include <QFile>

using namespace veyra::bilibili;
class PlaybackTests : public QObject {
    Q_OBJECT
private slots:
    void modelPlayback() {
        QTemporaryDir dir; MockNetwork network;
        veyra::ui::BilibiliModel model(dir.path(), nullptr, &network);
        QSignalSpy ready(&model, &veyra::ui::BilibiliModel::playbackReady);
        model.inspect("https://b23.tv/test");
        QTRY_VERIFY_WITH_TIMEOUT(!model.busy(), 2000);
        QCOMPARE(model.parts().size(), 2); QCOMPARE(model.selectedPart(), 1); QCOMPARE(model.qualities().size(), 3);
        QCOMPARE(ready.size(), 0); // Metadata inspection must not start playback.
        model.play(80); QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 2000);
        QVERIFY(ready.first()[0].toUrl().host() == "127.0.0.1");
        bool signedRequest = false;
        for (const auto& request : network.requests) {
            if (request.url().path().endsWith("playurl")) { QVERIFY(request.url().query().contains("w_rid=")); signedRequest = true; }
            if (request.url().path().endsWith(".m4s")) {
                QCOMPARE(request.rawHeader("Range"), QByteArray("bytes=100-143"));
                QCOMPARE(request.rawHeader("Referer"), QByteArray("https://www.bilibili.com/"));
                QCOMPARE(request.attribute(QNetworkRequest::CookieLoadControlAttribute).toInt(), int(QNetworkRequest::Manual));
            }
        }
        QVERIFY(signedRequest);
    }
    void cancelledAndDenied() {
        QTemporaryDir dir; MockNetwork network; network.delay = 50;
        veyra::ui::BilibiliModel model(dir.path(), nullptr, &network);
        QSignalSpy ready(&model, &veyra::ui::BilibiliModel::playbackReady);
        model.inspect("BV1xx411c7mD"); model.releasePlayback(); QTest::qWait(100);
        QVERIFY(!model.busy()); QVERIFY(model.parts().isEmpty()); QCOMPARE(ready.size(), 0);
        network.delay = 0;
        model.inspect("https://www.bilibili.com/video/BV1xx411c7mD?p=99"); QTRY_VERIFY_WITH_TIMEOUT(!model.busy(), 2000);
        QVERIFY(model.parts().isEmpty()); model.play(); QTest::qWait(50); QCOMPARE(ready.size(), 0);
        network.playCode = -10403;
        model.inspect("ep123"); QTRY_VERIFY_WITH_TIMEOUT(!model.busy(), 2000);
        QVERIFY(model.status().contains("-10403")); QCOMPARE(ready.size(), 0);
        network.malformed = true; model.inspect("BV1xx411c7mD"); QTRY_VERIFY_WITH_TIMEOUT(!model.busy(), 2000);
        QVERIFY(model.status().contains(QString::fromUtf8("请求失败")));
    }
    void episodesAndPreview() {
        QTemporaryDir dir; MockNetwork network;
        veyra::ui::BilibiliModel model(dir.path(), nullptr, &network);
        QSignalSpy ready(&model, &veyra::ui::BilibiliModel::playbackReady);
        model.inspect("ep999"); QTRY_VERIFY_WITH_TIMEOUT(!model.busy(), 2000);
        QVERIFY(model.parts().isEmpty()); QVERIFY(model.status().contains(QString::fromUtf8("不在此番剧")));
        network.drm = true; model.inspect("ep123"); QTRY_VERIFY_WITH_TIMEOUT(!model.busy(), 2000);
        QVERIFY(model.status().contains("DRM")); QCOMPARE(ready.size(), 0);
        network.drm = false; network.silent = true; network.preview = true;
        model.inspect("ep123"); QTRY_VERIFY_WITH_TIMEOUT(!model.busy(), 2000);
        QVERIFY(model.status().contains(QString::fromUtf8("试看")));
        model.play(); QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 2000);
        QVERIFY(ready.first()[1].toString().contains(QString::fromUtf8("试看")));
        QVERIFY(ready.first()[0].toUrl().path().endsWith("video.m3u8"));
        QNetworkAccessManager client; client.setProxy(QNetworkProxy::NoProxy);
        auto* reply = client.get(QNetworkRequest(ready.first()[0].toUrl())); QSignalSpy done(reply, &QNetworkReply::finished);
        QVERIFY(done.wait(2000)); const auto playlist = reply->readAll(); reply->deleteLater();
        QVERIFY(playlist.contains("#EXT-X-MAP")); QVERIFY(playlist.contains("#EXT-X-ENDLIST"));
        QVERIFY(!playlist.contains("audio"));
    }
    void qrAndEncryptedSession() {
        QTemporaryDir dir; MockNetwork network;
        veyra::ui::BilibiliModel model(dir.path(), nullptr, &network);
        model.beginLogin(); QTRY_VERIFY_WITH_TIMEOUT(!model.qrImage().isEmpty(), 2000);
        QVERIFY(model.qrImage().startsWith("data:image/png;base64,"));
        QTRY_VERIFY_WITH_TIMEOUT(!model.account().isEmpty(), 10000);
        QCOMPARE(network.pollCount, 2); QVERIFY(model.qrImage().isEmpty());
        QFile session(dir.filePath("bilibili-session.dpapi")); QVERIFY(session.open(QIODevice::ReadOnly));
        const auto encrypted = session.readAll(); QVERIFY(!encrypted.isEmpty()); QVERIFY(!encrypted.contains("mock-session-value")); session.close();
        MockNetwork restoredNetwork; veyra::ui::BilibiliModel restored(dir.path(), nullptr, &restoredNetwork);
        const auto saved = restoredNetwork.cookieJar()->cookiesForUrl(QUrl("https://www.bilibili.com/"));
        QVERIFY(!saved.isEmpty()); QCOMPARE(saved.first().value(), QByteArray("mock-session-value"));
        restored.logout(); QVERIFY(!QFile::exists(session.fileName()));
        QVERIFY(restoredNetwork.cookieJar()->cookiesForUrl(QUrl("https://www.bilibili.com/")).isEmpty());
        model.beginLogin(); QTRY_VERIFY_WITH_TIMEOUT(!model.qrImage().isEmpty(), 2000); model.cancelLogin();
        QTest::qWait(2700); QCOMPARE(network.pollCount, 2);
    }
    void live() {
        QTemporaryDir dir; MockNetwork network; veyra::ui::BilibiliModel model(dir.path(), nullptr, &network);
        QSignalSpy ready(&model, &veyra::ui::BilibiliModel::playbackReady);
        model.inspect("https://live.bilibili.com/123"); QTRY_VERIFY_WITH_TIMEOUT(!model.busy(), 2000);
        QCOMPARE(model.qualities().size(), 2); model.play(); QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 2000);
        QVERIFY(ready.first()[0].toUrl().path().endsWith("live.flv"));
    }
    void links() {
        QCOMPARE(parseLink("BV1xx411c7mD").kind, Link::Video);
        QCOMPARE(parseLink("https://www.bilibili.com/video/BV1xx411c7mD/?p=3&spm_id_from=333").page, 3);
        QCOMPARE(parseLink("av170001").id, QString("av170001"));
        QCOMPARE(parseLink("ep123").kind, Link::Episode);
        QCOMPARE(parseLink("ss123").kind, Link::Season);
        QCOMPARE(parseLink("https://live.bilibili.com/123").kind, Link::Live);
        QCOMPARE(parseLink("https://b23.tv/abc123").kind, Link::Short);
        for (const auto* bad : {"https://www.bilibili.com.evil.test/video/BV1xx411c7mD", "file:///etc/passwd", "https://evil@www.bilibili.com/video/BV1xx411c7mD", "https://www.bilibili.com:8080/video/BV1xx411c7mD", "https://www.bilibili.com/video/not-a-bv", "BV1xx411c7mD garbage"}) QCOMPARE(parseLink(bad).kind, Link::Invalid);
    }
    void hosts() {
        QVERIFY(mediaUrlAllowed(QUrl("https://upos-sz-mirrorcos.bilivideo.com/upgcxcode/v.m4s?token=example")));
        QVERIFY(!mediaUrlAllowed(QUrl("https://bilivideo.com.attacker.test/file")));
        QVERIFY(!mediaUrlAllowed(QUrl("http://127.0.0.1:8080/private")));
        QVERIFY(!mediaUrlAllowed(QUrl("https://user:secret@bilivideo.com/file")));
        auto track = MockNetwork::track(80);
        track["baseUrl"] = "https://evil.example/video.m4s";
        track["backupUrl"] = QJsonArray{"https://cdn.bilivideo.com/backup.m4s"};
        QCOMPARE(parseTrack(track).url, QUrl("https://cdn.bilivideo.com/backup.m4s"));
    }
    void wbi() {
        // Fixed public example keys. Digest independently checked with Python hashlib
        // using the mixed key ea1db124af3c7062474693fa704f4ff8.
        QCOMPARE(signedQuery({{"foo", "114"}, {"bar", "514"}, {"baz", "1919810"}},
            "7cd084941338484aae1ad9425b84077c", "4932caff0ff746eab6f01bf08b70ac45", 1702204169),
            QString("bar=514&baz=1919810&foo=114&wts=1702204169&w_rid=6149fdadf571698ca7e6a567265cd0ee"));
        QVERIFY(signedQuery({}, "short", "short", 1).isEmpty());
    }
    static QByteArray sidx(bool version1) {
        QByteArray data;
        auto u32 = [&](quint32 v) { char b[4]; qToBigEndian(v, b); data.append(b, 4); };
        auto u64 = [&](quint64 v) { char b[8]; qToBigEndian(v, b); data.append(b, 8); };
        u32(version1 ? 64 : 56); data += "sidx"; u32(version1 ? 0x01000000u : 0); u32(1); u32(1000);
        if (version1) { u64(0); u64(10); } else { u32(0); u32(10); }
        u32(2); u32(200); u32(1500); u32(0x90000000); u32(300); u32(2500); u32(0x90000000);
        return data;
    }
    void indexes() {
        for (bool version : {false, true}) {
            const auto bytes = sidx(version); const auto index = parseSidx(bytes, 100);
            QVERIFY2(index.error.isEmpty(), qPrintable(index.error)); QCOMPARE(index.segments.size(), 2);
            QCOMPARE(index.segments[0].offset, quint64(100 + bytes.size() + 10));
            QCOMPARE(index.segments[1].offset, quint64(100 + bytes.size() + 210));
            QCOMPARE(index.segments[1].seconds, 2.5);
            const auto playlist = mediaPlaylist(index, {0, 100}, "video.m4s");
            QVERIFY(playlist.contains("#EXT-X-TARGETDURATION:3\n"));
            QVERIFY(playlist.contains("#EXT-X-MAP:URI=\"video.m4s\",BYTERANGE=\"100@0\""));
            QVERIFY(playlist.endsWith("#EXT-X-ENDLIST\n"));
            QVERIFY(!parseSidx(bytes.chopped(1), 0).error.isEmpty());
        }
        auto invalid = sidx(false); invalid[32] = char(0x80); QVERIFY(!parseSidx(invalid, 0).error.isEmpty());
        QVERIFY(!parseSidx(sidx(true), quint64(INT64_MAX)).error.isEmpty());
        QCOMPARE(parseRange("0-999").size, quint64(1000));
        QVERIFY(!parseRange("20-10").valid()); QVERIFY(!parseRange("0-18446744073709551615").valid());
    }
    void server() {
        StreamServer server;
        const auto url = server.install({{"master.m3u8", "#EXTM3U\n"}}, {}, "master.m3u8"); QVERIFY(!url.isEmpty());
        QNetworkAccessManager network; network.setProxy(QNetworkProxy::NoProxy);
        auto* reply = network.get(QNetworkRequest(url)); QSignalSpy done(reply, &QNetworkReply::finished);
        QVERIFY(done.wait(5000)); QCOMPARE(reply->readAll(), QByteArray("#EXTM3U\n")); reply->deleteLater();
        QNetworkRequest browser(url); browser.setRawHeader("Origin", "https://attacker.test");
        reply = network.get(browser); QSignalSpy blocked(reply, &QNetworkReply::finished);
        QVERIFY(blocked.wait(5000)); QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 403); reply->deleteLater();
        auto stale = url; server.install({{"master.m3u8", "new"}}, {}, "master.m3u8");
        reply = network.get(QNetworkRequest(stale)); QSignalSpy expired(reply, &QNetworkReply::finished);
        QVERIFY(expired.wait(5000)); QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 404); reply->deleteLater();
        server.clear();
    }
};
QTEST_GUILESS_MAIN(PlaybackTests)
#include "PlaybackTests.moc"
