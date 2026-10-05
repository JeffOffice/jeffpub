#include "io/jpubfile.h"

#include "io/zip.h"

#include <QBuffer>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>

namespace jp {

static const char *kMime = "application/x-jeffpub";

QByteArray publicationBytes(const Document &doc, const QImage &thumbnail)
{
    ZipWriter z;
    z.add("mimetype", kMime);
    z.add("document.json", QJsonDocument(doc.toJson()).toJson(QJsonDocument::Compact));
    // Only pictures that are still used somewhere are written.
    QSet<QString> used;
    doc.forEachItem([&](Item *it, int, const QString &) {
        if (it->type() == ItemType::Picture) used.insert(static_cast<PictureItem *>(it)->imageId);
        if (!it->fill.imageId.isEmpty()) used.insert(it->fill.imageId);
    });
    for (const auto &p : doc.pages) used.insert(p->background.imageId);
    for (const auto &m : doc.masters) used.insert(m->background.imageId);
    for (const auto &b : doc.biz) used.insert(b.logoImageId);
    for (auto it = doc.images.cbegin(); it != doc.images.cend(); ++it)
        if (used.contains(it.key()) && !it->bytes.isEmpty()) z.add("images/" + it.key() + "." + it->format, it->bytes);
    if (!thumbnail.isNull()) {
        QByteArray png;
        QBuffer b(&png);
        b.open(QIODevice::WriteOnly);
        thumbnail.save(&b, "PNG");
        z.add("thumbnail.png", png);
    }
    return z.finish();
}

bool savePublication(const Document &doc, const QString &path, const QImage &thumbnail, QString *error)
{
    QSaveFile f(path);
    // Some sync and security tools refuse the temporary file; write in place then.
    f.setDirectWriteFallback(true);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error) *error = f.errorString();
        return false;
    }
    f.write(publicationBytes(doc, thumbnail));
    if (!f.commit()) {
        if (error) *error = f.errorString();
        return false;
    }
    return true;
}

std::unique_ptr<Document> publicationFromBytes(const QByteArray &bytes, QString *error)
{
    QMap<QString, QByteArray> entries;
    if (!readZip(bytes, entries, error)) return nullptr;
    if (!entries.contains("document.json")) {
        if (error) *error = QStringLiteral("The file has no publication content.");
        return nullptr;
    }
    QJsonParseError pe;
    const QJsonDocument jd = QJsonDocument::fromJson(entries["document.json"], &pe);
    if (jd.isNull()) {
        if (error) *error = QStringLiteral("The publication content is damaged: %1").arg(pe.errorString());
        return nullptr;
    }
    auto doc = std::make_unique<Document>();
    doc->images.clear();
    doc->fromJson(jd.object());
    for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
        if (!it.key().startsWith("images/")) continue;
        const QString file = it.key().mid(7);
        const QString id = file.section('.', 0, 0);
        ImageData &d = doc->images[id];
        d.bytes = it.value();
        if (d.format.isEmpty()) d.format = file.section('.', 1);
        if (!d.pixelSize.isValid()) d.pixelSize = d.image().size();
    }
    return doc;
}

std::unique_ptr<Document> loadPublication(const QString &path, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = f.errorString();
        return nullptr;
    }
    return publicationFromBytes(f.readAll(), error);
}

QImage publicationThumbnail(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    QMap<QString, QByteArray> entries;
    if (!readZip(f.readAll(), entries)) return {};
    return QImage::fromData(entries.value("thumbnail.png"), "PNG");
}

} // namespace jp
