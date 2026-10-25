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
#include <QJSValue>

class QmlTests : public QObject {
    Q_OBJECT
    static QQuickItem* visibleText(QQuickItem* root, const QString& text) {
        if (root->isVisible() && root->property("text").toString() == text) return root;
        for (auto* item : root->childItems()) if (auto* found = visibleText(item, text)) return found;
        return nullptr;
    }
    static void clickItem(QQuickWindow* window, QQuickItem* item) {
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, item->mapToScene(QPointF(item->width()/2, item->height()/2)).toPoint());
    }
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
        auto* partsMenu = item->findChild<QObject*>("bilibili-parts-menu"); QVERIFY(partsMenu);
        QTest::qWait(100);
        clickItem(&window, qobject_cast<QQuickItem*>(parts));
        QTRY_VERIFY(partsMenu->property("visible").toBool());
        QTRY_VERIFY(visibleText(window.contentItem(), QString::fromUtf8("P2 · 第二部分")));
        auto* partLabel = visibleText(window.contentItem(), QString::fromUtf8("P2 · 第二部分"));
        QVERIFY(partLabel->property("color").value<QColor>().lightnessF() > 0.6);
        QTest::qWait(500); // Let the menu's transform settle before clicking a row.
        const auto output = qEnvironmentVariable("VEYRA_BILIBILI_SCREENSHOT");
        if (!output.isEmpty()) QVERIFY(window.grabWindow().save(output + ".parts.png"));
        clickItem(&window, partLabel);
        QTRY_COMPARE_WITH_TIMEOUT(model.selectedPart(), 1, 2000);
        QTRY_VERIFY_WITH_TIMEOUT(!model.busy(), 2000); QCOMPARE(model.selectedPart(), 1);
        auto* quality = item->findChild<QQuickItem*>("bilibili-quality"); QVERIFY(quality);
        QTest::qWait(500); clickItem(&window, quality);
        auto* qualityMenu = item->findChild<QObject*>("bilibili-quality-menu"); QVERIFY(qualityMenu);
        QTRY_VERIFY(qualityMenu->property("visible").toBool());
        QTRY_VERIFY(visibleText(window.contentItem(), "720P"));
        QTest::qWait(500);
        if (!output.isEmpty()) QVERIFY(window.grabWindow().save(output + ".quality.png"));
        clickItem(&window, visibleText(window.contentItem(), "720P"));
        QTRY_COMPARE(quality->property("selectedQuality").toInt(), 64);
        model.beginLogin(); QTRY_VERIFY_WITH_TIMEOUT(!model.qrImage().isEmpty(), 2000);
        auto* image = item->findChild<QObject*>("bilibili-login-qr"); QVERIFY(image);
        QTRY_VERIFY_WITH_TIMEOUT(image->property("visible").toBool(), 2000);
        QTest::qWait(300);
        if (!output.isEmpty()) { const auto frame = window.grabWindow(); QVERIFY(!frame.isNull()); QVERIFY(frame.save(output)); }
        model.cancelLogin();
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
        delete item;
    }
};
QTEST_MAIN(QmlTests)
#include "QmlTests.moc"
