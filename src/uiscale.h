// Sizes in the browser's own interface (toolbar, buttons, fonts, dialogs, the keyboard) are
// written in pixels for the Clara BW's 300 dpi screen. kbPx() turns such a number into the
// same physical size on this screen, from the dpi the KOReader plugin reports (KB_DPI).
// At 300 dpi, or without KB_DPI, it returns the number unchanged.
#ifndef KB_UISCALE_H
#define KB_UISCALE_H
#include <QtCore/QByteArray>
#include <QtCore/QtGlobal>

inline double kbUiScale()
{
    static double scale = 0;
    if (scale == 0) {
        int dpi = qgetenv("KB_DPI").toInt();
        scale = (dpi >= 100 && dpi <= 600 && dpi != 300) ? dpi / 300.0 : 1.0;
    }
    return scale;
}

inline int kbPx(int px)
{
    const double s = kbUiScale();
    if (s == 1.0 || px == 0) return px;
    int v = qRound(px * s);
    return v == 0 ? (px > 0 ? 1 : -1) : v;
}

#endif
