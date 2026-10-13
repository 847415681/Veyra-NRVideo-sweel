#pragma once
#include <QObject>
#include <QByteArray>
#include <QUrl>
#include <QTimer>
#include <QNetworkAccessManager>
#include <QNetworkReply>

namespace veyra::ui {
// UI-only, bounded asynchronous metadata request; never touches the engine.
class UpdateChecker final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool checking READ checking NOTIFY checkingChanged)
    Q_PROPERTY(QString latestVersion READ latestVersion NOTIFY releaseChanged)
    Q_PROPERTY(QUrl releaseUrl READ releaseUrl NOTIFY releaseChanged)
public:
    explicit UpdateChecker(QString currentVersion, QObject* parent = nullptr,
        QUrl endpoint = QUrl(QStringLiteral("https://api.github.com/repos/Likely7/Veyra-NRVideo/releases/latest")),
        int timeoutMs = 12000);
    ~UpdateChecker() override;
    bool checking() const { return reply_ != nullptr; }
    QString latestVersion() const { return latestVersion_; }
    QUrl releaseUrl() const { return releaseUrl_; }
    Q_INVOKABLE void startAutomaticCheck();
    Q_INVOKABLE void check(bool manual = true);
    Q_INVOKABLE void openRelease();
signals:
    void checkingChanged();
    void releaseChanged();
    void updateAvailable(const QString& version);
    void notice(const QString& message, bool isError);
    // Terminal diagnostic result (also used by deterministic/real HTTP tests).
    void completed(const QString& result);
private:
    void finish();
    QNetworkAccessManager network_;
    QTimer deadline_;
    QNetworkReply* reply_ = nullptr;
    QString currentVersion_, latestVersion_;
    QUrl endpoint_, releaseUrl_;
    QByteArray body_;
    QString failure_;
    int timeoutMs_;
    bool automaticScheduled_ = false, manual_ = false;
};
}
