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
#include "pagescroll.h"
class QPushButton;

class KoboEink;
class KoboTouchInput;
class NativeTextEntry;

// QWebView with e-ink friendly touch handling. A short tap clicks. A quick flick
// turns one screen (up/left = next, down/right = previous; from the very left/right
// edge = history back/forward). A drag held longer than KB_FLICK_MS follows the
// finger: vertical drags scroll live, horizontal drags pan sideways on wide pages.
class EpubSaver;
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
    void backspacePressed();
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
    void setTouchInput(KoboTouchInput *t) { m_touch = t; }
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
    void releaseLoadHold();
    void sleepToKoreader();   // power button with KB_KO_SLEEP=1: hand off and exit
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
    void openSavedEpub(const QString &path);
    void showToc();
    void flashUrlBar();          // Clear / Del: redraw the address bar with a flashing refresh
    void nativeStarted();        // KOReader keyboard: hand over screen + touch
    void nativeReplied(const QString &action, const QString &text);
    void nativeUnavailable();
protected:
    bool eventFilter(QObject *obj, QEvent *e);
    void resizeEvent(QResizeEvent *e);
private:
    void placeKeyboard();
    bool nativeTextEntry();      // ask KOReader for text; false = use the built-in keyboard
    void showBuiltinKeyboard();
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
    QElapsedTimer m_lastPageTap;
    EpubSaver *m_saver;
    int m_restoreY;           // KB_RESTORE_Y: scroll position to restore after the first load (-1 = none)
    bool m_relaunch;          // started by KOReader after a sleep: flash once the first page is back
    bool m_offline;           // showing KB_RESTORE_FILE (the copy saved at sleep time)
    KoboTouchInput *m_touch;
    NativeTextEntry *m_native;
    QString m_nativeKind;     // what the pending KOReader text entry is for: url / field / text
};

#endif
