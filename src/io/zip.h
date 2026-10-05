#pragma once
// Minimal ZIP reader/writer (stored entries) for the .jpub container.

#include <QByteArray>
#include <QMap>
#include <QString>

namespace jp {

class ZipWriter {
public:
    void add(const QString &name, const QByteArray &data);
    QByteArray finish();
private:
    struct Entry { QString name; quint32 crc; quint32 size; quint32 offset; };
    QByteArray m_out;
    QVector<Entry> m_entries;
};

// Returns entries by name; empty map and false if the data is not a readable zip.
bool readZip(const QByteArray &zip, QMap<QString, QByteArray> &out, QString *error = nullptr);

quint32 crc32(const QByteArray &data);

} // namespace jp
