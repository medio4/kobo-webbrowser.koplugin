// kobo-browser: a minimal touch web browser for Kobo e-readers, built on the
// Qt 5.2.1 + QtWebKit libraries that ship inside Kobo's own firmware.
// Run with:  kobo-browser -platform kobo   (see run.sh for the environment).

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


// ----------------------------------------------------------------------- main

static volatile sig_atomic_t g_quitRequested = 0;
static void onSignal(int sig) { g_quitRequested = sig; }

// KB_KO_LIVE=1: KOReader keeps running beside us and owns sleep. When the power button or
// sleep cover is used it sends SIGTERM, so on SIGTERM we note the page in the handoff file
// first (Browser::sleepToKoreader) and the plugin reopens it on wake. The Exit button and
// SIGINT still quit without a sleep line.
class QuitPoller : public QObject {
public:
    QuitPoller() : m_browser(0), m_koLive(getenv("KB_KO_LIVE") && atoi(getenv("KB_KO_LIVE")) != 0)
    {
        startTimer(300);
    }
    void setBrowser(Browser *b) { m_browser = b; }
protected:
    void timerEvent(QTimerEvent *)
    {
        if (!g_quitRequested) return;
        if (m_koLive && m_browser && g_quitRequested == SIGTERM) {
            g_quitRequested = 0;
            Browser *b = m_browser;
            m_browser = 0;              // a second SIGTERM just quits
            b->sleepToKoreader();   // writes the sleep line, then quits
            return;
        }
        qApp->quit();
    }
private:
    Browser *m_browser;
    bool m_koLive;
};

// Heartbeat for run.sh's kb-escape watchdog: touch KB_HEARTBEAT_FILE once a second from
// the UI thread, so a hung event loop shows up as a stale file.
class Heartbeat : public QObject {
public:
    Heartbeat(const char *path, QObject *parent) : QObject(parent), m_path(path) { startTimer(1000); }
protected:
    void timerEvent(QTimerEvent *)
    {
        int fd = open(m_path.constData(), O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
        if (fd >= 0) { futimens(fd, 0); close(fd); }
    }
private:
    QByteArray m_path;
};

int main(int argc, char **argv)
{
    // Prevent QApplication from choking on nickel-style args we accept for parity.
    installCrashHandler();
    if (getenv("KB_VERBOSE"))
        QLoggingCategory::setFilterRules(QStringLiteral("*.debug=true"));
    QApplication app(argc, argv);
    // Flat, e-ink friendly look for dialog buttons and scrollbars (Qt's default style draws
    // beveled gray buttons and arrowed scrollbars). Buttons match the toolbar's FlatButton.
    app.setStyleSheet(
        "QPushButton { background: white; color: black; border: 2px solid black; border-radius: 10px;"
        "  padding: 6px 14px; }"
        "QPushButton:disabled { color: #9a9a9a; border-color: #b0b0b0; }"
        "QScrollBar:vertical { background: white; width: 10px; margin: 0; border: none; }"
        "QScrollBar:horizontal { background: white; height: 10px; margin: 0; border: none; }"
        "QScrollBar::handle:vertical { background: #333; border-radius: 4px; min-height: 60px; margin: 2px; }"
        "QScrollBar::handle:horizontal { background: #333; border-radius: 4px; min-width: 60px; margin: 2px; }"
        "QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; border: none; background: none; }"
        "QScrollBar::add-page, QScrollBar::sub-page { background: none; }");
    app.setApplicationName("kobo-browser");
    // A blinking text cursor would refresh the address bar twice a second.
    app.setCursorFlashTime(0);

    signal(SIGTERM, onSignal);
    signal(SIGINT, onSignal);
    QuitPoller quitPoller;
    const char *hbFile = getenv("KB_HEARTBEAT_FILE");
    if (hbFile && *hbFile) new Heartbeat(hbFile, &app);

    qDebug() << "SSL support:" << QSslSocket::supportsSsl() << QSslSocket::sslLibraryVersionString();

    // Use our own up-to-date CA bundle (the firmware's store may be stale/missing).
    {
        QString bundle = QDir(QCoreApplication::applicationDirPath()).filePath("ca-bundle.crt");
        const char *envBundle = getenv("KB_CA_BUNDLE");
        if (envBundle && *envBundle) bundle = QString::fromLocal8Bit(envBundle);
        QList<QSslCertificate> cas = QSslCertificate::fromPath(bundle, QSsl::Pem);
        if (!cas.isEmpty()) {
            QSslConfiguration cfg = QSslConfiguration::defaultConfiguration();
            cfg.setCaCertificates(cas);
            QSslConfiguration::setDefaultConfiguration(cfg);
        }
        qDebug() << "CA bundle:" << bundle << cas.size() << "certs";
    }

    // Panel size in portrait, for software rotation (rotation.h).
    if (QScreen *ps = app.primaryScreen()) KbRotation::init(ps->geometry().width(), ps->geometry().height());

    // Input. Default: our own evdev reader (Kobo's touch_device plugin crashes outside Nickel).
    const char *touchDev = getenv("KB_TOUCH_DEV");
    const char *keysDev  = getenv("KB_KEYS_DEV");
    if (!touchDev) touchDev = "/dev/input/event1";
    KoboTouchInput *touchInput = 0;
    if (*touchDev) {
        if (getenv("KB_TOUCH_PLUGIN")) {
            QObject *touch = QGenericPluginFactory::create("touch_device", QString::fromLatin1(touchDev));
            qDebug() << "touch_device plugin:" << (touch ? "loaded" : "FAILED") << touchDev;
        } else {
            QScreen *s0 = app.primaryScreen();
            int sw = s0 ? s0->geometry().width() : 1072;
            int sh = s0 ? s0->geometry().height() : 1448;
            bool swap = !getenv("KB_TOUCH_NOSWAP");
            bool mirX = !getenv("KB_TOUCH_NOMIRRORX");
            bool mirY = getenv("KB_TOUCH_MIRRORY") != 0;
            bool grab = !getenv("KB_TOUCH_NOGRAB");
            touchInput = new KoboTouchInput(QString::fromLatin1(touchDev), sw, sh, swap, mirX, mirY, grab, &app);
        }
    }
    if (keysDev && *keysDev) {
        QObject *keys = QGenericPluginFactory::create("keys", QString::fromLatin1(keysDev) + "::keymap=::grab=0");
        qDebug() << "keys plugin:" << (keys ? "loaded" : "FAILED") << keysDev;
    }

    QString home = QString::fromLocal8Bit(getenv("KB_HOME"));
    for (int i = 1; i < argc; ++i) {
        QString a = QString::fromLocal8Bit(argv[i]);
        if (a.startsWith("http://") || a.startsWith("https://")) { home = a; setenv("KB_START_FROM_ARG", "1", 1); }
    }
    if (home.isEmpty()) home = "https://lite.duckduckgo.com/lite/";

    KoboEink *eink = new KoboEink(&app);
    // Two-finger tap anywhere = full-screen GC16 refresh (KOReader's gestures can't reach us).
    if (touchInput) QObject::connect(touchInput, SIGNAL(twoFingerTap()), eink, SLOT(refreshAllFull()));
    // (pinch-to-zoom is connected to the Browser below, once it exists)
    app.installEventFilter(eink);
    // Frontlight (edge swipes) and power button / sleep cover / auto-sleep (power.cpp).
    KoboFrontlight *frontlight = new KoboFrontlight(&app);
    Browser w(home, eink);
    w.setTouchInput(touchInput);
    quitPoller.setBrowser(&w);
    KoboPower *power = new KoboPower(eink, &app);
    QObject::connect(power, SIGNAL(message(QString)), &w, SLOT(saveProgress(QString)));
    QObject::connect(power, SIGNAL(sleepRequested()), &w, SLOT(sleepToKoreader()));
    QObject::connect(frontlight, SIGNAL(message(QString)), &w, SLOT(saveProgress(QString)));
    if (touchInput) QObject::connect(touchInput, SIGNAL(pinch(double)), &w, SLOT(pinchZoom(double)));
    QScreen *scr = app.primaryScreen();
    if (scr) qDebug() << "screen:" << scr->geometry() << "dpi" << scr->logicalDotsPerInch();
    const char *showMode = getenv("KB_SHOWMODE");
    // Saved rotation (Menu > Rotate): lay the window out turned from the start. Only in
    // grab mode, where eink.cpp copies (and so can turn) every pixel itself.
    const int savedRotation = KbRotation::loadSaved();
    if (savedRotation && eink->canRotate()) {
        KbRotation::setAngle(savedRotation);
        qDebug() << "rotate: starting at" << savedRotation << "window" << KbRotation::logicalSize();
        w.setGeometry(QRect(QPoint(0, 0), KbRotation::logicalSize()));
        w.show();
        QTimer::singleShot(800, &w, SLOT(rotationSettled()));
    } else if (showMode && !strcmp(showMode, "show")) {
        if (scr) w.setGeometry(scr->geometry());
        w.show();
    } else if (showMode && !strcmp(showMode, "max")) {
        w.showMaximized();
    } else {
        w.showFullScreen();
    }
    qDebug() << "window geometry" << w.geometry() << "handle" << (w.windowHandle() ? w.windowHandle()->geometry() : QRect());
    // Initial full refresh once the first layout/paint is done.
    QTimer::singleShot(1500, eink, SLOT(refreshAllFull()));
    // Test hook: KB_TEST_SAVE=<seconds> triggers Save-as-EPUB automatically.
    if (getenv("KB_TEST_SAVE")) QTimer::singleShot(atoi(getenv("KB_TEST_SAVE")) * 1000, &w, SLOT(saveEpub()));
    if (getenv("KB_TEST_READ")) QTimer::singleShot(atoi(getenv("KB_TEST_READ")) * 1000, &w, SLOT(readInKoreader()));
    // Test hook: KB_TEST_RELOAD_SEC=<seconds> presses Reload automatically.
    if (getenv("KB_TEST_RELOAD_SEC")) QTimer::singleShot(atoi(getenv("KB_TEST_RELOAD_SEC")) * 1000, &w, SLOT(reloadClicked()));
    // Test hook: KB_TEST_NAV="url url back ..." loads one entry every KB_TEST_NAV_MS (12000) ms
    // ("back" = history back), logging "testnav:" lines; then quits after one more interval.
    if (getenv("KB_TEST_NAV")) {
        QStringList *steps = new QStringList(QString::fromLocal8Bit(getenv("KB_TEST_NAV")).split(' ', QString::SkipEmptyParts));
        QTimer *t = new QTimer(&w);
        QObject::connect(t, &QTimer::timeout, [steps, &w, &app]() {
            QWebView *v = w.findChild<QWebView *>();
            if (steps->isEmpty() || !v) { app.quit(); return; }
            QString s = steps->takeFirst();
            qDebug() << "testnav:" << s;
            if (s == "back") v->back(); else v->load(QUrl(s));
        });
        t->start(getenv("KB_TEST_NAV_MS") ? atoi(getenv("KB_TEST_NAV_MS")) : 12000);
    }
    return app.exec();
}
