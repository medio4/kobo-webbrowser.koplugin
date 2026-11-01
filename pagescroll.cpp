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
    : QObject(view), m_view(view)
{
    // Tunables (environment, see run.sh).
    m_overlapFrac = envDouble("KB_PAGE_OVERLAP", 0.06);    // ~6% of the screen stays visible
    m_overlapMin  = (int)envDouble("KB_PAGE_OVERLAP_MIN", 30);
    m_swipeMin    = (int)envDouble("KB_SWIPE_MIN", 60);
    m_flickMs     = (int)envDouble("KB_FLICK_MS", 300);    // held longer than this = live drag
    m_edgePx      = (int)envDouble("KB_EDGE_SWIPE", 48);   // 0 disables edge swipes
}

bool PageScroller::handleHorizontalSwipe(const QPoint &press, const QPoint &release)
{
    QPoint d = release - press;
    if (qAbs(d.x()) < m_swipeMin || qAbs(d.x()) < qAbs(d.y())) return false;
    int w = m_view->width();
    if (m_edgePx > 0 && press.x() < m_edgePx && d.x() > 0) { emit goBack(); return true; }
    if (m_edgePx > 0 && press.x() >= w - m_edgePx && d.x() < 0) { emit goForward(); return true; }
    if (d.x() < 0) nextPage(); else prevPage();
    return true;
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
