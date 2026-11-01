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

// QWebView with touch-drag scrolling: a drag scrolls the page, a short tap
// is forwarded as a click.
class TouchWebView : public QWebView {
    Q_OBJECT
public:
    explicit TouchWebView(QWidget *parent = 0);
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
    explicit Browser(const QString &home, QWidget *parent = 0);
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
protected:
    bool eventFilter(QObject *obj, QEvent *e);
private:
    TouchWebView *m_view;
    QLineEdit *m_url;
    OnScreenKeyboard *m_kbd;
    QLabel *m_status;
    QString m_home;
};

#endif
