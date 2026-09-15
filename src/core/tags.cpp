#include "tags.h"

#include <taglib/tfile.h>
#include <taglib/flacfile.h>
#include <taglib/flacpicture.h>
#include <taglib/mp4file.h>
#include <taglib/mp4tag.h>
#include <taglib/mp4item.h>
#include <taglib/mp4coverart.h>
#include <taglib/mpegfile.h>
#include <taglib/tpropertymap.h>
#include <taglib/tstring.h>
#include <taglib/tstringlist.h>
#include <taglib/tbytevector.h>
#include <taglib/audioproperties.h>

#include <QFile>
#include <QFileInfo>

namespace {

// QString(UTF-8) -> TagLib::String。
TagLib::String toTString(const QString &s)
{
    const QByteArray utf8 = s.toUtf8();
    return TagLib::String(utf8.constData(), TagLib::String::UTF8);
}

// TagLib::String(UTF-8) -> QString。
QString toQString(const TagLib::String &s)
{
    return QString::fromUtf8(s.toCString(true));
}

enum class AudioKind {
    Flac,
    Mp4,
    Mpeg,
    Unknown,
};

// 按内容魔数识别；读不到文件头时回退扩展名（Go 版 FileRef 仅按扩展名，
// 这里在其基础上补内容嗅探，Beatport 实际只产出 flac/m4a）。
AudioKind detectKind(const QString &path)
{
    QFile f(path);
    if (f.open(QIODevice::ReadOnly)) {
        const QByteArray head = f.read(12);
        static const char kFlacMagic[] = "fLaC";
        if (head.startsWith(kFlacMagic))
            return AudioKind::Flac;
        if (head.size() >= 12 && head[4] == 'f' && head[5] == 't'
            && head[6] == 'y' && head[7] == 'p')
            return AudioKind::Mp4;
        if (head.startsWith("ID3")
            || (head.size() >= 2 && static_cast<unsigned char>(head[0]) == 0xFF
                && (static_cast<unsigned char>(head[1]) & 0xE0) == 0xE0))
            return AudioKind::Mpeg;
    }

    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QLatin1String("flac") || suffix == QLatin1String("fla"))
        return AudioKind::Flac;
    if (suffix == QLatin1String("m4a") || suffix == QLatin1String("m4b")
        || suffix == QLatin1String("m4r") || suffix == QLatin1String("m4v")
        || suffix == QLatin1String("mp4") || suffix == QLatin1String("aac"))
        return AudioKind::Mp4;
    if (suffix == QLatin1String("mp3") || suffix == QLatin1String("mp2")
        || suffix == QLatin1String("mp1") || suffix == QLatin1String("mpa")
        || suffix == QLatin1String("mpeg") || suffix == QLatin1String("mpg"))
        return AudioKind::Mpeg;
    return AudioKind::Unknown;
}

TagLib::File *openByKind(AudioKind kind, const char *utf8Path)
{
    switch (kind) {
    case AudioKind::Flac:
        return new TagLib::FLAC::File(utf8Path);
    case AudioKind::Mp4:
        return new TagLib::MP4::File(utf8Path);
    case AudioKind::Mpeg:
        return new TagLib::MPEG::File(utf8Path);
    case AudioKind::Unknown:
        return nullptr;
    }
    return nullptr;
}

} // namespace

TaggedFile::TaggedFile() = default;

TaggedFile::~TaggedFile()
{
    // 析构仅关闭/释放文件，不做 save（与 Go taglib_file_free 一致）。
    delete m_file;
    m_file = nullptr;
    m_flac = nullptr;
    m_mp4 = nullptr;
}

std::unique_ptr<TaggedFile> TaggedFile::read(const QString &path, QString *err)
{
    // Windows 上 TagLib 2.x 的 FileName(const char*) 按 UTF-8 接收路径。
    const QByteArray utf8Path = path.toUtf8();

    const AudioKind kind = detectKind(path);
    TagLib::File *file = openByKind(kind, utf8Path.constData());
    if (!file || !file->isValid()) {
        delete file;
        if (err)
            *err = QStringLiteral("invalid file");
        return nullptr;
    }

    std::unique_ptr<TaggedFile> tagged(new TaggedFile());
    tagged->m_pathUtf8 = path;
    tagged->m_file = file;
    tagged->m_flac = dynamic_cast<TagLib::FLAC::File *>(file);
    tagged->m_mp4 = dynamic_cast<TagLib::MP4::File *>(file);
    return tagged;
}

QStringList TaggedFile::propertyKeys() const
{
    QStringList keys;
    if (!m_file)
        return keys;

    const TagLib::PropertyMap props = m_file->properties();
    for (auto it = props.begin(); it != props.end(); ++it)
        keys << toQString(it->first);
    return keys;
}

bool TaggedFile::hasProperty(const QString &key) const
{
    if (!m_file)
        return false;
    return m_file->properties().contains(toTString(key));
}

bool TaggedFile::setProperty(const QString &key, const QString &value, QString *err)
{
    if (!m_file) {
        if (err)
            *err = QStringLiteral("invalid file");
        return false;
    }

    // 对应 tag_c.cpp taglib_property_set：整体取出 PropertyMap，
    // value==NULL erase，否则 replace，再 setProperties 写回。
    TagLib::PropertyMap props = m_file->properties();
    const TagLib::String tagKey = toTString(key);
    if (value.isEmpty()) {
        props.erase(tagKey);
    } else {
        TagLib::StringList values;
        values.append(toTString(value));
        props.replace(tagKey, values);
    }

    const TagLib::PropertyMap rejected = m_file->setProperties(props);
    if (!value.isEmpty() && rejected.contains(tagKey)) {
        if (err)
            *err = QStringLiteral("unsupported property: %1").arg(key);
        return false;
    }
    return true;
}

bool TaggedFile::stripMp4(QString *err)
{
    if (!m_mp4) {
        if (err)
            *err = QStringLiteral("cannot strip mp4 tags");
        return false;
    }

    auto *tag = dynamic_cast<TagLib::MP4::Tag *>(m_mp4->tag());
    if (!tag || !tag->strip()) {
        if (err)
            *err = QStringLiteral("cannot strip mp4 tags");
        return false;
    }
    // Tag::strip() 在 2.x 已立即落盘，按规格再显式 save 一次（无变更时为 no-op）。
    if (!m_mp4->save()) {
        if (err)
            *err = QStringLiteral("cannot save file");
        return false;
    }
    return true;
}

bool TaggedFile::setItemMp4(const QString &key, const QString &value, QString *err)
{
    if (!m_mp4) {
        if (err)
            *err = QStringLiteral("not an mp4 file");
        return false;
    }
    auto *tag = dynamic_cast<TagLib::MP4::Tag *>(m_mp4->tag());
    if (!tag) {
        if (err)
            *err = QStringLiteral("not an mp4 file");
        return false;
    }

    // 对应 extensions.cpp BASE_MP4_ATOM + StringList(value)。
    TagLib::String atomKey("----:com.apple.iTunes:", TagLib::String::UTF8);
    atomKey.append(toTString(key));
    tag->setItem(atomKey, TagLib::MP4::Item(TagLib::StringList(toTString(value))));
    return true;
}

int TaggedFile::sampleRate() const
{
    if (!m_file)
        return 0;
    TagLib::AudioProperties *props = m_file->audioProperties();
    return props ? props->sampleRate() : 0;
}

bool TaggedFile::setCoverJpeg(const QByteArray &jpeg, QString *err)
{
    const TagLib::ByteVector data(jpeg.constData(), static_cast<unsigned int>(jpeg.size()));

    if (m_flac) {
        auto *picture = new TagLib::FLAC::Picture();
        picture->setType(TagLib::FLAC::Picture::FrontCover);
        picture->setMimeType(TagLib::String("image/jpeg", TagLib::String::Latin1));
        picture->setDescription(TagLib::String("Cover", TagLib::String::UTF8));
        picture->setData(data);
        // 先去旧图再挂新图，避免重复 PICTURE 块。
        m_flac->removePictures();
        m_flac->addPicture(picture); // 文件接管 picture 所有权
        return true;
    }

    if (m_mp4) {
        auto *tag = dynamic_cast<TagLib::MP4::Tag *>(m_mp4->tag());
        if (!tag) {
            if (err)
                *err = QStringLiteral("not an mp4 file");
            return false;
        }
        TagLib::MP4::CoverArtList covers;
        covers.append(TagLib::MP4::CoverArt(TagLib::MP4::CoverArt::JPEG, data));
        tag->setItem(TagLib::String("covr", TagLib::String::Latin1),
                     TagLib::MP4::Item(covers));
        return true;
    }

    if (err)
        *err = QStringLiteral("unsupported file type for cover art");
    return false;
}

bool TaggedFile::save(QString *err)
{
    if (!m_file || !m_file->save()) {
        if (err)
            *err = QStringLiteral("cannot save file");
        return false;
    }
    return true;
}
