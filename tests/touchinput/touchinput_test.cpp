// Desktop test for src/touchinput.cpp: evdev frames are written to a FIFO that stands in for
// /dev/input/eventN, and the mouse events the window receives are checked. Covers the Snow
// protocol (Clara BW) and standard multitouch protocol B (Libra Colour, issue #3).
// Run with tests/touchinput/run.sh.
#include "../../src/touchinput.h"
#include "../../src/rotation.h"
#include <QtGui/QGuiApplication>
#include <QtGui/QWindow>
#include <QtGui/QMouseEvent>
#include <QtCore/QStringList>
#include <QtCore/QDebug>
#include <linux/input.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>

static int failures = 0;
#define CHECK_EQ(a, b) do { QString x_ = (a), y_ = (b); if (x_ != y_) { ++failures; \
    printf("FAIL %s:%d: %s\n  got:  %s\n  want: %s\n", __FILE__, __LINE__, #a, qPrintable(x_), qPrintable(y_)); } } while (0)

// Records "P x,y" (press), "M x,y" (move) and "R" (release); the parked pointer of a
// two-finger gesture shows up as -10,-10.
class Win : public QWindow {
public:
    QStringList log;
protected:
    // Screen positions, so a window offset doesn't matter. Newer Qt adds a button-less move
    // before a press at a new place: not recorded.
    void mousePressEvent(QMouseEvent *e) { log << at("P", e); }
    void mouseMoveEvent(QMouseEvent *e) { if (e->buttons()) log << at("M", e); }
    static QString at(const char *what, QMouseEvent *e)
    { return QString("%1 %2,%3").arg(what).arg(e->globalPos().x()).arg(e->globalPos().y()); }
    void mouseReleaseEvent(QMouseEvent *) { log << "R"; }
};

struct Rig {
    QString path;
    int wfd;
    KoboTouchInput *in;
    int taps, pinches;
    Rig(const char *name, bool mirrorX, bool mirrorY) : wfd(-1), taps(0), pinches(0)
    {
        path = QString("/tmp/kb-touch-test-%1-%2").arg(getpid()).arg(name);
        unlink(path.toLocal8Bit().constData());
        mkfifo(path.toLocal8Bit().constData(), 0600);
        in = new KoboTouchInput(path, 1000, 2000, true, mirrorX, mirrorY, false);
        wfd = open(path.toLocal8Bit().constData(), O_WRONLY);
        QObject::connect(in, &KoboTouchInput::twoFingerTap, [this]() { ++taps; });
        QObject::connect(in, &KoboTouchInput::pinch, [this](double) { ++pinches; });
    }
    ~Rig() { delete in; close(wfd); unlink(path.toLocal8Bit().constData()); }
    void ev(int type, int code, int value)
    {
        struct input_event e; memset(&e, 0, sizeof e);
        e.type = type; e.code = code; e.value = value;
        if (write(wfd, &e, sizeof e) != (ssize_t)sizeof e) { ++failures; printf("FAIL write\n"); }
    }
    void abs(int code, int value) { ev(EV_ABS, code, value); }
    void syn() { ev(EV_SYN, SYN_REPORT, 0); QCoreApplication::processEvents(); QCoreApplication::processEvents(); }
};

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    KbRotation::init(1000, 2000);
    Win win;
    win.setGeometry(0, 0, 1000, 2000);
    win.show();
    QCoreApplication::processEvents();

    // Raw panel axes are swapped: raw x runs along the screen's height.
    {   // Snow (Clara BW): tracking id = slot, never -1, lift is BTN_TOUCH 0. Swap + mirror X.
        Rig r("snow", true, false);
        r.abs(ABS_MT_TRACKING_ID, 0); r.abs(ABS_MT_POSITION_X, 300); r.abs(ABS_MT_POSITION_Y, 100);
        r.ev(EV_KEY, BTN_TOUCH, 1); r.syn();
        r.abs(ABS_MT_TRACKING_ID, 0); r.abs(ABS_MT_POSITION_X, 320); r.syn();
        r.ev(EV_KEY, BTN_TOUCH, 0); r.syn();
        CHECK_EQ(win.log.join(" | "), "P 899,300 | M 899,320 | R");
        win.log.clear();
        // Two-finger tap: the second finger has tracking id 1.
        r.abs(ABS_MT_TRACKING_ID, 0); r.abs(ABS_MT_POSITION_X, 300); r.abs(ABS_MT_POSITION_Y, 100);
        r.ev(EV_KEY, BTN_TOUCH, 1); r.syn();
        r.abs(ABS_MT_TRACKING_ID, 1); r.abs(ABS_MT_POSITION_X, 400); r.abs(ABS_MT_POSITION_Y, 200); r.syn();
        r.ev(EV_KEY, BTN_TOUCH, 0); r.syn();
        CHECK_EQ(win.log.join(" | "), "P 899,300 | M -10,-10 | R");
        CHECK_EQ(QString::number(r.taps), "1");
        win.log.clear();
        // And a plain tap still works afterwards.
        r.abs(ABS_MT_TRACKING_ID, 0); r.abs(ABS_MT_POSITION_X, 500); r.abs(ABS_MT_POSITION_Y, 100);
        r.ev(EV_KEY, BTN_TOUCH, 1); r.syn();
        r.ev(EV_KEY, BTN_TOUCH, 0); r.syn();
        CHECK_EQ(win.log.join(" | "), "P 899,500 | R");
        win.log.clear();
    }
    {   // Protocol B (Libra Colour): growing tracking ids, -1 on lift, no BTN_TOUCH needed.
        // Swap + mirror Y. The very first touch already has a large id (KOReader used the
        // panel before us) and must work.
        Rig r("b", false, true);
        r.abs(ABS_MT_TRACKING_ID, 57); r.abs(ABS_MT_POSITION_X, 300); r.abs(ABS_MT_POSITION_Y, 100); r.syn();
        r.abs(ABS_MT_POSITION_X, 320); r.syn();
        r.abs(ABS_MT_TRACKING_ID, -1); r.syn();
        CHECK_EQ(win.log.join(" | "), "P 100,1699 | M 100,1679 | R");
        win.log.clear();
        for (int id = 58; id < 60; ++id) {   // later taps, each with a new id
            r.abs(ABS_MT_TRACKING_ID, id); r.abs(ABS_MT_POSITION_X, 300); r.abs(ABS_MT_POSITION_Y, 100); r.syn();
            r.abs(ABS_MT_TRACKING_ID, -1); r.syn();
        }
        CHECK_EQ(win.log.join(" | "), "P 100,1699 | R | P 100,1699 | R");
        win.log.clear();
        // Pinch: second finger in slot 1, the first finger lifts first.
        r.abs(ABS_MT_TRACKING_ID, 60); r.abs(ABS_MT_POSITION_X, 300); r.abs(ABS_MT_POSITION_Y, 100); r.syn();
        r.abs(ABS_MT_SLOT, 1); r.abs(ABS_MT_TRACKING_ID, 61); r.abs(ABS_MT_POSITION_X, 400); r.abs(ABS_MT_POSITION_Y, 200); r.syn();
        r.abs(ABS_MT_POSITION_X, 900); r.abs(ABS_MT_POSITION_Y, 700); r.syn();
        r.abs(ABS_MT_SLOT, 0); r.abs(ABS_MT_TRACKING_ID, -1); r.syn();
        r.abs(ABS_MT_SLOT, 1); r.abs(ABS_MT_POSITION_X, 910); r.syn();
        r.abs(ABS_MT_TRACKING_ID, -1); r.syn();
        CHECK_EQ(win.log.join(" | "), "P 100,1699 | M -10,-10 | R");
        CHECK_EQ(QString::number(r.pinches), "1");
        win.log.clear();
        // The kernel's current slot is still 1: a single finger that lands in slot 0 says so,
        // one that lands in slot 1 does not. Both are plain taps.
        r.abs(ABS_MT_SLOT, 0); r.abs(ABS_MT_TRACKING_ID, 62); r.abs(ABS_MT_POSITION_X, 300); r.abs(ABS_MT_POSITION_Y, 100); r.syn();
        r.abs(ABS_MT_TRACKING_ID, -1); r.syn();
        r.abs(ABS_MT_SLOT, 1); r.abs(ABS_MT_TRACKING_ID, 63); r.abs(ABS_MT_POSITION_X, 300); r.abs(ABS_MT_POSITION_Y, 100); r.syn();
        r.abs(ABS_MT_TRACKING_ID, -1); r.syn();
        r.abs(ABS_MT_TRACKING_ID, 64); r.abs(ABS_MT_POSITION_X, 300); r.abs(ABS_MT_POSITION_Y, 100); r.syn();
        r.abs(ABS_MT_TRACKING_ID, -1); r.syn();
        CHECK_EQ(win.log.join(" | "), "P 100,1699 | R | P 100,1699 | R | P 100,1699 | R");
        CHECK_EQ(QString::number(r.taps), "0");
        win.log.clear();
    }
    {   // Protocol B whose first touch happens to have tracking id 0: recognised at the lift.
        Rig r("b0", false, true);
        for (int id = 0; id < 3; ++id) {
            r.abs(ABS_MT_TRACKING_ID, id); r.abs(ABS_MT_POSITION_X, 300); r.abs(ABS_MT_POSITION_Y, 100); r.syn();
            r.abs(ABS_MT_TRACKING_ID, -1); r.syn();
        }
        CHECK_EQ(win.log.join(" | "), "P 100,1699 | R | P 100,1699 | R | P 100,1699 | R");
        CHECK_EQ(QString::number(r.taps), "0");
        win.log.clear();
    }
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
