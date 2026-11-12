#include "scrollmark.h"
#include <QtCore/QEvent>
#include <QtCore/QTimer>
#include <QtGui/QPainter>
#include <QtWebKitWidgets/QWebView>
#include <QtWebKitWidgets/QWebPage>
#include <QtWebKitWidgets/QWebFrame>

static const int kThick = 8;

ScrollMark::ScrollMark(QWebView *view) : QWidget(view), m_view(view)
{
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    QWebFrame *f = view->page()->mainFrame();
    f->setScrollBarPolicy(Qt::Vertical, Qt::ScrollBarAlwaysOff);
    f->setScrollBarPolicy(Qt::Horizontal, Qt::ScrollBarAlwaysOff);
    connect(view->page(), SIGNAL(scrollRequested(int,int,QRect)), this, SLOT(refresh()));
    connect(f, SIGNAL(contentsSizeChanged(QSize)), this, SLOT(refresh()));
    connect(view, SIGNAL(loadFinished(bool)), this, SLOT(refresh()));
    view->installEventFilter(this);
    resize(view->size());
    show();
}

bool ScrollMark::eventFilter(QObject *obj, QEvent *e)
{
    if (obj == m_view && e->type() == QEvent::Resize) { resize(m_view->size()); refresh(); }
    return false;
}

void ScrollMark::refresh()
{
    QWebFrame *f = m_view->page()->mainFrame();
    QSize content = f->contentsSize();
    QPoint pos = f->scrollPosition();
    QSize vp = m_view->size();
    QRect v, h;
    if (content.height() > vp.height() + 2) {
        int track = vp.height() - 8;
        int len = qMax(60, track * vp.height() / content.height());
        int maxScroll = content.height() - vp.height();
        int y = 4 + (maxScroll > 0 ? (track - len) * qBound(0, pos.y(), maxScroll) / maxScroll : 0);
        v = QRect(vp.width() - kThick - 2, y, kThick, len);
    }
    if (content.width() > vp.width() + 2) {
        int track = vp.width() - 8 - (v.isNull() ? 0 : kThick + 4);
        int len = qMax(60, track * vp.width() / content.width());
        int maxScroll = content.width() - vp.width();
        int x = 4 + (maxScroll > 0 ? (track - len) * qBound(0, pos.x(), maxScroll) / maxScroll : 0);
        h = QRect(x, vp.height() - kThick - 2, len, kThick);
    }
    if (v == m_vThumb && h == m_hThumb) return;
    QRegion dirty;
    dirty += m_vThumb.adjusted(-1, -1, 1, 1); dirty += v.adjusted(-1, -1, 1, 1);
    dirty += m_hThumb.adjusted(-1, -1, 1, 1); dirty += h.adjusted(-1, -1, 1, 1);
    m_vThumb = v; m_hThumb = h;
    raise();
    update(dirty);
}

void ScrollMark::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(40, 40, 40));
    if (!m_vThumb.isNull()) p.drawRoundedRect(m_vThumb, kThick / 2, kThick / 2);
    if (!m_hThumb.isNull()) p.drawRoundedRect(m_hThumb, kThick / 2, kThick / 2);
}
