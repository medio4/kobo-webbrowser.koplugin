// kobo-browser: a minimal touch web browser for Kobo e-readers, built on the
// Qt 5.2.1 + QtWebKit libraries that ship inside Kobo's own firmware.
// Run with:  kobo-browser -platform kobo   (see run.sh for the environment).

#include "browser.h"
#include "crashhandler.h"
#include "touchinput.h"
#include "eink.h"
#include "webpage.h"
#include "pagescroll.h"

#include <QtWidgets/QApplication>
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
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtCore/QSettings>
#include <QtWebKit/QWebSettings>
#include <QtWebKitWidgets/QWebFrame>
#include <QtWebKitWidgets/QWebPage>

#include <cstdlib>
#include <cstring>
#include <signal.h>

// ---------------------------------------------------------------- TouchWebView

TouchWebView::TouchWebView(QWidget *parent)
    : QWebView(parent), m_pressed(false), m_dragging(false), m_forwarding(false)
{
    m_pager = new PageScroller(this);
}

bool TouchWebView::pageMode() const { return m_pager->enabled(); }
void TouchWebView::setPageMode(bool on) { m_pager->setEnabled(on); }

bool TouchWebView::event(QEvent *e)
{
    // Qt synthesizes mouse events from touch; we only use the mouse events.
    if (e->type() == QEvent::TouchBegin || e->type() == QEvent::TouchUpdate ||
        e->type() == QEvent::TouchEnd) {
        e->ignore();
        return false;
    }
    return QWebView::event(e);
}

void TouchWebView::mousePressEvent(QMouseEvent *e)
{
    if (m_forwarding) { QWebView::mousePressEvent(e); return; }
    m_pressed = true;
    m_dragging = false;
    m_pressPos = e->pos();
    m_lastPos = e->pos();
    m_pressTime.start();
    m_pager->handlePress(e->pos());
    e->accept();
}

void TouchWebView::mouseMoveEvent(QMouseEvent *e)
{
    if (m_forwarding) { QWebView::mouseMoveEvent(e); return; }
    if (!m_pressed) return;
    if (m_pager->handleMove(e->pos())) { e->accept(); return; }   // page mode: no live drag
    QPoint delta = e->pos() - m_lastPos;
    if (!m_dragging && (e->pos() - m_pressPos).manhattanLength() > 25)
        m_dragging = true;
    if (m_dragging) {
        // Scroll in big steps only; e-ink can't follow continuous scrolling well.
        if (qAbs(delta.y()) >= 40 || qAbs(delta.x()) >= 40) {
            page()->mainFrame()->scroll(-delta.x(), -delta.y());
            m_lastPos = e->pos();
        }
    }
    e->accept();
}

void TouchWebView::mouseReleaseEvent(QMouseEvent *e)
{
    if (m_forwarding) { QWebView::mouseReleaseEvent(e); return; }
    if (!m_pressed) return;
    m_pressed = false;
    if (m_pager->handleRelease(e->pos())) { m_dragging = false; e->accept(); return; }  // page turned
    if (m_dragging) {
        // Finish a swipe: scroll by remaining delta.
        QPoint delta = e->pos() - m_lastPos;
        if (!delta.isNull())
            page()->mainFrame()->scroll(-delta.x(), -delta.y());
        m_dragging = false;
        e->accept();
        return;
    }
    // Short tap: replay as a real click at that spot.
    m_forwarding = true;
    QMouseEvent press(QEvent::MouseButtonPress, m_pressPos, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QWebView::mousePressEvent(&press);
    QMouseEvent release(QEvent::MouseButtonRelease, m_pressPos, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QWebView::mouseReleaseEvent(&release);
    m_forwarding = false;
    e->accept();
}

// ------------------------------------------------------------ OnScreenKeyboard

static const char *kRowsLower[] = { "qwertyuiop", "asdfghjkl", "zxcvbnm", 0 };
static const char *kRowsSym[]   = { "1234567890", "-/:;()$&@", ".,?!'\"", 0 };

OnScreenKeyboard::OnScreenKeyboard(QWidget *parent)
    : QWidget(parent), m_shift(false), m_symbols(false)
{
    setLayout(new QVBoxLayout);
    layout()->setContentsMargins(4, 4, 4, 4);
    layout()->setSpacing(4);
    rebuild();
}

static QPushButton *makeKey(const QString &label, int minW, int h)
{
    QPushButton *b = new QPushButton(label);
    b->setMinimumSize(minW, h);
    b->setFocusPolicy(Qt::NoFocus);
    QFont f = b->font(); f.setPixelSize(40); b->setFont(f);
    return b;
}

void OnScreenKeyboard::rebuild()
{
    QLayout *l = layout();
    while (QLayoutItem *item = l->takeAt(0)) {
        if (item->widget()) item->widget()->deleteLater();
        if (item->layout()) {
            while (QLayoutItem *sub = item->layout()->takeAt(0)) {
                if (sub->widget()) sub->widget()->deleteLater();
                delete sub;
            }
            delete item->layout();
        } else {
            delete item;
        }
    }
    const int keyH = 80;
    const int keyW = 90;
    const char **rows = m_symbols ? kRowsSym : kRowsLower;
    for (int r = 0; rows[r]; ++r) {
        QHBoxLayout *row = new QHBoxLayout;
        row->setSpacing(4);
        if (r == 1 && !m_symbols) row->addSpacing(keyW / 2);
        if (r == 2) {
            QPushButton *shift = makeKey(m_symbols ? "123" : (m_shift ? "ABC" : "abc"), keyW + 30, keyH);
            connect(shift, SIGNAL(clicked()), this, m_symbols ? SLOT(toggleSymbols()) : SLOT(toggleShift()));
            row->addWidget(shift);
        }
        for (const char *c = rows[r]; *c; ++c) {
            QString s = QString(QChar(*c));
            if (m_shift && !m_symbols) s = s.toUpper();
            QPushButton *b = makeKey(s, keyW, keyH);
            b->setProperty("keytext", s);
            connect(b, SIGNAL(clicked()), this, SLOT(keyClicked()));
            row->addWidget(b);
        }
        if (r == 2) {
            QPushButton *bs = makeKey("<-", keyW + 30, keyH);
            bs->setProperty("keycode", (int)Qt::Key_Backspace);
            connect(bs, SIGNAL(clicked()), this, SLOT(keyClicked()));
            row->addWidget(bs);
        }
        static_cast<QVBoxLayout *>(l)->addLayout(row);
    }
    QHBoxLayout *bottom = new QHBoxLayout;
    bottom->setSpacing(4);
    QPushButton *sym = makeKey(m_symbols ? "abc" : "123", keyW + 30, keyH);
    connect(sym, SIGNAL(clicked()), this, SLOT(toggleSymbols()));
    bottom->addWidget(sym);
    QPushButton *dot = makeKey(".", keyW, keyH);
    dot->setProperty("keytext", "."); connect(dot, SIGNAL(clicked()), this, SLOT(keyClicked()));
    bottom->addWidget(dot);
    QPushButton *space = makeKey("space", keyW * 4, keyH);
    space->setProperty("keytext", " "); connect(space, SIGNAL(clicked()), this, SLOT(keyClicked()));
    bottom->addWidget(space);
    QPushButton *slash = makeKey("/", keyW, keyH);
    slash->setProperty("keytext", "/"); connect(slash, SIGNAL(clicked()), this, SLOT(keyClicked()));
    bottom->addWidget(slash);
    QPushButton *hide = makeKey("Hide", keyW + 30, keyH);
    connect(hide, SIGNAL(clicked()), this, SIGNAL(hidePressed()));
    bottom->addWidget(hide);
    QPushButton *go = makeKey("Go", keyW + 30, keyH);
    connect(go, SIGNAL(clicked()), this, SIGNAL(goPressed()));
    bottom->addWidget(go);
    static_cast<QVBoxLayout *>(l)->addLayout(bottom);
}

void OnScreenKeyboard::keyClicked()
{
    QWidget *target = QApplication::focusWidget();
    if (!target) return;
    QObject *s = sender();
    QString text = s->property("keytext").toString();
    int code = s->property("keycode").toInt();
    if (code == 0 && !text.isEmpty())
        code = text.at(0).unicode();
    QKeyEvent press(QEvent::KeyPress, code, Qt::NoModifier, text);
    QApplication::sendEvent(target, &press);
    QKeyEvent release(QEvent::KeyRelease, code, Qt::NoModifier, text);
    QApplication::sendEvent(target, &release);
    if (m_shift) { m_shift = false; rebuild(); }
}

void OnScreenKeyboard::toggleShift()  { m_shift = !m_shift; rebuild(); }
void OnScreenKeyboard::toggleSymbols() { m_symbols = !m_symbols; rebuild(); }

// -------------------------------------------------------------------- Browser

Browser::Browser(const QString &home, KoboEink *eink, QWidget *parent)
    : QMainWindow(parent), m_home(home), m_eink(eink)
{
    setWindowTitle("Browser");

    QWidget *central = new QWidget;
    QVBoxLayout *vbox = new QVBoxLayout(central);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(0);

    // Toolbar: two rows (URL field on top, buttons below), sized in pixels for 300 dpi.
    QWidget *bar = new QWidget;
    QVBoxLayout *barBox = new QVBoxLayout(bar);
    barBox->setContentsMargins(4, 4, 4, 0);
    barBox->setSpacing(4);
    const int bh = 84;
    QFont bf; bf.setPixelSize(40);

    m_url = new QLineEdit; m_url->setMinimumHeight(bh); m_url->setFont(bf);
    m_url->setPlaceholderText("Search or type a web address");
    QPushButton *go = new QPushButton("Go"); go->setMinimumSize(130, bh); go->setFont(bf);
    QHBoxLayout *row1 = new QHBoxLayout; row1->setSpacing(4);
    row1->addWidget(m_url, 1); row1->addWidget(go);
    barBox->addLayout(row1);

    // Nine buttons + gaps must fit in 1064 px: widths below add up to 1030 + 8 gaps of 4.
    QPushButton *back = new QPushButton("<");   back->setMinimumSize(100, bh); back->setFont(bf);
    QPushButton *fwd  = new QPushButton(">");   fwd->setMinimumSize(100, bh);  fwd->setFont(bf);
    QPushButton *rel  = new QPushButton("Reload"); rel->setMinimumSize(150, bh); rel->setFont(bf);
    m_modeBtn = new QPushButton("Page"); m_modeBtn->setMinimumSize(120, bh); m_modeBtn->setFont(bf);
    m_modeBtn->setToolTip("Scroll mode: Page (swipe/edge tap turns a screen) or Live (drag)");
    QPushButton *kbd  = new QPushButton("Keys"); kbd->setMinimumSize(130, bh); kbd->setFont(bf);
    QPushButton *zi   = new QPushButton("A+");  zi->setMinimumSize(100, bh);   zi->setFont(bf);
    QPushButton *zo   = new QPushButton("A-");  zo->setMinimumSize(100, bh);   zo->setFont(bf);
    QPushButton *shot = new QPushButton("Shot"); shot->setMinimumSize(110, bh); shot->setFont(bf);
    QPushButton *quit = new QPushButton("Exit"); quit->setMinimumSize(120, bh); quit->setFont(bf);
    foreach (QPushButton *b, QList<QPushButton *>() << go << back << fwd << rel << m_modeBtn << kbd << zi << zo << shot << quit)
        b->setFocusPolicy(Qt::NoFocus);
    QHBoxLayout *row2 = new QHBoxLayout; row2->setSpacing(4);
    row2->addWidget(back); row2->addWidget(fwd); row2->addWidget(rel); row2->addWidget(m_modeBtn);
    row2->addStretch(1);
    row2->addWidget(kbd); row2->addWidget(zi); row2->addWidget(zo); row2->addWidget(shot); row2->addWidget(quit);
    barBox->addLayout(row2);
    vbox->addWidget(bar);

    m_status = new QLabel;
    QFont sf; sf.setPixelSize(30);
    m_status->setFont(sf);
    m_status->setContentsMargins(6, 0, 6, 0);
    vbox->addWidget(m_status);

    m_view = new TouchWebView;
    m_view->setPage(new KoboWebPage(m_view));
    vbox->addWidget(m_view, 1);

    m_kbd = new OnScreenKeyboard;
    m_kbd->hide();
    vbox->addWidget(m_kbd);

    setCentralWidget(central);
    m_kbd->hide();
    m_view->setFocus();

    QWebSettings *ws = m_view->settings();
    ws->setAttribute(QWebSettings::JavascriptEnabled, true);
    ws->setAttribute(QWebSettings::PluginsEnabled, false);
    ws->setAttribute(QWebSettings::AutoLoadImages, true);
    ws->setAttribute(QWebSettings::ZoomTextOnly, false);
    ws->setFontSize(QWebSettings::DefaultFontSize, 20);
    ws->setFontSize(QWebSettings::MinimumFontSize, 16);
    m_view->setZoomFactor(2.0);
    m_view->page()->setLinkDelegationPolicy(QWebPage::DontDelegateLinks);

    // Disk cache + cookies in our own folder (user partition).
    QNetworkDiskCache *cache = new QNetworkDiskCache(this);
    QString cacheDir = QDir::homePath() + "/.kobo-browser-cache";
    if (QDir("/mnt/onboard/.adds/kobo-browser").exists())
        cacheDir = "/mnt/onboard/.adds/kobo-browser/cache";
    cache->setCacheDirectory(cacheDir);
    m_view->page()->networkAccessManager()->setCache(cache);

    connect(go, SIGNAL(clicked()), this, SLOT(go()));
    connect(back, SIGNAL(clicked()), m_view, SLOT(back()));
    connect(fwd, SIGNAL(clicked()), m_view, SLOT(forward()));
    connect(rel, SIGNAL(clicked()), m_view, SLOT(reload()));
    connect(kbd, SIGNAL(clicked()), this, SLOT(toggleKeyboard()));
    connect(zi, SIGNAL(clicked()), this, SLOT(zoomIn()));
    connect(zo, SIGNAL(clicked()), this, SLOT(zoomOut()));
    connect(quit, SIGNAL(clicked()), this, SLOT(quit()));
    connect(shot, SIGNAL(clicked()), this, SLOT(screenshot()));
    connect(m_url, SIGNAL(returnPressed()), this, SLOT(go()));
    connect(m_kbd, SIGNAL(goPressed()), this, SLOT(go()));
    connect(m_kbd, SIGNAL(hidePressed()), this, SLOT(hideKeyboard()));
    connect(m_view, SIGNAL(urlChanged(QUrl)), this, SLOT(urlChanged(QUrl)));
    connect(m_view, SIGNAL(loadStarted()), this, SLOT(loadStarted()));
    connect(m_view, SIGNAL(loadProgress(int)), this, SLOT(loadProgress(int)));
    connect(m_view, SIGNAL(loadFinished(bool)), this, SLOT(loadFinished(bool)));
    connect(m_modeBtn, SIGNAL(clicked()), this, SLOT(toggleScrollMode()));
    if (m_eink)
        connect(m_view->pager(), SIGNAL(pageTurned()), m_eink, SLOT(hintPageTurn()));

    // Scroll mode is remembered between runs (settings.ini next to the binary).
    {
        QSettings st(QDir(QCoreApplication::applicationDirPath()).filePath("settings.ini"), QSettings::IniFormat);
        applyScrollMode(st.value("scroll/pageMode", true).toBool(), false);
    }

    // Tapping the URL field shows the keyboard.
    m_url->installEventFilter(this);

    loadUrlText(m_home);
}

bool Browser::eventFilter(QObject *obj, QEvent *e)
{
    if (obj == m_url && e->type() == QEvent::MouseButtonRelease)
        QTimer::singleShot(0, this, SLOT(showKeyboard()));
    return QMainWindow::eventFilter(obj, e);
}

void Browser::focusUrl()
{
    m_url->setFocus();
    m_url->selectAll();
    showKeyboard();
}

void Browser::go()
{
    loadUrlText(m_url->text());
    hideKeyboard();
    m_view->setFocus();
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
    } else {
        url = QUrl("https://lite.duckduckgo.com/lite/?q=" + QString::fromLatin1(QUrl::toPercentEncoding(text)));
    }
    m_view->load(KoboWebPage::mobileVariant(url));
}

void Browser::showKeyboard()   { m_kbd->show(); }
void Browser::hideKeyboard()   { m_kbd->hide(); }
void Browser::toggleKeyboard()
{
    if (m_kbd->isVisible()) hideKeyboard();
    else { if (!QApplication::focusWidget()) m_url->setFocus(); showKeyboard(); }
}

void Browser::zoomIn()  { m_view->setZoomFactor(m_view->zoomFactor() * 1.2); }
void Browser::zoomOut() { m_view->setZoomFactor(m_view->zoomFactor() / 1.2); }

void Browser::urlChanged(const QUrl &url) { m_url->setText(url.toString()); }
void Browser::loadProgress(int p)  { if (p % 25 == 0) m_status->setText(QString("Loading %1%").arg(p)); }

// E-ink friendly loading: while a page loads, WebKit repaints progressively. We let
// Qt paint into the backing store but hold the screen updates of the web view, then
// show the finished page with one clean full refresh (KoboEink hold, see eink.h).
void Browser::loadStarted()
{
    m_status->setText("Loading...");
    if (m_eink && !getenv("KB_NO_LOAD_HOLD")) m_eink->beginHold(m_view);
}
void Browser::loadFinished(bool ok)
{
    m_status->setText(ok ? m_view->title() : "Failed to load page");
    // WebKit usually does a last layout/paint shortly after loadFinished; give it a moment.
    QTimer::singleShot(250, this, SLOT(releaseLoadHold()));
}
void Browser::releaseLoadHold()
{
    if (m_eink) m_eink->releaseHold(true);
}

void Browser::toggleScrollMode() { applyScrollMode(!m_view->pageMode(), true); }
void Browser::applyScrollMode(bool pageMode, bool save)
{
    m_view->setPageMode(pageMode);
    m_modeBtn->setText(pageMode ? "Page" : "Live");   // shows the mode that is active
    if (save) {
        QSettings st(QDir(QCoreApplication::applicationDirPath()).filePath("settings.ini"), QSettings::IniFormat);
        st.setValue("scroll/pageMode", pageMode);
    }
}

void Browser::quit() { qApp->quit(); }

void Browser::screenshot()
{
    QString dir = "/mnt/onboard/screenshots";
    if (!QDir("/mnt/onboard").exists()) dir = QDir::homePath() + "/screenshots";
    QString path = m_eink ? m_eink->saveScreenshot(dir) : QString();
    m_status->setText(path.isEmpty() ? QString("Screenshot failed") : QString("Saved " + path.mid(path.indexOf("screenshots"))));
}

// ----------------------------------------------------------------------- main

static volatile sig_atomic_t g_quitRequested = 0;
static void onSignal(int) { g_quitRequested = 1; }

class QuitPoller : public QObject {
public:
    QuitPoller() { startTimer(300); }
protected:
    void timerEvent(QTimerEvent *) { if (g_quitRequested) qApp->quit(); }
};

int main(int argc, char **argv)
{
    // Prevent QApplication from choking on nickel-style args we accept for parity.
    installCrashHandler();
    if (getenv("KB_VERBOSE"))
        QLoggingCategory::setFilterRules(QStringLiteral("*.debug=true"));
    QApplication app(argc, argv);
    app.setApplicationName("kobo-browser");

    signal(SIGTERM, onSignal);
    signal(SIGINT, onSignal);
    QuitPoller quitPoller;

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
        if (a.startsWith("http://") || a.startsWith("https://")) home = a;
    }
    if (home.isEmpty()) home = "https://lite.duckduckgo.com/lite/";

    KoboEink *eink = new KoboEink(&app);
    app.installEventFilter(eink);
    Browser w(home, eink);
    QScreen *scr = app.primaryScreen();
    if (scr) qDebug() << "screen:" << scr->geometry() << "dpi" << scr->logicalDotsPerInch();
    const char *showMode = getenv("KB_SHOWMODE");
    if (showMode && !strcmp(showMode, "show")) {
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
    return app.exec();
}
