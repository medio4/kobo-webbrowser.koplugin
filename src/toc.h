// Table of contents overlay: lists the page's headings (h1-h4) and jumps to one.
// An in-window child widget like SavedDialog, so it reaches the e-ink grab path.
#ifndef TOC_H
#define TOC_H

#include <QtWidgets/QWidget>
#include <QtCore/QList>
#include <QtCore/QString>

class QWebView;
class QLabel;
class QVBoxLayout;
class QPushButton;
class QResizeEvent;
class QMouseEvent;

class TocOverlay : public QWidget {
    Q_OBJECT
public:
    // Collects the headings right away; returns an empty list from count() when there are none.
    TocOverlay(QWebView *view, QWidget *parent);
    int count() const { return m_items.size(); }

signals:
    void beforeJump();      // the page is about to scroll (e-ink: use the page-turn waveform)
    void closed();

private slots:
    void entryClicked();
    void prevPage();
    void nextPage();
    void closeClicked();

protected:
    void resizeEvent(QResizeEvent *e);
    void mousePressEvent(QMouseEvent *e);

private:
    struct Item { int level; QString text; int y; int x; int index; };
    void collect();
    void showPage(int page);
    void place();

    QWebView *m_view;
    QList<Item> m_items;
    QWidget *m_box;
    QVBoxLayout *m_list;
    QLabel *m_pageLabel;
    QPushButton *m_prev, *m_next;
    int m_page;
    int m_perPage;
};

#endif
