#pragma once

#include <QString>

enum class LinkType {
    Track,
    Release,
    Playlist,
    Chart,
    Label,
    Artist,
};

QString linkTypeName(LinkType t); // tracks/releases/playlists/charts/labels/artists

struct Link {
    QString original;
    LinkType type = LinkType::Track;
    qint64 id = 0;
    QString params; // 原始 RawQuery
};

// 解析 www.beatport.com / api.beatport.com 链接。
// 失败返回 false 并设置 err（"invalid url" 或 "invalid id: ..."）。
bool parseBeatportUrl(const QString &inputUrl, Link *out, QString *err);
