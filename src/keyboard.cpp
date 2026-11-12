// OnScreenKeyboard: the browser's built-in keys (fallback when KOReader's keyboard isn't used).
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


// ------------------------------------------------------------ OnScreenKeyboard

// Layers: lower / upper (shift) / symbols. Each string is one row, 10 keys.
static const char *kLower[] = { "1234567890", "qwertyuiop", "asdfghjkl",  "zxcvbnm",  0 };
static const char *kUpper[] = { "1234567890", "QWERTYUIOP", "ASDFGHJKL",  "ZXCVBNM",  0 };
static const char *kSym[]   = { "!@#$%^&*()", "-_=+[]{}\\|", ";:'\",<>?",  "~`/.",     0 };

// Key size: taller keys are easier to hit; 5 rows of 108 px take ~40% of the screen.
static const int kKeyH = 108;
static const int kKeyGap = 6;

OnScreenKeyboard::OnScreenKeyboard(QWidget *parent)
    : QWidget(parent), m_shift(0), m_sym(0), m_shift_on(false), m_symbols(false), m_rows(0),
      m_keyH(kKeyH), m_fitW(1072)
{
    setProperty("kb_fast", true);          // refreshed with the fast DU waveform
    // (no WA_OpaquePaintEvent here: it would stop Qt from filling the white background)
    setAutoFillBackground(true);
    QPalette pal = palette(); pal.setColor(QPalette::Window, Qt::white); setPalette(pal);
    build();
}


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
        if (r == 2) { row->addSpacing(keyW / 2); m_halfSpacers << row->itemAt(row->count() - 1)->spacerItem(); }
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
        if (r == 2) { row->addSpacing(keyW / 2); m_halfSpacers << row->itemAt(row->count() - 1)->spacerItem(); }
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
    // Remember each key's width at the portrait size, for fitWidth().
    foreach (FlatButton *b, findChildren<FlatButton *>()) b->setProperty("baseW", b->width());
    setFixedHeight(preferredHeight());
}

int OnScreenKeyboard::preferredHeight() const { return m_rows * (m_keyH + kKeyGap) + kKeyGap + 2; }

void OnScreenKeyboard::fitWidth(int w)
{
    if (w <= 0 || w == m_fitW) return;
    m_fitW = w;
    const int baseKeyW = (1072 - 11 * kKeyGap) / 10;
    const int keyW = (w - 11 * kKeyGap) / 10;
    // Landscape: same row count but much less height to spare, so flatter keys.
    m_keyH = w > 1200 ? 84 : kKeyH;
    foreach (FlatButton *b, findChildren<FlatButton *>()) {
        int bw = b->property("baseW").toInt();
        if (bw > 0) b->setFixedSize(bw * keyW / baseKeyW, m_keyH);
    }
    foreach (QSpacerItem *s, m_halfSpacers) if (s) s->changeSize(keyW / 2, 0);
    if (layout()) layout()->invalidate();
    setFixedHeight(preferredHeight());
}

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
