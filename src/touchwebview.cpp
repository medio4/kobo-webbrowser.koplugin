// TouchWebView: the page view with swipe/drag scrolling and edge gestures.
#include "browser.h"
#include "uiscale.h"
#include "crashhandler.h"
#include "touchinput.h"
#include "eink.h"
#include "webpage.h"
#include "pagescroll.h"
#include <QtWebKit/QWebHistory>
#include <QtCore/QDataStream>
#include <QtCore/QFileInfo>
#include <QtCore/QDateTime>
#include "epub.h"
#include "phoneshare.h"
#include "menupanel.h"
#include "bookmarks.h"
#include "scrollmark.h"
#include "adblock.h"
#include "toc.h"
#include "memtune.h"
#include "power.h"
#include "textentry.h"
#include "rotation.h"
#include "icons.h"
#include <QtWidgets/QFrame>
#include <QtGui/QBackingStore>

#include <QtWidgets/QApplication>
#include <QtWidgets/QSizePolicy>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QStyle>
#include <QtGui/QGenericPlugin>
#include <QtGui/QGenericPluginFactory>
#include <QtGui/QMouseEvent>
#include <QtGui/QKeyEvent>
#include <QtGui/QFont>
#include <QtGui/QPainter>
#include <QtGui/QPen>
#include <QtGui/QPalette>
#include <QtGui/QScreen>
#include <QtGui/QWindow>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QSslConfiguration>
#include <QtNetwork/QSslSocket>
#include <QtNetwork/QSslCertificate>
#include <QtNetwork/QNetworkDiskCache>
#include <QtCore/QDebug>
#include <QtCore/QLoggingCategory>
#include <QtCore/QDir>
#include <QtCore/QRegExp>
#include <QtCore/QStringList>
#include <QtCore/QTime>
#include <QtCore/QFile>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtWebKit/QWebSettings>
#include <QtWebKitWidgets/QWebFrame>
#include <QtWebKitWidgets/QWebPage>
#include <QtWebKitWidgets/QWebView>

#include <cstdlib>
#include <cstring>
#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>


// ---------------------------------------------------------------- EdgeBadge

// The round arrow that shows while a back / forward swipe from the edge is held, like the
// one in Chrome on a phone: let go to go back, slide back to the edge to cancel. An
// in-window child of the page view, so it reaches the e-ink grab path; "kb_overlay" gets
// its area refreshed cleanly when it goes away.
class EdgeBadge : public QWidget {
public:
    explicit EdgeBadge(QWidget *parent) : QWidget(parent), m_forward(false)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setProperty("kb_overlay", true);
        const int d = kbPx(132);
        resize(d, d);
        hide();
    }
    void setForward(bool forward) { m_forward = forward; }
protected:
    void paintEvent(QPaintEvent *)
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const int w = width(), pen = qMax(3, kbPx(5));
        p.setPen(QPen(Qt::black, pen));
        p.setBrush(Qt::white);
        p.drawEllipse(QRectF(pen, pen, w - 2 * pen, w - 2 * pen));
        // An arrow: shaft and head, pointing left for back.
        const qreal c = w / 2.0, a = w * 0.22, dir = m_forward ? 1.0 : -1.0;
        QPen ap(Qt::black, qMax(4, kbPx(9)), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(ap);
        p.drawLine(QPointF(c - dir * a, c), QPointF(c + dir * a, c));
        QPolygonF head;
        head << QPointF(c + dir * a * 0.15, c - a * 0.85) << QPointF(c + dir * a, c) << QPointF(c + dir * a * 0.15, c + a * 0.85);
        p.setBrush(Qt::NoBrush);
        p.drawPolyline(head);
    }
private:
    bool m_forward;
};

// ---------------------------------------------------------------- TouchWebView

TouchWebView::TouchWebView(QWidget *parent)
    : QWebView(parent), m_pressed(false), m_dragging(false), m_forwarding(false),
      m_horizontal(false), m_live(false), m_light(NoLight), m_lightStart(0), m_edge(NoEdge), m_edgeArmed(false)
{
    const char *le = getenv("KB_LIGHT_EDGE");
    m_lightEdge = (le && *le) ? atoi(le) : 48;
    m_pager = new PageScroller(this);
    // Edge swipes: in from the left = back one page in history, in from the right = forward.
    connect(m_pager, SIGNAL(goBack()), this, SLOT(back()));
    connect(m_pager, SIGNAL(goForward()), this, SLOT(forward()));
    m_badge = new EdgeBadge(this);
    m_badgeTimer.setSingleShot(true);
    connect(&m_badgeTimer, SIGNAL(timeout()), this, SLOT(showEdgeBadge()));
}

bool TouchWebView::event(QEvent *e)
{
    // Qt synthesizes mouse events from touch; we only use the mouse events.
    if (e->type() == QEvent::TouchBegin || e->type() == QEvent::TouchUpdate ||
        e->type() == QEvent::TouchEnd) {
        e->ignore();
        return false;
    }
    return QWebView::event(e);
}

void TouchWebView::mousePressEvent(QMouseEvent *e)
{
    if (m_forwarding) { QWebView::mousePressEvent(e); return; }
    m_pressed = true;
    m_dragging = false;
    m_horizontal = false;
    m_live = false;
    m_light = NoLight;
    endEdgeDrag();
    m_pressPos = e->pos();
    m_lastPos = e->pos();
    m_pressTime.start();
    e->accept();
}

// One gesture model, no modes: a drag locks to an axis once it starts. Vertical: if
// the finger is still down after KB_FLICK_MS it becomes live scrolling (the page
// follows the finger in big steps); released before that it was a flick and turns one
// page. Horizontal: a swipe (page turn, or history back/forward from the edges).
void TouchWebView::mouseMoveEvent(QMouseEvent *e)
{
    if (m_forwarding) { QWebView::mouseMoveEvent(e); return; }
    if (!m_pressed) return;
    if (KoboTouchInput::gestureActive()) { m_pressed = false; m_dragging = false; endEdgeDrag(); e->accept(); return; }
    QPoint fromPress = e->pos() - m_pressPos;
    if (!m_dragging && fromPress.manhattanLength() > 25) {
        m_dragging = true;
        m_horizontal = qAbs(fromPress.x()) > qAbs(fromPress.y());
        // Vertical drag from the very left edge = frontlight, right edge = warmth (KOReader).
        KoboFrontlight *fl = KoboFrontlight::instance();
        if (!m_horizontal && fl && fl->available() && m_lightEdge > 0) {
            if (m_pressPos.x() < m_lightEdge) { m_light = LightBrightness; m_lightStart = fl->brightness(); }
            else if (m_pressPos.x() >= width() - m_lightEdge && fl->hasWarmth()) { m_light = LightWarmth; m_lightStart = fl->warmth(); }
        }
        // Horizontal drag in from the very left / right edge = history back / forward.
        const int ep = m_pager->edgePx();
        if (m_horizontal && ep > 0) {
            if (m_pressPos.x() < ep && fromPress.x() > 0) m_edge = EdgeBack;
            else if (m_pressPos.x() >= width() - ep && fromPress.x() < 0) m_edge = EdgeForward;
        }
    }
    if (m_light != NoLight) {
        // Up = brighter / warmer. Three quarters of the page height covers the full range.
        KoboFrontlight *fl = KoboFrontlight::instance();
        int v = m_lightStart + (-fromPress.y()) * 100 / qMax(1, height() * 3 / 4);
        if (m_light == LightBrightness) fl->setBrightness(v); else fl->setWarmth(v);
        e->accept();
        return;
    }
    if (m_edge != NoEdge) {
        // Armed once the finger is a swipe's length in from the edge. Held there, the round
        // arrow appears (after the flick time, so a quick flick doesn't draw it for nothing).
        const int along = m_edge == EdgeBack ? fromPress.x() : -fromPress.x();
        const bool armed = along >= m_pager->swipeMin();
        m_edgePos = e->pos();
        if (armed != m_edgeArmed) {
            m_edgeArmed = armed;
            if (armed) m_badgeTimer.start(qMax(0, m_pager->flickMs() - int(m_pressTime.elapsed())));
            else { m_badgeTimer.stop(); m_badge->hide(); }
        }
        e->accept();
        return;
    }
    if (m_dragging) {
        // Same rule on both axes: held past the flick time, the drag follows the finger
        // (vertical scroll, or horizontal panning for wide tables / zoomed-in pages).
        if (!m_live && m_pressTime.elapsed() >= m_pager->flickMs()) {
            m_live = true;          // sustained drag: catch up with the finger, then follow it
            if (m_horizontal) {
                // Pan the wide thing under the finger (table, image box, inner frame),
                // or the page when nothing there scrolls sideways.
                m_hTarget = m_pager->findHScrollable(m_pressPos);
                m_pager->panHorizontal(m_hTarget, -fromPress.x());
            } else {
                page()->mainFrame()->scroll(0, -fromPress.y());
            }
            m_lastPos = e->pos();
        } else if (m_live) {
            // Scroll in big steps only; e-ink can't follow continuous scrolling well.
            QPoint delta = e->pos() - m_lastPos;
            if (m_horizontal ? qAbs(delta.x()) >= 40 : qAbs(delta.y()) >= 40) {
                if (m_horizontal) m_pager->panHorizontal(m_hTarget, -delta.x());
                else page()->mainFrame()->scroll(0, -delta.y());
                m_lastPos = e->pos();
            }
        }
    }
    e->accept();
}

void TouchWebView::mouseReleaseEvent(QMouseEvent *e)
{
    if (m_forwarding) { QWebView::mouseReleaseEvent(e); return; }
    if (!m_pressed) return;
    m_pressed = false;
    if (KoboTouchInput::gestureActive() || e->pos().x() < 0) { m_dragging = false; m_light = NoLight; endEdgeDrag(); e->accept(); return; }
    if (m_light != NoLight) {
        KoboFrontlight::instance()->announce(m_light == LightWarmth);
        m_light = NoLight;
        m_dragging = false;
        e->accept();
        return;
    }
    if (m_dragging) {
        m_dragging = false;
        if (m_edge != NoEdge) {
            // Back / forward from the edge: go if the finger ended far enough in, else cancel.
            const EdgeDrag edge = m_edge;
            const bool go = m_edgeArmed;
            endEdgeDrag();
            if (go) { if (edge == EdgeBack) back(); else forward(); }
            e->accept();
            return;
        }
        QPoint d = e->pos() - m_pressPos;
        if (m_live) {
            // Finish the live drag/pan: scroll by the remaining delta.
            QPoint delta = e->pos() - m_lastPos;
            if (m_horizontal) { m_pager->panHorizontal(m_hTarget, -delta.x()); m_hTarget = PageScroller::HTarget(); }
            else if (delta.y() != 0) page()->mainFrame()->scroll(0, -delta.y());
        } else if (m_horizontal) {
            // Quick horizontal flick: page turn, or history from the edges.
            m_pager->handleHorizontalSwipe(m_pressPos, e->pos());
        } else if (qAbs(d.y()) >= m_pager->swipeMin()) {
            // Flick: released quickly after a decent move. Up = next page, down = previous.
            if (d.y() < 0) m_pager->nextPage(); else m_pager->prevPage();
        }
        e->accept();
        return;
    }
    // Short tap: replay as a real click at that spot.
    m_forwarding = true;
    QMouseEvent press(QEvent::MouseButtonPress, m_pressPos, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QWebView::mousePressEvent(&press);
    QMouseEvent release(QEvent::MouseButtonRelease, m_pressPos, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QWebView::mouseReleaseEvent(&release);
    m_forwarding = false;
    e->accept();
}

void TouchWebView::showEdgeBadge()
{
    if (m_edge == NoEdge || !m_edgeArmed) return;
    EdgeBadge *b = static_cast<EdgeBadge *>(m_badge);
    b->setForward(m_edge == EdgeForward);
    const int margin = kbPx(28);
    int x = m_edge == EdgeBack ? margin : width() - b->width() - margin;
    int y = qBound(margin, m_edgePos.y() - b->height() / 2, qMax(margin, height() - b->height() - margin));
    b->move(x, y);
    b->raise();
    b->show();
}

void TouchWebView::endEdgeDrag()
{
    m_edge = NoEdge;
    m_edgeArmed = false;
    m_badgeTimer.stop();
    if (m_badge->isVisible()) m_badge->hide();
}
