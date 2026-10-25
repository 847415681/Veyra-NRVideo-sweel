#include "veyra/ui/BilibiliModel.h"
#include <QGuiApplication>
#include <QTemporaryDir>
#include <QTimer>
#include <QTextStream>

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() < 2) return 2;
    QTemporaryDir data;
    veyra::ui::BilibiliModel model(data.path());
    if (args.contains("--qr")) {
        bool generated = false;
        QObject::connect(&model, &veyra::ui::BilibiliModel::loginChanged, &app, [&] {
            QTextStream(stdout) << "login=" << model.loginStatus() << Qt::endl;
            if (!generated && !model.qrImage().isEmpty()) {
                generated = true;
                QTextStream(stdout) << "QR_GENERATED_LOCALLY" << Qt::endl;
                QTimer::singleShot(5000, &app, [&] { model.cancelLogin(); app.exit(0); });
            }
        });
        QTimer::singleShot(30000, &app, [&] { app.exit(1); });
        model.beginLogin(); return app.exec();
    }
    bool requested = false;
    QObject::connect(&model, &veyra::ui::BilibiliModel::changed, &app, [&] {
        QTextStream(stdout) << "state busy=" << model.busy() << " parts=" << model.parts().size() << " status=" << model.status() << Qt::endl;
        if (!model.busy() && !model.parts().isEmpty() && !requested) {
            requested = true;
            if (args.contains("--inspect")) { QTextStream(stdout) << "METADATA_READY qualities=" << model.qualities().size() << Qt::endl; app.exit(0); }
            else QTimer::singleShot(0, &model, [&] { model.play(); });
        }
    });
    QObject::connect(&model, &veyra::ui::BilibiliModel::notice, &app, [&](const QString& text, bool error) {
        QTextStream(stdout) << "notice=" << text << Qt::endl; if (error) app.exit(1);
    });
    QObject::connect(&model, &veyra::ui::BilibiliModel::playbackReady, &app, [&](const QUrl& url, const QString& title) {
        // Only the ephemeral loopback URL is printed. Signed CDN URLs/cookies never are.
        QTextStream(stdout) << "READY " << url.toString() << " title=" << title << Qt::endl;
        QTimer::singleShot(180000, &app, &QCoreApplication::quit);
    });
    QTimer::singleShot(240000, &app, [&] { app.exit(3); });
    model.inspect(args.last()); return app.exec();
}
