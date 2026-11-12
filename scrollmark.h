// Thin scroll position marks for the web view, drawn instead of WebKit's own beveled
// scrollbars (which ignore the app style): a slim rounded bar on the right edge, and one
// along the bottom when the page is wider than the screen. Display only, not draggable.
#ifndef SCROLLMARK_H
#define SCROLLMARK_H
#include <QtWidgets/QWidget>
class QWebView;
class ScrollMark : public QWidget {
    Q_OBJECT
public:
    explicit ScrollMark(QWebView *view);
public slots:
    void refresh();
protected:
    void paintEvent(QPaintEvent *e);
    bool eventFilter(QObject *obj, QEvent *e);
private:
    QWebView *m_view;
    QRect m_vThumb, m_hThumb;
};
#endif
