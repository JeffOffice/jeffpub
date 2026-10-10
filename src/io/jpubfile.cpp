#include "io/jpubfile.h"

#include "io/cfb.h"
#include "io/zip.h"
#include "render/metafile.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QDateTime>
#include <QHash>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QSaveFile>
#include <QtEndian>

namespace jp {

static const char *kMime = "application/x-jeffpub";

QByteArray publicationBytes(const Document &doc, const QImage &thumbnail, const QString &folder, bool embedLinks)
{
    ZipWriter z;
    z.add("mimetype", kMime);
    // A linked picture's path from the folder the file goes in is kept next
    // to its full path: it wins when the two are moved together.
    QJsonObject json = doc.toJson();
    QJsonArray imageList = json["images"].toArray();
    const QString base = folder.isEmpty() ? doc.folder : folder;
    for (int i = 0; i < imageList.size(); ++i) {
        QJsonObject io = imageList[i].toObject();
        const auto it = doc.images.constFind(io["id"].toString());
        if (it == doc.images.cend() || !it->linked) continue;
        if (!base.isEmpty() && !it->sourcePath.isEmpty()) {
            const QString relative = QDir(base).relativeFilePath(it->sourcePath);
            if (!QDir::isAbsolutePath(relative)) io["relative"] = relative;
        }
        if (embedLinks) io["copy"] = true;
        imageList[i] = io;
    }
    json["images"] = imageList;
    z.add("document.json", QJsonDocument(json).toJson(QJsonDocument::Compact));
    // Only pictures that are still used somewhere are written.
    QSet<QString> used;
    doc.forEachItem([&](Item *it, int, const QString &) {
        if (it->type() == ItemType::Picture) used.insert(static_cast<PictureItem *>(it)->imageId);
        if (!it->fill.imageId.isEmpty()) used.insert(it->fill.imageId);
    });
    walkItems(doc.extra, [&](const ItemPtr &it) {
        if (it->type() == ItemType::Picture) used.insert(static_cast<PictureItem *>(it.get())->imageId);
        if (!it->fill.imageId.isEmpty()) used.insert(it->fill.imageId);
    });
    for (const auto &p : doc.pages) used.insert(p->background.imageId);
    for (const auto &m : doc.masters) used.insert(m->background.imageId);
    for (const auto &b : doc.biz) used.insert(b.logoImageId);
    for (auto it = doc.images.cbegin(); it != doc.images.cend(); ++it) {
        if (!used.contains(it.key())) continue;
        if ((!it->linked || it->keepsCopy || embedLinks) && !it->bytes.isEmpty()) {
            z.add("images/" + it.key() + "." + it->format, it->bytes);
        } else if (it->linked) {
            // A link without a stored copy keeps only a small picture of it.
            ImageData made;
            if (it->preview.isEmpty()) {
                made = *it;
                made.makePreview();
            }
            const ImageData &p = it->preview.isEmpty() ? made : *it;
            if (!p.preview.isEmpty()) z.add("previews/" + it.key() + "." + p.previewFormat, p.preview);
        }
    }
    if (!thumbnail.isNull()) {
        QByteArray png;
        QBuffer b(&png);
        b.open(QIODevice::WriteOnly);
        thumbnail.save(&b, "PNG");
        z.add("thumbnail.png", png);
    }
    return z.finish();
}

bool savePublication(const Document &doc, const QString &path, const QImage &thumbnail, QString *error, bool embedLinks, const QString &folder)
{
    QSaveFile f(path);
    // Some sync and security tools refuse the temporary file; write in place then.
    f.setDirectWriteFallback(true);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error) *error = f.errorString();
        return false;
    }
    f.write(publicationBytes(doc, thumbnail, folder.isEmpty() ? QFileInfo(path).absolutePath() : folder, embedLinks));
    if (!f.commit()) {
        if (error) *error = f.errorString();
        return false;
    }
    return true;
}

std::unique_ptr<Document> publicationFromBytes(const QByteArray &bytes, QString *error, const QString &folder)
{
    QMap<QString, QByteArray> entries;
    if (!readZip(bytes, entries, error)) return nullptr;
    if (!entries.contains("document.json")) {
        if (error) *error = QCoreApplication::translate("Import", "The file has no publication content.");
        return nullptr;
    }
    QJsonParseError pe;
    const QJsonDocument jd = QJsonDocument::fromJson(entries["document.json"], &pe);
    if (jd.isNull()) {
        if (error) *error = QCoreApplication::translate("Import", "The publication content is damaged: %1").arg(pe.errorString());
        return nullptr;
    }
    auto doc = std::make_unique<Document>();
    doc->images.clear();
    doc->folder = folder;
    doc->fromJson(jd.object());
    for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
        const bool preview = it.key().startsWith("previews/");
        if (!preview && !it.key().startsWith("images/")) continue;
        const QString file = it.key().mid(preview ? 9 : 7);
        const QString id = file.section('.', 0, 0);
        ImageData &d = doc->images[id];
        if (preview) {
            d.preview = it.value();
            d.previewFormat = file.section('.', 1);
            continue;
        }
        d.bytes = it.value();
        if (d.format.isEmpty()) d.format = file.section('.', 1);
        if (!d.pixelSize.isValid()) d.pixelSize = d.image().size();
    }
    // Only the files in the publication's own folder are read for the links
    // it names; the rest wait until the user asks for them.
    doc->fromFile = true;
    for (auto &image : doc->images)
        if (image.linked) image.followed = false;
    doc->refreshLinks();
    return doc;
}

std::unique_ptr<Document> loadPublication(const QString &path, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = f.errorString();
        return nullptr;
    }
    return publicationFromBytes(f.readAll(), error, QFileInfo(path).absolutePath());
}

// The preview picture a .pub file keeps in its summary information
// (property 17, a clipboard picture: a metafile, an enhanced metafile or a
// bitmap), drawn at most maxSide pixels across.
static QImage readThumbnail(const QString &path);

static QImage pubPreview(const QString &path, int maxSide)
{
    const QByteArray si = cfb::readStream(path, QStringLiteral("\x05SummaryInformation"));
    auto u32 = [&](qint64 o) -> quint32 { return o >= 0 && o + 4 <= si.size() ? qFromLittleEndian<quint32>(si.constData() + o) : 0; };
    if (si.size() < 48 || qFromLittleEndian<quint16>(si.constData()) != 0xFFFE) return {};
    const qint64 section = u32(44);
    const quint32 count = u32(section + 4);
    for (quint32 i = 0; i < count && i < 1000; ++i) {
        if (u32(section + 8 + qint64(i) * 8) != 17) continue;   // PIDSI_THUMBNAIL
        const qint64 at = section + qint64(u32(section + 12 + qint64(i) * 8));
        if (u32(at) != 0x47) return {};                          // VT_CF
        const qint64 size = u32(at + 4);
        if (size < 8 || at + 8 + size > si.size()) return {};
        const QByteArray cf = si.mid(at + 8, size);
        if (qint32(qFromLittleEndian<quint32>(cf.constData())) != -1) return {};   // a Windows clipboard format
        const quint32 format = qFromLittleEndian<quint32>(cf.constData() + 4);
        QByteArray data = cf.mid(8);
        QSizeF shape;
        if (format == 3 && data.size() > 8) {
            // CF_METAFILEPICT: mapping mode and the picture's size (a 16-bit
            // header), then the metafile, which has no size of its own.
            shape = QSizeF(qFromLittleEndian<qint16>(data.constData() + 2), qFromLittleEndian<qint16>(data.constData() + 4));
            data = data.mid(8);
        }
        if (format == 3 || format == 14) {
            Metafile m;
            if (!m.load(data)) return {};
            if (shape.width() <= 0 || shape.height() <= 0) return m.toImage(maxSide);
            const QSize px = shape.scaled(maxSide, maxSide, Qt::KeepAspectRatio).toSize().expandedTo(QSize(1, 1));
            QImage img(px, QImage::Format_ARGB32_Premultiplied);
            img.fill(Qt::white);
            QPainter p(&img);
            p.setRenderHint(QPainter::Antialiasing);
            p.setRenderHint(QPainter::SmoothPixmapTransform);
            m.play(&p, QRectF(QPointF(0, 0), px));
            return img;
        }
        if (format == 8 && data.size() > 40) {   // CF_DIB: a bitmap without its file header
            QByteArray bmp("BM");
            const quint32 header = qFromLittleEndian<quint32>(data.constData());
            const quint16 bits = qFromLittleEndian<quint16>(data.constData() + 14);
            const quint32 colors = qFromLittleEndian<quint32>(data.constData() + 32);
            const quint32 palette = (colors ? colors : bits <= 8 ? (1u << bits) : 0) * 4;
            QByteArray head(12, '\0');
            qToLittleEndian<quint32>(quint32(14 + data.size()), head.data());
            qToLittleEndian<quint32>(14 + header + palette, head.data() + 8);
            const QImage img = QImage::fromData(bmp + head + data, "BMP");
            return img.isNull() ? img : img.scaled(maxSide, maxSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
        return {};
    }
    return {};
}

QImage publicationThumbnail(const QString &path)
{
    // Kept for the session by file, date and size (lists of recent files
    // are redrawn often).
    static QHash<QString, QImage> cache;   // main thread only
    const QFileInfo fi(path);
    const QString key = path + QLatin1Char('|') + QString::number(fi.lastModified().toMSecsSinceEpoch()) + QLatin1Char('|') + QString::number(fi.size());
    if (const auto it = cache.constFind(key); it != cache.constEnd()) return *it;
    const QImage img = readThumbnail(path);
    if (cache.size() > 200) cache.clear();
    cache.insert(key, img);
    return img;
}

static QImage readThumbnail(const QString &path)
{
    // Just the thumbnail: a publication with large photos needn't be read
    // whole to show its picture in a list of files.
    QFile f(path);
    if (f.open(QIODevice::ReadOnly) && f.peek(8) == QByteArray::fromHex("D0CF11E0A1B11AE1")) {
        f.close();
        return pubPreview(path, 256);
    }
    return QImage::fromData(readZipEntry(path, QStringLiteral("thumbnail.png")), "PNG");
}

} // namespace jp
