#pragma once
// Windows Metafile (WMF) and Enhanced Metafile (EMF) player. Old .pub
// clip art is stored in these formats; this draws them as vectors through
// QPainter so they stay sharp on screen, in print and in PDFs.

#include <functional>

#include <QByteArray>
#include <QColor>
#include <QImage>
#include <QRectF>

class QPainter;

namespace jp {

class Metafile {
public:
    bool load(const QByteArray &data);
    bool isValid() const { return m_valid; }
    bool isEmf() const { return m_emf; }
    QSizeF naturalSize() const;          // points
    // `recolor`, when given, changes every color the picture draws (a
    // picture's recoloring, kept as vectors).
    void play(QPainter *p, const QRectF &target, const std::function<QColor(const QColor &)> &recolor = {}) const;
    QImage toImage(int maxSide = 1600) const;

    static bool looksLikeMetafile(const QByteArray &data);

private:
    QByteArray m_data;
    bool m_valid = false;
    bool m_emf = false;
    QRectF m_bounds;      // logical (WMF) or device (EMF) bounds
    QSizeF m_sizePt;
};

} // namespace jp
