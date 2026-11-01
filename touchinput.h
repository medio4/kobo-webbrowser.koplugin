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
private slots:
    void readable();
private:
    void emitState();
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
};
#endif
