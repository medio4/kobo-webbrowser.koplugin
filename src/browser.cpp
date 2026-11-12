// Browser: the main window - toolbar, page, Menu and everything it opens.
#include "browser.h"
#include "crashhandler.h"
#include "touchinput.h"
#include "eink.h"
#include "webpage.h"
#include "pagescroll.h"
#include <QtWebKit/QWebHistory>
#include <QtCore/QDataStream>
#include <QtCore/QFileInfo>
#include <QtCore/QDateTime>
#include "epub.h"
#include "phoneshare.h"
#include "menupanel.h"
#include "bookmarks.h"
#include "scrollmark.h"
#include "adblock.h"
#include "toc.h"
#include "memtune.h"
#include "power.h"
#include "textentry.h"
#include "rotation.h"
#include "icons.h"
#include <QtWidgets/QFrame>
#include <QtGui/QBackingStore>

#include <QtWidgets/QApplication>
#include <QtWidgets/QSizePolicy>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QStyle>
#include <QtGui/QGenericPlugin>
#include <QtGui/QGenericPluginFactory>
#include <QtGui/QMouseEvent>
#include <QtGui/QKeyEvent>
#include <QtGui/QFont>
#include <QtGui/QPainter>
#include <QtGui/QPen>
#include <QtGui/QPalette>
#include <QtGui/QScreen>
#include <QtGui/QWindow>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QSslConfiguration>
#include <QtNetwork/QSslSocket>
#include <QtNetwork/QSslCertificate>
#include <QtNetwork/QNetworkDiskCache>
#include <QtCore/QDebug>
#include <QtCore/QLoggingCategory>
#include <QtCore/QDir>
#include <QtCore/QRegExp>
#include <QtCore/QStringList>
#include <QtCore/QTime>
#include <QtCore/QFile>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtWebKit/QWebSettings>
#include <QtWebKitWidgets/QWebFrame>
#include <QtWebKitWidgets/QWebPage>
#include <QtWebKitWidgets/QWebView>

#include <cstdlib>
#include <cstring>
#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>


// -------------------------------------------------------------------- Browser

static FlatButton *toolButton(const QString &text, int w, int h)
{
    FlatButton *b = new FlatButton(text);
    b->setFixedSize(w, h);
    b->setPixelSize(38);
    return b;
}

// Swipe down that starts on the toolbar or status line: open KOReader's top menu over the page. Seen on
// the application (whichever toolbar child got the touch); never eats the event.
static const int kMenuSwipePx = 120;

class BarSwipeFilter : public QObject {
public:
    BarSwipeFilter(Browser *b) : QObject(b), m_b(b) {}
protected:
    bool eventFilter(QObject *obj, QEvent *e) {
        if (obj->isWidgetType() && (e->type() == QEvent::MouseButtonPress || e->type() == QEvent::MouseButtonRelease))
            m_b->barSwipeEvent(static_cast<QMouseEvent *>(e));
        return false;
    }
private:
    Browser *m_b;
};

Browser::Browser(const QString &home, KoboEink *eink, QWidget *parent)
    : QMainWindow(parent), m_home(home), m_eink(eink), m_saver(0), m_probe(0), m_probeTimer(0), m_phone(0), m_restoreY(-1), m_relaunch(false), m_offline(false), m_offlineShown(false), m_onlineTimer(0)
{
    m_rotPrev = 0;
    m_rotTarget = 0;
    m_sizeBar = 0;
    m_sizeLabel = 0;
    setWindowTitle("Browser");
    // Home page: one chosen in the browser (Menu > Set as home) wins over the launcher's.
    // m_home is the page this run starts on; a URL given on the command line (the page
    // being restored after sleep, a link opened from KOReader) keeps priority for that.
    m_homePage = m_home;
    {
        QFile hf(QDir(QCoreApplication::applicationDirPath()).filePath("home.txt"));
        if (hf.open(QIODevice::ReadOnly)) {
            QString h = QString::fromUtf8(hf.readAll()).trimmed();
            if (!h.isEmpty()) {
                m_homePage = h;
                if (!getenv("KB_START_FROM_ARG")) m_home = h;
            }
        }
    }

    m_central = new QWidget;
    QVBoxLayout *vbox = new QVBoxLayout(m_central);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(0);

    // Toolbar: one row (Back, Fwd, address field, Menu), sized in pixels for 300 dpi. The
    // less frequent actions live in the Menu panel (menupanel.h).
    // Marked kb_fast so its repaints use the quick DU waveform while typing.
    m_bar = new QWidget;
    m_bar->setProperty("kb_fast", true);
    m_bar->setAutoFillBackground(true);
    QPalette pal = m_bar->palette(); pal.setColor(QPalette::Window, Qt::white); m_bar->setPalette(pal);
    QVBoxLayout *barBox = new QVBoxLayout(m_bar);
    barBox->setContentsMargins(6, 6, 6, 6);
    barBox->setSpacing(6);
    const int bh = 84;
    QFont bf; bf.setPixelSize(36);

    m_url = new QLineEdit; m_url->setFixedHeight(bh); m_url->setFont(bf);
    m_url->setPlaceholderText("Search or address");
    m_url->setStyleSheet("QLineEdit { border: 2px solid black; border-radius: 8px; background: white; color: black;"
                         " padding: 0 12px; selection-background-color: black; selection-color: white; }");
    FlatButton *back = toolButton("Back", 110, bh);   // NB: "<" renders as a clock in the Kobo UI font
    FlatButton *fwd  = toolButton("Fwd", 96, bh);
    FlatButton *menu = toolButton("Menu", 130, bh); menu->setBold(true);
    // KOReader's icons when its icon set is there (text otherwise).
    back->setIconName("chevron.left", true);
    fwd->setIconName("chevron.right", true);
    menu->setIconName("appbar.menu", true);
    QHBoxLayout *row1 = new QHBoxLayout; row1->setSpacing(6);
    row1->addWidget(back); row1->addWidget(fwd); row1->addWidget(m_url, 1); row1->addWidget(menu);
    barBox->addLayout(row1);
    vbox->addWidget(m_bar);

    // Status line: page title / load progress on the left, clock + battery + free
    // memory on the right (refreshed once a minute with the fast waveform).
    QWidget *statusRow = new QWidget;
    QHBoxLayout *statusBox = new QHBoxLayout(statusRow);
    statusBox->setContentsMargins(0, 0, 0, 0);
    statusBox->setSpacing(6);
    m_status = new QLabel;
    QFont sf; sf.setPixelSize(30);
    m_status->setFont(sf);
    m_status->setContentsMargins(6, 0, 6, 0);
    // A long page title must never widen the window past the screen (it pushed the
    // right-hand toolbar buttons off-screen): the label clips instead of demanding width.
    m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_status->setMinimumWidth(1);
    statusBox->addWidget(m_status, 1);
    m_sysInfo = new QLabel;
    m_sysInfo->setFont(sf);
    m_sysInfo->setContentsMargins(6, 0, 6, 0);
    m_sysInfo->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_sysInfo->setProperty("kb_fast", true);
    statusBox->addWidget(m_sysInfo, 0);
    vbox->addWidget(statusRow);
    m_sysTimer = new QTimer(this);
    connect(m_sysTimer, SIGNAL(timeout()), this, SLOT(updateSysInfo()));
    updateSysInfo();

    m_view = new TouchWebView;
    m_view->setPage(new KoboWebPage(m_view));
    new ScrollMark(m_view);   // slim position marks instead of WebKit's beveled scrollbars
    vbox->addWidget(m_view, 1);

    // The keyboard is NOT in the layout: it floats over the bottom of the page, so
    // showing it doesn't re-layout and repaint the whole web view.
    m_kbd = new OnScreenKeyboard(m_central);
    m_kbd->hide();

    setCentralWidget(m_central);
    m_view->setFocus();

    QWebSettings *ws = m_view->settings();
    MemTune::applySettings(ws);   // caches, JS (KB_NO_JS), plugins etc.
    ws->setAttribute(QWebSettings::AutoLoadImages, true);
    ws->setAttribute(QWebSettings::ZoomTextOnly, false);
    // Readable on a 300 dpi e-ink panel: pages may not shrink text below this.
    ws->setFontSize(QWebSettings::DefaultFontSize, 22);
    ws->setFontSize(QWebSettings::MinimumFontSize, 20);
    m_view->setZoomFactor(2.0);
    // A+/A- scale the text only (saved in bin/text-size.txt), so the page keeps its layout
    // width; pinch still zooms the whole page.
    // "Send to Kobo" page for a phone on the same Wi-Fi (KB_PHONE_PORT, 0 = off). It only
    // runs after Menu > From phone, until its window's Stop (Minimize keeps it running).
    {
        bool okp = false;
        int port = QString::fromLocal8Bit(qgetenv("KB_PHONE_PORT")).toInt(&okp);
        if (!okp) port = 8080;
        if (port > 0) {
            m_phone = new PhoneShare(this);
            m_phone->setProperty("wantedPort", port);
            connect(m_phone, SIGNAL(urlReceived(QString)), this, SLOT(phoneUrl(QString)));
            // Setting "Start phone link with the browser" (KB_PHONE_AUTOSTART=1).
            if (qgetenv("KB_PHONE_AUTOSTART") == "1" && m_phone->listen(port))
                QTimer::singleShot(4000, this, SLOT(showPhoneHint()));
        }
    }

    m_textZoom = 1.0;
    {
        QFile tf(QDir(QCoreApplication::applicationDirPath()).filePath("text-size.txt"));
        bool ok = false;
        qreal t = 1.0;
        if (tf.open(QIODevice::ReadOnly)) t = QString::fromLatin1(tf.readAll()).trimmed().toDouble(&ok);
        setTextZoom(ok ? t : 1.25, false);   // first run: a notch larger than the pages ask for
    }
    m_view->page()->setLinkDelegationPolicy(QWebPage::DontDelegateLinks);

    // Disk cache + cookies in our own folder (user partition).
    QNetworkDiskCache *cache = new QNetworkDiskCache(this);
    // KB_CACHE_DIR (set by the KOReader plugin) wins; otherwise next to the binary when
    // it lives on the user partition, else in $HOME.
    QString cacheDir = QString::fromLocal8Bit(qgetenv("KB_CACHE_DIR"));
    if (cacheDir.isEmpty()) {
        QString appDir = QCoreApplication::applicationDirPath();
        cacheDir = appDir.startsWith("/mnt/onboard/") ? appDir + "/cache"
                                                      : QDir::homePath() + "/.kobo-browser-cache";
    }
    cache->setCacheDirectory(cacheDir);
    cache->setMaximumCacheSize(MemTune::diskCacheBytes());
    m_view->page()->networkAccessManager()->setCache(cache);

    connect(back, SIGNAL(clicked()), m_view, SLOT(back()));
    connect(fwd, SIGNAL(clicked()), m_view, SLOT(forward()));
    // Tap: the Menu. Hold: the quick action chosen in KOReader's Web browser > Settings.
    m_menuHeld = false;
    m_holdTimer = new QTimer(this);
    m_holdTimer->setSingleShot(true);
    m_holdTimer->setInterval(600);
    connect(m_holdTimer, SIGNAL(timeout()), this, SLOT(menuHoldFired()));
    connect(menu, SIGNAL(pressed()), this, SLOT(menuPressed()));
    connect(menu, SIGNAL(released()), this, SLOT(menuReleased()));
    connect(menu, SIGNAL(clicked()), this, SLOT(menuClicked()));
    connect(m_url, SIGNAL(returnPressed()), this, SLOT(go()));
    connect(m_kbd, SIGNAL(goPressed()), this, SLOT(keyboardGo()));
    connect(m_kbd, SIGNAL(hidePressed()), this, SLOT(hideKeyboard()));
    connect(m_kbd, SIGNAL(backspacePressed()), this, SLOT(flashUrlBar()));
    // KOReader's own keyboard (when launched by the plugin with KB_KBD_FILE set).
    m_touch = 0;
    m_native = new NativeTextEntry("KB_KBD_FILE", "KB_KBD_REPLY", this);
    if (getenv("KB_NO_NATIVE_KBD")) { delete m_native; m_native = 0; }
    if (m_native) connect(m_native, SIGNAL(started()), this, SLOT(nativeStarted()));
    if (m_native) connect(m_native, SIGNAL(replied(QString,QString)), this, SLOT(nativeReplied(QString,QString)));
    if (m_native) connect(m_native, SIGNAL(unavailable()), this, SLOT(nativeUnavailable()));
    if (m_native) connect(m_native, SIGNAL(keyLine(QString)), this, SLOT(nativeKey(QString)));
    m_holdFirst = getenv("KB_HOLD_UNTIL_LOADED") != 0;
    if (m_holdFirst && m_eink) m_eink->setSuspended(true);
    m_live = false;
    // KOReader's top menu over the browser: swipe down from the toolbar (see eventFilter).
    m_menu = new NativeTextEntry("KB_MENU_FILE", "KB_MENU_REPLY", this);
    m_barSwipe = false; m_swipeMenu = false;
    connect(m_menu, SIGNAL(started()), this, SLOT(nativeStarted()));
    connect(m_menu, SIGNAL(replied(QString,QString)), this, SLOT(menuReplied(QString)));
    connect(m_menu, SIGNAL(unavailable()), this, SLOT(menuUnavailable()));
    if (m_menu->available()) qApp->installEventFilter(new BarSwipeFilter(this));
    connect(m_view, SIGNAL(urlChanged(QUrl)), this, SLOT(urlChanged(QUrl)));
    connect(m_view, SIGNAL(loadStarted()), this, SLOT(loadStarted()));
    connect(m_view, SIGNAL(loadProgress(int)), this, SLOT(loadProgress(int)));
    connect(m_view, SIGNAL(loadFinished(bool)), this, SLOT(loadFinished(bool)));
    connect(m_view, SIGNAL(loadFinished(bool)), new MemTune(this), SLOT(scheduleTrim()));
    if (m_eink)
        connect(m_view->pager(), SIGNAL(pageTurned()), m_eink, SLOT(hintPageTurn()));

    // A tap on a text field in the page pops the keyboard up.
    // (The keyboard appears only for a tap on a text box: see eventFilter / pageTapped.)

    // Tapping the URL field shows the keyboard.
    m_url->installEventFilter(this);
    m_view->installEventFilter(this);

    // Relaunched after a KOReader sleep: put the page back where it was.
    const char *ry = getenv("KB_RESTORE_Y");
    m_restoreY = (ry && *ry) ? atoi(ry) : -1;
    const char *rz = getenv("KB_RESTORE_ZOOM");
    if (rz && *rz && atof(rz) > 0.2) m_view->setZoomFactor(atof(rz));
    m_relaunch = (ry && *ry) || (rz && *rz);

    // KB_RESTORE_FILE: the page saved at sleep time (wake screen "Cancel": no connection);
    // shown with its original address as the base so links still work once online.
    const char *rf = getenv("KB_RESTORE_FILE");
    QFile restore(rf && *rf ? QString::fromLocal8Bit(rf) : QString());
    if (rf && *rf && restore.open(QIODevice::ReadOnly)) {
        QString html = QString::fromUtf8(restore.readAll());
        restore.close();
        qDebug() << "restore: offline copy" << restore.fileName() << html.size() << "chars, base" << m_home
                 << "scroll" << m_restoreY;
        m_offline = true;
        m_relaunch = true;
        m_view->setHtml(html, QUrl(m_home));
        m_status->setText("Offline copy (no connection)");
        // Every 3 s: once the device is online, the status line says to press Reload.
        m_onlineTimer = new QTimer(this);
        connect(m_onlineTimer, SIGNAL(timeout()), this, SLOT(checkOnline()));
        m_onlineTimer->start(3000);
        return;
    }
    if (rf && *rf) qDebug() << "restore: can't read" << restore.fileName() << restore.errorString() << "- loading online";

    // Back from a temporary book or a sleep: bring the Back/Fwd history back too.
    if (restoreSessionHistory(m_home)) return;
    loadUrlText(m_home);
}

// A mouse press / release anywhere in the app (one finger: the touch driver feeds the
// first contact only). Qt delivers it to each widget up the parent chain; only the first
// delivery of a press/release counts.
void Browser::barSwipeEvent(QMouseEvent *me)
{
    QPoint g = me->globalPos();
    if (me->type() == QEvent::MouseButtonPress) {
        if (m_barTouch.isValid() && m_barTouch.elapsed() < 30 && g == m_barPress) return;   // same press, parent widget
        // The whole top strip above the page counts: toolbar rows and the status line.
        int pageTop = m_view->mapToGlobal(QPoint(0, 0)).y();
        m_barSwipe = g.y() < pageTop && !m_kbd->isVisible();
        m_swipeMenu = false;
        m_barPress = g;
        m_barTouch.start();
    } else if (m_barSwipe) {
        m_barSwipe = false;
        QPoint d = g - m_barPress;
        if (g.x() >= 0 && d.y() >= kMenuSwipePx && qAbs(d.x()) < d.y() && !KoboTouchInput::gestureActive()) {
            m_swipeMenu = true;   // the address bar's own release handler skips this one
            QTimer::singleShot(0, this, SLOT(openKoreaderMenu()));
        }
    }
}

bool Browser::eventFilter(QObject *obj, QEvent *e)
{
    if (obj == m_url && e->type() == QEvent::MouseButtonRelease && m_swipeMenu) {
        m_swipeMenu = false;
        return QMainWindow::eventFilter(obj, e);
    }
    if (obj == m_view && (e->type() == QEvent::MouseButtonPress || e->type() == QEvent::MouseButtonRelease)) {
        m_lastPageTap.start();
        QPoint p = static_cast<QMouseEvent *>(e)->pos();
        if (e->type() == QEvent::MouseButtonPress) {
            m_tapPress = p;
        } else if (p.x() >= 0 && (p - m_tapPress).manhattanLength() < 25 && !KoboTouchInput::gestureActive()) {
            // A tap (not a drag, not the end of a two-finger gesture). The view handles
            // the click first; then we look at what was tapped.
            m_tapRelease = p;
            QTimer::singleShot(0, this, SLOT(pageTapped()));
        }
    }
    if (obj == m_url && e->type() == QEvent::MouseButtonRelease) {
        // Typing replaces the whole address (there is no Clear button any more).
        if (!m_kbd->isVisible()) QTimer::singleShot(0, m_url, SLOT(selectAll()));
        QTimer::singleShot(0, this, SLOT(showKeyboard()));
    }
    // The status line only shows information: its actions live in the Menu panel.
    return QMainWindow::eventFilter(obj, e);
}

void Browser::resizeEvent(QResizeEvent *e)
{
    QMainWindow::resizeEvent(e);
    placeKeyboard();
    placeTextSizeBar();
}

// Menu > Text size: a bar along the bottom of the screen with smaller / bigger and Done.
// It stays open while you adjust, and the page above it shows each change.
void Browser::showTextSizeBar()
{
    if (m_sizeBar) { m_sizeBar->raise(); return; }
    hideKeyboard();
    QFrame *bar = new QFrame(m_central);
    bar->setFrameStyle(QFrame::Box | QFrame::Plain);
    bar->setLineWidth(3);
    bar->setAutoFillBackground(true);
    QPalette pal = bar->palette(); pal.setColor(QPalette::Window, Qt::white); bar->setPalette(pal);
    bar->setProperty("kb_overlay", true);   // one clean flash when it opens and closes
    QHBoxLayout *row = new QHBoxLayout(bar);
    row->setContentsMargins(12, 10, 12, 10);
    row->setSpacing(10);
    FlatButton *smaller = toolButton(QString::fromUtf8("A\xe2\x88\x92"), 150, 96);
    FlatButton *bigger = toolButton("A+", 150, 96);
    FlatButton *done = toolButton("Done", 230, 96); done->setBold(true);
    done->setIconName("check");
    QLabel *lbl = new QLabel(QString("Text %1%").arg(qRound(m_textZoom * 100)));
    QFont lf; lf.setPixelSize(36); lbl->setFont(lf);
    lbl->setAlignment(Qt::AlignCenter);
    row->addWidget(smaller); row->addWidget(lbl, 1); row->addWidget(bigger); row->addWidget(done);
    connect(smaller, SIGNAL(clicked()), this, SLOT(zoomOut()));
    connect(bigger, SIGNAL(clicked()), this, SLOT(zoomIn()));
    connect(done, SIGNAL(clicked()), this, SLOT(closeTextSizeBar()));
    m_sizeBar = bar;
    m_sizeLabel = lbl;
    placeTextSizeBar();
    bar->show();
    bar->raise();
}

void Browser::placeTextSizeBar()
{
    if (!m_sizeBar || !m_central) return;
    int hgt = 120;
    m_sizeBar->setGeometry(0, m_central->height() - hgt, m_central->width(), hgt);
}

void Browser::closeTextSizeBar()
{
    if (!m_sizeBar) return;
    QWidget *b = m_sizeBar;
    m_sizeBar = 0;
    m_sizeLabel = 0;
    b->hide();
    b->deleteLater();
}

void Browser::placeKeyboard()
{
    if (!m_central) return;
    m_kbd->fitWidth(m_central->width());
    int h = m_kbd->preferredHeight();
    m_kbd->setGeometry(0, m_central->height() - h, m_central->width(), h);
}

// The address bar is normally refreshed with the fast 1-bit waveform; after Clear or a
// backspace redraw it at once with a flashing GC16 so nothing of the old text lingers.
void Browser::flashUrlBar()
{
    if (!m_eink) return;
    if (QApplication::focusWidget() != m_url && sender() == m_kbd) return;
    QRect r(m_url->mapTo(this, QPoint(0, 0)), m_url->size());
    // A grayscale update (no black flash) is enough to clear deleted characters.
    m_eink->markDirty(r.adjusted(-4, -4, 4, 4), m_eink->flagsPartial());
}

void Browser::go()
{
    loadUrlText(m_url->text());
    hideKeyboard();
    m_view->setFocus();
}

// Keyboard's Go/Enter key: load the URL when editing the address bar, otherwise
// act as Enter in whatever page field has focus (search boxes, logins...).
void Browser::keyboardGo()
{
    if (QApplication::focusWidget() == m_url) { go(); return; }
    QWidget *target = QApplication::focusWidget();
    if (!target) return;
    QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier, "\r");
    QApplication::sendEvent(target, &press);
    QKeyEvent release(QEvent::KeyRelease, Qt::Key_Return, Qt::NoModifier, "\r");
    QApplication::sendEvent(target, &release);
}

// Keyboard only when a text box is tapped (never for scripts focusing a field); a tap
// anywhere else in the page puts it away. The page is scrolled so the box stays visible
// above the keyboard.
void Browser::pageTapped()
{
    QWebFrame *frame = m_view->page()->mainFrame();
    QWebHitTestResult hit = frame->hitTestContent(m_tapRelease);
    bool editable = hit.isContentEditable();
    if (getenv("KB_TAP_LOG")) qDebug() << "tap: hit editable" << editable << hit.element().tagName();
    if (!editable) {
        // Fallback for fields the hit test misses. Only what is actually under the finger
        // counts: a page that focuses its search box on load (autofocus) used to pop the
        // keyboard up for any tap or small swipe anywhere, because the focused element was
        // checked instead of the tapped one. The focused element is used only when the tap
        // lands on an inner frame, whose content the outer page cannot see.
        qreal z = m_view->zoomFactor();
        QString js = QString(
            "(function(x,y){function ed(e){if(!e)return false;var t=(e.tagName||'').toLowerCase();"
            "if(t=='textarea'||e.isContentEditable)return true;"
            "return t=='input'&&!/^(button|submit|reset|checkbox|radio|file|image|hidden|range|color)$/i.test(e.type||'');}"
            "var e=document.elementFromPoint(x,y);if(!e)return '';"
            "var t=(e.tagName||'').toLowerCase();"
            "if(t=='iframe'||t=='frame')return ed(document.activeElement)?'y':'';"
            "if(t=='label'&&e.control)e=e.control;"
            "return ed(e)?'y':'';})(%1,%2)").arg(m_tapRelease.x() / z).arg(m_tapRelease.y() / z);
        QString tag = frame->evaluateJavaScript(js).toString();
        editable = (tag == "y") && hit.linkUrl().isEmpty();
        if (getenv("KB_TAP_LOG")) qDebug() << "tap:" << m_tapRelease << "zoom" << z << "field" << tag;
    }
    if (!editable) {
        if (m_kbd->isVisible()) hideKeyboard();
        return;
    }
    m_view->setFocus();
    showKeyboard();
    if (!m_kbd->isVisible()) return;   // KOReader keyboard took over instead
    // Bring the box above the keyboard: its viewport rect in CSS px times the zoom factor.
    QString rect = frame->evaluateJavaScript(
        "(function(){var e=document.activeElement;if(!e||!e.getBoundingClientRect)return '';"
        "var r=e.getBoundingClientRect();return r.top+','+r.bottom;})()").toString();
    QStringList tb = rect.split(',');
    if (tb.size() == 2) {
        qreal z = m_view->zoomFactor();
        int top = int(tb[0].toDouble() * z), bottom = int(tb[1].toDouble() * z);
        // The keyboard is not inside the view: go through global coordinates (mapTo needs an ancestor).
        int kbdTop = m_view->mapFromGlobal(m_kbd->mapToGlobal(QPoint(0, 0))).y();
        const int margin = 30;
        if (bottom > kbdTop - margin) {
            int dy = bottom - (kbdTop - margin);
            if (top - dy < margin) dy = top - margin;   // never push the top of the box off screen
            if (dy > 0) frame->scroll(0, dy);
        }
    }
}

void Browser::loadUrlText(const QString &textIn)
{
    QString text = textIn.trimmed();
    if (text.isEmpty()) return;
    QUrl url;
    bool looksLikeUrl = text.contains("://") ||
        (!text.contains(' ') && text.contains('.') );
    if (text.startsWith("localhost")) looksLikeUrl = true;
    if (looksLikeUrl) {
        if (!text.contains("://")) text = "http://" + text;
        url = QUrl::fromUserInput(text);
    } else if (QRegExp("^(img|image|images|pic|pics|!i|!img)\\s+(.+)$", Qt::CaseInsensitive).exactMatch(text)) {
        // Image search: "img cats" (or "!i cats") goes to Bing Images, which this WebKit
        // can show once webpage.cpp fills in its thumbnails. Tap one for the full image,
        // then Save keeps it in Pictures.
        QRegExp re("^\\S+\\s+(.+)$");
        re.exactMatch(text);
        url = QUrl("https://www.bing.com/images/search?q=" + QString::fromLatin1(QUrl::toPercentEncoding(re.cap(1))));
    } else {
        url = QUrl("https://lite.duckduckgo.com/lite/?q=" + QString::fromLatin1(QUrl::toPercentEncoding(text)));
    }
    m_view->load(KoboWebPage::mobileVariant(url));
}

void Browser::showKeyboard()
{
    if (m_kbd->isVisible() || (m_native && m_native->active())) return;
    if (nativeTextEntry()) return;
    showBuiltinKeyboard();
}

void Browser::showBuiltinKeyboard()
{
    if (m_kbd->isVisible()) return;
    placeKeyboard();
    m_kbd->raise();
    m_kbd->show();
    // The keys are painted with the fast waveform; give the area one grayscale update so
    // they come up crisp, without flashing the screen black.
    if (m_eink) m_eink->markDirty(QRect(m_kbd->mapTo(this, QPoint(0, 0)), m_kbd->size()), m_eink->flagsPartial());
}

// ---- Text entry with KOReader's keyboard (textentry.h). The plugin draws its InputDialog
// over our frozen picture; we stop refreshing and hand it the touchscreen meanwhile.

static QString jsString(const QString &s)
{
    QString out = "\"";
    foreach (QChar c, s) {
        ushort u = c.unicode();
        if (c == '\"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (u < 0x20 || u == 0x2028 || u == 0x2029) out += QString("\\u%1").arg(u, 4, 16, QChar('0'));
        else out += c;
    }
    return out + "\"";
}

// What the focused page element is: "kind\nlabel\nvalue", or empty if it isn't editable.
static const char *kJsActiveField =
    "(function(){var e=document.activeElement;if(!e)return '';"
    "var tag=e.tagName?e.tagName.toLowerCase():'';"
    "var editable=(tag=='input'&&!/^(button|submit|reset|checkbox|radio|file|image|hidden|range|color)$/i.test(e.type||''))"
    "||tag=='textarea'||e.isContentEditable;if(!editable)return '';"
    "var kind=(tag=='textarea'||e.isContentEditable)?'text':'field';"
    "var label=e.placeholder||e.getAttribute('aria-label')||e.title||e.name||'';"
    "var val=('value' in e)?e.value:e.textContent;"
    "return kind+'\\n'+String(label).replace(/\\n/g,' ')+'\\n'+val;})()";

bool Browser::nativeTextEntry()
{
    if (!m_native || !m_native->available() || m_native->active()) return false;
    QWidget *focus = QApplication::focusWidget();
    if (focus == m_view) {
        QString info = m_view->page()->mainFrame()->evaluateJavaScript(kJsActiveField).toString();
        int a = info.indexOf('\n');
        int b = a < 0 ? -1 : info.indexOf('\n', a + 1);
        if (a > 0 && b > a) {
            m_nativeKind = info.left(a);
            m_liveOrig = info.mid(b + 1);
            return m_native->request(m_nativeKind, info.mid(a + 1, b - a - 1), info.mid(b + 1));
        }
        // Nothing editable has focus in the page: the keyboard is for the address bar.
        m_url->setFocus();
    }
    m_nativeKind = "url";
    m_liveOrig = m_url->text();
    return m_native->request("url", "Address or search", m_url->text());
}

void Browser::nativeStarted()
{
    if (m_kbd->isVisible()) m_kbd->hide();
    if (m_eink) m_eink->setSuspended(true);
    if (m_touch) m_touch->setHandedOff(true);
}

// Live typing: one key from KOReader's keyboard, typed into the address bar or page field.
void Browser::sendLiveKey(int key, const QString &text, Qt::KeyboardModifiers mods)
{
    QWidget *target = m_nativeKind == "url" ? static_cast<QWidget *>(m_url) : static_cast<QWidget *>(m_view);
    QKeyEvent press(QEvent::KeyPress, key, mods, text);
    QApplication::sendEvent(target, &press);
    QKeyEvent release(QEvent::KeyRelease, key, mods, text);
    QApplication::sendEvent(target, &release);
}

void Browser::nativeKey(const QString &line)
{
    QString cmd = line.section('\t', 0, 0), arg = line.section('\t', 1);
    if (cmd == "live") {
        // KOReader's keyboard is up: draw the page again, above it, and type into our field.
        m_live = true;
        int kbH = arg.toInt();
        QWidget *w = window();
        if (m_eink) {
            m_eink->setClip(QRect(0, 0, w->width(), qMax(0, w->height() - kbH)));
            m_eink->setSuspended(false, false);
        }
        if (m_nativeKind == "url") {
            m_url->setFocus();
            m_url->end(false);
        } else {
            m_view->setFocus();
            m_view->page()->mainFrame()->evaluateJavaScript(
                "(function(){var e=document.activeElement;if(e&&e.scrollIntoView)e.scrollIntoView(true);})()");
        }
        qDebug() << "textentry: live typing, keyboard" << kbH << "px";
        return;
    }
    if (!m_live) return;
    if (cmd == "add") {
        QString t = arg;
        t.replace("\\n", "\n").replace("\\\\", "\\");
        if (t == "\n") sendLiveKey(Qt::Key_Return, "\r");
        else sendLiveKey(0, t);
    } else if (cmd == "del") sendLiveKey(Qt::Key_Backspace, QString());
    else if (cmd == "delword") sendLiveKey(Qt::Key_Backspace, QString(), Qt::ControlModifier);
    else if (cmd == "dellinestart") { sendLiveKey(Qt::Key_Home, QString(), Qt::ShiftModifier); sendLiveKey(Qt::Key_Backspace, QString()); }
    else if (cmd == "left") sendLiveKey(Qt::Key_Left, QString());
    else if (cmd == "right") sendLiveKey(Qt::Key_Right, QString());
    else if (cmd == "up") sendLiveKey(Qt::Key_Up, QString());
    else if (cmd == "down") sendLiveKey(Qt::Key_Down, QString());
    else if (cmd == "home") sendLiveKey(Qt::Key_Home, QString());
    else if (cmd == "end") sendLiveKey(Qt::Key_End, QString());
}

void Browser::nativeReplied(const QString &action, const QString &text)
{
    if (m_touch) m_touch->setHandedOff(false);
    if (m_live) {
        // Live typing ended: the text is already in the field.
        m_live = false;
        if (m_eink) { m_eink->setClip(QRect()); m_eink->setSuspended(false); m_eink->refreshAllFull(); }
        if (m_nativeKind == "url") {
            if (action == "go") { loadUrlText(m_url->text()); m_view->setFocus(); }
            else if (action == "cancel") { m_url->setText(m_liveOrig); m_view->setFocus(); }
            else m_view->setFocus();
        } else if (action == "go") {
            sendLiveKey(Qt::Key_Return, "\r");
        }
        return;
    }
    if (m_eink) m_eink->setSuspended(false);   // puts our picture back, flashed
    if (action == "cancel") { if (m_nativeKind == "url") m_view->setFocus(); return; }
    if (m_nativeKind == "url") {
        m_url->setText(text);
        if (action == "go") { loadUrlText(text); m_view->setFocus(); }
        return;
    }
    // Page field: set the value (with input/change events for scripts that watch it),
    // then Enter if asked, which submits forms the way a hardware keyboard would.
    QString js = QString(
        "(function(v){var e=document.activeElement;if(!e)return false;"
        "if('value' in e){e.value=v;}else if(e.isContentEditable){e.textContent=v;}else return false;"
        "try{var ev=document.createEvent('Event');ev.initEvent('input',true,true);e.dispatchEvent(ev);"
        "var ev2=document.createEvent('Event');ev2.initEvent('change',true,true);e.dispatchEvent(ev2);}catch(x){}"
        "return true;})(%1)").arg(jsString(text));
    bool ok = m_view->page()->mainFrame()->evaluateJavaScript(js).toBool();
    if (!ok) qWarning() << "textentry: no editable element has focus any more";
    if (ok && action == "go") {
        m_view->setFocus();
        QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier, "\r");
        QApplication::sendEvent(m_view, &press);
        QKeyEvent release(QEvent::KeyRelease, Qt::Key_Return, Qt::NoModifier, "\r");
        QApplication::sendEvent(m_view, &release);
    }
}

// ---- KOReader's top menu over the browser (KB_MENU_FILE). Same hand-off as the keyboard:
// we freeze, KOReader draws its menu over our picture; "back" = take the screen back as it
// was (the page is never reloaded), "quit" = KOReader went elsewhere (home, a book): exit
// without drawing anything, KOReader already owns the screen.

void Browser::openKoreaderMenu()
{
    if (!m_menu || m_menu->active() || (m_native && m_native->active())) return;
    qDebug() << "menu: opening KOReader's menu";
    m_menu->request("menu", "", m_url->text());
}

void Browser::menuReplied(const QString &action)
{
    qDebug() << "menu: KOReader replied" << action;
    if (action == "quit") {
        qApp->quit();
        return;
    }
    if (m_touch) m_touch->setHandedOff(false);
    if (m_eink) m_eink->setSuspended(false);   // puts our picture back, flashed
}

void Browser::menuUnavailable()
{
    if (m_touch) m_touch->setHandedOff(false);
    if (m_eink) m_eink->setSuspended(false);
}

void Browser::nativeUnavailable()
{
    if (m_touch) m_touch->setHandedOff(false);
    if (m_eink) m_eink->setSuspended(false);
    showBuiltinKeyboard();
}

void Browser::hideKeyboard()
{
    if (!m_kbd->isVisible()) return;
    // The page repaints where the keys were with a normal grayscale update (no black flash).
    m_kbd->hide();
    // Typing used the rough DU waveform; redraw the toolbar cleanly once it settles.
    QTimer::singleShot(600, this, SLOT(cleanToolbar()));
}

void Browser::cleanToolbar()
{
    if (!m_eink) return;
    QRect r = QRect(m_bar->mapTo(this, QPoint(0, 0)), m_bar->size());
    m_eink->cleanRect(r);
}

void Browser::fullRefresh()    { if (m_eink) m_eink->refreshAllFull(); }

// ---- Screen rotation (rotation.h). The window is laid out at the turned size; eink.cpp
// turns the pixels and touchinput.cpp the taps. Saved for the next start.
// Tapped in the Menu: rotate once the Menu's closing refresh is over (a pop-up's close is
// drawn as one batched update, which would otherwise swallow the turned window's repaint).
void Browser::rotateNow()   { applyRotation(m_rotTarget, true); }

// Menu > Rotate screen: the four orientations, as KOReader offers them. The current one
// is marked; picking another turns the screen once the panel has closed.
void Browser::showRotateMenu()
{
    int top = m_bar->mapTo(centralWidget(), QPoint(0, m_bar->height())).y() + 4;
    MenuPanel *m = new MenuPanel(centralWidget(), top);
    const int cur = KbRotation::angle();
    m->addSection("Rotate screen");
    // The current orientation is the gray button.
    m->addAction("Portrait", this, SLOT(rotateTo0()), false, "rotation.0UR")->setDark(cur == 0);
    m->addAction("Landscape", this, SLOT(rotateTo90()), false, "rotation.90CW")->setDark(cur == 90);
    m->addAction("Portrait, flipped", this, SLOT(rotateTo180()), false, "rotation.180UD")->setDark(cur == 180);
    m->addAction("Landscape, flipped", this, SLOT(rotateTo270()), false, "rotation.90CCW")->setDark(cur == 270);
    connect(m, SIGNAL(closed()), this, SLOT(menuClosed()));
    m->popup();
}
void Browser::rotateTo(int a) { if (a == KbRotation::angle()) return; m_rotTarget = a; QTimer::singleShot(500, this, SLOT(rotateNow())); }
void Browser::rotateTo0()   { rotateTo(0); }
void Browser::rotateTo90()  { rotateTo(90); }
void Browser::rotateTo180() { rotateTo(180); }
void Browser::rotateTo270() { rotateTo(270); }

void Browser::applyRotation(int angle, bool save)
{
    if (!m_eink || !m_eink->canRotate()) return;
    hideKeyboard();
    m_rotPrev = KbRotation::angle();
    KbRotation::setAngle(angle);
    QRect g(QPoint(0, 0), KbRotation::logicalSize());
    qDebug() << "rotate:" << m_rotPrev << "->" << KbRotation::angle() << "window" << g;
    setWindowState(windowState() & ~Qt::WindowFullScreen);
    setGeometry(g);
    if (save) KbRotation::save();
    m_status->setText("Screen: " + KbRotation::name());
    // Once the window has its new size and has repainted, check it and flash it all clean.
    QTimer::singleShot(300, this, SLOT(rotationSettled()));
}

void Browser::rotationSettled()
{
    QSize want = KbRotation::logicalSize();
    QSize got = windowHandle() ? windowHandle()->geometry().size() : size();
    {
        QBackingStore *bs = backingStore();
        QSize bss = bs && bs->paintDevice() ? QSize(bs->paintDevice()->width(), bs->paintDevice()->height()) : QSize();
        qDebug() << "rotate: settled; widget" << size() << "central" << (centralWidget() ? centralWidget()->size() : QSize())
                 << "handle" << got << "backing store" << bss << "want" << want;
    }
    // Make sure every widget paints at the new size (the platform may not repaint on resize).
    repaint();
    if (got != want) {
        // The platform kept the old size: go back, or the picture and taps won't line up.
        qWarning() << "rotate: window stayed" << got << "wanted" << want << "- undoing";
        KbRotation::setAngle(m_rotPrev);
        KbRotation::save();
        setGeometry(QRect(QPoint(0, 0), KbRotation::logicalSize()));
        m_status->setText("Rotation didn't work on this screen");
    }
    if (m_eink) m_eink->refreshAllFull();
}
void Browser::pinchZoom(double ratio)
{
    ratio = qBound(0.5, ratio, 2.0);
    qreal z = qBound(0.5, m_view->zoomFactor() * ratio, 5.0);
    if (m_eink) m_eink->hintFull();   // relayout of the whole page: flash it clean
    m_view->setZoomFactor(z);
}

void Browser::toggleKeyboard()
{
    if (m_kbd->isVisible()) hideKeyboard();
    else { if (!QApplication::focusWidget() || QApplication::focusWidget() == m_view) m_url->setFocus(); showKeyboard(); }
}

// Text size steps for A+/A-. Text-only zoom: WebKit scales the fonts and reflows the
// text at the same layout width. (Full page zoom, which A+/A- used to change, also
// narrows the layout viewport, so sites switched to their narrow/mobile layout or
// overflowed sideways.) The page zoom stays where pinch put it.
static const qreal kTextSteps[] = { 0.8, 0.9, 1.0, 1.1, 1.25, 1.4, 1.6, 1.8, 2.0 };
static const int kTextStepCount = sizeof(kTextSteps) / sizeof(kTextSteps[0]);

void Browser::setTextZoom(qreal z, bool save)
{
    z = qBound(kTextSteps[0], z, kTextSteps[kTextStepCount - 1]);
    m_textZoom = z;
    // QWebFrame::setZoomFactor sets the text zoom while ZoomTextOnly is on and the page
    // zoom otherwise; WebKit keeps the two factors separately. Everything else in the
    // browser (pinch, restore, the sleep line) reads and sets the page zoom, so the
    // attribute is only on for this one call.
    QWebSettings *ws = m_view->settings();
    ws->setAttribute(QWebSettings::ZoomTextOnly, true);
    m_view->setZoomFactor(z);
    ws->setAttribute(QWebSettings::ZoomTextOnly, false);
    if (!save) return;
    QFile tf(QDir(QCoreApplication::applicationDirPath()).filePath("text-size.txt"));
    if (tf.open(QIODevice::WriteOnly | QIODevice::Truncate))
        tf.write(QByteArray::number(double(z), 'f', 2) + '\n');
    m_status->setText(QString("Text size %1%").arg(qRound(z * 100)));
    if (m_sizeLabel) m_sizeLabel->setText(QString("Text %1%").arg(qRound(z * 100)));
}

static int textStep(qreal z)
{
    int best = 0;
    for (int i = 1; i < kTextStepCount; ++i)
        if (qAbs(kTextSteps[i] - z) < qAbs(kTextSteps[best] - z)) best = i;
    return best;
}

void Browser::zoomIn()
{
    int i = textStep(m_textZoom);
    if (i + 1 >= kTextStepCount) { m_status->setText("Text size: largest"); return; }
    if (m_eink) m_eink->hintFull();
    setTextZoom(kTextSteps[i + 1], true);
}
void Browser::zoomOut()
{
    int i = textStep(m_textZoom);
    if (i <= 0) { m_status->setText("Text size: smallest"); return; }
    if (m_eink) m_eink->hintFull();
    setTextZoom(kTextSteps[i - 1], true);
}

void Browser::urlChanged(const QUrl &url) { m_url->setText(url.toString()); }
void Browser::loadProgress(int p)  { if (p % 25 == 0) m_status->setText(QString("Loading %1%").arg(p)); }

// E-ink friendly loading: while a page loads, WebKit repaints progressively. We let
// Qt paint into the backing store but hold the screen updates of the web view, then
// show the finished page with one clean full refresh (KoboEink hold, see eink.h).
void Browser::loadStarted()
{
    if (m_offlineShown) {   // any load after the offline copy (Reload, a link) is live
        m_offlineShown = false;
        if (m_onlineTimer) m_onlineTimer->stop();
    }
    m_status->setText("Loading...");
    if (m_eink && !getenv("KB_NO_LOAD_HOLD")) m_eink->beginHold(m_view);
}
void Browser::loadFinished(bool ok)
{
    // KB_HOLD_UNTIL_LOADED: the wake screen stays on the panel until the page is ready.
    if (m_holdFirst) {
        m_holdFirst = false;
        if (m_eink) m_eink->setSuspended(false);   // first picture, one full refresh
    }
    if (m_offline) {
        m_status->setText("Offline copy (no connection)");
        m_offline = false;
        m_offlineShown = true;
        checkOnline();
    } else {
        m_status->setText(ok ? m_view->title() : "Failed to load page");
    }
    if (m_phone) m_phone->setCurrentPage(m_view->title(), m_view->url().toString());
    if (ok && !m_offlineShown) HistoryStore::record(m_view->url().toString(), m_view->title());
    if (ok && m_restoreY > 0) {
        m_view->page()->mainFrame()->setScrollPosition(QPoint(0, m_restoreY));
        qDebug() << "restore: scrolled to" << m_restoreY;
        m_restoreY = -1;
    }
    // A page wider than the screen (a wide table) sometimes ends up scrolled sideways after
    // loading, cutting off the start of every line. Unless the address points at an anchor,
    // start at the left edge.
    if (ok && !m_view->url().hasFragment()) {
        QWebFrame *f = m_view->page()->mainFrame();
        if (f->scrollPosition().x() != 0) {
            qDebug() << "load: page was scrolled sideways by" << f->scrollPosition().x() << "- back to the left edge";
            f->setScrollPosition(QPoint(0, f->scrollPosition().y()));
        }
    }
    // WebKit usually does a last layout/paint shortly after loadFinished; give it a moment.
    QTimer::singleShot(250, this, SLOT(releaseLoadHold()));
    // Back from a sleep (online or the offline copy): once the page and its scroll position
    // are on screen, flash it clean, since the wake screen ghosts through otherwise.
    if (m_relaunch) {
        m_relaunch = false;
        QTimer::singleShot(250 + 300, this, SLOT(fullRefresh()));
    }
}
// Hand sleep to KOReader: note the page (URL, scroll, zoom) in the handoff file and
// exit. The KOReader plugin puts KOReader to sleep with its own screensaver and
// relaunches the browser on this page when the device wakes.
void Browser::sleepToKoreader()
{
    QString url = m_view->url().toString();
    if (url.isEmpty() || url == "about:blank") url = m_home;
    int y = m_view->page()->mainFrame()->scrollPosition().y();
    QString line = url + '\t' + QString::number(y) + '\t' + QString::number(m_view->zoomFactor(), 'f', 2);
    // For the wake screen (kobo-webbrowser.koplugin): what the screen showed, and the page
    // itself, so "Cancel" on the wake screen can reopen it without a connection.
    // The plugin deletes both once the wake is over.
    QString dir = QCoreApplication::applicationDirPath();
    const char *sf = getenv("KB_SLEEP_SHOT_FILE");
    QString shot = (sf && *sf) ? QString::fromLocal8Bit(sf) : dir + "/sleep-shot.png";
    const char *pf = getenv("KB_SLEEP_PAGE_FILE");
    QString pagePath = (pf && *pf) ? QString::fromLocal8Bit(pf) : dir + "/sleep-page.html";
    // Both are written before the handoff line, so the plugin never sees a sleep line
    // without its files (it SIGKILLs us only after STOP_WAIT_SEC).
    QElapsedTimer t;
    t.start();
    if (!m_eink || !m_eink->saveScreenshotTo(shot))
        qDebug() << "screenshot: no sleep shot" << shot << (m_eink ? "(save failed)" : "(no e-ink)");
    QFile page(pagePath);
    if (page.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qint64 n = page.write(m_view->page()->mainFrame()->toHtml().toUtf8());
        page.close();
        qDebug() << "power: sleep page" << pagePath << n << "bytes";
    } else {
        qDebug() << "power: can't write sleep page" << pagePath << page.errorString();
    }
    qDebug() << "power: sleep files took" << t.elapsed() << "ms";
    saveSessionHistory();
    EpubSaver::appendHandoff("sleep", line);
    qDebug() << "power: sleep handoff" << line;
    qApp->quit();
}

// Online = a default route (Destination 00000000) on an interface other than lo. If
// wlan0 exists, it must also be "up".
static bool deviceOnline()
{
    QFile oper("/sys/class/net/wlan0/operstate");
    if (oper.open(QIODevice::ReadOnly)) {
        QByteArray state = oper.readAll().trimmed();
        if (state != "up") return false;
    }
    QFile route("/proc/net/route");
    if (!route.open(QIODevice::ReadOnly)) return false;
    route.readLine();   // header
    while (!route.atEnd()) {
        QList<QByteArray> f = route.readLine().simplified().split(' ');
        if (f.size() > 1 && f[0] != "lo" && f[1] == "00000000") return true;
    }
    return false;
}

void Browser::checkOnline()
{
    if (m_offline) return;   // the offline copy is still loading
    if (!m_offlineShown) { if (m_onlineTimer) m_onlineTimer->stop(); return; }
    if (!deviceOnline()) return;
    qDebug() << "restore: online, suggesting Reload";
    m_status->setText(QString("Offline copy ") + QChar(0x00b7) + " back online, press Reload");
    if (m_onlineTimer) m_onlineTimer->stop();
}

void Browser::reloadLive()
{
    // Leaving the offline copy (a page set with setHtml) for the live one: stop whatever
    // WebKit still has in flight for the offline document, drop the timer, and start the
    // live load from a clean event-loop turn. Crashed in QtWidgets when done inline.
    if (m_onlineTimer) { m_onlineTimer->stop(); }
    m_offlineShown = false;
    m_view->stop();
    QTimer::singleShot(150, this, SLOT(reloadLiveNow()));
}

void Browser::reloadLiveNow() { loadUrlText(m_home); }

// Reload button: on the offline copy (a setHtml page) reload() would only redisplay that
// HTML, so leave it for the original address through reloadLive().
void Browser::reloadClicked()
{
    if (m_offline || m_offlineShown) {
        qDebug() << "restore: reloading the live page" << m_home;
        reloadLive();
    } else {
        m_view->reload();
    }
}

void Browser::releaseLoadHold()
{
    if (m_eink) m_eink->releaseHold(true);
}


// Reads the first line of a sysfs/proc file, trimmed; empty if unreadable.
static QString readLine(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QString();
    return QString::fromLatin1(f.readLine()).trimmed();
}

void Browser::updateSysInfo()
{
    QStringList parts;
    parts << QTime::currentTime().toString("h:mm AP");

    // Battery: first power_supply entry that reports a capacity (bd71827_bat on Clara BW).
    QDir ps("/sys/class/power_supply");
    foreach (const QString &name, ps.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QString cap = readLine(ps.filePath(name + "/capacity"));
        if (cap.isEmpty()) continue;
        QString status = readLine(ps.filePath(name + "/status"));
        QString s = cap + "%";
        if (status == "Charging") s += "+";
        parts << s;
        break;
    }

    // Memory: MemAvailable (fallback MemFree) in MB.
    QFile mi("/proc/meminfo");
    if (mi.open(QIODevice::ReadOnly)) {
        long avail = -1, freeKb = -1;
        while (!mi.atEnd()) {
            QByteArray line = mi.readLine();
            if (line.startsWith("MemAvailable:")) avail = line.mid(13).trimmed().split(' ').first().toLong();
            else if (line.startsWith("MemFree:")) freeKb = line.mid(8).trimmed().split(' ').first().toLong();
        }
        long kb = avail >= 0 ? avail : freeKb;
        if (kb >= 0) parts << QString("%1M free").arg(kb / 1024);
    }

    QString text = parts.join("   ");
    if (m_sysInfo->text() != text) m_sysInfo->setText(text);

    // Re-arm so the next update lands just after the minute changes.
    int msToNextMinute = (60 - QTime::currentTime().second()) * 1000 + 500;
    m_sysTimer->start(msToNextMinute);
}

void Browser::quit()
{
    // Instant feedback that the browser is closing: white screen with a full flash.
    // KOReader repaints itself (another full refresh) once the process has exited.
    if (m_eink) m_eink->clearScreenForExit();
    qApp->quit();
}

void Browser::screenshot()
{
    QString dir = "/mnt/onboard/screenshots";
    if (!QDir("/mnt/onboard").exists()) dir = QDir::homePath() + "/screenshots";
    QString path = m_eink ? m_eink->saveScreenshot(dir) : QString();
    m_status->setText(path.isEmpty() ? QString("Screenshot failed") : QString("Saved " + path.mid(path.indexOf("screenshots"))));
}

// ---- Save as EPUB (see epub.cpp)

void Browser::saveEpub()
{
    if (m_saver && m_saver->isRunning()) return;
    // Part of a multi-part work? Offer the whole thing as one book.
    QString listTitle;
    m_seriesLinks = EpubSaver::findChapters(m_view->page(), &listTitle);
    if (m_seriesLinks.size() >= 2) {
        m_seriesTitle = listTitle;
        if (EpubSaver::looksLikeComic(m_view->page())) { showSaveChoice(true); return; }
        // A list of parts (an artist's or a series' page): look at the first part to see
        // whether they are comics, which adds "each part as its own comic".
        if (!m_probe) {
            m_probe = new QWebPage(this);
            m_probe->setNetworkAccessManager(m_view->page()->networkAccessManager());
            m_probe->settings()->setAttribute(QWebSettings::AutoLoadImages, false);
            m_probe->settings()->setAttribute(QWebSettings::PluginsEnabled, false);
            m_probe->setViewportSize(QSize(1000, 1400));
            connect(m_probe, SIGNAL(loadFinished(bool)), this, SLOT(probeLoaded(bool)));
            m_probeTimer = new QTimer(this);
            m_probeTimer->setSingleShot(true);
            connect(m_probeTimer, SIGNAL(timeout()), this, SLOT(probeTimedOut()));
        }
        m_status->setText("Saving: checking the series...");
        m_probeTimer->start(20000);
        m_probe->mainFrame()->load(m_seriesLinks.first().url);
        return;
    }
    savePageOnly();
}

void Browser::probeLoaded(bool ok)
{
    if (!m_probeTimer->isActive()) return;   // timed out already
    m_probeTimer->stop();
    showSaveChoice(ok && EpubSaver::isComicChapter(m_probe));
    m_probe->mainFrame()->setHtml(QString());
}

void Browser::probeTimedOut()
{
    m_probe->triggerAction(QWebPage::Stop);
    showSaveChoice(false);
}

void Browser::showSaveChoice(bool comic)
{
    SaveChoiceDialog *d = new SaveChoiceDialog(m_seriesLinks.size(), QString(), centralWidget(), false, comic);
    connect(d, SIGNAL(pageChosen()), this, SLOT(savePageOnly()));
    connect(d, SIGNAL(seriesChosen()), this, SLOT(saveSeries()));
    connect(d, SIGNAL(seriesSplitChosen()), this, SLOT(saveSeriesSplit()));
}

// Each part of a comic series as its own CBZ.
void Browser::saveSeriesSplit()
{
    if (m_saver && m_saver->isRunning()) return;
    delete m_saver;
    m_saver = new EpubSaver(m_view->page(), EpubSaver::defaultOutDir(), this);
    m_saver->setComicSplit(true);
    connect(m_saver, SIGNAL(progress(QString)), this, SLOT(saveProgress(QString)));
    connect(m_saver, SIGNAL(finished(bool,QString)), this, SLOT(saveFinished(bool,QString)));
    m_saver->startSeries(m_seriesLinks, m_seriesTitle);
}

void Browser::savePageOnly()
{
    if (m_saver && m_saver->isRunning()) return;
    if (saveImageIfImagePage()) return;
    delete m_saver;
    m_saver = new EpubSaver(m_view->page(), EpubSaver::defaultOutDir(), this);
    m_saver->setComic(EpubSaver::looksLikeComic(m_view->page()));
    connect(m_saver, SIGNAL(progress(QString)), this, SLOT(saveProgress(QString)));
    connect(m_saver, SIGNAL(finished(bool,QString)), this, SLOT(saveFinished(bool,QString)));
    m_saver->start();
}

void Browser::saveSeries()
{
    if (m_saver && m_saver->isRunning()) return;
    delete m_saver;
    m_saver = new EpubSaver(m_view->page(), EpubSaver::defaultOutDir(), this);
    if (EpubSaver::looksLikeComic(m_view->page())) m_saver->setComic(true);
    connect(m_saver, SIGNAL(progress(QString)), this, SLOT(saveProgress(QString)));
    connect(m_saver, SIGNAL(finished(bool,QString)), this, SLOT(saveFinished(bool,QString)));
    m_saver->startSeries(m_seriesLinks, m_seriesTitle);
}

// Reader mode: the article (Readability, as Save does) goes into a temporary EPUB in
// EpubSaver::readerDir(), not the library. The "read" handoff line names it together with
// this page, scroll position and zoom; kobo-webbrowser.koplugin opens it in KOReader and,
// once that book is closed, deletes it and asks whether to go back to the browser on the same spot.
void Browser::readInKoreader()
{
    if (m_saver && m_saver->isRunning()) return;
    hideKeyboard();
    // A page that lists the parts of a series: offer them all as one temporary book.
    QString listTitle;
    m_seriesLinks = EpubSaver::findChapters(m_view->page(), &listTitle);
    if (m_seriesLinks.size() >= 2) {
        m_seriesTitle = listTitle;
        SaveChoiceDialog *d = new SaveChoiceDialog(m_seriesLinks.size(), QString(), centralWidget(), true);
        connect(d, SIGNAL(pageChosen()), this, SLOT(readPageInKoreader()));
        connect(d, SIGNAL(seriesChosen()), this, SLOT(readSeriesInKoreader()));
        return;
    }
    readPageInKoreader();
}

// The whole series as one temporary book (chapters + table of contents), removed on close
// like a single page.
void Browser::readSeriesInKoreader()
{
    if (m_saver && m_saver->isRunning()) return;
    EpubSaver::clearReaderDir();
    delete m_saver;
    m_saver = new EpubSaver(m_view->page(), EpubSaver::readerDir(), this);
    m_saver->setLibrary(false);
    if (EpubSaver::looksLikeComic(m_view->page())) m_saver->setComic(true);
    connect(m_saver, SIGNAL(progress(QString)), this, SLOT(saveProgress(QString)));
    connect(m_saver, SIGNAL(finished(bool,QString)), this, SLOT(readerFinished(bool,QString)));
    m_saver->startSeries(m_seriesLinks, m_seriesTitle);
}

void Browser::readPageInKoreader()
{
    if (m_saver && m_saver->isRunning()) return;
    EpubSaver::clearReaderDir();
    delete m_saver;
    m_saver = new EpubSaver(m_view->page(), EpubSaver::readerDir(), this);
    m_saver->setLibrary(false);
    m_saver->setComic(EpubSaver::looksLikeComic(m_view->page()));
    connect(m_saver, SIGNAL(progress(QString)), this, SLOT(saveProgress(QString)));
    connect(m_saver, SIGNAL(finished(bool,QString)), this, SLOT(readerFinished(bool,QString)));
    m_saver->start();
}

// Back/Fwd history kept across a relaunch (back from a temporary book, or after sleep):
// written just before the browser hands over to KOReader, read once by the next start.
static QString sessionHistoryFile()
{
    return QDir(QCoreApplication::applicationDirPath()).filePath("session-history.dat");
}

void Browser::saveSessionHistory()
{
    QFile f(sessionHistoryFile());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    QDataStream out(&f);
    out << *m_view->history();
    qDebug() << "history: saved" << m_view->history()->count() << "entries for the next start";
}

// Restores the saved history when this start reopens its current page; true if it did
// (the history then loads that page itself).
bool Browser::restoreSessionHistory(const QString &startUrl)
{
    QFile f(sessionHistoryFile());
    if (!f.exists()) return false;
    QFileInfo fi(f);
    bool fresh = fi.lastModified().secsTo(QDateTime::currentDateTime()) < 7 * 24 * 3600;
    bool ok = false;
    if (fresh && getenv("KB_START_FROM_ARG") && f.open(QIODevice::ReadOnly)) {
        QDataStream in(&f);
        in >> *m_view->history();
        f.close();
        QWebHistory *h = m_view->history();
        QString cur = h->currentItem().isValid() ? h->currentItem().url().toString() : QString();
        QString want = QUrl(startUrl).toString();
        if (cur.endsWith('/')) cur.chop(1);
        if (want.endsWith('/')) want.chop(1);
        ok = h->count() > 1 && !cur.isEmpty() && cur == want;
        if (!ok) h->clear();
        qDebug() << "history:" << (ok ? "restored" : "not restored") << h->count() << "entries, current" << cur << "start" << want;
    }
    QFile::remove(sessionHistoryFile());   // used once
    return ok;
}

void Browser::readerFinished(bool ok, const QString &msg)
{
    if (!ok) {
        m_status->setText("Reader mode failed: " + msg);
        return;
    }
    QString url = m_view->url().toString();
    if (m_offline || url.isEmpty() || url == "about:blank") url = m_home;
    int y = m_view->page()->mainFrame()->scrollPosition().y();
    QString line = msg + '\t' + url + '\t' + QString::number(y) + '\t' + QString::number(m_view->zoomFactor(), 'f', 2);
    if (!EpubSaver::appendHandoff("read", line)) {
        m_status->setText("Reader mode failed: can't reach KOReader");
        return;
    }
    qDebug() << "reader: handoff" << line;
    saveSessionHistory();
    m_status->setText("Opening in KOReader…");
    QTimer::singleShot(400, qApp, SLOT(quit()));
}

// Menu > Minimize: hand the screen back to KOReader, remembering the page, its scroll
// position and Back/Fwd. Tools > Web browser > Resume (or the usual launch) reopens it there.
void Browser::minimize()
{
    QString url = m_view->url().toString();
    if (m_offline || url.isEmpty() || url == "about:blank") url = m_home;
    int y = m_view->page()->mainFrame()->scrollPosition().y();
    QString title = m_view->title().simplified();
    title.replace('\t', ' ');
    QString line = url + '\t' + QString::number(y) + '\t' + QString::number(m_view->zoomFactor(), 'f', 2) + '\t' + title;
    if (!EpubSaver::appendHandoff("minimized", line)) {
        m_status->setText("Minimize needs the KOReader plugin");
        return;
    }
    saveSessionHistory();
    qDebug() << "minimize: handoff" << line;
    m_status->setText("Minimizing…");
    QTimer::singleShot(300, qApp, SLOT(quit()));
}

void Browser::phoneUrl(const QString &url)
{
    qDebug() << "phone: open" << url;
    m_url->setText(url);
    loadUrlText(url);
}

// Where the phone page lives, shown once in the status line after start-up.
void Browser::showPhoneHint()
{
    if (!m_phone) return;
    QString a = m_phone->address();
    if (a.isEmpty()) return;
    m_status->setText("Phone link on: " + a + "  (Menu > From phone to stop)");
}

void Browser::menuPressed()  { m_menuHeld = false; m_holdTimer->start(); }
void Browser::menuReleased() { m_holdTimer->stop(); }
void Browser::menuClicked()
{
    if (m_menuHeld) { m_menuHeld = false; return; }   // the hold already ran its action
    showMenu();
}

void Browser::menuHoldFired()
{
    m_menuHeld = true;
    QString key = "reader";
    QFile f(QDir(QCoreApplication::applicationDirPath()).filePath("hold-menu.txt"));
    if (f.open(QIODevice::ReadOnly)) {
        QString k = QString::fromUtf8(f.readAll()).trimmed();
        if (!k.isEmpty()) key = k;
    }
    runQuickAction(key);
}

void Browser::runQuickAction(const QString &key)
{
    const bool plugin = m_menu && m_menu->available();
    static const char *const table[][2] = {
        { "reader", "readInKoreader" }, { "menu", "showMenu" }, { "textsize", "showTextSizeBar" },
        { "contents", "showToc" }, { "bookmarks", "showBookmarks" }, { "history", "showHistory" },
        { "addbookmark", "addBookmark" }, { "save", "saveEpub" }, { "home", "goHome" },
        { "reload", "reloadClicked" }, { "clean", "fullRefresh" }, { "rotate", "showRotateMenu" },
        { "keyboard", "toggleKeyboard" }, { "phone", "showPhoneDialog" }, { "screenshot", "screenshot" },
        { "koreader", "openKoreaderMenu" }, { "minimize", "minimize" },
    };
    const char *slot = "showMenu";
    for (unsigned i = 0; i < sizeof table / sizeof table[0]; ++i)
        if (key == QLatin1String(table[i][0])) { slot = table[i][1]; break; }
    // Reader mode and the KOReader menu need the plugin; without it, just open the Menu.
    if (!plugin && (key == "reader" || key == "koreader" || key == "minimize")) slot = "showMenu";
    if (key == "rotate" && !(m_eink && m_eink->canRotate())) slot = "showMenu";
    QMetaObject::invokeMethod(this, slot, Qt::QueuedConnection);
}

void Browser::showMenu()
{
    hideKeyboard();
    int top = m_bar->mapTo(centralWidget(), QPoint(0, m_bar->height())).y() + 4;
    MenuPanel *m = new MenuPanel(centralWidget(), top);
    const bool plugin = m_menu && m_menu->available();   // started from the KOReader plugin
    m->addSection("Go to");
    m->addAction("Home", this, SLOT(goHome()), false, "home");
    m->addAction("Bookmarks", this, SLOT(showBookmarks()), false, "bookmark");
    m->addAction("History", this, SLOT(showHistory()), false, "back.top");
    m->addAction("From phone", this, SLOT(showPhoneDialog()), false, "wifi");
    m->addSection("This page");
    // Pairs: view, then keep, then read/save, then screen. Rows stay full in both columns.
    m->addAction("Reload", this, SLOT(reloadClicked()), false, "cre.render.reload");
    m->addAction("Contents", this, SLOT(showToc()), false, "align.left");
    m->addAction("Text size", this, SLOT(showTextSizeBar()), false, "appbar.textsize");
    m->addAction("Keyboard", this, SLOT(toggleKeyboard()), false, "edit");
    m->addAction("Add bookmark", this, SLOT(addBookmark()), false, "star.empty");
    m->addAction("Set as home", this, SLOT(setAsHome()), false, "check");
    if (plugin) m->addAction("Reader mode", this, SLOT(readInKoreader()), false, "book.opened");
    m->addAction("Save", this, SLOT(saveEpub()), false, "move.down");
    m->addAction("Share", this, SLOT(sharePage()), false, "move.up");
    m->addAction("Screenshot", this, SLOT(screenshot()), false, "appbar.crop");
    m->addAction("Clean screen", this, SLOT(fullRefresh()), m->atRowStart(), "appbar.contrast");
    m->addSection("Settings");
    m->addAction(HistoryStore::enabled() ? "History: on" : "History: off", this, SLOT(toggleHistory()), false, "back.top");
    m->addAction(AdBlock::instance()->isEnabled() ? "Ad blocking: on" : "Ad blocking: off", this, SLOT(toggleAdBlock()), false, "cancel");
    const bool rot = m_eink && m_eink->canRotate();
    if (rot) m->addAction("Rotate screen", this, SLOT(showRotateMenu()), !plugin, "appbar.rotation");
    if (plugin) m->addAction("KOReader menu", this, SLOT(openKoreaderMenu()), !rot, "appbar.settings");
    m->addSpacer();
    if (plugin) m->addAction("Minimize", this, SLOT(minimize()), false, "zoom.page");
    m->addAction("Exit browser", this, SLOT(quit()), !plugin, "exit");
    connect(m, SIGNAL(closed()), this, SLOT(menuClosed()));
    m->popup();
    // No full-screen flash: the panel's own repaint is a plain partial update of its area.
    m_menuRect = QRect(m->mapTo(this, QPoint(0, 0)), m->size());
    foreach (QObject *o, m->children())
        if (QWidget *w = qobject_cast<QWidget *>(o)) { m_menuRect = QRect(w->mapTo(this, QPoint(0, 0)), w->size()); break; }
}

// The panel was drawn over the page: once the page has repainted there, give that area
// a non-flashing grayscale pass so no outline of the buttons lingers.
void Browser::menuClosed()
{
    // KoboEink flashes the area the panel covered once it is gone (kb_overlay).
    m_menuRect = QRect();
}

void Browser::toggleAdBlock()
{
    AdBlock::instance()->toggle();
    bool on = AdBlock::instance()->isEnabled();
    m_status->setText(on ? "Ad blocking on (reloading)" : "Ad blocking off (reloading)");
    reloadClicked();
}

void Browser::showHistory()
{
    BookmarksOverlay *b = new BookmarksOverlay(centralWidget(), true);
    connect(b, SIGNAL(openUrl(QString)), this, SLOT(phoneUrl(QString)));
}

void Browser::toggleHistory()
{
    bool on = !HistoryStore::enabled();
    HistoryStore::setEnabled(on);
    m_status->setText(on ? "History on: pages you open are remembered"
                         : "History off: pages you open are not remembered");
}

void Browser::goHome()
{
    m_url->setText(m_homePage);
    loadUrlText(m_homePage);
}

void Browser::showBookmarks()
{
    BookmarksOverlay *b = new BookmarksOverlay(centralWidget());
    connect(b, SIGNAL(openUrl(QString)), this, SLOT(phoneUrl(QString)));
}

static QString currentPageUrl(QWebView *v)
{
    QString u = v->url().toString();
    return (u.isEmpty() || u == "about:blank" || u.startsWith("data:")) ? QString() : u;
}

void Browser::addBookmark()
{
    QString u = currentPageUrl(m_view);
    if (u.isEmpty()) { m_status->setText("Nothing to bookmark on this page"); return; }
    if (BookmarkStore::add(u, m_view->title())) m_status->setText("Bookmarked: " + (m_view->title().isEmpty() ? u : m_view->title()));
    else m_status->setText("Already in your bookmarks");
}

void Browser::setAsHome()
{
    QString u = currentPageUrl(m_view);
    if (u.isEmpty()) { m_status->setText("This page can't be the home page"); return; }
    QFile hf(QDir(QCoreApplication::applicationDirPath()).filePath("home.txt"));
    if (!hf.open(QIODevice::WriteOnly | QIODevice::Truncate)) { m_status->setText("Could not save the home page"); return; }
    hf.write(u.toUtf8());
    hf.close();
    m_homePage = u;
    m_status->setText("Home page set: " + (m_view->title().isEmpty() ? u : m_view->title()));
}

void Browser::showPhoneDialog()
{
    if (!m_phone) return;
    if (!m_phone->isListening() && !m_phone->listen(m_phone->property("wantedPort").toInt())) {
        m_status->setText("Phone link: could not start (port in use?)");
        return;
    }
    // Closing it repaints the page under it with a normal partial update (no flash).
    PhoneDialog *d = new PhoneDialog(m_phone->address(), centralWidget());
    connect(d, SIGNAL(minimized()), this, SLOT(showPhoneHint()));
    connect(d, SIGNAL(stopped()), this, SLOT(stopPhone()));
}

void Browser::sharePage()
{
    QString url = m_view->url().toString();
    if (url.isEmpty() || url == "about:blank") { m_status->setText("Nothing to share"); return; }
    PhoneDialog::share(m_view->title().simplified(), url, centralWidget());
}

void Browser::stopPhone()
{
    if (m_phone) m_phone->stop();
    m_status->setText("Phone link off");
}

void Browser::saveProgress(const QString &msg) { m_status->setText(msg); }

// ---- Save an image (an image opened on its own, e.g. a tapped Bing Images result)

static QString imageOutDir()
{
    const char *env = getenv("KB_IMAGES_DIR");
    if (env && *env) return QString::fromLocal8Bit(env);
    if (QDir("/mnt/onboard").exists()) return "/mnt/onboard/Pictures";
    return QDir::homePath() + "/kobo-browser-pictures";
}

bool Browser::saveImageIfImagePage()
{
    QWebFrame *f = m_view->page()->mainFrame();
    QUrl url = f->url();
    if (url.scheme() != "http" && url.scheme() != "https") return false;
    // WebKit shows a bare image as a document holding just that one image.
    QVariant v = f->evaluateJavaScript(
        "(function(){var b=document.body;return !!b&&document.images.length==1&&b.children.length==1"
        "&&document.images[0].src==location.href;})()");
    if (!v.toBool()) return false;
    m_status->setText("Saving image...");
    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::PreferCache);
    QNetworkReply *r = m_view->page()->networkAccessManager()->get(req);
    connect(r, SIGNAL(finished()), this, SLOT(imageDownloaded()));
    return true;
}

void Browser::imageDownloaded()
{
    QNetworkReply *r = qobject_cast<QNetworkReply *>(sender());
    if (!r) return;
    r->deleteLater();
    if (r->error() != QNetworkReply::NoError) {
        m_status->setText("Save failed: " + r->errorString());
        return;
    }
    QByteArray data = r->readAll();
    QString type = r->header(QNetworkRequest::ContentTypeHeader).toString().toLower();
    QString ext;
    if (type.contains("jpeg") || data.startsWith("\xff\xd8")) ext = "jpg";
    else if (type.contains("png") || data.startsWith("\x89PNG")) ext = "png";
    else if (type.contains("gif") || data.startsWith("GIF8")) ext = "gif";
    else if (type.contains("webp") || data.mid(8, 4) == "WEBP") ext = "webp";
    else if (type.contains("svg")) ext = "svg";
    else if (type.contains("bmp") || data.startsWith("BM")) ext = "bmp";
    else { m_status->setText("Save failed: not an image"); return; }

    QString base = r->url().path().section('/', -1);
    base = QUrl::fromPercentEncoding(base.toUtf8());
    base.replace(QRegExp("\\.[A-Za-z0-9]{2,5}$"), "");
    base.replace(QRegExp("[^A-Za-z0-9._ -]+"), "-");
    base = base.left(60).trimmed();
    if (base.isEmpty() || base == "-") base = "image";
    QDir dir(imageOutDir());
    if (!dir.exists() && !dir.mkpath(".")) { m_status->setText("Save failed: can't create " + dir.path()); return; }
    QString path = dir.filePath(base + "." + ext);
    for (int i = 2; QFile::exists(path); i++) path = dir.filePath(base + QString(" (%1).").arg(i) + ext);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size()) {
        m_status->setText("Save failed: " + f.errorString());
        return;
    }
    f.close();
    QString shown = path;
    if (shown.startsWith("/mnt/onboard/")) shown = shown.mid(13);
    m_status->setText("Saved image: " + shown);
}

// ---- Table of contents overlay (see toc.cpp)

void Browser::showToc()
{
    hideKeyboard();
    TocOverlay *t = new TocOverlay(m_view, centralWidget());
    if (m_eink) {
        connect(t, SIGNAL(beforeJump()), m_eink, SLOT(hintPageTurn()));
        // Closing it repaints the page under it with a normal partial update (no flash).
    }
}

// "Open now": the handoff line is already written; exit so KOReader (kobo-webbrowser.koplugin)
// adds the file to its collection and opens it.
void Browser::openSavedEpub(const QString &path)
{
    m_status->setText("Opening in KOReader: " + path.section('/', -1));
    QTimer::singleShot(400, qApp, SLOT(quit()));
}

void Browser::saveFinished(bool ok, const QString &msg)
{
    if (ok) {
        QString shown = msg;
        if (shown.startsWith("/mnt/onboard/")) shown = shown.mid(13);
        int count = m_saver ? m_saver->savedFiles().size() : 1;
        m_status->setText(count > 1 ? QString("Saved %1 comics").arg(count) : "Saved: " + shown);
        SavedDialog *d = new SavedDialog(msg, centralWidget(), qMax(1, count));
        connect(d, SIGNAL(openRequested(QString)), this, SLOT(openSavedEpub(QString)));
    } else {
        m_status->setText("Save failed: " + msg);
    }
}
