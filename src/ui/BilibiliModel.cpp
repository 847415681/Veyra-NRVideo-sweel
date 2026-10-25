#include "veyra/ui/BilibiliModel.h"
#include "qrcodegen.hpp"
#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkCookieJar>
#include <QNetworkCookie>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QSet>
#include <QUrlQuery>
#include <algorithm>
#include <cmath>
#include <memory>
#ifdef Q_OS_WIN
#include <windows.h>
#include <wincrypt.h>
#endif

namespace veyra::ui {
using namespace bilibili;
namespace {
constexpr qsizetype maxJson = 4 * 1024 * 1024;
QUrl endpoint(const QString& path, const QMap<QString, QString>& params = {}, const QString& host = "api.bilibili.com") {
    QUrl url("https://" + host + path); QUrlQuery query;
    for (auto it = params.cbegin(); it != params.cend(); ++it) query.addQueryItem(it.key(), it.value());
    url.setQuery(query); return url;
}
QString number(const QJsonValue& value) { return value.isString() ? value.toString() : QString::number(qint64(value.toDouble())); }
QList<QUrl> locations(const Track& track) { QList<QUrl> urls{track.url}; urls.append(track.backups); return urls; }
QByteArray protect(const QByteArray& bytes, bool decrypt) {
#ifdef Q_OS_WIN
    DATA_BLOB input{DWORD(bytes.size()), reinterpret_cast<BYTE*>(const_cast<char*>(bytes.constData()))}, output{};
    const BOOL ok = decrypt ? CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)
                            : CryptProtectData(&input, L"Veyra Bilibili session", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output);
    if (!ok) return {};
    QByteArray result(reinterpret_cast<char*>(output.pbData), qsizetype(output.cbData));
    SecureZeroMemory(output.pbData, output.cbData); LocalFree(output.pbData); return result;
#else
    Q_UNUSED(bytes); Q_UNUSED(decrypt); return {}; // Never fall back to plaintext credentials.
#endif
}
QString qrPng(const QString& text) {
    const auto qr = qrcodegen::QrCode::encodeText(text.toUtf8().constData(), qrcodegen::QrCode::Ecc::MEDIUM);
    const int scale = 5, border = 4, size = (qr.getSize() + border * 2) * scale;
    QImage image(size, size, QImage::Format_RGB32); image.fill(Qt::white);
    for (int y = 0; y < qr.getSize(); ++y) for (int x = 0; x < qr.getSize(); ++x) if (qr.getModule(x, y))
        for (int dy = 0; dy < scale; ++dy) for (int dx = 0; dx < scale; ++dx) image.setPixelColor((x + border) * scale + dx, (y + border) * scale + dy, Qt::black);
    QByteArray bytes; QBuffer buffer(&bytes); buffer.open(QIODevice::WriteOnly); image.save(&buffer, "PNG");
    return "data:image/png;base64," + QString::fromLatin1(bytes.toBase64());
}
}
class BilibiliCookieJar final : public QNetworkCookieJar {
public:
    using QNetworkCookieJar::QNetworkCookieJar;
    QByteArray serialize() const {
        QJsonArray array;
        for (const auto& cookie : allCookies()) array.append(QString::fromLatin1(cookie.toRawForm().toBase64()));
        return QJsonDocument(array).toJson(QJsonDocument::Compact);
    }
    void restore(const QByteArray& bytes) {
        QList<QNetworkCookie> list;
        for (const auto& item : QJsonDocument::fromJson(bytes).array()) for (const auto& cookie : QNetworkCookie::parseCookies(QByteArray::fromBase64(item.toString().toLatin1())))
            if ((cookie.domain() == "bilibili.com" || cookie.domain().endsWith(".bilibili.com")) && (cookie.isSessionCookie() || cookie.expirationDate() > QDateTime::currentDateTimeUtc())) list.append(cookie);
        setAllCookies(list);
    }
    void clear() { setAllCookies({}); }
};
BilibiliModel::BilibiliModel(QString dataDirectory, QObject* parent, QNetworkAccessManager* network) : QObject(parent) {
    cookieFile_ = QDir(dataDirectory).filePath("bilibili-session.dpapi");
    network_ = network ? network : new QNetworkAccessManager(this);
    cookies_ = new BilibiliCookieJar(network_); network_->setCookieJar(cookies_); loadCookies();
    loginTimer_.setSingleShot(true); loginTimer_.setInterval(2500);
    connect(&loginTimer_, &QTimer::timeout, this, [this] { pollLogin(loginEpoch_); });
    loginStatus_ = tr("未登录，可播放公开内容");
}
BilibiliModel::~BilibiliModel() { cancel(); cancelLogin(); }
void BilibiliModel::json(const QUrl& url, int epoch, bool login, JsonCallback callback, bool optional) {
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", userAgent()); request.setRawHeader("Referer", "https://www.bilibili.com/");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(15000);
    auto* reply = network_->get(request); reply->setReadBufferSize(maxJson + 1);
    (login ? loginReplies_ : playbackReplies_).append(reply);
    auto body = std::make_shared<QByteArray>();
    connect(reply, &QIODevice::readyRead, this, [reply, body] {
        body->append(reply->read(maxJson + 1 - body->size())); if (body->size() > maxJson) reply->abort();
    });
    QTimer::singleShot(20000, reply, [reply] { if (!reply->isFinished()) reply->abort(); });
    connect(reply, &QNetworkReply::finished, this, [this, reply, body, epoch, login, optional, callback = std::move(callback)] {
        auto& requests = login ? loginReplies_ : playbackReplies_; requests.removeAll(reply);
        reply->deleteLater();
        if (epoch != (login ? loginEpoch_ : epoch_)) return;
        if (reply->isOpen()) body->append(reply->read(maxJson + 1 - body->size()));
        QJsonParseError error; const auto doc = QJsonDocument::fromJson(*body, &error);
        if (body->size() > maxJson || reply->error() != QNetworkReply::NoError || reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200 || error.error != QJsonParseError::NoError || !doc.isObject()) {
            if (optional) { callback({}); return; }
            const QString text = tr("Bilibili 请求失败，请检查网络或稍后重试");
            if (login) { loginStatus_ = text; loginTimer_.stop(); emit loginChanged(); } else fail(text);
            return;
        }
        callback(doc.object());
    });
}
void BilibiliModel::fail(const QString& message) { busy_ = false; status_ = message; emit changed(); emit notice(message, true); }
void BilibiliModel::cancel() {
    ++epoch_; const auto pending = playbackReplies_; playbackReplies_.clear();
    for (const auto& reply : pending) if (reply) reply->abort();
    if (busy_) status_ = tr("已取消解析");
    busy_ = false; emit changed();
}
void BilibiliModel::releasePlayback() {
    cancel(); server_.clear(); playbackUrl_.clear(); activeLink_ = {}; activeParts_.clear(); activeQualities_.clear(); activeAudioTracks_.clear();
    replaying_ = false; activeWindow_ = 0; pausedLiveTime_ = 0; emit changed();
}
void BilibiliModel::inspect(const QString& input) {
    cancel(); describeOnly_ = true; parts_.clear(); qualities_.clear(); selectedPart_ = 0; title_.clear(); address_ = input.trimmed(); link_ = parseLink(input);
    if (link_.kind == Link::Invalid) { fail(tr("请输入 BV/AV 号、B 站视频、番剧、直播或 b23.tv 链接")); return; }
    busy_ = true; status_ = tr("正在获取视频信息…"); emit changed();
    if (link_.kind == Link::Short) followShort(link_.url, 0, epoch_); else inspectLink(link_, epoch_);
}
void BilibiliModel::followShort(const QUrl& url, int redirects, int epoch) {
    if (redirects >= 5) { fail(tr("短链接跳转过多")); return; }
    QNetworkRequest request(url); request.setRawHeader("User-Agent", userAgent());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy); request.setTransferTimeout(10000);
    auto* reply = network_->head(request); playbackReplies_.append(reply);
    QTimer::singleShot(15000, reply, [reply] { if (!reply->isFinished()) reply->abort(); });
    connect(reply, &QNetworkReply::finished, this, [this, reply, redirects, epoch] {
        playbackReplies_.removeAll(reply); reply->deleteLater(); if (epoch != epoch_) return;
        const auto target = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
        const auto link = parseLink(reply->url().resolved(target).toString());
        if (target.isEmpty() || link.kind == Link::Invalid) { fail(tr("无法解析此 B 站短链接")); return; }
        if (link.kind == Link::Short) followShort(link.url, redirects + 1, epoch); else inspectLink(link, epoch);
    });
}
void BilibiliModel::inspectLink(const Link& link, int epoch) {
    link_ = link; address_ = link.url.toString();
    if (link.kind == Link::Live) {
        json(endpoint("/room/v1/Room/get_info", {{"room_id", link.id}}, "api.live.bilibili.com"), epoch, false, [this](const QJsonObject& root) {
            const auto data = root.value("data").toObject();
            if (root.value("code").toInt(-1) != 0 || data.value("live_status").toInt() != 1) { fail(tr("直播间不存在或尚未开播")); return; }
            link_.id = number(data.value("room_id")); title_ = data.value("title").toString();
            parts_ = {QVariantMap{{"label", title_}, {"cid", link_.id}}};
            loadLive(0, epoch_);
        });
    } else if (link.kind == Link::Video) {
        json(endpoint("/x/web-interface/view", {{link.id.startsWith("BV") ? "bvid" : "aid", link.id.startsWith("BV") ? link.id : link.id.mid(2)}}), epoch, false, [this, link](const QJsonObject& root) {
            if (root.value("code").toInt(-1) != 0) { fail(tr("视频不可用、已删除或需要登录")); return; }
            loadVideo(root.value("data").toObject(), link);
        });
    } else {
        json(endpoint("/pgc/view/web/season", {{link.kind == Link::Episode ? "ep_id" : "season_id", link.id.mid(2)}}), epoch, false, [this, link](const QJsonObject& root) {
            if (root.value("code").toInt(-1) != 0) { fail(tr("番剧不可用，可能需要登录或受地区限制")); return; }
            const auto result = root.value("result").toObject(); title_ = result.value("title").toString(); parts_.clear();
            bool found = link.kind != Link::Episode;
            const auto episodes = result.value("episodes").toArray();
            for (const auto& value : episodes) {
                const auto ep = value.toObject();
                if (link.kind == Link::Episode && number(ep.value("id")) == link.id.mid(2)) { selectedPart_ = int(parts_.size()); found = true; }
                parts_.append(QVariantMap{{"label", ep.value("title").toString() + " " + ep.value("long_title").toString()}, {"cid", number(ep.value("cid"))}, {"aid", number(ep.value("aid"))}, {"ep", number(ep.value("id"))}});
            }
            if (parts_.isEmpty()) { fail(tr("此番剧没有可播放的剧集")); return; }
            if (!found) { parts_.clear(); fail(tr("链接指定的剧集不在此番剧列表中")); return; }
            requestPlay(0, epoch_);
        });
    }
}
void BilibiliModel::loadVideo(const QJsonObject& data, const Link& link) {
    title_ = data.value("title").toString(); parts_.clear();
    for (const auto& value : data.value("pages").toArray()) {
        const auto part = value.toObject();
        parts_.append(QVariantMap{{"label", "P" + number(part.value("page")) + " · " + part.value("part").toString()}, {"cid", number(part.value("cid"))}, {"aid", number(data.value("aid"))}, {"bvid", data.value("bvid").toString()}});
    }
    if (parts_.isEmpty()) { fail(tr("此视频没有可播放的分 P")); return; }
    if (link.page > parts_.size()) { parts_.clear(); fail(tr("链接指定的分 P 不存在")); return; }
    selectedPart_ = link.page - 1;
    requestPlay(0, epoch_);
}
void BilibiliModel::selectPart(int index) {
    if (index < 0 || index >= parts_.size() || busy_) return;
    selectedPart_ = index; describeOnly_ = true; busy_ = true;
    status_ = tr("正在获取可用画质…"); emit changed(); requestPlay(0, epoch_);
}
void BilibiliModel::play(int quality) {
    if (busy_ || parts_.isEmpty()) return;
    cancel(); describeOnly_ = false; busy_ = true; status_ = tr("正在解析播放地址…"); emit changed();
    operation_ = NewPlayback; requestedAudio_ = 0; requestedQuality_ = quality; pendingLiveTarget_ = 0; startPlaying_ = false;
    requestPlay(quality, epoch_);
}
void BilibiliModel::switchPlayback(int part, int quality, int audio, int operation, double target) {
    if (!active() || part < 0 || part >= activeParts_.size()) return;
    cancel();
    // Playback controls use the committed session, even if the user has inspected
    // another URL in the source dialog. Never let that metadata replace the movie.
    link_ = activeLink_; title_ = activeTitle_; address_ = activeAddress_; parts_ = activeParts_;
    selectedPart_ = part; qualities_ = activeQualities_; audioTracks_ = activeAudioTracks_;
    operation_ = operation; requestedAudio_ = audio; requestedQuality_ = quality; pendingLiveTarget_ = target; startPlaying_ = false;
    describeOnly_ = false; busy_ = true; status_ = tr("正在切换播放…"); emit changed();
    requestPlay(quality, epoch_);
}
void BilibiliModel::changePart(int index) {
    if (index == activePart_ || live()) return;
    switchPlayback(index, 0, 0, Part);
}
void BilibiliModel::changeQuality(int quality) {
    switchPlayback(activePart_, quality, activeAudio_, Quality, live() && liveLag() > 3 ? QDateTime::currentMSecsSinceEpoch() / 1000.0 - liveLag() : 0);
}
void BilibiliModel::changeAudio(int id) {
    if (live() || id == activeAudio_) return;
    switchPlayback(activePart_, activeRequestedQuality_, id, Audio);
}
double BilibiliModel::timeshiftDuration() const {
    return timeshiftAvailable() ? std::clamp(QDateTime::currentMSecsSinceEpoch() / 1000.0 - activeLiveStart_, 0.0, activeWindow_) : 0;
}
double BilibiliModel::liveLag() const {
    if (!live()) return 0;
    const double now = QDateTime::currentMSecsSinceEpoch() / 1000.0;
    if (replaying_) return std::max(0.0, now - playbackStart_ - clockPosition_);
    return pausedLiveTime_ > 0 ? std::max(0.0, now - pausedLiveTime_) : 0;
}
double BilibiliModel::timeshiftPosition() const { return std::max(0.0, timeshiftDuration() - liveLag()); }
void BilibiliModel::updatePlaybackClock(double seconds, bool paused) {
    if (!live()) return;
    // The engine can report decoded frames before its first presented PTS; the
    // position is still zero during that short startup interval.
    if (seconds <= 0 || !std::isfinite(seconds)) return;
    // Live container PTS can start days into a broadcast. Anchor to the first
    // presented timestamp, never treat it as elapsed time since this open.
    if (nativeClockStart_ < 0) nativeClockStart_ = seconds;
    clockPosition_ = std::max(0.0, seconds - nativeClockStart_);
    if (!replaying_) {
        if (paused && pausedLiveTime_ == 0) pausedLiveTime_ = QDateTime::currentMSecsSinceEpoch() / 1000.0;
        if (!paused) pausedLiveTime_ = 0;
    }
}
void BilibiliModel::seekTimeshift(double seconds) {
    if (!timeshiftAvailable() || !std::isfinite(seconds)) return;
    const double lag = std::clamp(timeshiftDuration() - seconds, 0.0, timeshiftDuration());
    if (lag <= 3) { returnToLive(); return; }
    switchPlayback(activePart_, activeRequestedQuality_, 0, Timeshift, QDateTime::currentMSecsSinceEpoch() / 1000.0 - lag);
}
void BilibiliModel::returnToLive() {
    if (live()) { switchPlayback(activePart_, activeRequestedQuality_, 0, Timeshift); startPlaying_ = true; }
}
void BilibiliModel::resumeLive() {
    const double lag = liveLag();
    if (timeshiftAvailable() && lag > 3) seekTimeshift(timeshiftDuration() - lag);
    else returnToLive();
    startPlaying_ = true;
}
void BilibiliModel::requestPlay(int quality, int epoch) {
    if (link_.kind == Link::Live) { loadLive(quality, epoch); return; }
    if (link_.kind != Link::Video) { resolvePlay(quality, epoch); return; }
    json(endpoint("/x/web-interface/nav"), epoch, false, [this, quality, epoch](const QJsonObject& root) {
        const auto keys = root.value("data").toObject().value("wbi_img").toObject();
        imageKey_ = QUrl(keys.value("img_url").toString()).fileName().section('.', 0, 0);
        subKey_ = QUrl(keys.value("sub_url").toString()).fileName().section('.', 0, 0);
        if (imageKey_.size() != 32 || subKey_.size() != 32) { fail(tr("无法获取 Bilibili 播放签名，请稍后重试")); return; }
        resolvePlay(quality, epoch);
    });
}
void BilibiliModel::resolvePlay(int quality, int epoch) {
    const auto part = parts_.value(selectedPart_).toMap();
    QMap<QString, QString> params{{"cid", part.value("cid").toString()}, {"qn", QString::number(quality > 0 ? quality : 127)}, {"fnval", "4048"}, {"fnver", "0"}, {"fourk", "1"}};
    QUrl url;
    if (link_.kind == Link::Video) {
        params["bvid"] = part.value("bvid").toString();
        url = endpoint("/x/player/wbi/playurl"); url.setQuery(signedQuery(params, imageKey_, subKey_, QDateTime::currentSecsSinceEpoch()));
    } else { params["ep_id"] = part.value("ep").toString(); url = endpoint("/pgc/player/web/playurl", params); }
    json(url, epoch, false, [this, quality, epoch](const QJsonObject& root) {
        if (root.value("code").toInt(-1) != 0) { fail(tr("无法播放：请确认登录状态、会员权限或地区限制（%1）").arg(root.value("code").toInt())); return; }
        auto data = root.value("data").toObject(); if (data.isEmpty()) data = root.value("result").toObject();
        loadPlay(data, quality, epoch);
    });
}
void BilibiliModel::loadPlay(const QJsonObject& data, int quality, int epoch) {
    if (data.value("is_drm").toBool() || data.value("is_drm").toInt() != 0) { fail(tr("此内容受 DRM 保护，无法在此播放器中播放")); return; }
    preview_ = data.value("is_preview").toBool() || data.value("is_preview").toInt() != 0;
    audioTracks_.clear(); resolvedAudio_ = 0; liveWindow_ = 0;
    const auto dash = data.value("dash").toObject();
    QList<Track> audios;
    QJsonArray audioObjects = dash.value("audio").toArray();
    for (const auto& item : dash.value("dolby").toObject().value("audio").toArray()) audioObjects.append(item);
    const auto flac = dash.value("flac").toObject().value("audio").toObject();
    if (!flac.isEmpty()) audioObjects.append(flac);
    QSet<int> audioIds;
    for (const auto& item : audioObjects) {
        const auto object = item.toObject(); const auto track = parseTrack(object);
        if (track.url.isEmpty() || !track.initialization.valid() || !track.index.valid() || audioIds.contains(track.quality)) continue;
        audioIds.insert(track.quality); audios.append(track);
        QString label = track.quality == 30251 ? tr("Hi-Res 无损") : track.quality == 30250 ? tr("杜比全景声") : QStringLiteral("AAC · %1 kbps").arg(track.bandwidth / 1000);
        const auto language = object.value("lang").toString(object.value("language").toString());
        const auto title = object.value("title").toString();
        if (!language.isEmpty()) label = language + " · " + label;
        if (!title.isEmpty()) label = title + " · " + label;
        audioTracks_.append(QVariantMap{{"id", track.quality}, {"label", label}});
    }
    QList<Track> videos; QSet<int> available;
    for (const auto& item : dash.value("video").toArray()) {
        const auto track = parseTrack(item.toObject());
        if (!track.url.isEmpty() && track.initialization.valid() && track.index.valid()) { videos.append(track); available.insert(track.quality); }
    }
    if (videos.isEmpty()) {
        const auto durl = data.value("durl").toArray();
        if (durl.size() == 1) {
            const QUrl url(durl.first().toObject().value("url").toString());
            if (mediaUrlAllowed(url)) {
                resolvedQuality_ = data.value("quality").toInt(); resolvedQualityLabel_ = qualityLabel(resolvedQuality_);
                qualities_ = {QVariantMap{{"id", resolvedQuality_}, {"label", resolvedQualityLabel_}}};
                if (describeOnly_) {
                    busy_ = false; status_ = preview_ ? tr("当前为试看，仅播放账号有权观看的片段") : tr("已就绪，选择画质后播放"); emit changed(); return;
                }
                if (quality > 0 && quality != resolvedQuality_) { fail(tr("当前账号无法播放所选画质，请选择可用画质")); return; }
                ready(server_.install({}, {{"video.mp4", {url}}}, "video.mp4"), epoch); return;
            }
        }
        fail(tr("未获得可播放的音视频流，可能需要登录或购买此内容")); return;
    }
    QList<int> sorted = available.values(); std::sort(sorted.begin(), sorted.end(), std::greater<int>());
    qualities_ = {QVariantMap{{"id", 0}, {"label", tr("自动 · 最高可用画质")}}};
    for (int id : sorted) qualities_.append(QVariantMap{{"id", id}, {"label", qualityLabel(id)}});
    if (describeOnly_) { busy_ = false; status_ = preview_ ? tr("当前为试看，仅播放账号有权观看的片段") : tr("已就绪，选择画质后播放"); emit changed(); return; }
    int chosen = quality;
    if (chosen <= 0) {
        chosen = sorted.first();
        // HDR and Dolby Vision are opt-in; prefer the highest available SDR representation.
        for (int id : sorted) if (id != 125 && id != 126) { chosen = id; break; }
    }
    if (!available.contains(chosen)) { fail(tr("当前账号无法播放所选画质，请选择可用画质")); return; }
    std::sort(videos.begin(), videos.end(), [](const Track& a, const Track& b) {
        // Prefer AVC for decoder compatibility at the selected resolution, then HEVC, then AV1.
        const auto rank = [](const QString& codec) { return codec.startsWith("avc") ? 0 : codec.startsWith("hev") || codec.startsWith("hvc") ? 1 : 2; };
        return rank(a.codecs) < rank(b.codecs);
    });
    Track video, audio;
    for (const auto& track : videos) if (track.quality == chosen) { video = track; break; }
    for (const auto& track : audios) {
        if (requestedAudio_ > 0) { if (track.quality == requestedAudio_) { audio = track; break; } }
        else if ((track.codecs.startsWith("mp4a") || audios.size() == 1) && track.bandwidth > audio.bandwidth) audio = track;
    }
    if (requestedAudio_ <= 0 && audio.url.isEmpty() && !audios.isEmpty()) audio = audios.first();
    if ((!audioObjects.isEmpty() || requestedAudio_ > 0) && audio.url.isEmpty()) { fail(tr("所选音轨不可用，请重新选择")); return; }
    resolvedQuality_ = chosen; resolvedQualityLabel_ = qualityLabel(chosen); resolvedAudio_ = audio.quality;
    status_ = tr("正在准备 %1 音视频…").arg(qualityLabel(chosen)); emit changed();
    prepareDash(video, audio, epoch);
}
void BilibiliModel::fetchIndex(const Track& track, int epoch, std::function<void(const Index&)> callback) {
    if (!track.index.valid() || track.index.size > 1024 * 1024) { fail(tr("视频分段索引无效")); return; }
    QNetworkRequest request(track.url);
    request.setRawHeader("User-Agent", userAgent()); request.setRawHeader("Referer", "https://www.bilibili.com/");
    request.setRawHeader("Range", "bytes=" + QByteArray::number(track.index.start) + '-' + QByteArray::number(track.index.start + track.index.size - 1));
    request.setRawHeader("Accept-Encoding", "identity");
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy); request.setTransferTimeout(15000);
    auto* reply = network_->get(request); reply->setReadBufferSize(qint64(track.index.size + 1)); playbackReplies_.append(reply);
    auto body = std::make_shared<QByteArray>();
    connect(reply, &QIODevice::readyRead, this, [reply, body, limit = track.index.size] {
        body->append(reply->read(qint64(limit + 1) - body->size())); if (quint64(body->size()) > limit) reply->abort();
    });
    QTimer::singleShot(20000, reply, [reply] { if (!reply->isFinished()) reply->abort(); });
    connect(reply, &QNetworkReply::finished, this, [this, reply, body, track, epoch, callback] {
        playbackReplies_.removeAll(reply); reply->deleteLater(); if (epoch != epoch_) return;
        if (reply->isOpen()) body->append(reply->read(qint64(track.index.size + 1) - body->size()));
        const auto rangePrefix = "bytes " + QByteArray::number(track.index.start) + '-';
        if (reply->error() != QNetworkReply::NoError || reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 206 || quint64(body->size()) != track.index.size || !reply->rawHeader("Content-Range").startsWith(rangePrefix)) {
            if (!track.backups.isEmpty()) { auto next = track; next.url = next.backups.takeFirst(); fetchIndex(next, epoch, callback); return; }
            fail(tr("音视频分段下载失败，请检查网络后重试")); return;
        }
        const auto index = parseSidx(*body, track.index.start);
        if (!index.error.isEmpty()) { fail(tr("不支持此视频的分段索引")); return; }
        callback(index);
    });
}
void BilibiliModel::prepareDash(Track video, Track audio, int epoch) {
    fetchIndex(video, epoch, [this, video, audio, epoch](const Index& videoIndex) {
        if (audio.url.isEmpty()) {
            ready(server_.install({{"video.m3u8", mediaPlaylist(videoIndex, video.initialization, "video.m4s")}}, {{"video.m4s", locations(video)}}, "video.m3u8"), epoch);
            return;
        }
        fetchIndex(audio, epoch, [this, video, audio, videoIndex, epoch](const Index& audioIndex) {
            const auto videoList = mediaPlaylist(videoIndex, video.initialization, "video.m4s");
            const auto audioList = mediaPlaylist(audioIndex, audio.initialization, "audio.m4s");
            QByteArray master = "#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"audio\",NAME=\"Audio\",DEFAULT=YES,AUTOSELECT=YES,URI=\"audio.m3u8\"\n";
            master += "#EXT-X-STREAM-INF:BANDWIDTH=" + QByteArray::number(std::max<qint64>(1, video.bandwidth + audio.bandwidth)) + ",AUDIO=\"audio\"\nvideo.m3u8\n";
            ready(server_.install({{"master.m3u8", master}, {"video.m3u8", videoList}, {"audio.m3u8", audioList}}, {{"video.m4s", locations(video)}, {"audio.m4s", locations(audio)}}, "master.m3u8"), epoch);
        });
    });
}
void BilibiliModel::loadLive(int quality, int epoch) {
    preview_ = false; audioTracks_.clear(); resolvedAudio_ = 0;
    json(endpoint("/xlive/web-room/v2/index/getRoomPlayInfo", {{"room_id", link_.id}, {"protocol", "0,1"}, {"format", "0,2"}, {"codec", "0,1"}, {"qn", QString::number(quality > 0 ? quality : 10000)}, {"platform", "web"}, {"ptype", "8"}}, "api.live.bilibili.com"), epoch, false, [this, epoch, quality](const QJsonObject& root) {
        const auto data = root.value("data").toObject();
        if (root.value("code").toInt(-1) != 0 || data.value("live_status").toInt() != 1) { fail(tr("直播未开播或当前账号无权观看")); return; }
        const auto play = data.value("playurl_info").toObject().value("playurl").toObject();
        QMap<int, QString> names;
        for (const auto& q : play.value("g_qn_desc").toArray()) names[q.toObject().value("qn").toInt()] = q.toObject().value("desc").toString();
        QJsonObject flvCodec, hlsCodec; QList<QUrl> flvUrls, hlsUrls;
        for (const auto& stream : play.value("stream").toArray()) for (const auto& format : stream.toObject().value("format").toArray()) for (const auto& value : format.toObject().value("codec").toArray()) {
            const auto codec = value.toObject(); QList<QUrl> urls;
            if (codec.value("is_drm").toBool() || codec.value("is_drm").toInt() != 0 || codec.value("drm_type").toInt() != 0) continue;
            for (const auto& info : codec.value("url_info").toArray()) {
                const auto object = info.toObject(); const QUrl url(object.value("host").toString() + codec.value("base_url").toString() + object.value("extra").toString());
                if (mediaUrlAllowed(url)) urls.append(url);
            }
            if (urls.isEmpty()) continue;
            const bool hls = format.toObject().value("format_name").toString() == "fmp4";
            const bool flv = format.toObject().value("format_name").toString() == "flv";
            // The TS CDN currently ignores tmshift. Only fMP4 is a verified
            // server timeshift transport; never claim a seek succeeded on TS.
            if (!hls && !flv) continue;
            auto& saved = hls ? hlsCodec : flvCodec;
            auto& targets = hls ? hlsUrls : flvUrls;
            if (saved.isEmpty() || (saved.value("codec_name").toString() != "avc" && codec.value("codec_name").toString() == "avc")) { saved = codec; targets = urls; }
        }
        const bool shifted = pendingLiveTarget_ > 0 && !describeOnly_;
        const bool useHls = shifted || flvUrls.isEmpty();
        const auto codec = useHls ? hlsCodec : flvCodec;
        if (codec.isEmpty()) { fail(tr("未获得可用的直播流")); return; }
        resolvedQuality_ = codec.value("current_qn").toInt();
        resolvedQualityLabel_ = names.value(resolvedQuality_, QString::number(resolvedQuality_));
        qualities_ = {QVariantMap{{"id", 0}, {"label", tr("自动 · 最高可用画质")}}};
        for (const auto& id : codec.value("accept_qn").toArray()) qualities_.append(QVariantMap{{"id", id.toInt()}, {"label", names.value(id.toInt(), QString::number(id.toInt()))}});
        liveStart_ = data.value("live_time").toDouble(); liveWindow_ = 0;
        const bool supportsReplay = data.value("all_special_types").toArray().contains(19) && !hlsUrls.isEmpty();
        auto finish = [this, epoch, quality, useHls, hlsUrls, flvUrls, shifted](const QJsonObject& capability) {
            const auto info = capability.value("data").toObject();
            if (capability.value("code").toInt(-1) == 0 && info.value("bar_status").toString() == "ENABLE") {
                liveWindow_ = std::clamp(info.value("bar_special_time").toDouble(3600), 3600.0, 7.0 * 86400);
                if (info.value("bar_start_time").toDouble() > 0) liveStart_ = info.value("bar_start_time").toDouble();
            }
            if (describeOnly_) { busy_ = false; status_ = liveWindow_ > 0 ? tr("已就绪，此直播支持拖动进度条回看") : tr("已就绪，选择画质后播放"); emit changed(); return; }
            if (quality > 0 && resolvedQuality_ != quality) emit notice(tr("B 站实际返回 %1，所选画质可能需要登录或相应权限").arg(resolvedQualityLabel_), false);
            const double now = QDateTime::currentMSecsSinceEpoch() / 1000.0;
            preparedLiveStart_ = now;
            if (!useHls) { ready(server_.install({}, {{"live.flv", flvUrls}}, "live.flv"), epoch); return; }
            auto urls = hlsUrls;
            if (shifted) {
                const double window = std::min(liveWindow_, now - liveStart_);
                if (window <= 3) { fail(tr("此直播当前不支持回看，或回看窗口已结束")); return; }
                const int lag = int(std::clamp(std::round(now - pendingLiveTarget_), 1.0, window - 1));
                preparedLiveStart_ = now - lag;
                for (auto& url : urls) { QUrlQuery query(url); query.removeAllQueryItems("tmshift"); query.addQueryItem("tmshift", QString::number(lag)); url.setQuery(query); }
            }
            prepareLiveHls(urls, epoch);
        };
        if (supportsReplay) json(endpoint("/xlive/web-room/v1/index/getProgressBarInfo", {{"roomid", link_.id}, {"platform", "web"}}, "api.live.bilibili.com"), epoch, false, finish, true);
        else finish({});
    });
}
void BilibiliModel::prepareLiveHls(QList<QUrl> urls, int epoch) {
    if (urls.isEmpty()) { fail(tr("这段直播回看暂不可用，已保留当前播放；请换一个时间点或回到直播")); return; }
    validateLivePlaylist(urls.first(), epoch, 0, [this, urls, epoch](bool ok) {
        if (epoch != epoch_) return;
        if (!ok) { auto remaining = urls; remaining.removeFirst(); prepareLiveHls(remaining, epoch); return; }
        ready(server_.installHls(urls), epoch);
    });
}
void BilibiliModel::validateLivePlaylist(const QUrl& url, int epoch, int depth, std::function<void(bool)> callback) {
    if (depth > 3 || !mediaUrlAllowed(url)) { callback(false); return; }
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", userAgent()); request.setRawHeader("Referer", "https://live.bilibili.com/");
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(15000);
    auto* reply = network_->get(request); playbackReplies_.append(reply); reply->setReadBufferSize(2 * 1024 * 1024 + 1);
    auto body = std::make_shared<QByteArray>();
    connect(reply, &QIODevice::readyRead, this, [reply, body] { body->append(reply->read(2 * 1024 * 1024 + 1 - body->size())); if (body->size() > 2 * 1024 * 1024) reply->abort(); });
    QTimer::singleShot(20000, reply, [reply] { if (!reply->isFinished()) reply->abort(); });
    connect(reply, &QNetworkReply::finished, this, [this, reply, body, epoch, depth, callback] {
        playbackReplies_.removeAll(reply); reply->deleteLater(); if (epoch != epoch_) return;
        if (reply->isOpen()) body->append(reply->read(2 * 1024 * 1024 + 1 - body->size()));
        if (reply->error() != QNetworkReply::NoError || reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200 || !proxyPlaylist(*body, reply->url(), "/probe/").error.isEmpty()) {
            callback(false); return;
        }
        // ENABLE describes the advertised window, but its CDN can still return
        // 404 for individual older fragments. Check an actual fragment before
        // replacing the working session, so a missing replay never blanks it.
        QUrl target;
        for (const auto& raw : body->split('\n')) {
            const auto line = raw.trimmed();
            if (!line.isEmpty() && !line.startsWith('#')) { target = reply->url().resolved(QUrl(QString::fromUtf8(line))); break; }
        }
        if (!mediaUrlAllowed(target)) { callback(false); return; }
        if (target.path().endsWith(".m3u8")) {
            QUrlQuery query(target), parent(reply->url());
            if (!query.hasQueryItem("tmshift") && parent.hasQueryItem("tmshift")) { query.addQueryItem("tmshift", parent.queryItemValue("tmshift")); target.setQuery(query); }
            validateLivePlaylist(target, epoch, depth + 1, callback); return;
        }
        QNetworkRequest check(target);
        check.setRawHeader("User-Agent", userAgent()); check.setRawHeader("Referer", "https://live.bilibili.com/");
        check.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
        check.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
        check.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        check.setTransferTimeout(10000);
        auto* fragment = network_->head(check); playbackReplies_.append(fragment);
        QTimer::singleShot(12000, fragment, [fragment] { if (!fragment->isFinished()) fragment->abort(); });
        connect(fragment, &QNetworkReply::finished, this, [this, fragment, epoch, callback] {
            playbackReplies_.removeAll(fragment); fragment->deleteLater(); if (epoch != epoch_) return;
            const int code = fragment->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            callback(fragment->error() == QNetworkReply::NoError && (code == 200 || code == 206));
        });
    });
}
void BilibiliModel::ready(const QUrl& url, int epoch) {
    if (epoch != epoch_) return;
    if (url.isEmpty()) { fail(tr("无法启动本地播放连接")); return; }
    playbackUrl_ = url.toString(QUrl::FullyEncoded); busy_ = false; status_ = tr("已交给播放器，正在打开…");
    activeLink_ = link_; activeTitle_ = title_; activeAddress_ = address_; activeParts_ = parts_; activePart_ = selectedPart_;
    activeQualities_ = qualities_; activeAudioTracks_ = audioTracks_; activeQuality_ = resolvedQuality_; activeAudio_ = resolvedAudio_;
    activeQualityLabel_ = resolvedQualityLabel_; activeRequestedQuality_ = requestedQuality_;
    activeWindow_ = liveWindow_; activeLiveStart_ = liveStart_; playbackStart_ = preparedLiveStart_; clockPosition_ = 0; pausedLiveTime_ = 0; nativeClockStart_ = -1;
    replaying_ = link_.kind == Link::Live && pendingLiveTarget_ > 0;
    emit changed();
    const auto label = parts_.value(selectedPart_).toMap().value("label").toString();
    emit playbackReady(url, title_ + (parts_.size() > 1 ? " · " + label : QString()) + (preview_ ? tr("（试看）") : QString()));
}
void BilibiliModel::cancelLogin() {
    ++loginEpoch_; loginTimer_.stop(); qrKey_.clear(); qrImage_.clear();
    const auto pending = loginReplies_; loginReplies_.clear(); for (const auto& reply : pending) if (reply) reply->abort();
    emit loginChanged();
}
void BilibiliModel::beginLogin() {
    cancelLogin(); loginStatus_ = tr("正在获取登录二维码…"); emit loginChanged(); const int epoch = loginEpoch_;
    json(endpoint("/x/passport-login/web/qrcode/generate", {}, "passport.bilibili.com"), epoch, true, [this, epoch](const QJsonObject& root) {
        const auto data = root.value("data").toObject(); const QUrl url(data.value("url").toString());
        const bool loginHost = url.host() == "passport.bilibili.com" || url.host() == "account.bilibili.com";
        if (root.value("code").toInt(-1) != 0 || url.scheme() != "https" || !loginHost || !url.userInfo().isEmpty() || url.port(-1) != -1 || url.toString().size() > 2048 || data.value("qrcode_key").toString().isEmpty()) {
            loginStatus_ = tr("获取二维码失败，请重试"); emit loginChanged(); return;
        }
        qrKey_ = data.value("qrcode_key").toString(); qrImage_ = qrPng(url.toString()); loginStatus_ = tr("使用哔哩哔哩 App 扫码，并在手机上确认登录");
        emit loginChanged(); if (epoch == loginEpoch_) loginTimer_.start();
    });
}
void BilibiliModel::pollLogin(int epoch) {
    if (qrKey_.isEmpty() || epoch != loginEpoch_) return;
    json(endpoint("/x/passport-login/web/qrcode/poll", {{"qrcode_key", qrKey_}}, "passport.bilibili.com"), epoch, true, [this](const QJsonObject& root) {
        const int code = root.value("data").toObject().value("code").toInt(-1);
        if (root.value("code").toInt(-1) != 0) { loginStatus_ = tr("登录请求失败，请刷新二维码"); }
        else if (code == 0) {
            qrImage_.clear(); qrKey_.clear();
            if (!saveCookies()) emit notice(tr("登录已生效，但无法安全保存，下次启动需要重新登录"), true);
            refreshAccount(); return;
        } else if (code == 86101) { loginStatus_ = tr("等待扫码…"); loginTimer_.start(); }
        else if (code == 86090) { loginStatus_ = tr("已扫码，请在手机上确认"); loginTimer_.start(); }
        else { qrImage_.clear(); qrKey_.clear(); loginStatus_ = tr("二维码已失效，请刷新"); }
        emit loginChanged();
    });
}
void BilibiliModel::refreshAccount() {
    json(endpoint("/x/web-interface/nav"), loginEpoch_, true, [this](const QJsonObject& root) {
        const auto data = root.value("data").toObject();
        const auto previousAccount = account_;
        account_ = data.value("isLogin").toBool() ? data.value("uname").toString() : QString();
        loginStatus_ = account_.isEmpty() ? tr("未登录，可播放公开内容") : tr("已登录：%1").arg(account_); emit loginChanged();
        if (account_ != previousAccount && !parts_.isEmpty() && !busy_) {
            describeOnly_ = true; busy_ = true; status_ = tr("正在获取可用画质…"); emit changed(); requestPlay(0, epoch_);
        }
    });
}
void BilibiliModel::logout() {
    cancelLogin(); cancel(); cookies_->clear();
    if (QFile::exists(cookieFile_) && !QFile::remove(cookieFile_)) emit notice(tr("无法删除本地登录文件，请检查数据目录权限"), true);
    account_.clear();
    loginStatus_ = tr("已退出登录"); emit loginChanged();
    if (!parts_.isEmpty()) {
        qualities_.clear(); describeOnly_ = true; busy_ = true;
        status_ = tr("正在获取可用画质…"); emit changed(); requestPlay(0, epoch_);
    }
}
bool BilibiliModel::saveCookies() {
    const auto encrypted = protect(cookies_->serialize(), false);
    if (encrypted.isEmpty() || !QDir().mkpath(QFileInfo(cookieFile_).absolutePath())) return false;
    QSaveFile file(cookieFile_); return file.open(QIODevice::WriteOnly) && file.write(encrypted) == encrypted.size() && file.commit();
}
void BilibiliModel::loadCookies() {
    QFile file(cookieFile_); if (!file.open(QIODevice::ReadOnly) || file.size() > 256 * 1024) return;
    cookies_->restore(protect(file.readAll(), true));
}
}
