// Page overview (experimental): every screen-sized "page" of the loaded web page as a
// small picture, images left out, so a long page can be skimmed and jumped through.
// Tap a picture to scroll there. Off by default: Menu > Settings > Page overview.
// An in-window child widget like TocOverlay, so it reaches the e-ink grab path.
#ifndef NAVIGATOR_H
#define NAVIGATOR_H

#include <QtWidgets/QWidget>
#include <QtCore/QHash>
#include <QtCore/QList>
#include <QtCore/QRect>
#include <QtGui/QImage>

class QWebView;
class QLabel;
class QPushButton;
class QResizeEvent;
class QFont;
class QMouseEvent;
class NavGrid;

class PageNavigator : public QWidget {
    Q_OBJECT
public:
    PageNavigator(QWebView *view, QWidget *parent);
    static bool enabled();            // bin/navigator-on.txt exists
    static void setEnabled(bool on);

signals:
    void beforeJump();      // the page is about to scroll (e-ink: use the page-turn waveform)
    void closed();

private slots:
    void firstPage();
    void prevPage();
    void nextPage();
    void lastPage();
    void closeClicked();
    void renderVisible();

protected:
    void resizeEvent(QResizeEvent *e);
    void mousePressEvent(QMouseEvent *e);

private:
    friend class NavGrid;
    void measure();
    void layoutGrid();
    void showPage(int page);
    void jumpTo(int index);
    QImage renderThumb(int index, const QList<QRect> &images);

    QWebView *m_view;
    QWidget *m_box;
    NavGrid *m_grid;
    QLabel *m_head, *m_pageLabel;
    QPushButton *m_first, *m_prev, *m_next, *m_last;
    static QPushButton *navButton(const QString &text, const QString &icon, const QFont &font);
    int m_viewW, m_viewH;     // the page's viewport, frame pixels
    int m_step;               // scroll distance of one page turn (as PageScroller)
    int m_count;              // screens in the page
    int m_current;            // screen shown now
    int m_scrollX;
    int m_cols, m_rows;
    int m_thumbW, m_thumbH;
    int m_page;               // overview page (m_cols * m_rows screens each)
    QHash<int, QImage> m_thumbs;
};

#endif
