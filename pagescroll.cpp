#include "pagescroll.h"
#include <QtWebKitWidgets/QWebView>
#include <QtWebKitWidgets/QWebFrame>
#include <QtWebKitWidgets/QWebPage>
#include <QtCore/QDebug>
#include <stdlib.h>

static double envDouble(const char *name, double def)
{
    const char *v = getenv(name);
    if (!v || !*v) return def;
    return atof(v);
}

PageScroller::PageScroller(QWebView *view)
    : QObject(view), m_view(view), m_enabled(true), m_pressed(false), m_swiped(false)
{
    // Tunables (environment, see run.sh): overlap and tap-zone width as fractions.
    m_overlapFrac = envDouble("KB_PAGE_OVERLAP", 0.06);    // ~6% of the screen stays visible
    m_overlapMin  = (int)envDouble("KB_PAGE_OVERLAP_MIN", 30);
    m_zoneFrac    = envDouble("KB_TAP_ZONE", 0.14);        // left/right 14% strips turn pages
    m_swipeMin    = (int)envDouble("KB_SWIPE_MIN", 60);
}

bool PageScroller::inTapZone(const QPoint &p) const
{
    if (m_zoneFrac <= 0) return false;
    int zone = (int)(m_view->width() * m_zoneFrac);
    return p.x() < zone || p.x() >= m_view->width() - zone;
}

bool PageScroller::handlePress(const QPoint &p)
{
    if (!m_enabled) return false;
    m_pressed = true;
    m_swiped = false;
    m_press = p;
    return true;
}

bool PageScroller::handleMove(const QPoint &p)
{
    if (!m_enabled || !m_pressed) return false;
    // No live scrolling in page mode: just remember whether the finger moved far
    // enough to count as a swipe. The turn happens on release.
    if ((p - m_press).manhattanLength() >= m_swipeMin) m_swiped = true;
    return true;
}

bool PageScroller::handleRelease(const QPoint &p)
{
    if (!m_enabled || !m_pressed) return false;
    m_pressed = false;
    QPoint d = p - m_press;
    if (m_swiped || d.manhattanLength() >= m_swipeMin) {
        // Dominant axis decides. Up/left = forward, down/right = back.
        if (qAbs(d.y()) >= qAbs(d.x())) {
            if (d.y() < 0) nextPage(); else prevPage();
        } else {
            // Horizontal swipe: shift sideways when the page is wider than the view,
            // otherwise treat it like a page turn.
            if (!turn(d.x() < 0 ? 1 : -1, 0)) {
                if (d.x() < 0) nextPage(); else prevPage();
            }
        }
        return true;
    }
    if (inTapZone(m_press)) {
        if (m_press.x() < m_view->width() / 2) prevPage(); else nextPage();
        return true;
    }
    return false;   // plain tap: let the view forward it as a click
}

void PageScroller::nextPage() { turn(0, 1); }
void PageScroller::prevPage() { turn(0, -1); }

bool PageScroller::turn(int dxScreens, int dyScreens)
{
    QWebFrame *f = m_view->page() ? m_view->page()->mainFrame() : 0;
    if (!f) return false;
    int h = m_view->height(), w = m_view->width();
    int overlapY = qMax(m_overlapMin, (int)(h * m_overlapFrac));
    int overlapX = qMax(m_overlapMin, (int)(w * m_overlapFrac));
    int stepY = qMax(1, h - overlapY);
    int stepX = qMax(1, w - overlapX);
    QPoint before = f->scrollPosition();
    QPoint target = before + QPoint(dxScreens * stepX, dyScreens * stepY);
    QSize content = f->contentsSize();
    int maxX = qMax(0, content.width() - w), maxY = qMax(0, content.height() - h);
    target.setX(qBound(0, target.x(), maxX));
    target.setY(qBound(0, target.y(), maxY));
    if (target == before) return false;
    emit pageTurned();
    f->setScrollPosition(target);
    return true;
}
