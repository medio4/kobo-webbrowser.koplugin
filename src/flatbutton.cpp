// FlatButton: the flat, KOReader-style button used across the browser.
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


// ---------------------------------------------------------------- FlatButton

FlatButton::FlatButton(const QString &text, QWidget *parent)
    : QAbstractButton(parent), m_dark(false), m_flash(getenv("KB_KEY_FLASH") != 0), m_iconOnly(false)
{
    setText(text);
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setPixelSize(40);
}

void FlatButton::setBold(bool b) { QFont f = font(); f.setBold(b); setFont(f); }
void FlatButton::setPixelSize(int px) { QFont f = font(); f.setPixelSize(px); setFont(f); }

int FlatButton::fitPixelSize(int maxPx) const
{
    QRect r = rect().adjusted(1, 1, -2, -2);
    int isz = qMin(r.height() - 16, 64);
    bool icon = !m_iconName.isEmpty() && !m_iconOnly && !KbIcons::get(m_iconName, isz).isNull();
    int room = r.width() - 24 - (icon ? isz + 14 : 0);
    QFont f = font();
    int px = maxPx;
    for (; px > 14; px -= 2) {
        f.setPixelSize(px);
        if (QFontMetrics(f).width(text()) <= room) break;
    }
    return px;
}

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
    int isz = qMin(r.height() - 16, 64);
    QPixmap icon = m_iconName.isEmpty() ? QPixmap() : KbIcons::get(m_iconName, isz);
    // Labels never get cut off: shrink the font (down to 70%) until the label fits its box,
    // and only then fall back to "...".
    auto fitFont = [&](int room) {
        QFont f = font();
        int px = f.pixelSize() > 0 ? f.pixelSize() : QFontMetrics(f).height();
        int minPx = qMax(14, px * 7 / 10);
        while (px > minPx && QFontMetrics(f).width(text()) > room) {
            px -= 2;
            f.setPixelSize(px);
        }
        p.setFont(f);
        return f;
    };
    if (icon.isNull()) {
        QFont f = fitFont(r.width() - 16);
        p.drawText(r, Qt::AlignCenter, QFontMetrics(f).elidedText(text(), Qt::ElideRight, r.width() - 8));
        return;
    }
    if (!isEnabled()) p.setOpacity(0.4);
    if (m_iconOnly || text().isEmpty()) {
        p.drawPixmap(r.center().x() - isz / 2 + 1, r.center().y() - isz / 2 + 1, icon);
        return;
    }
    // Icon then text, centred together as one group.
    int gap = 14;
    int room = r.width() - 24 - isz;
    if (QFontMetrics(font()).width(text()) + gap > room) gap = 8;
    QFont f = fitFont(room - gap);
    int tw = QFontMetrics(f).width(text());
    int total = qMin(r.width() - 16, isz + gap + tw);
    int x = r.center().x() - total / 2;
    p.drawPixmap(x, r.center().y() - isz / 2 + 1, icon);
    QRect tr(x + isz + gap, r.top(), r.right() - (x + isz + gap), r.height());
    p.drawText(tr, Qt::AlignVCenter | Qt::AlignLeft, QFontMetrics(f).elidedText(text(), Qt::ElideRight, tr.width()));
}
