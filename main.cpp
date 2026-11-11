// kobo-browser: a minimal touch web browser for Kobo e-readers, built on the
// Qt 5.2.1 + QtWebKit libraries that ship inside Kobo's own firmware.
// Run with:  kobo-browser -platform kobo   (see run.sh for the environment).

#include "browser.h"
#include "crashhandler.h"
#include "touchinput.h"
#include "eink.h"
#include "webpage.h"
#include "pagescroll.h"
#include "epub.h"
#include "phoneshare.h"
#include "toc.h"
#include "memtune.h"
#include "power.h"
#include "textentry.h"

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

// ---------------------------------------------------------------- TouchWebView

TouchWebView::TouchWebView(QWidget *parent)
    : QWebView(parent), m_pressed(false), m_dragging(false), m_forwarding(false),
      m_horizontal(false), m_live(false), m_light(NoLight), m_lightStart(0)
{
    const char *le = getenv("KB_LIGHT_EDGE");
    m_lightEdge = (le && *le) ? atoi(le) : 48;
    m_pager = new PageScroller(this);
    // Edge swipes: in from the left = back one page in history, in from the right = forward.
    connect(m_pager, SIGNAL(goBack()), this, SLOT(back()));
    connect(m_pager, SIGNAL(goForward()), this, SLOT(forward()));
}

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
    m_horizontal = false;
    m_live = false;
    m_light = NoLight;
    m_pressPos = e->pos();
    m_lastPos = e->pos();
    m_pressTime.start();
    e->accept();
}

// One gesture model, no modes: a drag locks to an axis once it starts. Vertical: if
// the finger is still down after KB_FLICK_MS it becomes live scrolling (the page
// follows the finger in big steps); released before that it was a flick and turns one
// page. Horizontal: a swipe (page turn, or history back/forward from the edges).
void TouchWebView::mouseMoveEvent(QMouseEvent *e)
{
    if (m_forwarding) { QWebView::mouseMoveEvent(e); return; }
    if (!m_pressed) return;
    if (KoboTouchInput::gestureActive()) { m_pressed = false; m_dragging = false; e->accept(); return; }
    QPoint fromPress = e->pos() - m_pressPos;
    if (!m_dragging && fromPress.manhattanLength() > 25) {
        m_dragging = true;
        m_horizontal = qAbs(fromPress.x()) > qAbs(fromPress.y());
        // Vertical drag from the very left edge = frontlight, right edge = warmth (KOReader).
        KoboFrontlight *fl = KoboFrontlight::instance();
        if (!m_horizontal && fl && fl->available() && m_lightEdge > 0) {
            if (m_pressPos.x() < m_lightEdge) { m_light = LightBrightness; m_lightStart = fl->brightness(); }
            else if (m_pressPos.x() >= width() - m_lightEdge && fl->hasWarmth()) { m_light = LightWarmth; m_lightStart = fl->warmth(); }
        }
    }
    if (m_light != NoLight) {
        // Up = brighter / warmer. Three quarters of the page height covers the full range.
        KoboFrontlight *fl = KoboFrontlight::instance();
        int v = m_lightStart + (-fromPress.y()) * 100 / qMax(1, height() * 3 / 4);
        if (m_light == LightBrightness) fl->setBrightness(v); else fl->setWarmth(v);
        e->accept();
        return;
    }
    if (m_dragging) {
        // Same rule on both axes: held past the flick time, the drag follows the finger
        // (vertical scroll, or horizontal panning for wide tables / zoomed-in pages).
        if (!m_live && m_pressTime.elapsed() >= m_pager->flickMs()) {
            m_live = true;          // sustained drag: catch up with the finger, then follow it
            if (m_horizontal) {
                // Pan the wide thing under the finger (table, image box, inner frame),
                // or the page when nothing there scrolls sideways.
                m_hTarget = m_pager->findHScrollable(m_pressPos);
                m_pager->panHorizontal(m_hTarget, -fromPress.x());
            } else {
                page()->mainFrame()->scroll(0, -fromPress.y());
            }
            m_lastPos = e->pos();
        } else if (m_live) {
            // Scroll in big steps only; e-ink can't follow continuous scrolling well.
            QPoint delta = e->pos() - m_lastPos;
            if (m_horizontal ? qAbs(delta.x()) >= 40 : qAbs(delta.y()) >= 40) {
                if (m_horizontal) m_pager->panHorizontal(m_hTarget, -delta.x());
                else page()->mainFrame()->scroll(0, -delta.y());
                m_lastPos = e->pos();
            }
        }
    }
    e->accept();
}

void TouchWebView::mouseReleaseEvent(QMouseEvent *e)
{
    if (m_forwarding) { QWebView::mouseReleaseEvent(e); return; }
    if (!m_pressed) return;
    m_pressed = false;
    if (KoboTouchInput::gestureActive() || e->pos().x() < 0) { m_dragging = false; m_light = NoLight; e->accept(); return; }
    if (m_light != NoLight) {
        KoboFrontlight::instance()->announce(m_light == LightWarmth);
        m_light = NoLight;
        m_dragging = false;
        e->accept();
        return;
    }
    if (m_dragging) {
        m_dragging = false;
        QPoint d = e->pos() - m_pressPos;
        if (m_live) {
            // Finish the live drag/pan: scroll by the remaining delta.
            QPoint delta = e->pos() - m_lastPos;
            if (m_horizontal) { m_pager->panHorizontal(m_hTarget, -delta.x()); m_hTarget = PageScroller::HTarget(); }
            else if (delta.y() != 0) page()->mainFrame()->scroll(0, -delta.y());
        } else if (m_horizontal) {
            // Quick horizontal flick: page turn, or history from the edges.
            m_pager->handleHorizontalSwipe(m_pressPos, e->pos());
        } else if (qAbs(d.y()) >= m_pager->swipeMin()) {
            // Flick: released quickly after a decent move. Up = next page, down = previous.
            if (d.y() < 0) m_pager->nextPage(); else m_pager->prevPage();
        }
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

// ---------------------------------------------------------------- FlatButton

FlatButton::FlatButton(const QString &text, QWidget *parent)
    : QAbstractButton(parent), m_dark(false), m_flash(getenv("KB_KEY_FLASH") != 0)
{
    setText(text);
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setPixelSize(40);
}

void FlatButton::setBold(bool b) { QFont f = font(); f.setBold(b); setFont(f); }
void FlatButton::setPixelSize(int px) { QFont f = font(); f.setPixelSize(px); setFont(f); }

void FlatButton::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    QRect r = rect().adjusted(1, 1, -2, -2);
    const bool inv = m_flash && isDown();
    QColor face = inv ? Qt::black : (m_dark ? QColor(0xd8, 0xd8, 0xd8) : Qt::white);
    QColor ink = inv ? Qt::white : Qt::black;
    if (!isEnabled()) ink = QColor(0x90, 0x90, 0x90);
    p.fillRect(rect(), Qt::white);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(Qt::black, 2));
    p.setBrush(face);
    p.drawRoundedRect(r, 8, 8);
    p.setPen(ink);
    p.setFont(font());
    p.drawText(r, Qt::AlignCenter, text());
}

// ------------------------------------------------------------ OnScreenKeyboard

// Layers: lower / upper (shift) / symbols. Each string is one row, 10 keys.
static const char *kLower[] = { "1234567890", "qwertyuiop", "asdfghjkl",  "zxcvbnm",  0 };
static const char *kUpper[] = { "1234567890", "QWERTYUIOP", "ASDFGHJKL",  "ZXCVBNM",  0 };
static const char *kSym[]   = { "!@#$%^&*()", "-_=+[]{}\\|", ";:'\",<>?",  "~`/.",     0 };

OnScreenKeyboard::OnScreenKeyboard(QWidget *parent)
    : QWidget(parent), m_shift(0), m_sym(0), m_shift_on(false), m_symbols(false), m_rows(0)
{
    setProperty("kb_fast", true);          // refreshed with the fast DU waveform
    // (no WA_OpaquePaintEvent here: it would stop Qt from filling the white background)
    setAutoFillBackground(true);
    QPalette pal = palette(); pal.setColor(QPalette::Window, Qt::white); setPalette(pal);
    build();
}

// Key size: taller keys are easier to hit; 5 rows of 108 px take ~40% of the screen.
static const int kKeyH = 108;
static const int kKeyGap = 6;

static FlatButton *key(const QString &label, int w, int h, bool dark = false, int px = 50)
{
    FlatButton *b = new FlatButton(label);
    b->setFixedSize(w, h);
    b->setDark(dark);
    b->setPixelSize(px);
    return b;
}

void OnScreenKeyboard::build()
{
    const int gap = kKeyGap;
    const int keyH = kKeyH;
    const int keyW = (1072 - 11 * gap) / 10;   // 10 keys per row at 1072 px wide
    QVBoxLayout *v = new QVBoxLayout(this);
    v->setContentsMargins(gap, gap, gap, gap);
    v->setSpacing(gap);

    // Row 0..3: characters (row 2 is indented half a key like KOReader; row 3 has
    // shift on the left and backspace on the right).
    for (int r = 0; kLower[r]; ++r) {
        QHBoxLayout *row = new QHBoxLayout; row->setSpacing(gap);
        if (r == 2) row->addSpacing(keyW / 2);
        if (r == 3) {
            m_shift = key("Shift", keyW + keyW / 2 + gap / 2, keyH, true, 38);
            connect(m_shift, SIGNAL(clicked()), this, SLOT(toggleShift()));
            row->addWidget(m_shift);
        }
        for (const char *c = kLower[r]; *c; ++c) {
            FlatButton *b = key(QString(QChar(*c)), keyW, keyH);
            connect(b, SIGNAL(clicked()), this, SLOT(keyClicked()));
            m_charKeys.append(b);
            row->addWidget(b);
        }
        if (r == 2) row->addSpacing(keyW / 2);
        if (r == 3) {
            FlatButton *bs = key("Del", keyW + keyW / 2 + gap / 2, keyH, true, 40);   // backspace ("<" renders as a clock glyph in the Kobo UI font; no arrow glyphs)
            bs->setProperty("keycode", (int)Qt::Key_Backspace);
            bs->setAutoRepeat(true); bs->setAutoRepeatDelay(500); bs->setAutoRepeatInterval(120);
            connect(bs, SIGNAL(clicked()), this, SLOT(keyClicked()));
            row->addWidget(bs);
        }
        row->addStretch(1);
        v->addLayout(row);
        ++m_rows;
    }
    // Bottom row: symbols, URL helpers, space, Enter/Go, hide.
    QHBoxLayout *bottom = new QHBoxLayout; bottom->setSpacing(gap);
    m_sym = key("?123", keyW + keyW / 2 + gap / 2, keyH, true, 38);
    connect(m_sym, SIGNAL(clicked()), this, SLOT(toggleSymbols()));
    bottom->addWidget(m_sym);
    struct { const char *label, *text; int w; } fixed[] = {
        { ".com", ".com", keyW + keyW / 2 },
        { ".", ".", keyW },
        { "/", "/", keyW },
    };
    for (unsigned i = 0; i < sizeof fixed / sizeof fixed[0]; ++i) {
        FlatButton *b = key(fixed[i].label, fixed[i].w, keyH, false, 40);
        b->setProperty("keytext", QString::fromLatin1(fixed[i].text));
        connect(b, SIGNAL(clicked()), this, SLOT(keyClicked()));
        bottom->addWidget(b);
    }
    FlatButton *space = key("", keyW * 2 + gap, keyH);
    space->setProperty("keytext", " ");
    connect(space, SIGNAL(clicked()), this, SLOT(keyClicked()));
    bottom->addWidget(space);
    FlatButton *hide = key("Hide", keyW + keyW / 2, keyH, true, 40);
    connect(hide, SIGNAL(clicked()), this, SIGNAL(hidePressed()));
    bottom->addWidget(hide);
    FlatButton *go = key("Go", keyW + keyW / 2 + gap / 2, keyH, true, 40);
    go->setBold(true);
    connect(go, SIGNAL(clicked()), this, SIGNAL(goPressed()));
    bottom->addWidget(go);
    bottom->addStretch(1);
    v->addLayout(bottom);
    ++m_rows;
    setFixedHeight(preferredHeight());
}

int OnScreenKeyboard::preferredHeight() const { return m_rows * (kKeyH + kKeyGap) + kKeyGap + 2; }

void OnScreenKeyboard::relabel()
{
    const char **rows = m_symbols ? kSym : (m_shift_on ? kUpper : kLower);
    int i = 0;
    for (int r = 0; rows[r] && kLower[r]; ++r)
        for (const char *c = kLower[r]; *c; ++c, ++i) {
            // Symbol rows can be shorter than the letter rows; blank the extra keys.
            int pos = c - kLower[r];
            QString s = (int)strlen(rows[r]) > pos ? QString(QChar(rows[r][pos])) : QString();
            if (i < m_charKeys.size() && m_charKeys[i]->text() != s) {
                m_charKeys[i]->setText(s);
                m_charKeys[i]->setEnabled(!s.isEmpty());
                m_charKeys[i]->update();
            }
        }
    QString shiftLabel = m_shift_on ? "SHIFT" : "Shift";
    if (m_shift->text() != shiftLabel) { m_shift->setText(shiftLabel); m_shift->update(); }
    QString symLabel = m_symbols ? "abc" : "?123";
    if (m_sym->text() != symLabel) { m_sym->setText(symLabel); m_sym->update(); }
    m_shift->setEnabled(!m_symbols);
}

void OnScreenKeyboard::sendKey(int code, const QString &text)
{
    QWidget *target = QApplication::focusWidget();
    if (!target) return;
    QKeyEvent press(QEvent::KeyPress, code, Qt::NoModifier, text);
    QApplication::sendEvent(target, &press);
    QKeyEvent release(QEvent::KeyRelease, code, Qt::NoModifier, text);
    QApplication::sendEvent(target, &release);
}

void OnScreenKeyboard::keyClicked()
{
    QObject *s = sender();
    QString text = s->property("keytext").toString();
    int code = s->property("keycode").toInt();
    if (text.isEmpty() && code == 0) text = static_cast<QAbstractButton *>(s)->text();
    if (text.isEmpty() && code == 0) return;
    if (code == 0) {
        QChar ch = text.at(0);
        code = ch.isLetter() ? ch.toUpper().unicode() : ch.unicode();
    }
    sendKey(code, text);
    if (code == Qt::Key_Backspace) emit backspacePressed();
    if (m_shift_on && !m_symbols) { m_shift_on = false; relabel(); }
}

void OnScreenKeyboard::toggleShift()   { m_shift_on = !m_shift_on; relabel(); }
void OnScreenKeyboard::toggleSymbols() { m_symbols = !m_symbols; relabel(); }

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
    : QMainWindow(parent), m_home(home), m_eink(eink), m_saver(0), m_phone(0), m_restoreY(-1), m_relaunch(false), m_offline(false), m_offlineShown(false), m_onlineTimer(0)
{
    setWindowTitle("Browser");

    m_central = new QWidget;
    QVBoxLayout *vbox = new QVBoxLayout(m_central);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(0);

    // Toolbar: two rows (URL field on top, buttons below), sized in pixels for 300 dpi.
    // Marked kb_fast so its repaints use the quick DU waveform while typing.
    m_bar = new QWidget;
    m_bar->setProperty("kb_fast", true);
    m_bar->setAutoFillBackground(true);
    QPalette pal = m_bar->palette(); pal.setColor(QPalette::Window, Qt::white); m_bar->setPalette(pal);
    QVBoxLayout *barBox = new QVBoxLayout(m_bar);
    barBox->setContentsMargins(6, 6, 6, 6);
    barBox->setSpacing(6);
    const int bh = 84;
    QFont bf; bf.setPixelSize(38);

    m_url = new QLineEdit; m_url->setFixedHeight(bh); m_url->setFont(bf);
    m_url->setPlaceholderText("Search or type a web address");
    m_url->setStyleSheet("QLineEdit { border: 2px solid black; border-radius: 8px; background: white; color: black;"
                         " padding: 0 12px; selection-background-color: black; selection-color: white; }");
    FlatButton *clear = toolButton("Clear", 130, bh);
    FlatButton *go = toolButton("Go", 130, bh); go->setBold(true);
    QHBoxLayout *row1 = new QHBoxLayout; row1->setSpacing(6);
    row1->addWidget(m_url, 1); row1->addWidget(clear); row1->addWidget(go);
    barBox->addLayout(row1);

    // Nine buttons + 8 gaps of 6 px must fit in 1064 px: widths add up to 900 (164 spare).
    FlatButton *back = toolButton("Back", 100, bh);   // NB: "<" renders as a clock in the Kobo UI font
    FlatButton *fwd  = toolButton("Fwd", 90, bh);
    FlatButton *rel  = toolButton("Reload", 140, bh);
    FlatButton *toc  = toolButton("TOC", 100, bh);
    toc->setToolTip("Jump to a heading on this page");
    FlatButton *kbd  = toolButton("Keys", 110, bh);
    FlatButton *zi   = toolButton("A+", 90, bh);
    FlatButton *zo   = toolButton("A-", 90, bh);
    FlatButton *save = toolButton("Save", 110, bh);
    save->setToolTip("Save this page as an EPUB in the library, or an image to Pictures");
    FlatButton *shot = toolButton("Shot", 100, bh);
    FlatButton *quit = toolButton("Exit", 80, bh);
    QHBoxLayout *row2 = new QHBoxLayout; row2->setSpacing(6);
    row2->addWidget(back); row2->addWidget(fwd); row2->addWidget(rel); row2->addWidget(toc);
    row2->addStretch(1);
    row2->addWidget(kbd); row2->addWidget(zi); row2->addWidget(zo); row2->addWidget(save); row2->addWidget(shot); row2->addWidget(quit);
    barBox->addLayout(row2);
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
    // "Send to Kobo" page for a phone on the same Wi-Fi (KB_PHONE_PORT, 0 = off).
    {
        bool okp = false;
        int port = QString::fromLocal8Bit(qgetenv("KB_PHONE_PORT")).toInt(&okp);
        if (!okp) port = 8080;
        if (port > 0) {
            m_phone = new PhoneShare(this);
            if (m_phone->listen(port)) {
                connect(m_phone, SIGNAL(urlReceived(QString)), this, SLOT(phoneUrl(QString)));
                QTimer::singleShot(4000, this, SLOT(showPhoneHint()));
            } else { delete m_phone; m_phone = 0; }
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

    connect(go, SIGNAL(clicked()), this, SLOT(go()));
    connect(clear, SIGNAL(clicked()), this, SLOT(clearUrl()));
    connect(back, SIGNAL(clicked()), m_view, SLOT(back()));
    connect(fwd, SIGNAL(clicked()), m_view, SLOT(forward()));
    connect(rel, SIGNAL(clicked()), this, SLOT(reloadClicked()));
    connect(kbd, SIGNAL(clicked()), this, SLOT(toggleKeyboard()));
    connect(zi, SIGNAL(clicked()), this, SLOT(zoomIn()));
    connect(zo, SIGNAL(clicked()), this, SLOT(zoomOut()));
    connect(save, SIGNAL(clicked()), this, SLOT(saveEpub()));
    connect(toc, SIGNAL(clicked()), this, SLOT(showToc()));
    connect(quit, SIGNAL(clicked()), this, SLOT(quit()));
    connect(shot, SIGNAL(clicked()), this, SLOT(screenshot()));
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
    // Tapping the title/status line forces a full-screen refresh (so does a two-finger tap).
    m_status->installEventFilter(this);

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
    if (obj == m_url && e->type() == QEvent::MouseButtonRelease)
        QTimer::singleShot(0, this, SLOT(showKeyboard()));
    // Status line: a plain tap forces a full refresh. A drag (a swipe down for KOReader's
    // menu that started here) doesn't.
    if (obj == m_status && e->type() == QEvent::MouseButtonPress)
        m_statusPress = static_cast<QMouseEvent *>(e)->globalPos();
    if (obj == m_status && e->type() == QEvent::MouseButtonRelease && !KoboTouchInput::gestureActive()
            && (static_cast<QMouseEvent *>(e)->globalPos() - m_statusPress).manhattanLength() < 40) {
        fullRefresh();
    }
    return QMainWindow::eventFilter(obj, e);
}

void Browser::resizeEvent(QResizeEvent *e)
{
    QMainWindow::resizeEvent(e);
    placeKeyboard();
}

void Browser::placeKeyboard()
{
    if (!m_central) return;
    int h = m_kbd->preferredHeight();
    m_kbd->setGeometry(0, m_central->height() - h, m_central->width(), h);
}

void Browser::focusUrl()
{
    m_url->setFocus();
    m_url->selectAll();
    showKeyboard();
}

void Browser::clearUrl()
{
    m_url->clear();
    m_url->setFocus();
    flashUrlBar();
    showKeyboard();
}

// The address bar is normally refreshed with the fast 1-bit waveform; after Clear or a
// backspace redraw it at once with a flashing GC16 so nothing of the old text lingers.
void Browser::flashUrlBar()
{
    if (!m_eink) return;
    if (QApplication::focusWidget() != m_url && sender() == m_kbd) return;
    QRect r(m_url->mapTo(this, QPoint(0, 0)), m_url->size());
    m_eink->markDirty(r.adjusted(-4, -4, 4, 4), m_eink->flagsFull());
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
    if (!editable) {
        // Fallback for fields the hit test misses (e.g. inside inner frames).
        QString tag = frame->evaluateJavaScript(
            "(function(){var e=document.activeElement;if(!e)return '';var t=(e.tagName||'').toLowerCase();"
            "if(t=='textarea'||e.isContentEditable)return 'y';"
            "if(t=='input'&&!/^(button|submit|reset|checkbox|radio|file|image|hidden|range|color)$/i.test(e.type||''))return 'y';"
            "return '';})()").toString();
        editable = (tag == "y") && m_lastPageTap.isValid() && m_lastPageTap.elapsed() < 400 && hit.linkUrl().isEmpty();
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
        int kbdTop = m_kbd->mapTo(m_view, QPoint(0, 0)).y();
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
    // The keys are painted with the fast waveform; flash the area once so they come up crisp.
    if (m_eink) m_eink->markDirty(QRect(m_kbd->mapTo(this, QPoint(0, 0)), m_kbd->size()), m_eink->flagsFull());
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
            return m_native->request(m_nativeKind, info.mid(a + 1, b - a - 1), info.mid(b + 1));
        }
        // Nothing editable has focus in the page: the keyboard is for the address bar.
        m_url->setFocus();
    }
    m_nativeKind = "url";
    return m_native->request("url", "Address or search", m_url->text());
}

void Browser::nativeStarted()
{
    if (m_kbd->isVisible()) m_kbd->hide();
    if (m_eink) m_eink->setSuspended(true);
    if (m_touch) m_touch->setHandedOff(true);
}

void Browser::nativeReplied(const QString &action, const QString &text)
{
    if (m_touch) m_touch->setHandedOff(false);
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
    qDebug() << "menu: toolbar swipe, opening KOReader's menu";
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
    if (m_eink) m_eink->hintFull();   // the keys leave ghosts: flash the relayout that follows
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
    if (m_offline) {
        m_status->setText("Offline copy (no connection)");
        m_offline = false;
        m_offlineShown = true;
        checkOnline();
    } else {
        m_status->setText(ok ? m_view->title() : "Failed to load page");
    }
    if (m_phone) m_phone->setCurrentPage(m_view->title(), m_view->url().toString());
    if (ok && m_restoreY > 0) {
        m_view->page()->mainFrame()->setScrollPosition(QPoint(0, m_restoreY));
        qDebug() << "restore: scrolled to" << m_restoreY;
        m_restoreY = -1;
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
// exit. applauncher.koplugin puts KOReader to sleep with its own screensaver and
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
        SaveChoiceDialog *d = new SaveChoiceDialog(m_seriesLinks.size(), QString(), centralWidget());
        connect(d, SIGNAL(pageChosen()), this, SLOT(savePageOnly()));
        connect(d, SIGNAL(seriesChosen()), this, SLOT(saveSeries()));
        return;
    }
    savePageOnly();
}

void Browser::savePageOnly()
{
    if (m_saver && m_saver->isRunning()) return;
    if (saveImageIfImagePage()) return;
    delete m_saver;
    m_saver = new EpubSaver(m_view->page(), EpubSaver::defaultOutDir(), this);
    connect(m_saver, SIGNAL(progress(QString)), this, SLOT(saveProgress(QString)));
    connect(m_saver, SIGNAL(finished(bool,QString)), this, SLOT(saveFinished(bool,QString)));
    m_saver->start();
}

void Browser::saveSeries()
{
    if (m_saver && m_saver->isRunning()) return;
    delete m_saver;
    m_saver = new EpubSaver(m_view->page(), EpubSaver::defaultOutDir(), this);
    connect(m_saver, SIGNAL(progress(QString)), this, SLOT(saveProgress(QString)));
    connect(m_saver, SIGNAL(finished(bool,QString)), this, SLOT(saveFinished(bool,QString)));
    m_saver->startSeries(m_seriesLinks, m_seriesTitle);
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
    m_status->setText("From your phone: " + a);
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
        connect(t, SIGNAL(closed()), this, SLOT(fullRefresh()));
    }
}

// "Open now": the handoff line is already written; exit so KOReader (applauncher.koplugin)
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
        m_status->setText("Saved: " + shown);
        SavedDialog *d = new SavedDialog(msg, centralWidget());
        connect(d, SIGNAL(openRequested(QString)), this, SLOT(openSavedEpub(QString)));
    } else {
        m_status->setText("Save failed: " + msg);
    }
}

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
    // Test hook: KB_TEST_SAVE=<seconds> triggers Save-as-EPUB automatically.
    if (getenv("KB_TEST_SAVE")) QTimer::singleShot(atoi(getenv("KB_TEST_SAVE")) * 1000, &w, SLOT(saveEpub()));
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
