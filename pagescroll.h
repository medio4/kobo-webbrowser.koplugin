// Page-mode scrolling for the web view, the way KOReader turns pages: a swipe or a
// tap in an edge zone moves the page one screen (minus a little overlap) in one go,
// instead of free dragging. The Browser toggles between this and "live" drag mode.
#ifndef PAGESCROLL_H
#define PAGESCROLL_H
#include <QtCore/QObject>
#include <QtCore/QPoint>
#include <QtCore/QRect>
class QWebView;

class PageScroller : public QObject {
    Q_OBJECT
public:
    explicit PageScroller(QWebView *view);
    bool enabled() const { return m_enabled; }
    void setEnabled(bool on) { m_enabled = on; }

    // Gesture entry points, called from TouchWebView's mouse handlers (view coords).
    // handlePress/handleMove return true while a gesture is being tracked.
    // handleRelease returns true when the gesture was consumed as a page turn;
    // false means "treat it as a normal tap" (forward as a click).
    bool handlePress(const QPoint &p);
    bool handleMove(const QPoint &p);
    bool handleRelease(const QPoint &p);
    bool inTapZone(const QPoint &p) const;   // left/right edge strips

public slots:
    void nextPage();
    void prevPage();

signals:
    // Emitted right before the page content moves, so the e-ink layer can pick a
    // page-turn waveform for the repaint that follows.
    void pageTurned();

private:
    // Scroll by (dx, dy) screens; returns true when the position actually changed.
    bool turn(int dxScreens, int dyScreens);
    QWebView *m_view;
    bool m_enabled;
    bool m_pressed;
    bool m_swiped;
    QPoint m_press;
    double m_overlapFrac;   // fraction of the viewport kept visible after a turn
    int m_overlapMin;       // but at least this many pixels
    double m_zoneFrac;      // width of the left/right tap zones as a fraction of the view
    int m_swipeMin;         // pixels of movement that make a swipe
};
#endif
