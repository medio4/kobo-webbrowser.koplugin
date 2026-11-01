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
#include "memtune.h"

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
#include <QtCore/QStringList>
#include <QtCore/QTime>
#include <QtCore/QFile>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtWebKit/QWebSettings>
#include <QtWebKitWidgets/QWebFrame>
#include <QtWebKitWidgets/QWebPage>

#include <cstdlib>
#include <cstring>
#include <signal.h>

// ---------------------------------------------------------------- TouchWebView

TouchWebView::TouchWebView(QWidget *parent)
    : QWebView(parent), m_pressed(false), m_dragging(false), m_forwarding(false),
      m_horizontal(false), m_live(false)
{
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
    }
    if (m_dragging && !m_horizontal) {
        if (!m_live && m_pressTime.elapsed() >= m_pager->flickMs()) {
            m_live = true;          // sustained drag: catch up with the finger, then follow it
            page()->mainFrame()->scroll(0, -fromPress.y());
            m_lastPos = e->pos();
        } else if (m_live) {
            // Scroll in big steps only; e-ink can't follow continuous scrolling well.
            QPoint delta = e->pos() - m_lastPos;
            if (qAbs(delta.y()) >= 40) {
                page()->mainFrame()->scroll(0, -delta.y());
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
    if (KoboTouchInput::gestureActive() || e->pos().x() < 0) { m_dragging = false; e->accept(); return; }
    if (m_dragging) {
        m_dragging = false;
        QPoint d = e->pos() - m_pressPos;
        if (m_horizontal) {
            m_pager->handleHorizontalSwipe(m_pressPos, e->pos());
        } else if (m_live) {
            // Finish the live drag: scroll by the remaining delta.
            QPoint delta = e->pos() - m_lastPos;
            if (delta.y() != 0) page()->mainFrame()->scroll(0, -delta.y());
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

static FlatButton *key(const QString &label, int w, int h, bool dark = false, int px = 44)
{
    FlatButton *b = new FlatButton(label);
    b->setFixedSize(w, h);
    b->setDark(dark);
    b->setPixelSize(px);
    return b;
}

void OnScreenKeyboard::build()
{
    const int gap = 6;
    const int keyH = 88;
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
            m_shift = key("Shift", keyW + keyW / 2 + gap / 2, keyH, true, 34);
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
            FlatButton *bs = key("<x", keyW + keyW / 2 + gap / 2, keyH, true, 40);   // backspace (the Kobo fonts lack arrow glyphs)
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
    m_sym = key("?123", keyW + keyW / 2 + gap / 2, keyH, true, 34);
    connect(m_sym, SIGNAL(clicked()), this, SLOT(toggleSymbols()));
    bottom->addWidget(m_sym);
    struct { const char *label, *text; int w; } fixed[] = {
        { ".com", ".com", keyW + keyW / 2 },
        { ".", ".", keyW },
        { "/", "/", keyW },
    };
    for (unsigned i = 0; i < sizeof fixed / sizeof fixed[0]; ++i) {
        FlatButton *b = key(fixed[i].label, fixed[i].w, keyH, false, 36);
        b->setProperty("keytext", QString::fromLatin1(fixed[i].text));
        connect(b, SIGNAL(clicked()), this, SLOT(keyClicked()));
        bottom->addWidget(b);
    }
    FlatButton *space = key("", keyW * 2 + gap, keyH);
    space->setProperty("keytext", " ");
    connect(space, SIGNAL(clicked()), this, SLOT(keyClicked()));
    bottom->addWidget(space);
    FlatButton *hide = key("Hide", keyW + keyW / 2, keyH, true, 36);
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

int OnScreenKeyboard::preferredHeight() const { return m_rows * (88 + 6) + 6 + 2; }

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

Browser::Browser(const QString &home, KoboEink *eink, QWidget *parent)
    : QMainWindow(parent), m_home(home), m_eink(eink), m_saver(0)
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
    FlatButton *back = toolButton("<", 90, bh);
    FlatButton *fwd  = toolButton(">", 90, bh);
    FlatButton *rel  = toolButton("Reload", 140, bh);
    FlatButton *kbd  = toolButton("Keys", 110, bh);
    FlatButton *zi   = toolButton("A+", 90, bh);
    FlatButton *zo   = toolButton("A-", 90, bh);
    FlatButton *save = toolButton("Save", 110, bh);
    save->setToolTip("Save this page as an EPUB in the library");
    FlatButton *shot = toolButton("Shot", 100, bh);
    FlatButton *quit = toolButton("Exit", 80, bh);
    QHBoxLayout *row2 = new QHBoxLayout; row2->setSpacing(6);
    row2->addWidget(back); row2->addWidget(fwd); row2->addWidget(rel);
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
    cache->setMaximumCacheSize(MemTune::diskCacheBytes());
    m_view->page()->networkAccessManager()->setCache(cache);

    connect(go, SIGNAL(clicked()), this, SLOT(go()));
    connect(clear, SIGNAL(clicked()), this, SLOT(clearUrl()));
    connect(back, SIGNAL(clicked()), m_view, SLOT(back()));
    connect(fwd, SIGNAL(clicked()), m_view, SLOT(forward()));
    connect(rel, SIGNAL(clicked()), m_view, SLOT(reload()));
    connect(kbd, SIGNAL(clicked()), this, SLOT(toggleKeyboard()));
    connect(zi, SIGNAL(clicked()), this, SLOT(zoomIn()));
    connect(zo, SIGNAL(clicked()), this, SLOT(zoomOut()));
    connect(save, SIGNAL(clicked()), this, SLOT(saveEpub()));
    connect(quit, SIGNAL(clicked()), this, SLOT(quit()));
    connect(shot, SIGNAL(clicked()), this, SLOT(screenshot()));
    connect(m_url, SIGNAL(returnPressed()), this, SLOT(go()));
    connect(m_kbd, SIGNAL(goPressed()), this, SLOT(keyboardGo()));
    connect(m_kbd, SIGNAL(hidePressed()), this, SLOT(hideKeyboard()));
    connect(m_view, SIGNAL(urlChanged(QUrl)), this, SLOT(urlChanged(QUrl)));
    connect(m_view, SIGNAL(loadStarted()), this, SLOT(loadStarted()));
    connect(m_view, SIGNAL(loadProgress(int)), this, SLOT(loadProgress(int)));
    connect(m_view, SIGNAL(loadFinished(bool)), this, SLOT(loadFinished(bool)));
    connect(m_view, SIGNAL(loadFinished(bool)), new MemTune(this), SLOT(scheduleTrim()));
    if (m_eink)
        connect(m_view->pager(), SIGNAL(pageTurned()), m_eink, SLOT(hintPageTurn()));

    // A tap on a text field in the page pops the keyboard up.
    connect(m_view->page(), SIGNAL(microFocusChanged()), this, SLOT(pageFocusChanged()));

    // Tapping the URL field shows the keyboard.
    m_url->installEventFilter(this);
    m_view->installEventFilter(this);
    // Tapping the title/status line forces a full-screen refresh (so does a two-finger tap).
    m_status->installEventFilter(this);

    loadUrlText(m_home);
}

bool Browser::eventFilter(QObject *obj, QEvent *e)
{
    if (obj == m_view && (e->type() == QEvent::MouseButtonPress || e->type() == QEvent::MouseButtonRelease))
        m_lastPageTap.start();
    if (obj == m_url && e->type() == QEvent::MouseButtonRelease)
        QTimer::singleShot(0, this, SLOT(showKeyboard()));
    if (obj == m_status && e->type() == QEvent::MouseButtonRelease && !KoboTouchInput::gestureActive())
        fullRefresh();
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
    showKeyboard();
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

void Browser::pageFocusChanged()
{
    if (m_kbd->isVisible()) return;
    // Only pop the keyboard for a focus change caused by a recent tap on the page,
    // not for scripts focusing a field while the page loads.
    if (!m_lastPageTap.isValid() || m_lastPageTap.elapsed() > 1500) return;
    QVariant v = m_view->page()->inputMethodQuery(Qt::ImEnabled);
    if (v.isValid() && v.toBool() && QApplication::focusWidget() == m_view)
        showKeyboard();
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

void Browser::showKeyboard()
{
    if (m_kbd->isVisible()) return;
    placeKeyboard();
    m_kbd->raise();
    m_kbd->show();
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

void Browser::zoomIn()  { if (m_eink) m_eink->hintFull(); m_view->setZoomFactor(m_view->zoomFactor() * 1.2); }
void Browser::zoomOut() { if (m_eink) m_eink->hintFull(); m_view->setZoomFactor(m_view->zoomFactor() / 1.2); }

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
    delete m_saver;
    m_saver = new EpubSaver(m_view->page(), EpubSaver::defaultOutDir(), this);
    connect(m_saver, SIGNAL(progress(QString)), this, SLOT(saveProgress(QString)));
    connect(m_saver, SIGNAL(finished(bool,QString)), this, SLOT(saveFinished(bool,QString)));
    m_saver->start();
}

void Browser::saveProgress(const QString &msg) { m_status->setText(msg); }

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
    // A blinking text cursor would refresh the address bar twice a second.
    app.setCursorFlashTime(0);

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
    // Two-finger tap anywhere = full-screen GC16 refresh (KOReader's gestures can't reach us).
    if (touchInput) QObject::connect(touchInput, SIGNAL(twoFingerTap()), eink, SLOT(refreshAllFull()));
    // (pinch-to-zoom is connected to the Browser below, once it exists)
    app.installEventFilter(eink);
    Browser w(home, eink);
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
    return app.exec();
}
