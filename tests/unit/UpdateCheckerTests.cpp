#include "veyra/ui/UpdateChecker.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>
#include <memory>

using veyra::ui::UpdateChecker;
namespace {
QByteArray release(QString tag = QStringLiteral("v2.0.8"), bool prerelease = false, bool draft = false,
                   QString url = {}) {
    if (url.isEmpty()) url = QStringLiteral("https://github.com/Likely7/Veyra-NRVideo/releases/tag/") + tag;
    return QJsonDocument(QJsonObject{{"tag_name", tag}, {"draft", draft}, {"prerelease", prerelease},
                                    {"html_url", url}}).toJson();
}
class Server : public QTcpServer {
public:
    QByteArray body = release(), request;
    int status = 200, requests = 0;
    bool hang = false;
    Server() {
        if (!listen(QHostAddress::LocalHost)) qFatal("Cannot listen on localhost");
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                auto* socket = nextPendingConnection();
                auto header = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket, header] {
                    *header += socket->readAll();
                    if (!header->contains("\r\n\r\n")) return;
                    socket->disconnect(socket, &QTcpSocket::readyRead, nullptr, nullptr);
                    ++requests; request = *header;
                    if (hang) return;
                    socket->write("HTTP/1.1 " + QByteArray::number(status) + " Test\r\nContent-Type: application/json\r\nContent-Length: "
                                  + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
            }
        });
    }
    QUrl endpoint() const { return QUrl(QStringLiteral("http://127.0.0.1:%1/latest").arg(serverPort())); }
};
}
class UpdateCheckerTests : public QObject {
    Q_OBJECT
private slots:
    void response_data() {
        QTest::addColumn<QString>("local"); QTest::addColumn<QByteArray>("body");
        QTest::addColumn<int>("http"); QTest::addColumn<QString>("result");
        QTest::newRow("new") << "2.0.7" << release() << 200 << "available";
        QTest::newRow("same-test-r2") << "2.0.7-test-r2" << release("v2.0.7") << 200 << "current";
        QTest::newRow("same") << "2.0.7" << release("v2.0.7") << 200 << "current";
        QTest::newRow("old") << "2.0.9" << release() << 200 << "current";
        QTest::newRow("numeric-10-over-9") << "2.0.9" << release("v2.0.10") << 200 << "available";
        QTest::newRow("numeric-9-below-10") << "2.0.10" << release("v2.0.9") << 200 << "current";
        QTest::newRow("major") << "2.99.99" << release("v3.0.0") << 200 << "available";
        QTest::newRow("four-component-local") << "2.0.7.0" << release() << 200 << "available";
        QTest::newRow("draft") << "2.0.7" << release("v2.0.8", false, true) << 200 << "invalid-release";
        QTest::newRow("prerelease") << "2.0.7" << release("v2.0.8", true) << 200 << "invalid-release";
        QTest::newRow("beta-tag") << "2.0.7" << release("v2.0.8-beta") << 200 << "invalid-release";
        QTest::newRow("sources-tag") << "2.0.7" << release("v2.0.8-sources") << 200 << "invalid-release";
        QTest::newRow("json") << "2.0.7" << QByteArray("not json") << 200 << "invalid-release";
        QTest::newRow("array") << "2.0.7" << QByteArray("[]") << 200 << "invalid-release";
        QTest::newRow("missing-fields") << "2.0.7" << QByteArray("{\"tag_name\":\"v2.0.8\"}") << 200 << "invalid-release";
        QTest::newRow("untrusted-link") << "2.0.7" << release("v2.0.8", false, false, "https://example.com/update") << 200 << "invalid-release";
        QTest::newRow("other-repo") << "2.0.7" << release("v2.0.8", false, false, "https://github.com/other/repo/releases/tag/v2.0.8") << 200 << "invalid-release";
        QTest::newRow("mismatched-tag") << "2.0.7" << release("v2.0.8", false, false, "https://github.com/Likely7/Veyra-NRVideo/releases/tag/v2.0.9") << 200 << "invalid-release";
        QTest::newRow("overflow") << "2.0.7" << release("v2.0.2147483648") << 200 << "invalid-release";
        QTest::newRow("unknown-local") << "dev" << release() << 200 << "invalid-release";
        QTest::newRow("404") << "2.0.7" << release() << 404 << "http-404-network-203";
        QTest::newRow("rate-limit") << "2.0.7" << release() << 403 << "http-403-network-201";
        QTest::newRow("too-large") << "2.0.7" << QByteArray(128*1024+1, 'x') << 200 << "response-too-large";
    }
    void response() {
        QFETCH(QString, local); QFETCH(QByteArray, body); QFETCH(int, http); QFETCH(QString, result);
        for (bool manual : {false, true}) {
            Server server; server.body = body; server.status = http;
            UpdateChecker checker(local, nullptr, server.endpoint());
            QSignalSpy done(&checker, &UpdateChecker::completed), available(&checker, &UpdateChecker::updateAvailable),
                       notices(&checker, &UpdateChecker::notice);
            checker.check(manual);
            QVERIFY(checker.checking());
            QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 3000);
            QCOMPARE(done[0][0].toString(), result);
            QCOMPARE(checker.checking(), false);
            QCOMPARE(server.requests, 1);
            QCOMPARE(available.size(), result == "available" ? 1 : 0);
            QCOMPARE(notices.size(), manual && result != "available" ? 1 : 0);
            QVERIFY(server.request.contains("User-Agent: Veyra-Update-Check"));
            QVERIFY(!server.request.contains("Authorization:"));
            if (result == "available") {
                QVERIFY(!checker.latestVersion().isEmpty()); QVERIFY(!checker.releaseUrl().isEmpty());
            } else { QVERIFY(checker.latestVersion().isEmpty()); QVERIFY(checker.releaseUrl().isEmpty()); }
        }
    }
    void timeoutAndCoalescing() {
        Server server; server.hang = true;
        UpdateChecker checker("2.0.7", nullptr, server.endpoint(), 200);
        QSignalSpy done(&checker, &UpdateChecker::completed), notices(&checker, &UpdateChecker::notice);
        checker.check(false); checker.check(true); checker.check(true);
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 2000);
        QCOMPARE(server.requests, 1); QCOMPARE(notices.size(), 1);
        QCOMPARE(done[0][0].toString(), QString("timeout"));
        checker.check(false);
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 2, 2000);
        QCOMPARE(server.requests, 2); QCOMPARE(notices.size(), 1);
    }
    void startupOnce() {
        Server server; server.body = release("v2.0.7");
        UpdateChecker checker("2.0.7-test-r2", nullptr, server.endpoint());
        QSignalSpy done(&checker, &UpdateChecker::completed), notices(&checker, &UpdateChecker::notice);
        checker.startAutomaticCheck(); checker.startAutomaticCheck();
        QCOMPARE(server.requests, 0);
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 4000);
        checker.startAutomaticCheck(); QTest::qWait(2700);
        QCOMPARE(server.requests, 1); QCOMPARE(notices.size(), 0);
    }
    void manualBeforeStartup() {
        Server server;
        UpdateChecker checker("2.0.7", nullptr, server.endpoint());
        QSignalSpy done(&checker, &UpdateChecker::completed);
        checker.startAutomaticCheck(); checker.check(true);
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 3000);
        QTest::qWait(2700); QCOMPARE(server.requests, 1);
    }
    void destructionInFlight() {
        Server server; server.hang = true;
        auto checker = std::make_unique<UpdateChecker>("2.0.7", nullptr, server.endpoint());
        checker->startAutomaticCheck(); checker->check(false);
        QTRY_COMPARE(server.requests, 1);
        checker.reset(); QTest::qWait(2700); QCOMPARE(server.requests, 1);
    }
};
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().contains("--live")) {
        UpdateChecker checker("2.0.7-test-r2");
        QObject::connect(&checker, &UpdateChecker::completed, &app, [&](const QString& result) {
            qInfo().noquote() << "LIVE GitHub result:" << result << "version:" << checker.latestVersion();
            app.exit(result == "current" || result == "available" ? 0 : 1);
        });
        checker.check(false); return app.exec();
    }
    UpdateCheckerTests tests; return QTest::qExec(&tests, argc, argv);
}
#include "UpdateCheckerTests.moc"
