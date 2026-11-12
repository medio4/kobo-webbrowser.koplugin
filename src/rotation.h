// Screen rotation in software (Menu > Rotate left / Rotate right). The Clara BW has no
// rotation sensor and the framebuffer stays in portrait, so the browser lays its window
// out at the rotated (logical) size, turns each painted rect as it copies it to /dev/fb0
// (eink.cpp, grab mode), and turns touch points back (touchinput.cpp).
// Angle is clockwise: 0 = portrait, 90 = landscape (top of the page on the right edge),
// 180 = portrait upside down, 270 = landscape (top on the left edge). Saved in
// bin/rotation.txt; KB_ROTATE=0|90|180|270 overrides it at start.
#ifndef ROTATION_H
#define ROTATION_H
#include <QtCore/QRect>
#include <QtCore/QSize>
#include <QtCore/QPoint>
#include <QtGui/QImage>
namespace KbRotation {
    void init(int physW, int physH);   // panel size in portrait (from the screen)
    int angle();
    void setAngle(int a);              // 0/90/180/270 (others rounded); does not save
    int loadSaved();                   // KB_ROTATE, else bin/rotation.txt, else 0
    void save();
    QSize physicalSize();
    QSize logicalSize();               // the size the window is laid out at
    QRect toPhysical(const QRect &logical);
    QPoint fromPhysical(const QPoint &physical);
    QImage toPhysical(const QImage &logical);   // the picture turned to the panel's orientation
    QString name();                    // "Portrait", "Landscape", ...
}
#endif
