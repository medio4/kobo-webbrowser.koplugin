#ifndef KOBO_BROWSER_H
#define KOBO_BROWSER_H

#include <QtWidgets/QMainWindow>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QToolBar>
#include <QtWidgets/QWidget>
#include <QtWidgets/QLabel>
#include <QtCore/QPoint>
#include <QtCore/QElapsedTimer>
#include <QtWebKitWidgets/QWebView>
class PageScroller;
class QPushButton;

class KoboEink;

// QWebView with two scroll modes. Live: a drag scrolls the page as it moves.
// Page: a swipe or an edge tap turns one screen at a time (PageScroller).
// In both, a short tap is forwarded as a click.

class TouchWebView : public QWebView {
    Q_OBJECT
public:
    explicit TouchWebView(QWidget *parent = 0);
    PageScroller *pager() const { return m_pager; }
    bool pageMode() const;
    void setPageMode(bool on);
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
    QElapsedTimer m_pressTime;
    PageScroller *m_pager;
};

// A simple on-screen keyboard that sends key events to the focused widget.
class OnScreenKeyboard : public QWidget {
    Q_OBJECT
public:
    explicit OnScreenKeyboard(QWidget *parent = 0);
signals:
    void goPressed();
    void hidePressed();
private slots:
    void keyClicked();
    void toggleShift();
    void toggleSymbols();
private:
    void rebuild();
    bool m_shift;
    bool m_symbols;
};

class Browser : public QMainWindow {
    Q_OBJECT
public:
    Browser(const QString &home, KoboEink *eink, QWidget *parent = 0);
public slots:
    void go();
    void loadUrlText(const QString &text);
    void showKeyboard();
    void hideKeyboard();
    void toggleKeyboard();
    void zoomIn();
    void zoomOut();
    void urlChanged(const QUrl &url);
    void loadStarted();
    void loadProgress(int p);
    void loadFinished(bool ok);
    void quit();
    void focusUrl();
    void screenshot();
    void toggleScrollMode();
    void releaseLoadHold();
protected:
    bool eventFilter(QObject *obj, QEvent *e);
private:
    TouchWebView *m_view;
    QLineEdit *m_url;
    OnScreenKeyboard *m_kbd;
    QLabel *m_status;
    QString m_home;
    KoboEink *m_eink;
    QPushButton *m_modeBtn;
    void applyScrollMode(bool pageMode, bool save);
};

#endif
