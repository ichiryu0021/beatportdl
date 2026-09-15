#pragma once

#include "models.h"

// 解析 beatport.com 首页 HTML 中的曲风导航链接（两种锚点形态），
// 按 slug 去重（保留第一个），按名称排序。一个都找不到时报错。
QList<Genre> parseGenresHtml(const QString &html, QString *err);
