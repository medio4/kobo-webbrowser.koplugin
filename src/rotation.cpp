#include "rotation.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtGui/QTransform>
#include <cstdlib>

static int s_angle = 0;
static int s_pw = 1072, s_ph = 1448;

static int normalize(int a)
{
    a = ((a % 360) + 360) % 360;
    return ((a + 45) / 90 % 4) * 90;
}

static QString savePath()
{
    return QDir(QCoreApplication::applicationDirPath()).filePath("rotation.txt");
}

void KbRotation::init(int w, int h)
{
    if (w > 0 && h > 0) { s_pw = w; s_ph = h; }
}

int KbRotation::angle() { return s_angle; }
void KbRotation::setAngle(int a) { s_angle = normalize(a); }

int KbRotation::loadSaved()
{
    const char *env = getenv("KB_ROTATE");
    if (env && *env) return normalize(atoi(env));
    QFile f(savePath());
    if (f.open(QIODevice::ReadOnly)) return normalize(f.readAll().trimmed().toInt());
    return 0;
}

void KbRotation::save()
{
    QFile f(savePath());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QByteArray::number(s_angle) + '\n');
}

QSize KbRotation::physicalSize() { return QSize(s_pw, s_ph); }
QSize KbRotation::logicalSize()
{
    return (s_angle == 90 || s_angle == 270) ? QSize(s_ph, s_pw) : QSize(s_pw, s_ph);
}

// Logical (x, y) -> panel (px, py), turning the picture clockwise by the angle.
static QPoint mapPoint(int x, int y)
{
    switch (s_angle) {
    case 90:  return QPoint(s_pw - 1 - y, x);
    case 180: return QPoint(s_pw - 1 - x, s_ph - 1 - y);
    case 270: return QPoint(y, s_ph - 1 - x);
    default:  return QPoint(x, y);
    }
}

QRect KbRotation::toPhysical(const QRect &r)
{
    if (!s_angle || r.isEmpty()) return r;
    QPoint a = mapPoint(r.left(), r.top()), b = mapPoint(r.right(), r.bottom());
    return QRect(QPoint(qMin(a.x(), b.x()), qMin(a.y(), b.y())),
                 QPoint(qMax(a.x(), b.x()), qMax(a.y(), b.y())));
}

QPoint KbRotation::fromPhysical(const QPoint &p)
{
    switch (s_angle) {
    case 90:  return QPoint(p.y(), s_pw - 1 - p.x());
    case 180: return QPoint(s_pw - 1 - p.x(), s_ph - 1 - p.y());
    case 270: return QPoint(s_ph - 1 - p.y(), p.x());
    default:  return p;
    }
}

QImage KbRotation::toPhysical(const QImage &img)
{
    if (!s_angle || img.isNull()) return img;
    // Multiples of 90 degrees take QImage's exact pixel-rotation path (no smoothing).
    return img.transformed(QTransform().rotate(s_angle));
}

QString KbRotation::name()
{
    switch (s_angle) {
    case 90:  return "Landscape";
    case 180: return "Portrait (upside down)";
    case 270: return "Landscape (flipped)";
    default:  return "Portrait";
    }
}
