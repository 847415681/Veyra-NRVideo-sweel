#pragma once
#include "veyra/bilibili/Playback.h"
#include "veyra/bilibili/StreamServer.h"
#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QTimer>
#include <QVariantList>
#include <functional>

namespace veyra::ui {
class BilibiliCookieJar;
class BilibiliModel final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString title READ title NOTIFY changed)
    Q_PROPERTY(QString address READ address NOTIFY changed)
    Q_PROPERTY(QVariantList parts READ parts NOTIFY changed)
    Q_PROPERTY(QVariantList qualities READ qualities NOTIFY changed)
    Q_PROPERTY(int selectedPart READ selectedPart NOTIFY changed)
    Q_PROPERTY(QString account READ account NOTIFY loginChanged)
    Q_PROPERTY(QString loginStatus READ loginStatus NOTIFY loginChanged)
    Q_PROPERTY(QString qrImage READ qrImage NOTIFY loginChanged)
public:
    explicit BilibiliModel(QString dataDirectory, QObject* parent = nullptr,
                           QNetworkAccessManager* network = nullptr);
    ~BilibiliModel() override;
    bool busy() const { return busy_; }
    QString status() const { return status_; }
    QString title() const { return title_; }
    QString address() const { return address_; }
    QVariantList parts() const { return parts_; }
    QVariantList qualities() const { return qualities_; }
    int selectedPart() const { return selectedPart_; }
    QString account() const { return account_; }
    QString loginStatus() const { return loginStatus_; }
    QString qrImage() const { return qrImage_; }
    Q_INVOKABLE void inspect(const QString& input);
    Q_INVOKABLE void selectPart(int index);
    Q_INVOKABLE void play(int quality = 0);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void beginLogin();
    Q_INVOKABLE void cancelLogin();
    Q_INVOKABLE void logout();
    Q_INVOKABLE void refreshAccount();
    // Called by the player when another source/stop supersedes this operation.
    void releasePlayback();
    bool ownsPlayback(const QString& url) const { return !playbackUrl_.isEmpty() && playbackUrl_ == url; }
signals:
    void changed();
    void loginChanged();
    void playbackReady(const QUrl& url, const QString& title);
    void notice(const QString& message, bool error);
private:
    using JsonCallback = std::function<void(const QJsonObject&)>;
    void json(const QUrl& url, int epoch, bool login, JsonCallback callback);
    void followShort(const QUrl& url, int redirects, int epoch);
    void inspectLink(const bilibili::Link& link, int epoch);
    void loadVideo(const QJsonObject& data, const bilibili::Link& link);
    void requestPlay(int quality, int epoch);
    void resolvePlay(int quality, int epoch);
    void loadPlay(const QJsonObject& data, int quality, int epoch);
    void loadLive(int quality, int epoch);
    void prepareDash(bilibili::Track video, bilibili::Track audio, int epoch);
    void fetchIndex(const bilibili::Track& track, int epoch, std::function<void(const bilibili::Index&)> callback);
    void ready(const QUrl& url, int epoch);
    void fail(const QString& message);
    void pollLogin(int epoch);
    bool saveCookies();
    void loadCookies();
    QNetworkAccessManager* network_ = nullptr;
    BilibiliCookieJar* cookies_ = nullptr;
    bilibili::StreamServer server_;
    QList<QPointer<QNetworkReply>> playbackReplies_, loginReplies_;
    QTimer loginTimer_;
    QString cookieFile_, status_, title_, address_, playbackUrl_;
    QString account_, loginStatus_, qrImage_, qrKey_, imageKey_, subKey_;
    bilibili::Link link_;
    QVariantList parts_, qualities_;
    int selectedPart_ = 0, epoch_ = 0, loginEpoch_ = 0;
    bool busy_ = false, describeOnly_ = false, preview_ = false;
};
}
