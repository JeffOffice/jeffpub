#pragma once
// Compound File Binary format (the "OLE2" container that
// .pub files use): read every storage and stream with its class id, state
// bits and timestamps, and write a valid compound file back.

#include <QByteArray>
#include <QString>
#include <QVector>

namespace jp::cfb {

struct Entry {
    enum Type { Storage = 1, Stream = 2, Root = 5 };
    Type type = Stream;
    QString name;
    QByteArray clsid;            // 16 bytes
    quint32 stateBits = 0;
    quint64 created = 0, modified = 0;
    QByteArray data;             // streams only
    QVector<int> children;       // indexes into File::entries (storages and root)
};

struct File {
    QVector<Entry> entries;      // entries[0] is the root
    // Path lookup with '/' separators, e.g. "Quill/QuillSub/CONTENTS". -1 if absent.
    int find(const QString &path) const;
    QByteArray stream(const QString &path) const;
    bool setStream(const QString &path, const QByteArray &data);   // existing streams only
    QStringList streamPaths() const;
};

bool read(const QByteArray &bytes, File *out, QString *error = nullptr);
QByteArray write(const File &f);   // version 3 (512-byte sectors)

} // namespace jp::cfb
