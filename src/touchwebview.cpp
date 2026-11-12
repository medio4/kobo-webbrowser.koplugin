// TouchWebView: the page view with swipe/drag scrolling and edge gestures.
#include "browser.h"
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


// ---------------------------------------------------------------- TouchWebView

TouchWebView::TouchWebView(QWidget *parent)
    : QWebView(parent), m_pressed(false), m_dragging(false), m_forwarding(false),
      m_horizontal(false), m_live(false), m_light(NoLight), m_lightStart(0)
{
    const char *le = getenv("KB_LIGHT_EDGE");
    m_lightEdge = (le && *le) ? atoi(le) : 48;
    m_pager = new PageScroller(this);
    // Edge swipes: in from the left = back one page in history, in from the right = forward.
    connect(m_pager, SIGNAL(goBack()), this, SLOT(back()));
    connect(m_pager, SIGNAL(goForward()), this, SLOT(forward()));
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
    if (KoboTouchInput::gestureActive()) { m_pressed = false; m_dragging = false; e->accept(); return; }
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
    }
    if (m_light != NoLight) {
        // Up = brighter / warmer. Three quarters of the page height covers the full range.
        KoboFrontlight *fl = KoboFrontlight::instance();
        int v = m_lightStart + (-fromPress.y()) * 100 / qMax(1, height() * 3 / 4);
        if (m_light == LightBrightness) fl->setBrightness(v); else fl->setWarmth(v);
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
    if (KoboTouchInput::gestureActive() || e->pos().x() < 0) { m_dragging = false; m_light = NoLight; e->accept(); return; }
    if (m_light != NoLight) {
        KoboFrontlight::instance()->announce(m_light == LightWarmth);
        m_light = NoLight;
        m_dragging = false;
        e->accept();
        return;
    }
    if (m_dragging) {
        m_dragging = false;
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
