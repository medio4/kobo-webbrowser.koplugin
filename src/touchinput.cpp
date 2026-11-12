#include "touchinput.h"
#include "rotation.h"
#include <QtCore/QSocketNotifier>
#include <QtCore/QDebug>
#include <QtGui/QGuiApplication>
#include <QtGui/QWindow>
#include <qpa/qwindowsysteminterface.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <math.h>

#ifndef ABS_MT_SLOT
#define ABS_MT_SLOT 0x2f
#endif
#ifndef ABS_MT_TRACKING_ID
#define ABS_MT_TRACKING_ID 0x39
#endif

KoboTouchInput::KoboTouchInput(const QString &device, int screenW, int screenH,
                               bool swapXY, bool mirrorX, bool mirrorY, bool grab, QObject *parent)
    : QObject(parent), m_fd(-1), m_notifier(0), m_screenW(screenW), m_screenH(screenH),
      m_swapXY(swapXY), m_mirrorX(mirrorX), m_mirrorY(mirrorY),
      m_minX(0), m_maxX(0), m_minY(0), m_maxY(0),
      m_slot(0), m_rawX(0), m_rawY(0), m_down(false), m_pendingDown(false),
      m_havePos(false), m_sentDown(false), m_slot1Down(false), m_multiSeen(false),
      m_raw1X(0), m_raw1Y(0), m_have1(false), m_pinchStart(0), m_pinchLast(0), m_haveMT(false)
{
    m_fd = open(device.toLocal8Bit().constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (m_fd < 0) {
        qWarning() << "touch: cannot open" << device << strerror(errno);
        return;
    }
    struct input_absinfo ai;
    if (ioctl(m_fd, EVIOCGABS(ABS_MT_POSITION_X), &ai) == 0) { m_minX = ai.minimum; m_maxX = ai.maximum; }
    if (ioctl(m_fd, EVIOCGABS(ABS_MT_POSITION_Y), &ai) == 0) { m_minY = ai.minimum; m_maxY = ai.maximum; }
    m_haveMT = (m_maxX > m_minX && m_maxY > m_minY);
    if (!m_haveMT) {
        // Fall back to single-touch axes
        if (ioctl(m_fd, EVIOCGABS(ABS_X), &ai) == 0) { m_minX = ai.minimum; m_maxX = ai.maximum; }
        if (ioctl(m_fd, EVIOCGABS(ABS_Y), &ai) == 0) { m_minY = ai.minimum; m_maxY = ai.maximum; }
    }
    if (grab && ioctl(m_fd, EVIOCGRAB, 1) != 0)
        qWarning() << "touch: EVIOCGRAB failed:" << strerror(errno);
    m_grab = grab; m_handedOff = false;
    qDebug() << "touch: opened" << device << "abs x" << m_minX << m_maxX << "y" << m_minY << m_maxY
             << "swap" << m_swapXY << "mirrorX" << m_mirrorX << "mirrorY" << m_mirrorY;
    m_notifier = new QSocketNotifier(m_fd, QSocketNotifier::Read, this);
    connect(m_notifier, SIGNAL(activated(int)), this, SLOT(readable()));
}

bool KoboTouchInput::s_gesture = false;

KoboTouchInput::~KoboTouchInput()
{
    if (m_fd >= 0) { ioctl(m_fd, EVIOCGRAB, 0); close(m_fd); }
}

void KoboTouchInput::readable()
{
    struct input_event ev[32];
    for (;;) {
        ssize_t n = read(m_fd, ev, sizeof ev);
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) return;
            qWarning() << "touch: read error" << strerror(errno);
            m_notifier->setEnabled(false);
            return;
        }
        if (n == 0) return;
        if (m_handedOff) continue;   // KOReader owns the screen right now: drop our copy
        int count = n / sizeof(struct input_event);
        for (int i = 0; i < count; ++i) {
            const struct input_event &e = ev[i];
            if (e.type == EV_ABS) {
                switch (e.code) {
                case ABS_MT_SLOT:
                    m_slot = e.value;
                    break;
                case ABS_MT_TRACKING_ID:
                    if (e.value == -1) {
                        if (m_slot == 0) m_pendingDown = false;
                        else m_slot1Down = false;
                    } else {
                        // Snow protocol (Clara BW and other Kobo panels, see KOReader's
                        // handleTouchEvSnow): there is never an ABS_MT_SLOT event, the tracking
                        // id IS the slot number (0 = first finger, 1 = second), it is never set
                        // to -1, and lift comes as BTN_TOUCH 0.
                        m_slot = e.value;
                        if (m_slot == 0) m_pendingDown = true;
                        else { m_slot1Down = true; m_multiSeen = true; }
                    }
                    break;
                case ABS_X:
                case ABS_Y:
                    // Single-touch duplicates; with multitouch axes they may describe either
                    // contact (or several on the lift frame), so only use them without MT.
                    if (m_haveMT) break;
                    if (e.code == ABS_X) { m_rawX = e.value; } else { m_rawY = e.value; }
                    m_havePos = true; m_pendingDown = true;
                    break;
                case ABS_MT_POSITION_X:
                    if (m_slot == 0) { m_rawX = e.value; m_havePos = true; m_pendingDown = true; }
                    else { m_raw1X = e.value; m_have1 = true; m_slot1Down = true; m_multiSeen = true; }
                    break;
                case ABS_MT_POSITION_Y:
                    if (m_slot == 0) { m_rawY = e.value; m_havePos = true; m_pendingDown = true; }
                    else { m_raw1Y = e.value; m_have1 = true; m_slot1Down = true; m_multiSeen = true; }
                    break;
                default: break;
                }
            } else if (e.type == EV_KEY && e.code == BTN_TOUCH) {
                m_pendingDown = (e.value != 0);
                if (e.value == 0) { m_slot1Down = false; m_slot = 0; }
            } else if (e.type == EV_SYN && e.code == SYN_REPORT) {
                emitState();
                // The current slot persists: a frame may omit both ABS_MT_SLOT and
                // ABS_MT_TRACKING_ID when the last contact merely moved.
            }
        }
    }
}

void KoboTouchInput::emitState()
{
    if (!m_havePos && !m_sentDown) return;
    // raw -> screen
    int rx = m_rawX, ry = m_rawY;
    int sx, sy;
    if (m_swapXY) { sx = ry; sy = rx; } else { sx = rx; sy = ry; }
    // scale if the axis range doesn't match the screen
    int rangeX = m_swapXY ? (m_maxY - m_minY + 1) : (m_maxX - m_minX + 1);
    int rangeY = m_swapXY ? (m_maxX - m_minX + 1) : (m_maxY - m_minY + 1);
    if (rangeX > 1 && rangeX != m_screenW) sx = (long long)sx * m_screenW / rangeX;
    if (rangeY > 1 && rangeY != m_screenH) sy = (long long)sy * m_screenH / rangeY;
    if (m_mirrorX) sx = (m_screenW - 1) - sx;
    if (m_mirrorY) sy = (m_screenH - 1) - sy;
    if (sx < 0) sx = 0; if (sx >= m_screenW) sx = m_screenW - 1;
    if (sy < 0) sy = 0; if (sy >= m_screenH) sy = m_screenH - 1;
    // Rotated screen (rotation.h): turn the panel point into the window's coordinates.
    QPoint pos = KbRotation::fromPhysical(QPoint(sx, sy));

    QWindow *win = QGuiApplication::focusWindow();
    if (!win) {
        QWindowList wl = QGuiApplication::topLevelWindows();
        if (!wl.isEmpty()) win = wl.first();
    }
    if (!win) return;
    QPointF local = pos - win->geometry().topLeft();

    // Multi-finger gesture: once a second finger shows up, park the first finger's
    // mouse pointer outside the window so no button/link gets clicked, and report a
    // two-finger tap when everything is lifted.
    const QPointF away(-10, -10);
    bool anyDown = m_pendingDown || m_slot1Down;
    if (m_multiSeen && !s_gesture) {
        s_gesture = true;
        if (m_sentDown) QWindowSystemInterface::handleMouseEvent(win, away, away, Qt::LeftButton);
    }
    if (s_gesture) {
        // Track the distance between the two fingers for pinch-to-zoom.
        if (m_pendingDown && m_slot1Down && m_have1) {
            double dx = m_rawX - m_raw1X, dy = m_rawY - m_raw1Y;
            double dist = sqrt(dx * dx + dy * dy);
            if (m_pinchStart <= 0) m_pinchStart = dist;
            m_pinchLast = dist;
        }
        if (!anyDown) {
            if (m_sentDown) QWindowSystemInterface::handleMouseEvent(win, away, away, Qt::NoButton);
            m_sentDown = false;
            s_gesture = false;
            m_multiSeen = false;
            m_have1 = false;
            double ratio = (m_pinchStart > 0) ? m_pinchLast / m_pinchStart : 1.0;
            m_pinchStart = m_pinchLast = 0;
            if (ratio > 1.25 || ratio < 0.8) emit pinch(ratio);
            else emit twoFingerTap();
        }
        m_down = anyDown;
        return;
    }

    static const bool dbg = getenv("KB_TOUCH_DEBUG") != 0;
    if (dbg && m_pendingDown && !m_sentDown) qDebug() << "touch: press" << pos << "raw" << m_rawX << m_rawY;
    if (m_pendingDown && !m_sentDown) {
        QWindowSystemInterface::handleMouseEvent(win, local, QPointF(pos), Qt::LeftButton);
        m_sentDown = true;
        m_lastPos = pos;
    } else if (m_pendingDown && m_sentDown) {
        if (pos != m_lastPos) {
            QWindowSystemInterface::handleMouseEvent(win, local, QPointF(pos), Qt::LeftButton);
            m_lastPos = pos;
        }
    } else if (!m_pendingDown && m_sentDown) {
        QWindowSystemInterface::handleMouseEvent(win, local, QPointF(pos), Qt::NoButton);
        m_sentDown = false;
    }
    m_down = m_pendingDown;
}

void KoboTouchInput::setHandedOff(bool off)
{
    if (m_fd < 0 || off == m_handedOff) return;
    m_handedOff = off;
    if (off) {
        // Forget any contact in progress so no stale press is completed later.
        m_pendingDown = false; m_sentDown = false; m_havePos = false;
        m_slot1Down = false; m_multiSeen = false; s_gesture = false;
        if (m_grab && ioctl(m_fd, EVIOCGRAB, 0) != 0)
            qWarning() << "touch: EVIOCGRAB release failed:" << strerror(errno);
    } else {
        if (m_grab && ioctl(m_fd, EVIOCGRAB, 1) != 0)
            qWarning() << "touch: EVIOCGRAB failed:" << strerror(errno);
    }
    qDebug() << "touch:" << (off ? "handed off to KOReader" : "taken back");
}
