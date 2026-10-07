#pragma once
// Minimal ZIP reader/writer for the .jpub container (stored entries) and
// EPUB books (text compressed).

#include <QByteArray>
#include <QMap>
#include <QString>

namespace jp {

class ZipWriter {
public:
    // `compress` deflates the entry when that makes it smaller.
    void add(const QString &name, const QByteArray &data, bool compress = false);
    QByteArray finish();
private:
    struct Entry { QString name; quint32 crc; quint32 size; quint32 csize; quint16 method; quint32 offset; };
    QByteArray m_out;
    QVector<Entry> m_entries;
};

// Returns entries by name; empty map and false if the data is not a readable
// zip. Deflated entries are inflated (up to 1 GB each, checked by CRC).
bool readZip(const QByteArray &zip, QMap<QString, QByteArray> &out, QString *error = nullptr);
// One stored entry read straight from a file (the directory, then just that
// entry), without loading the rest; empty if it isn't there.
QByteArray readZipEntry(const QString &path, const QString &name);

quint32 crc32(const QByteArray &data);

} // namespace jp
