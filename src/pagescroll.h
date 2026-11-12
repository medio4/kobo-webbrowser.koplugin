// Page turning for the web view, the way KOReader turns pages: one screen (minus a
// little overlap) per flick, as opposed to the live scrolling of a sustained drag.
// TouchWebView decides which gesture happened; this class does the moving.
#ifndef PAGESCROLL_H
#define PAGESCROLL_H
#include <QtCore/QObject>
#include <QtCore/QPoint>
#include <QtCore/QRect>
#include <QtWebKitWidgets/QWebFrame>
#include <QtWebKit/QWebElement>
class QWebView;

class PageScroller : public QObject {
    Q_OBJECT
public:
    explicit PageScroller(QWebView *view);
    // A horizontal swipe from `press` to `release`: a swipe that starts at the very
    // left/right edge goes back/forward in history; any other horizontal swipe turns a
    // page (left = next, right = previous). Returns true when it did something.
    bool handleHorizontalSwipe(const QPoint &press, const QPoint &release);
    // Horizontal panning of the thing under the finger: the nearest ancestor of the
    // element at `viewPos` that can scroll sideways (wide table in an overflow box,
    // scrollable div, inner frame), else the page itself.
    struct HTarget { bool found; QWebFrame *frame; HTarget() : found(false), frame(0) {} };
    HTarget findHScrollable(const QPoint &viewPos) const;
    void panHorizontal(const HTarget &t, int dx);   // dx > 0 scrolls content to the left
    int swipeMin() const { return m_swipeMin; }
    int flickMs() const { return m_flickMs; }

public slots:
    void nextPage();
    void prevPage();

signals:
    // Emitted right before the page content moves, so the e-ink layer can pick a
    // page-turn waveform for the repaint that follows.
    void pageTurned();
    void goBack();      // swipe in from the left edge
    void goForward();   // swipe in from the right edge

private:
    // Scroll by (dx, dy) screens; returns true when the position actually changed.
    bool turn(int dxScreens, int dyScreens);
    QWebView *m_view;
    double m_overlapFrac;   // fraction of the viewport kept visible after a turn
    int m_overlapMin;       // but at least this many pixels
    int m_swipeMin;         // pixels of movement that make a swipe/flick
    int m_flickMs;          // a drag released within this time is a flick (page turn)
    int m_edgePx;           // a swipe starting within this many px of the edge = history
};
#endif
