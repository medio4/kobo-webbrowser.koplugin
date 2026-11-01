#ifndef KOBO_BROWSER_H
#define KOBO_BROWSER_H

#include <QtWidgets/QMainWindow>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QWidget>
#include <QtWidgets/QLabel>
#include <QtCore/QPoint>
#include <QtCore/QElapsedTimer>
#include <QtCore/QList>
#include <QtCore/QTimer>
#include <QtWebKitWidgets/QWebView>
class PageScroller;
class QPushButton;

class KoboEink;

// QWebView with two scroll modes. Live: a drag scrolls the page as it moves.
// Page: a swipe or an edge tap turns one screen at a time (PageScroller).
// In both, a short tap is forwarded as a click.

class EpubSaver;
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
    bool m_horizontal;      // live mode: this drag is horizontal (page turn / history swipe)
    QElapsedTimer m_pressTime;
    PageScroller *m_pager;
};

// A flat, e-ink friendly button in the style of KOReader's keys: white face,
// thin black border, rounded corners, plain text. It draws no pressed state
// unless KB_KEY_FLASH is set, so a tap costs no extra screen refresh.
class FlatButton : public QAbstractButton {
    Q_OBJECT
public:
    explicit FlatButton(const QString &text, QWidget *parent = 0);
    void setBold(bool b);
    void setPixelSize(int px);
    void setDark(bool d) { m_dark = d; update(); }   // gray face for utility keys
    void setFlash(bool f) { m_flash = f; }
protected:
    void paintEvent(QPaintEvent *e);
private:
    bool m_dark;
    bool m_flash;
};

// On-screen keyboard laid out like KOReader's English keyboard, with URL
// helpers. Built once; shift / symbol layers only relabel the keys.
class OnScreenKeyboard : public QWidget {
    Q_OBJECT
public:
    explicit OnScreenKeyboard(QWidget *parent = 0);
    int preferredHeight() const;
signals:
    void goPressed();
    void hidePressed();
private slots:
    void keyClicked();
    void toggleShift();
    void toggleSymbols();
private:
    void build();
    void relabel();
    void sendKey(int code, const QString &text);
    QList<FlatButton *> m_charKeys;   // keys whose label changes with the layer
    FlatButton *m_shift;
    FlatButton *m_sym;
    bool m_shift_on;
    bool m_symbols;
    int m_rows;
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
    void clearUrl();
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
private slots:
    void keyboardGo();
    void pageFocusChanged();
    void cleanToolbar();
    void saveEpub();
    void saveProgress(const QString &msg);
    void saveFinished(bool ok, const QString &msg);
    void updateSysInfo();
    void fullRefresh();
    void pinchZoom(double ratio);   // two-finger pinch: ratio > 1 zooms in
protected:
    bool eventFilter(QObject *obj, QEvent *e);
    void resizeEvent(QResizeEvent *e);
private:
    void placeKeyboard();
    TouchWebView *m_view;
    QLineEdit *m_url;
    OnScreenKeyboard *m_kbd;
    QWidget *m_bar;
    QWidget *m_central;
    QLabel *m_status;
    QLabel *m_sysInfo;     // time / battery / free memory, right side of the status line
    QTimer *m_sysTimer;
    QString m_home;
    KoboEink *m_eink;
    FlatButton *m_modeBtn;
    QElapsedTimer m_lastPageTap;
    void applyScrollMode(bool pageMode, bool save);
    EpubSaver *m_saver;
};

#endif
