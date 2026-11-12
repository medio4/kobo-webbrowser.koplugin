#ifndef TOUCHWEBVIEW_H
#define TOUCHWEBVIEW_H
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QWidget>
#include <QtWidgets/QLabel>
#include <QtCore/QPoint>
#include <QtCore/QRect>
#include <QtCore/QElapsedTimer>
#include <QtCore/QList>
#include <QtCore/QTimer>
#include <QtWebKitWidgets/QWebView>
#include "pagescroll.h"
class QPushButton;

class KoboEink;
class KoboTouchInput;
class QMouseEvent;
class NativeTextEntry;
class PhoneShare;

// QWebView with e-ink friendly touch handling. A short tap clicks. A quick flick
// turns one screen (up/left = next, down/right = previous; from the very left/right
// edge = history back/forward). A drag held longer than KB_FLICK_MS follows the
// finger: vertical drags scroll live, horizontal drags pan sideways on wide pages.
class TouchWebView : public QWebView {
    Q_OBJECT
public:
    explicit TouchWebView(QWidget *parent = 0);
    PageScroller *pager() const { return m_pager; }
protected:
    bool event(QEvent *e);
    void mousePressEvent(QMouseEvent *e);
    void mouseMoveEvent(QMouseEvent *e);
    void mouseReleaseEvent(QMouseEvent *e);
private:
    QPoint m_pressPos;
    QPoint m_lastPos;
    bool m_pressed;
    bool m_dragging;
    bool m_forwarding;
    bool m_horizontal;      // this drag is horizontal (page turn / history swipe)
    bool m_live;            // this drag turned into live scrolling (held past KB_FLICK_MS)
    PageScroller::HTarget m_hTarget;   // what a horizontal live drag pans
    // Vertical drag that started at the left/right edge: frontlight brightness / warmth
    // (horizontal swipes from the same edges stay history back/forward).
    enum LightDrag { NoLight, LightBrightness, LightWarmth };
    LightDrag m_light;
    int m_lightStart;       // brightness/warmth when the drag started
    int m_lightEdge;        // KB_LIGHT_EDGE, px (0 = off)
    QElapsedTimer m_pressTime;
    PageScroller *m_pager;
};
#endif
