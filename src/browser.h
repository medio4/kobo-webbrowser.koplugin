#ifndef KOBO_BROWSER_H
#define KOBO_BROWSER_H

#include <QtWidgets/QMainWindow>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QWidget>
#include <QtWidgets/QLabel>
#include <QtCore/QPoint>
#include <QtCore/QRect>
#include <QtCore/QElapsedTimer>
#include <QtCore/QList>
#include <QtCore/QTimer>
#include <QtWebKitWidgets/QWebView>
#include "pagescroll.h"
class QPushButton;

class KoboEink;
class KoboTouchInput;
class QMouseEvent;
class NativeTextEntry;
class PhoneShare;

#include "epub.h"
#include "touchwebview.h"
#include "flatbutton.h"
#include "keyboard.h"

class Browser : public QMainWindow {
    Q_OBJECT
public:
    Browser(const QString &home, KoboEink *eink, QWidget *parent = 0);
    void setTouchInput(KoboTouchInput *t) { m_touch = t; }
    void barSwipeEvent(QMouseEvent *me);   // app-wide press/release: toolbar swipe down
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
    void checkOnline();          // offline copy: say "press Reload" once the device is online
    void reloadLive();
    void reloadLiveNow();
    void reloadClicked();        // Reload button; on the offline copy loads the original URL
    void loadProgress(int p);
    void loadFinished(bool ok);
    void quit();
    void screenshot();
    void releaseLoadHold();
    void sleepToKoreader();   // power button with KB_KO_SLEEP=1: hand off and exit
    void readInKoreader();
    void readPageInKoreader();
    void readSeriesInKoreader();    // reader mode: article as a temporary EPUB in KOReader, then back here
private slots:
    void keyboardGo();
    void pageTapped();           // a tap landed in the page: keyboard for a text box, else hide it
    void cleanToolbar();
    void phoneUrl(const QString &url);
    void showPhoneHint();
    void showPhoneDialog();
    void stopPhone();
    void sharePage();
    void nativeKey(const QString &line);
    void showMenu();
    void minimize();
    void menuPressed();
    void menuReleased();
    void menuHoldFired();
    void menuClicked();
    void runQuickAction(const QString &key);
    void goHome();
    void rotateNow();
    void showTextSizeBar();
    void closeTextSizeBar();
    void showRotateMenu();
    void rotateTo0();
    void rotateTo90();
    void rotateTo180();
    void rotateTo270();
    void saveSessionHistory();
    bool restoreSessionHistory(const QString &startUrl);
    void showHistory();
    void toggleHistory();
    void toggleAdBlock();
    void showBookmarks();
    void addBookmark();
    void setAsHome();
    void menuClosed();
    void saveEpub();
    void savePageOnly();
    void saveSeriesSplit();
    void probeLoaded(bool ok);
    void probeTimedOut();
    void saveSeries();
    void saveProgress(const QString &msg);
    void saveFinished(bool ok, const QString &msg);
    void readerFinished(bool ok, const QString &msg);
    void imageDownloaded();
    void updateSysInfo();
    void fullRefresh();
    void rotationSettled();
    void pinchZoom(double ratio);   // two-finger pinch: ratio > 1 zooms in
    void openSavedEpub(const QString &path);
    void showToc();
    void flashUrlBar();          // Clear / Del: redraw the address bar with a flashing refresh
    void nativeStarted();        // KOReader keyboard: hand over screen + touch
    void nativeReplied(const QString &action, const QString &text);
    void nativeUnavailable();
    void openKoreaderMenu();     // toolbar swipe down: KOReader's top menu over the page
    void menuReplied(const QString &action);
    void menuUnavailable();
protected:
    bool eventFilter(QObject *obj, QEvent *e);
    void resizeEvent(QResizeEvent *e);
private:
    void placeKeyboard();
    void applyRotation(int angle, bool save);
    int m_rotPrev;
    bool nativeTextEntry();      // ask KOReader for text; false = use the built-in keyboard
    void showBuiltinKeyboard();
    bool saveImageIfImagePage();   // Save on a page that is just an image: keep the image file
    TouchWebView *m_view;
    QLineEdit *m_url;
    OnScreenKeyboard *m_kbd;
    QWidget *m_bar;
    QWidget *m_central;
    QLabel *m_status;
    QLabel *m_sysInfo;     // time / battery / free memory, right side of the status line
    QTimer *m_sysTimer;
    QString m_home;       // page this run starts on (also the restore base)
    QString m_homePage;   // Menu > Home
    KoboEink *m_eink;
    QElapsedTimer m_lastPageTap;
    QPoint m_tapPress, m_tapRelease;   // last press/release in the page (view coordinates)
    EpubSaver *m_saver;
    PhoneShare *m_phone;
    int m_rotTarget;
    QTimer *m_holdTimer;
    bool m_holdFirst;       // draw nothing until the first page has loaded
    bool m_live;            // typing with KOReader's keyboard, live into our field
    QString m_liveOrig;     // the field's text before, for Cancel
    void sendLiveKey(int key, const QString &text, Qt::KeyboardModifiers mods = Qt::NoModifier);
    bool m_menuHeld;
    QWidget *m_sizeBar;
    QLabel *m_sizeLabel;
    void placeTextSizeBar();
    void rotateTo(int angle);
    QRect m_menuRect;
    QList<EpubSaver::ChapterLink> m_seriesLinks;
    QString m_seriesTitle;
    QWebPage *m_probe;
    QTimer *m_probeTimer;
    void showSaveChoice(bool comic);
    qreal m_textZoom;         // A+/A-: text-only zoom on top of the page zoom (1.0 = normal)
    void setTextZoom(qreal z, bool save);
    int m_restoreY;           // KB_RESTORE_Y: scroll position to restore after the first load (-1 = none)
    bool m_relaunch;          // started by KOReader after a sleep: flash once the first page is back
    bool m_offline;           // showing KB_RESTORE_FILE (the copy saved at sleep time)
    bool m_offlineShown;      // the offline copy has loaded; the next load leaves offline mode
    QTimer *m_onlineTimer;    // offline copy: polls for a network connection
    KoboTouchInput *m_touch;
    NativeTextEntry *m_native;
    NativeTextEntry *m_menu;  // KOReader's menu over the browser (KB_MENU_FILE)
    bool m_barSwipe;          // the current touch started on the toolbar
    bool m_swipeMenu;         // ... and turned into a menu swipe (ignore its release)
    QPoint m_barPress;
    QElapsedTimer m_barTouch;
    QPoint m_statusPress;     // where the last touch on the status line started
    QString m_nativeKind;     // what the pending KOReader text entry is for: url / field / text
};

#endif
