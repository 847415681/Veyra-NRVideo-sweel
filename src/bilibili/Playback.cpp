#include "veyra/bilibili/Playback.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QRegularExpression>
#include <QUrlQuery>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <limits>

namespace veyra::bilibili {
namespace {
bool domain(const QString& host, const QString& suffix) {
    return host == suffix || host.endsWith('.' + suffix);
}
}
QByteArray userAgent() {
    return "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/140.0.0.0 Safari/537.36";
}
Link parseLink(QString text) {
    Link out;
    text = text.trimmed();
    if (text.size() > 8192) return out;
    static const QRegularExpression bare("^(BV[0-9A-Za-z]{10}|av[0-9]+|ep[0-9]+|ss[0-9]+)$");
    if (bare.match(text).hasMatch()) {
        text = "https://www.bilibili.com/" + QString(text.startsWith("ep") || text.startsWith("ss") ? "bangumi/play/" : "video/") + text;
    }
    if (text.startsWith("www.bilibili.com/") || text.startsWith("b23.tv/") || text.startsWith("live.bilibili.com/")) text.prepend("https://");
    const QUrl url(text, QUrl::StrictMode);
    if (!url.isValid() || (url.scheme() != "https" && url.scheme() != "http") || !url.userInfo().isEmpty() || url.port(-1) != -1) return out;
    const QString host = url.host().toLower();
    if (host == "b23.tv" || host == "bili2233.cn") {
        out.kind = Link::Short; out.url = url; out.url.setScheme("https"); return out;
    }
    if (!domain(host, "bilibili.com")) return out;
    QRegularExpression pattern(host == "live.bilibili.com" ? "^/([0-9]+)/?$" : "^/(?:video/(BV[0-9A-Za-z]{10}|av[0-9]+)|bangumi/play/(ep[0-9]+|ss[0-9]+))/?$");
    const auto match = pattern.match(url.path());
    if (!match.hasMatch()) return out;
    out.id = match.captured(1).isEmpty() ? match.captured(2) : match.captured(1);
    out.kind = host == "live.bilibili.com" ? Link::Live : out.id.startsWith("ep") ? Link::Episode : out.id.startsWith("ss") ? Link::Season : Link::Video;
    bool ok = false;
    const int page = QUrlQuery(url).queryItemValue("p").toInt(&ok);
    out.page = ok && page > 0 && page <= 10000 ? page : 1;
    out.url = QUrl("https://" + QString(out.kind == Link::Live ? "live.bilibili.com/" : "www.bilibili.com/" + QString(out.kind == Link::Video ? "video/" : "bangumi/play/")) + out.id);
    if (out.page > 1 && out.kind == Link::Video) out.url.setQuery("p=" + QString::number(out.page));
    return out;
}
bool mediaUrlAllowed(const QUrl& url) {
    if ((url.scheme() != "https" && url.scheme() != "http") || !url.userInfo().isEmpty() || url.port(-1) != -1) return false;
    const QString host = url.host().toLower();
    for (const auto* suffix : {"bilivideo.com", "bilivideo.cn", "bilivideo.net", "bilibili.com", "hdslb.com", "akamaized.net"})
        if (domain(host, QString::fromLatin1(suffix))) return true;
    return false;
}
QString signedQuery(QMap<QString, QString> params, const QString& imageKey, const QString& subKey, qint64 timestamp) {
    // Published WBI permutation; the input keys are refreshed from /x/web-interface/nav.
    static constexpr int order[] = {46,47,18,2,53,8,23,32,15,50,10,31,58,3,45,35,27,43,5,49,33,9,42,19,29,28,14,39,12,38,41,13,37,48,7,16,24,55,40,61,26,17,0,1,60,51,30,4,22,25,54,21,56,59,6,63,57,62,11,36,20,34,44,52};
    const auto keys = imageKey + subKey;
    if (keys.size() != 64) return {};
    QString mixin;
    for (int i = 0; i < 32; ++i) mixin += keys[order[i]];
    params["wts"] = QString::number(timestamp);
    QStringList pairs;
    for (auto it = params.cbegin(); it != params.cend(); ++it) {
        QString value = it.value();
        value.remove(QRegularExpression("[!'()*]"));
        pairs << QString::fromLatin1(QUrl::toPercentEncoding(it.key())) + '=' + QString::fromLatin1(QUrl::toPercentEncoding(value));
    }
    const auto query = pairs.join('&');
    return query + "&w_rid=" + QString::fromLatin1(QCryptographicHash::hash((query + mixin).toUtf8(), QCryptographicHash::Md5).toHex());
}
ByteRange parseRange(const QString& text) {
    const auto parts = text.split('-');
    if (parts.size() != 2) return {};
    bool a = false, b = false;
    const auto start = parts[0].toULongLong(&a), end = parts[1].toULongLong(&b);
    if (!a || !b || end < start || end == std::numeric_limits<quint64>::max()) return {};
    return {start, end - start + 1};
}
Index parseSidx(const QByteArray& bytes, quint64 fileOffset) {
    Index result;
    const auto fail = [&result](const QString& reason) { result.segments.clear(); result.error = reason; return result; };
    const auto u32 = [&bytes](qsizetype pos) { return qFromBigEndian<quint32>(bytes.constData() + pos); };
    const auto u64 = [&bytes](qsizetype pos) { return qFromBigEndian<quint64>(bytes.constData() + pos); };
    for (qsizetype box = 0; box + 8 <= bytes.size();) {
        quint64 size = u32(box); qsizetype header = 8;
        if (size == 1) { if (box + 16 > bytes.size()) return fail("Truncated extended box"); size = u64(box + 8); header = 16; }
        if (size < quint64(header) || size > quint64(bytes.size() - box)) return fail("Invalid box size");
        if (bytes.mid(box + 4, 4) != "sidx") { box += qsizetype(size); continue; }
        const qsizetype end = box + qsizetype(size), begin = box + header;
        if (begin + 24 > end) return fail("Truncated SIDX");
        const auto version = quint8(bytes[begin]);
        if (version > 1) return fail("Unsupported SIDX version");
        const quint32 timescale = u32(begin + 8);
        if (!timescale) return fail("Invalid timescale");
        qsizetype pos = begin + 12;
        quint64 firstOffset;
        if (version == 1) {
            if (pos + 20 > end) return fail("Truncated SIDX v1");
            firstOffset = u64(pos + 8); pos += 16;
        } else { firstOffset = u32(pos + 4); pos += 8; }
        const auto count = qFromBigEndian<quint16>(bytes.constData() + pos + 2); pos += 4;
        if (!count || pos + qsizetype(count) * 12 > end) return fail("Invalid reference count");
        if (fileOffset > quint64(INT64_MAX) - quint64(end) || firstOffset > quint64(INT64_MAX) - fileOffset - quint64(end)) return fail("SIDX offset overflow");
        quint64 offset = fileOffset + quint64(end) + firstOffset;
        for (int i = 0; i < count; ++i, pos += 12) {
            const quint32 reference = u32(pos), duration = u32(pos + 4);
            if ((reference & 0x80000000u) || !reference || !duration) return fail("Unsupported indirect/empty reference");
            if (offset > quint64(INT64_MAX) - reference) return fail("SIDX range overflow");
            result.segments.append({offset, reference, double(duration) / timescale});
            offset += reference;
        }
        return result;
    }
    return fail("SIDX not found");
}
QByteArray mediaPlaylist(const Index& index, ByteRange initialization, const QByteArray& uri) {
    if (!index.error.isEmpty() || index.segments.isEmpty() || !initialization.valid()) return {};
    double longest = 0;
    for (const auto& segment : index.segments) longest = std::max(longest, segment.seconds);
    QByteArray out = "#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-PLAYLIST-TYPE:VOD\n#EXT-X-TARGETDURATION:" + QByteArray::number(qint64(std::ceil(longest))) + "\n#EXT-X-MEDIA-SEQUENCE:0\n";
    out += "#EXT-X-MAP:URI=\"" + uri + "\",BYTERANGE=\"" + QByteArray::number(initialization.size) + '@' + QByteArray::number(initialization.start) + "\"\n";
    // Keep fragment identities distinct. The shipped FFmpeg coalesces adjacent
    // ranges with the same URI, then stops demuxing after the first fragment.
    // The gateway removes this local-only query before forwarding the Range.
    for (const auto& segment : index.segments)
        out += "#EXTINF:" + QByteArray::number(segment.seconds, 'f', 9) + ",\n#EXT-X-BYTERANGE:" + QByteArray::number(segment.size) + '@' + QByteArray::number(segment.offset) + '\n' + uri + "?segment=" + QByteArray::number(segment.offset) + '\n';
    return out + "#EXT-X-ENDLIST\n";
}
Track parseTrack(const QJsonObject& object) {
    Track track;
    track.url = QUrl(object.value("baseUrl").toString(object.value("base_url").toString()));
    if (!mediaUrlAllowed(track.url)) track.url.clear();
    auto backups = object.value("backupUrl").toArray();
    if (backups.isEmpty()) backups = object.value("backup_url").toArray();
    for (const auto& entry : backups) { const QUrl url(entry.toString()); if (mediaUrlAllowed(url)) track.backups.append(url); }
    if (track.url.isEmpty() && !track.backups.isEmpty()) track.url = track.backups.takeFirst();
    auto base = object.value("SegmentBase").toObject();
    if (base.isEmpty()) base = object.value("segment_base").toObject();
    track.initialization = parseRange(base.value("Initialization").toString(base.value("initialization").toString()));
    track.index = parseRange(base.value("indexRange").toString(base.value("index_range").toString()));
    track.quality = object.value("id").toInt();
    track.bandwidth = qint64(object.value("bandwidth").toDouble());
    track.codecs = object.value("codecs").toString();
    track.width = object.value("width").toInt(); track.height = object.value("height").toInt();
    return track;
}
HlsPlaylist proxyPlaylist(const QByteArray& bytes, const QUrl& upstream, const QString& localPrefix) {
    HlsPlaylist out;
    if (bytes.size() > 2 * 1024 * 1024 || !bytes.startsWith("#EXTM3U") || !mediaUrlAllowed(upstream)) {
        out.error = QStringLiteral("Invalid HLS playlist"); return out;
    }
    const auto reference = [&](const QString& value) -> QString {
        QUrl target = upstream.resolved(QUrl(value, QUrl::StrictMode));
        // A master may point to a relative child without its timeshift parameter.
        // Apply tmshift only to child playlists, never to signed media fragments.
        if (target.path().endsWith(".m3u8", Qt::CaseInsensitive)) {
            QUrlQuery query(target), parent(upstream);
            if (!query.hasQueryItem("tmshift") && parent.hasQueryItem("tmshift")) {
                query.addQueryItem("tmshift", parent.queryItemValue("tmshift")); target.setQuery(query);
            }
        }
        if (!mediaUrlAllowed(target) || target.toEncoded().size() > 8192 || out.resources.size() >= 4096) {
            out.error = QStringLiteral("Unsupported HLS reference"); return {};
        }
        const QString suffix = target.path().endsWith(".m3u8", Qt::CaseInsensitive) ? ".m3u8"
                             : target.path().endsWith(".ts", Qt::CaseInsensitive) ? ".ts" : ".m4s";
        const QString key = "hls/" + QString::fromLatin1(QCryptographicHash::hash(target.toEncoded(), QCryptographicHash::Sha256).toHex()) + suffix;
        out.resources.insert(key, target);
        return localPrefix + key;
    };
    static const QRegularExpression uri("URI=\"([^\"]*)\"");
    for (const auto& raw : bytes.split('\n')) {
        QString line = QString::fromUtf8(raw).trimmed();
        if (line.isEmpty()) continue;
        if ((line.startsWith("#EXT-X-KEY:") || line.startsWith("#EXT-X-SESSION-KEY:")) && !line.contains("METHOD=NONE")) {
            out.error = QStringLiteral("Encrypted HLS is not supported"); break;
        }
        if (!line.startsWith('#')) line = reference(line);
        else {
            auto matches = uri.globalMatch(line);
            QList<QRegularExpressionMatch> replacements;
            while (matches.hasNext()) replacements.prepend(matches.next());
            for (const auto& match : replacements) line.replace(match.capturedStart(1), match.capturedLength(1), reference(match.captured(1)));
        }
        if (!out.error.isEmpty()) break;
        out.body += line.toUtf8() + '\n';
    }
    if (!out.error.isEmpty()) { out.body.clear(); out.resources.clear(); }
    return out;
}
QString qualityLabel(int id) {
    switch (id) {
    case 6: return "240P"; case 16: return "360P"; case 32: return "480P"; case 64: return "720P";
    case 74: return "720P 60fps"; case 80: return "1080P"; case 112: return "1080P+"; case 116: return "1080P 60fps";
    case 120: return "4K"; case 125: return "HDR"; case 126: return "Dolby Vision"; case 127: return "8K";
    default: return QString::number(id);
    }
}
}
