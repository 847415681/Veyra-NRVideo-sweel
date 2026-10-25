#include "veyra/ui/BilibiliModel.h"
#ifndef Q_MOC_RUN
#include "MockNetwork.h"
#endif
#include <QtTest>
#include <QQmlEngine>
#include <QQmlComponent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QImage>

class QmlTests : public QObject {
    Q_OBJECT
private slots:
    void panel() {
        QTemporaryDir data; MockNetwork network;
        veyra::ui::BilibiliModel model(data.path(), nullptr, &network);
        QQmlEngine engine; engine.addImportPath(QString::fromUtf8(VEYRA_QML_ROOT));
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this, [&](const QList<QQmlError>& errors) { for (const auto& error : errors) warnings << error.toString(); });
        QQmlComponent component(&engine, QUrl::fromLocalFile(QString::fromUtf8(VEYRA_QML_ROOT) + "/Veyra/BilibiliPanel.qml"));
        QTRY_VERIFY_WITH_TIMEOUT(!component.isLoading(), 5000);
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        auto* object = component.createWithInitialProperties({{"model", QVariant::fromValue(&model)}});
        QVERIFY2(object, qPrintable(component.errorString()));
        auto* item = qobject_cast<QQuickItem*>(object); QVERIFY(item);
        QQuickWindow window; window.setColor(QColor("#141418")); window.resize(660, 660);
        item->setParentItem(window.contentItem()); item->setPosition({24, 24}); item->setWidth(612);
        item->setHeight(item->implicitHeight());
        window.show();
        model.inspect("BV1xx411c7mD"); QTRY_VERIFY_WITH_TIMEOUT(!model.busy(), 2000);
        auto* parts = item->findChild<QObject*>("bilibili-parts"); QVERIFY(parts);
        QTRY_COMPARE(parts->property("count").toInt(), 2);
        QVERIFY(QMetaObject::invokeMethod(parts, "activated", Q_ARG(int, 1)));
        QTRY_VERIFY_WITH_TIMEOUT(!model.busy(), 2000); QCOMPARE(model.selectedPart(), 1);
        auto* quality = item->findChild<QObject*>("bilibili-quality"); QVERIFY(quality); QCOMPARE(quality->property("count").toInt(), 3);
        model.beginLogin(); QTRY_VERIFY_WITH_TIMEOUT(!model.qrImage().isEmpty(), 2000);
        auto* image = item->findChild<QObject*>("bilibili-login-qr"); QVERIFY(image);
        QTRY_VERIFY_WITH_TIMEOUT(image->property("visible").toBool(), 2000);
        QTest::qWait(300);
        const auto output = qEnvironmentVariable("VEYRA_BILIBILI_SCREENSHOT");
        if (!output.isEmpty()) { const auto frame = window.grabWindow(); QVERIFY(!frame.isNull()); QVERIFY(frame.save(output)); }
        model.cancelLogin();
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
        delete item;
    }
};
QTEST_MAIN(QmlTests)
#include "QmlTests.moc"
