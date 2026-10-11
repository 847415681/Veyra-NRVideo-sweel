// Private test executable only: installed into a copy of app main by the build script.
// QScreen::grabWindow reads PRESENTED pixels. QQuickWindow::grabWindow would render
// afresh and could hide the very backing-store corruption being tested.
#pragma once
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScreen>
#include <QTimer>
#include <algorithm>
#include <memory>

inline QQuickItem* damageProbeFind(QQuickItem* root, const QString& name) {
    if (!root) return nullptr;
    if (root->objectName() == name) return root;
    for (auto* child : root->childItems())
        if (auto* found = damageProbeFind(child, name)) return found;
    return nullptr;
}

inline void damageProbeImages(QQuickItem* root, int& count, int& notReady) {
    if (!root) return;
    if (root->objectName() == QStringLiteral("software-icon-image") && root->isVisible()) {
        ++count;
        if (root->property("status").toInt() != 1) ++notReady; // Image.Ready
    }
    for (auto* child : root->childItems()) damageProbeImages(child, count, notReady);
}

inline void installSoftwareUiDamageProbe(QQuickWindow* window, QObject* bridge) {
    const auto evidence = qEnvironmentVariable("VEYRA_TEST_DAMAGE_EVIDENCE");
    if (evidence.isEmpty()) return;
    QDir().mkpath(evidence);
    struct State {
        QPointer<QQuickItem> flick;
        QTimer* scroll = nullptr;
        QElapsedTimer render;
        int frames = 0, steps = 0, idleStart = 0;
        qint64 renderNs = 0;
        QJsonObject result;
    };
    const auto state = std::make_shared<State>();
    QTimer::singleShot(700, window, [bridge, state] {
        while (bridge->property("nrLayers").toList().size() < 3) {
            int added = -1;
            if (!QMetaObject::invokeMethod(bridge, "addEffect", Q_RETURN_ARG(int, added), Q_ARG(QString, QStringLiteral("nr"))) || added < 0) break;
        }
        bridge->setProperty("nrEnabled", true);
        state->result["nrLayers"] = bridge->property("nrLayers").toList().size();
    });
    QTimer::singleShot(1300, window, [window, state] {
        auto* card = damageProbeFind(window->contentItem(), QStringLiteral("nr-card-1"));
        state->result["firstLayerExpanded"] = card && card->setProperty("open", true);
    });
    QObject::connect(window, &QQuickWindow::beforeRendering, window, [state] { state->render.start(); }, Qt::DirectConnection);
    QObject::connect(window, &QQuickWindow::afterRendering, window, [state] {
        ++state->frames; state->renderNs += state->render.nsecsElapsed();
    }, Qt::DirectConnection);
    const auto capture = [window, state, evidence](const QString& name) {
        const int before = state->frames;
        const auto pixels = window->screen()->grabWindow(window->winId());
        const bool saved = !pixels.isNull() && pixels.save(evidence + "/" + name + ".png");
        state->result[name + "Saved"] = saved;
        state->result[name + "GrabTriggeredRender"] = state->frames != before;
        if (state->flick) {
            state->result[name + "ContentY"] = state->flick->property("contentY").toDouble();
            state->result[name + "ContentHeight"] = state->flick->property("contentHeight").toDouble();
        }
        state->result["pixelWidth"] = pixels.width();
        state->result["pixelHeight"] = pixels.height();
        int images = 0, notReady = 0;
        damageProbeImages(window->contentItem(), images, notReady);
        state->result[name + "RasterIcons"] = images;
        state->result[name + "RasterIconsNotReady"] = notReady;
    };
    const auto finish = [state, evidence] {
        state->result["framesInThreeIdleSeconds"] = state->frames - state->idleStart;
        state->result["totalFrames"] = state->frames;
        state->result["renderMs"] = double(state->renderNs) / 1000000.0;
        state->result["scrollSteps"] = state->steps;
        const auto json = QJsonDocument(state->result).toJson(QJsonDocument::Compact);
        QFile file(evidence + "/probe.json"); if (file.open(QIODevice::WriteOnly)) file.write(json);
        qInfo().noquote() << "SOFTWARE_DAMAGE_PROBE" << json;
        QCoreApplication::quit();
    };
    QTimer::singleShot(2000, window, [window, state, capture, finish] {
        auto* bar = damageProbeFind(window->contentItem(), QStringLiteral("pro-inspector-scrollbar"));
        auto* flick = bar ? bar->parentItem() : nullptr;
        while (flick && !flick->property("contentY").isValid()) flick = flick->parentItem();
        state->flick = flick;
        state->result["foundInspector"] = flick && flick->isVisible();
        if (!flick) return;
        state->result["overflow"] = flick->property("contentHeight").toDouble() - flick->height();
        const auto top = flick->mapToScene(QPointF());
        state->result["inspectorX"] = top.x(); state->result["inspectorY"] = top.y();
        state->result["inspectorWidth"] = flick->width(); state->result["inspectorHeight"] = flick->height();
        flick->setProperty("interactive", false);
        flick->setProperty("contentY", 0.0);
        state->scroll = new QTimer(window); state->scroll->setInterval(16);
        QObject::connect(state->scroll, &QTimer::timeout, window, [window, state, capture, finish] {
            if (!state->flick) { state->scroll->stop(); return; }
            ++state->steps;
            const double maximum = std::max(0.0, state->flick->property("contentHeight").toDouble() - state->flick->height());
            state->flick->setProperty("contentY", std::min(maximum, state->steps * 1.25));
            if (state->steps >= 80) {
                state->scroll->stop();
                QTimer::singleShot(700, window, [window, state, capture, finish] {
                    capture("scrolled"); state->result["framesBeforeReference"] = state->frames;
                    const double scrollY = state->flick->property("contentY").toDouble();
                    window->resize(window->width() + 1, window->height());
                    QTimer::singleShot(200, window, [window, state, capture, finish, scrollY] {
                        window->resize(window->width() - 1, window->height());
                        QTimer::singleShot(400, window, [window, state, capture, finish, scrollY] {
                            state->flick->setProperty("contentY", scrollY);
                            QTimer::singleShot(700, window, [window, state, capture, finish] {
                                capture("reference"); state->idleStart = state->frames;
                                QTimer::singleShot(3000, window, finish);
                            });
                        });
                    });
                });
            }
        });
        QTimer::singleShot(250, window, [state, capture] { capture("initial"); state->scroll->start(); });
    });
}
