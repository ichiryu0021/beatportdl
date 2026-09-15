#pragma once

#include <QString>
#include <QStringList>
#include <QByteArray>
#include <memory>

namespace TagLib {
class File;
namespace FLAC { class File; }
namespace MP4 { class File; class Tag; class Item; }
}

// TagLib C++ API 的薄封装，替代 Go internal/taglib 绑定。
// Beatport 实际只产出 flac 与 m4a 两种格式，但 Read 按内容兜底 MPEG/OGG。
class TaggedFile {
public:
    ~TaggedFile();

    // 打开文件（失败返回 nullptr，err 为 TagLib 错误文本）。
    static std::unique_ptr<TaggedFile> read(const QString &path, QString *err);

    // 复合属性键（FLAC Vorbis Comment / MP4 atom）。
    QStringList propertyKeys() const;
    bool hasProperty(const QString &key) const;
    // value 为空 → 删除该 property；非空 → 覆盖设置。
    bool setProperty(const QString &key, const QString &value, QString *err);

    // MP4 专用：先 strip 全部原子（Go StripMp4），后续再重写。
    bool stripMp4(QString *err);
    // MP4 freeform atom：----:com.apple.iTunes:<key>
    bool setItemMp4(const QString &key, const QString &value, QString *err);

    int sampleRate() const; // 0 表示不可读

    // 写入 JPEG 封面（FLAC: FLAC::Picture FrontCover image/jpeg；M4A: covr CoverArt(JPEG)）。
    bool setCoverJpeg(const QByteArray &jpeg, QString *err);

    bool save(QString *err);

private:
    TaggedFile();
    Q_DISABLE_COPY(TaggedFile)

    QString m_pathUtf8;
    TagLib::File *m_file = nullptr;
    TagLib::FLAC::File *m_flac = nullptr;
    TagLib::MP4::File *m_mp4 = nullptr;
};
