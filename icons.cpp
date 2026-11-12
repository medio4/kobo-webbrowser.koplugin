#include "icons.h"
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QHash>
#include <QtGui/QPainter>
#include <QtSvg/QSvgRenderer>
#include <cstdlib>

static QString iconDir()
{
    const char *kd = getenv("KOREADER_DIR");
    QString base = (kd && *kd) ? QString::fromLocal8Bit(kd) : QString("/mnt/onboard/.adds/koreader");
    return QDir(base).filePath("resources/icons/mdlight");
}

QPixmap KbIcons::get(const QString &name, int size)
{
    if (getenv("KB_NO_ICONS") || name.isEmpty()) return QPixmap();
    static QHash<QString, QPixmap> cache;
    QString key = name + "@" + QString::number(size);
    if (cache.contains(key)) return cache.value(key);
    QPixmap pm;
    QString path = QDir(iconDir()).filePath(name + ".svg");
    if (QFile::exists(path)) {
        QSvgRenderer r(path);
        if (r.isValid()) {
            QImage img(size, size, QImage::Format_ARGB32_Premultiplied);
            img.fill(Qt::transparent);
            QPainter p(&img);
            p.setRenderHint(QPainter::Antialiasing, true);
            r.render(&p, QRectF(0, 0, size, size));
            p.end();
            pm = QPixmap::fromImage(img);
        }
    }
    cache.insert(key, pm);
    return pm;
}
