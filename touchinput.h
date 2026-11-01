// Minimal evdev touchscreen reader for Kobo e-readers, feeding Qt mouse events.
// Replaces Kobo's own touch_device generic plugin, which crashes outside Nickel.
#ifndef TOUCHINPUT_H
#define TOUCHINPUT_H
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QPoint>
class QSocketNotifier;
class KoboTouchInput : public QObject {
    Q_OBJECT
public:
    // swapXY / mirrorX / mirrorY: KOReader-style adjustment (Clara BW: swap + mirror X).
    KoboTouchInput(const QString &device, int screenW, int screenH,
                   bool swapXY, bool mirrorX, bool mirrorY, bool grab, QObject *parent = 0);
    ~KoboTouchInput();
    bool isOpen() const { return m_fd >= 0; }
    // True while a multi-finger gesture is in progress: widgets should ignore the
    // mouse events of the first finger (they get a release at (-10,-10) at the end).
    static bool gestureActive() { return s_gesture; }
    // Hand the touchscreen to another process (KOReader's keyboard): drop the EVIOCGRAB and
    // discard everything we read until setHandedOff(false) grabs it again.
    void setHandedOff(bool off);
    bool handedOff() const { return m_handedOff; }
signals:
    // Two fingers touched and lifted without moving much: used for a full refresh.
    void twoFingerTap();
    // Two fingers moved apart (ratio > 1) or together (ratio < 1) and lifted.
    void pinch(double ratio);
private slots:
    void readable();
private:
    void emitState();
    static bool s_gesture;
    int m_fd;
    QSocketNotifier *m_notifier;
    int m_screenW, m_screenH;
    bool m_swapXY, m_mirrorX, m_mirrorY;
    int m_minX, m_maxX, m_minY, m_maxY;
    // current frame state
    int m_slot;          // current MT slot (we only follow the first contact)
    int m_rawX, m_rawY;  // last raw position of followed contact
    bool m_down;         // contact currently down (as of last SYN)
    bool m_pendingDown;  // state being assembled in this frame
    bool m_havePos;
    bool m_sentDown;     // mouse press already sent
    QPoint m_lastPos;
    bool m_slot1Down;    // a second contact is currently down
    bool m_multiSeen;    // a second contact was seen during this touch
    int m_raw1X, m_raw1Y;        // last raw position of the second contact
    bool m_have1;
    double m_pinchStart, m_pinchLast;   // finger distance when both were first seen / last
    bool m_haveMT;               // device reports ABS_MT_POSITION_*; ignore ABS_X/ABS_Y then
    bool m_grab;                 // we were asked to grab the device
    bool m_handedOff;
};
#endif
