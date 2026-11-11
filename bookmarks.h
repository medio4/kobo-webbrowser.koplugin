// Bookmarks: a plain list (bin/bookmarks.txt, one "url<TAB>title" per line) and an
// in-window overlay to open or remove them.
#ifndef BOOKMARKS_H
#define BOOKMARKS_H

#include <QtWidgets/QWidget>
#include <QtCore/QList>
#include <QtCore/QString>

class QVBoxLayout;
class QLabel;
class QPushButton;
class QResizeEvent;
class QMouseEvent;

struct Bookmark { QString url; QString title; };

class BookmarkStore {
public:
    static QList<Bookmark> load();
    static bool save(const QList<Bookmark> &list);
    static bool add(const QString &url, const QString &title);   // false if already there
    static QString file();
};

class BookmarksOverlay : public QWidget {
    Q_OBJECT
public:
    explicit BookmarksOverlay(QWidget *parent);
signals:
    void openUrl(const QString &url);
    void closed();
private slots:
    void entryClicked();
    void removeClicked();
    void prevPage();
    void nextPage();
    void closeClicked();
protected:
    void resizeEvent(QResizeEvent *e);
    void mousePressEvent(QMouseEvent *e);
private:
    void showPage(int page);
    void place();
    QList<Bookmark> m_items;
    QWidget *m_box;
    QVBoxLayout *m_list;
    QLabel *m_pageLabel;
    QPushButton *m_prev, *m_next;
    int m_page, m_perPage;
};

#endif
